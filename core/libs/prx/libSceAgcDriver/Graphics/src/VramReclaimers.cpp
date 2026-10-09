#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdio>
#include <mutex>

namespace AgcDriver::Graphics {

namespace {

unsigned long long asMib(VkDeviceSize bytes) {
    return static_cast<unsigned long long>((bytes + (VkDeviceSize{1} << 19u)) >> 20u);
}

// Cold entries of the driver's caches (held by the cache alone and, for builds and textures, old
// enough) and the device-local bytes they hold: what the reclaimers could still free. Busy caches
// are skipped; builds and textures are counted over their 4096 least recently used entries.
std::string census() {
    std::string line;
    char text[256];
    // Builds are read (their regions and textures) only under the GPU mutex, where they change.
    const bool locked = GuestMemory::GpuMutex().HeldByThisThread();
    if (const auto builds = locked ? SharedResourceCache().ColdCensus() : VramCacheCensus{}; builds.measured) {
        std::snprintf(text, sizeof(text), "; cold: builds %llu of %llu (%llu MiB alone)", static_cast<unsigned long long>(builds.cold), static_cast<unsigned long long>(builds.entries), asMib(builds.coldBytes));
        line += text;
    } else {
        line += "; cold: builds busy";
    }
    if (const auto textures = SampledTextureCensus(); textures.measured) {
        std::snprintf(text, sizeof(text), ", textures %llu of %llu (%llu MiB)", static_cast<unsigned long long>(textures.cold), static_cast<unsigned long long>(textures.entries), asMib(textures.coldBytes));
        line += text;
    }
    if (const auto shadows = StagedShadowCensus(); shadows.measured) {
        std::snprintf(text, sizeof(text), ", shadow registry %llu of %llu (%llu MiB)", static_cast<unsigned long long>(shadows.cold), static_cast<unsigned long long>(shadows.entries), asMib(shadows.coldBytes));
        line += text;
    }
    return line;
}

}

void RegisterDriverVramReclaimers() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& budget = Vram();
        // The registry walk runs inside allocations: at most every 10 ms.
        budget.AddReclaimer("shadows", VramReclaimLevel::Inline, 10, [](VkDeviceSize want, std::uint64_t& evicted) { return TrimStagedShadows(want, evicted); }, 10);
        budget.AddReclaimer("textures", VramReclaimLevel::SafePoint, 20, [](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
            std::vector<std::shared_ptr<void>> victims;
            VkDeviceSize dedicated = 0;
            const auto freed = EvictColdTextures(want, victims, dedicated);
            evicted = victims.size();
            // Destroyed off this thread once the open batch completed; their own memory is
            // certain to go (a pooled one's only becomes slack).
            DisposeVramVictims(std::move(victims), dedicated);
            return freed;
        });
        budget.AddReclaimer("builds", VramReclaimLevel::SafePoint, 30, [](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
            // A build is revalidated and refreshed under the GPU mutex (a recipe may take a cold one
            // through its weak reference meanwhile): its regions are read only under that mutex.
            if (!GuestMemory::GpuMutex().HeldByThisThread()) return 0;
            std::vector<std::shared_ptr<ShaderResources>> victims;
            const auto held = SharedResourceCache().EvictCold(want, victims);
            evicted = victims.size();
            std::vector<std::shared_ptr<void>> objects(victims.begin(), victims.end());
            victims.clear();
            // Pending: only what their destruction frees for certain (shadows only they hold).
            // What they shared (textures, registered shadows) is the other reclaimers' to free
            // next; it counts as reclaimed so this pass does not evict more builds for it.
            DisposeVramVictims(std::move(objects), held.alone);
            return held.Total();
        });
        budget.AddReclaimer("resident", VramReclaimLevel::SafePoint, 40, [](VkDeviceSize want, std::uint64_t& evicted) { return TrimResidentReads(want, evicted); });
        SetVramCensus(&census);
    });
}

void DisposeVramVictims(std::vector<std::shared_ptr<void>> victims, VkDeviceSize estimate) {
    if (victims.empty()) return;
    auto* recorder = Recorder::Active();
    if (recorder != nullptr && GuestMemory::GpuMutex().HeldByThisThread() && recorder->Recording()) {
        // Kept by the open batch: destroyed on the release thread once it completed (an evicted
        // object no batch referenced, so nothing waits on it), never under the GPU mutex.
        struct Held {
            std::vector<std::shared_ptr<void>> objects;
            VkDeviceSize bytes = 0;
            ~Held() {
                // No longer pending before they go: a buffer they release into the pool sees the
                // pressure as it is and is destroyed instead of retained while over.
                Vram().RemovePending(bytes);
                objects.clear();
            }
        };
        try {
            auto held = std::make_shared<Held>();
            held->objects = std::move(victims);
            Vram().AddPending(estimate);
            held->bytes = estimate;
            recorder->Keep(std::move(held), static_cast<std::size_t>(estimate));
            return;
        } catch (...) {
            // Not kept: what was moved into the holder went with it; the rest goes below.
        }
    }
    victims.clear();
}

}
