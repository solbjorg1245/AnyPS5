// Video-memory budget (Graphics::VramBudget, port/reports/s53-gpu3-vram.md): the accounting by
// class, the target and the pressure it derives, the order the reclaimers run in, the refusals of
// allocations with a fallback, and a scripted area load against a simulated 2 GiB heap that
// completes without a refused allocation while the budget is on (and refuses with it off, the
// t421-t424 failure). Pure arithmetic: no Vulkan device.
#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

constexpr VkDeviceSize Mib = VkDeviceSize{1} << 20u;
constexpr VkDeviceSize Gib = VkDeviceSize{1} << 30u;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

VramBudget::Settings settings(bool enabled = true) {
    VramBudget::Settings value;
    value.enabled = enabled;
    value.backoffMs = 50;
    return value;
}

VkDevice fakeDevice(std::uintptr_t value) {
    return reinterpret_cast<VkDevice>(value);
}

VkDeviceMemory fakeMemory(std::uintptr_t value) {
    return reinterpret_cast<VkDeviceMemory>(value);
}

void accountingTests() {
    VramBudget budget(settings());
    const auto device = fakeDevice(0x10);
    budget.NoteAllocation(device, fakeMemory(1), VramClass::Buffers, 4 * Mib);
    budget.NoteAllocation(device, fakeMemory(2), VramClass::Textures, 64 * Mib);
    budget.NoteAllocation(device, fakeMemory(3), VramClass::Targets, 32 * Mib);
    Require(budget.Tracked() == 100 * Mib && budget.Tracked(VramClass::Buffers) == 4 * Mib && budget.Tracked(VramClass::Textures) == 64 * Mib && budget.Tracked(VramClass::Targets) == 32 * Mib, "accounting: the classes do not hold what was noted");
    Require(budget.Allocations() == 3, "accounting: three allocations noted, not three in the ledger");
    // Released into the pool and taken again under another class.
    budget.Reclass(device, fakeMemory(1), VramClass::Pool);
    Require(budget.Tracked(VramClass::Pool) == 4 * Mib && budget.Tracked(VramClass::Buffers) == 0 && budget.Tracked() == 100 * Mib, "accounting: a pool return did not move the bytes to the pool");
    budget.Reclass(device, fakeMemory(1), VramClass::Resident);
    Require(budget.Tracked(VramClass::Pool) == 0 && budget.Tracked(VramClass::Resident) == 4 * Mib, "accounting: a take did not move the bytes to the taker's class");
    // The same handle value on another device is another allocation; an unknown one is ignored.
    budget.Forget(fakeDevice(0x20), fakeMemory(2));
    budget.Reclass(fakeDevice(0x20), fakeMemory(2), VramClass::Pool);
    Require(budget.Tracked() == 100 * Mib && budget.Tracked(VramClass::Textures) == 64 * Mib, "accounting: another device's handle changed this device's allocation");
    budget.Forget(device, fakeMemory(2));
    budget.Forget(device, fakeMemory(2));
    Require(budget.Tracked() == 36 * Mib && budget.Tracked(VramClass::Textures) == 0 && budget.Allocations() == 2, "accounting: a free did not remove its bytes exactly once");
    // A handle noted again without a free (the driver reused it): its old bytes are replaced.
    budget.NoteAllocation(device, fakeMemory(3), VramClass::Other, 8 * Mib);
    Require(budget.Tracked(VramClass::Targets) == 0 && budget.Tracked(VramClass::Other) == 8 * Mib && budget.Tracked() == 12 * Mib, "accounting: a handle noted twice kept its old bytes");
    // Image-pool slack: a block is slack until images are placed in it.
    budget.NoteAllocation(device, fakeMemory(4), VramClass::Slack, 256 * Mib);
    budget.Move(VramClass::Slack, VramClass::Textures, 48 * Mib);
    Require(budget.Tracked(VramClass::Slack) == 208 * Mib && budget.Tracked(VramClass::Textures) == 48 * Mib, "accounting: a pooled image did not move from slack to textures");
    budget.Move(VramClass::Textures, VramClass::Slack, 48 * Mib);
    budget.Forget(device, fakeMemory(4));
    Require(budget.Tracked(VramClass::Slack) == 0 && budget.Tracked(VramClass::Textures) == 0, "accounting: an emptied block did not leave the accounting whole");
    // Removing more than a class holds clamps at zero instead of wrapping.
    budget.Remove(VramClass::Pool, 5 * Mib);
    Require(budget.Tracked(VramClass::Pool) == 0, "accounting: a class wrapped below zero");
    std::printf("vram accounting: ok\n");
}

void targetTests() {
    VramBudget budget(settings());
    Require(budget.Target() == ~VkDeviceSize{0} && budget.Excess(Gib) == 0, "target: an unknown heap bounds something");
    budget.SetHeap(16 * Gib);
    Require(budget.Headroom() == Gib && budget.Target() == 15 * Gib, "target: a 16 GiB heap without a report is not the heap minus 1 GiB");
    budget.SetHeap(8 * Gib);
    Require(budget.Headroom() == 512 * Mib && budget.Target() == 8 * Gib - 512 * Mib, "target: an 8 GiB heap does not keep 512 MiB");
    budget.SetHeap(2 * Gib);
    Require(budget.Headroom() == 256 * Mib && budget.Target() == 2 * Gib - 256 * Mib, "target: the headroom floor is not 256 MiB");
    // The driver's budget (other processes' use subtracted) wins over the heap.
    budget.SetHeap(16 * Gib);
    budget.Report(14 * Gib, 3 * Gib);
    Require(budget.Target() == 13 * Gib, "target: the reported budget minus the headroom is not the target");
    // Never below min(heap / 4, 1 GiB), whatever another process took.
    budget.Report(512 * Mib, 400 * Mib);
    Require(budget.Target() == Gib, "target: a tiny reported budget went below the floor");
    // The override is the target.
    budget.SetTargetOverride(2 * Gib);
    Require(budget.Target() == 2 * Gib, "target: APS5_VRAM_BUDGET_MIB is not the target");
    budget.SetTargetOverride(0);
    budget.SetHeadroom(64 * Mib);
    budget.Report(14 * Gib, 3 * Gib);
    Require(budget.Target() == 14 * Gib - 64 * Mib, "target: APS5_VRAM_HEADROOM_MIB is not the headroom");
    std::printf("vram target: ok\n");
}

void usedTests() {
    VramBudget budget(settings());
    const auto device = fakeDevice(0x10);
    budget.SetHeap(16 * Gib);
    budget.NoteAllocation(device, fakeMemory(1), VramClass::Textures, 3 * Gib);
    Require(budget.Used() == 3 * Gib, "used: without a report the accounting is not the use");
    // The driver sees more (its own allocations, pipelines): its figure, moved by what the
    // accounting saw since.
    budget.Report(15 * Gib, 4 * Gib);
    Require(budget.Used() == 4 * Gib, "used: the report is not the use");
    budget.NoteAllocation(device, fakeMemory(2), VramClass::Buffers, Gib);
    Require(budget.Used() == 5 * Gib, "used: an allocation after the report did not count");
    budget.Forget(device, fakeMemory(1));
    Require(budget.Used() == 2 * Gib, "used: a free after the report did not count");
    budget.ClearReport();
    Require(budget.Used() == Gib, "used: a cleared report still counts");
    std::printf("vram used: ok\n");
}

void excessTests() {
    VramBudget budget(settings());
    const auto device = fakeDevice(0x10);
    budget.SetHeap(4 * Gib);
    budget.SetTargetOverride(Gib);
    budget.NoteAllocation(device, fakeMemory(1), VramClass::Buffers, Gib);
    Require(budget.Excess() == 0 && budget.Excess(Mib) == Mib && budget.Over(Mib), "excess: an allocation past the target is not over it");
    // Slack counts as room up to the headroom (4 GiB heap: 256 MiB), no further.
    budget.NoteAllocation(device, fakeMemory(2), VramClass::Slack, 200 * Mib);
    Require(budget.Excess() == 0, "excess: slack within the headroom counts as pressure");
    budget.NoteAllocation(device, fakeMemory(3), VramClass::Slack, 200 * Mib);
    Require(budget.Excess() == 144 * Mib, "excess: slack past the headroom does not count as pressure");
    budget.Forget(device, fakeMemory(3));
    budget.Forget(device, fakeMemory(2));
    // Evicted objects whose destruction waits for their batch.
    budget.NoteAllocation(device, fakeMemory(4), VramClass::Textures, 300 * Mib);
    Require(budget.Excess() == 300 * Mib, "excess: an allocation over the target is not the excess");
    budget.AddPending(200 * Mib);
    Require(budget.Excess() == 100 * Mib, "excess: pending bytes count as pressure");
    budget.RemovePending(200 * Mib);
    // Off (APS5_NO_VRAM_BUDGET=1): never over, nothing refused.
    budget.SetEnabled(false);
    Require(budget.Excess(Gib) == 0 && budget.Admit(VramClass::Buffers, Gib), "excess: the budget is off but refuses");
    std::printf("vram excess: ok\n");
}

// The reclaimers run in their order (lower first), only those the level allows, until the excess
// is freed; one that freed nothing waits the backoff.
void reclaimOrderTests() {
    VramBudget budget(settings());
    std::uint64_t clock = 1000;
    budget.SetClock([&] { return clock; });
    const auto device = fakeDevice(0x10);
    budget.SetHeap(4 * Gib);
    budget.SetTargetOverride(Gib);
    std::vector<std::string> calls;
    // Each holds bytes of the ledger and gives them back when asked.
    struct Holder {
        VkDeviceSize bytes;
        std::uintptr_t handle;
    };
    std::vector<Holder> pool{{100 * Mib, 10}}, shadows{{50 * Mib, 20}}, textures{{200 * Mib, 30}, {200 * Mib, 31}}, builds{{300 * Mib, 40}};
    for (const auto* holders : {&pool, &shadows, &textures, &builds}) {
        for (const auto& holder : *holders) budget.NoteAllocation(device, fakeMemory(holder.handle), VramClass::Buffers, holder.bytes);
    }
    const auto reclaimer = [&](const char* name, std::vector<Holder>& holders) {
        return [&, name](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
            calls.push_back(name);
            VkDeviceSize freed = 0;
            while (freed < want && !holders.empty()) {
                freed += holders.front().bytes;
                budget.Forget(device, fakeMemory(holders.front().handle));
                holders.erase(holders.begin());
                ++evicted;
            }
            return freed;
        };
    };
    // Registered out of order.
    budget.AddReclaimer("builds", VramReclaimLevel::SafePoint, 30, reclaimer("builds", builds));
    budget.AddReclaimer("pool", VramReclaimLevel::Inline, 0, reclaimer("pool", pool));
    budget.AddReclaimer("textures", VramReclaimLevel::SafePoint, 20, reclaimer("textures", textures));
    budget.AddReclaimer("shadows", VramReclaimLevel::Inline, 10, reclaimer("shadows", shadows));
    Require(budget.Tracked() == 850 * Mib, "reclaim: the reclaimers' memory is not accounted");
    // 850 MiB held + 1 GiB of other memory: 850 MiB over.
    budget.NoteAllocation(device, fakeMemory(99), VramClass::Targets, Gib);
    Require(budget.Excess() == 850 * Mib, "reclaim: the excess is not what the reclaimers hold");
    // Inline: only the pool and the shadows.
    Require(budget.Relieve(VramReclaimLevel::Inline) == 150 * Mib, "reclaim: an inline relieve freed what it may not");
    Require((calls == std::vector<std::string>{"pool", "shadows"}), "reclaim: the inline relieve did not run the pool before the shadows");
    calls.clear();
    // A safe point: the rest in order until the 700 MiB left are freed (both textures, then the
    // build); the inline relieve fell short of the excess.
    Require(budget.Shortfalls() == 1, "reclaim: the inline relieve's shortfall was not counted");
    const auto freed = budget.Relieve(VramReclaimLevel::SafePoint);
    Require(freed == 700 * Mib && budget.Excess() == 0, "reclaim: the safe point did not free the excess");
    // The pool and shadows freed nothing: they wait the backoff, the textures and builds ran.
    Require((calls == std::vector<std::string>{"pool", "shadows", "textures", "builds"}), "reclaim: the safe point did not run the reclaimers in order");
    calls.clear();
    // Over again by 100 MiB with nothing left to free anywhere: every reclaimer ran once and now
    // backs off; the relieve within the backoff calls none.
    budget.NoteAllocation(device, fakeMemory(97), VramClass::Targets, 100 * Mib);
    Require(budget.Relieve(VramReclaimLevel::SafePoint) == 0 && budget.Shortfalls() == 2, "reclaim: a relieve with nothing to free was no shortfall");
    Require((calls == std::vector<std::string>{"textures", "builds"}), "reclaim: reclaimers inside their backoff were called");
    calls.clear();
    clock += 10;
    budget.Relieve(VramReclaimLevel::SafePoint);
    Require(calls.empty(), "reclaim: the backoff did not hold");
    clock += 50;
    budget.Relieve(VramReclaimLevel::SafePoint);
    Require((calls == std::vector<std::string>{"pool", "shadows", "textures", "builds"}), "reclaim: the reclaimers were not retried after the backoff");
    // Counts per reclaimer, in order.
    const auto counts = budget.Reclaimers();
    Require(counts.size() == 4 && counts[0].name == "pool" && counts[0].evicted == 1 && counts[0].bytes == 100 * Mib && counts[2].name == "textures" && counts[2].evicted == 2 && counts[3].evicted == 1 && counts[3].bytes == 300 * Mib, "reclaim: the per-reclaimer counts are wrong");
    // A removed reclaimer is not called again.
    budget.RemoveReclaimer(1);
    calls.clear();
    clock += 100;
    budget.Relieve(VramReclaimLevel::SafePoint);
    Require((calls == std::vector<std::string>{"pool", "shadows", "textures"}), "reclaim: a removed reclaimer was called");
    std::printf("vram reclaim order: ok\n");
}

void admitTests() {
    VramBudget budget(settings());
    const auto device = fakeDevice(0x10);
    budget.SetHeap(4 * Gib);
    budget.SetTargetOverride(Gib);
    budget.NoteAllocation(device, fakeMemory(1), VramClass::Textures, Gib - 8 * Mib);
    std::vector<VkDeviceSize> retained{4 * Mib, 4 * Mib};
    budget.AddReclaimer("pool", VramReclaimLevel::Inline, 0, [&](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
        VkDeviceSize freed = 0;
        while (freed < want && !retained.empty()) {
            freed += retained.back();
            budget.Remove(VramClass::Pool, retained.back());
            retained.pop_back();
            ++evicted;
        }
        return freed;
    });
    budget.Add(VramClass::Pool, 8 * Mib);
    // Fits: admitted without a relieve.
    Require(budget.Admit(VramClass::Buffers, 0) && budget.Relieves() == 0, "admit: an allocation within the target relieved");
    // 4 MiB over: the pool gives one slot back and the allocation is admitted.
    Require(budget.Admit(VramClass::Buffers, 4 * Mib) && retained.size() == 1 && budget.Refusals(VramClass::Buffers) == 0, "admit: the pool's retained memory did not make room");
    // 16 MiB over with 4 MiB retained: refused and counted.
    Require(!budget.Admit(VramClass::Resident, 16 * Mib) && budget.Refusals(VramClass::Resident) == 1 && retained.empty(), "admit: an allocation the reclaimers could not make room for was admitted");
    const auto line = budget.Digest(true);
    Require(line.find("[vram] target 1024 MiB") == 0 && line.find("refusals resident 1 (16 MiB)") != std::string::npos && line.find("pool 2 (8 MiB)") != std::string::npos, "admit: the [vram] line misses the refusal or the evictions: " + line);
    std::printf("%s", line.c_str());
    const auto next = budget.Digest(true);
    Require(next.find("refusals 0") != std::string::npos && next.find("pool 0 (0 MiB)") != std::string::npos, "admit: the [vram] window was not restarted: " + next);
    std::printf("vram admit: ok\n");
}

// An area load against a simulated 2 GiB heap: per step a new texture (16-64 MiB) and a lookup of
// an older one, a build whose staging shadow (2-8 MiB) is cached with it, and a scratch buffer
// (8 MiB) released into the pool at the end of the step; work recorded in the last four steps
// still references what it used (the shared_ptr a batch keeps), 512 MiB of targets stay. The
// "driver" refuses any allocation past 2 GiB, as the RTX 5080 did at 16 GiB in t421-t424. With the
// budget on, the reclaimers (pool inline, then textures and builds at the safe points) keep the
// heap under its target and nothing is refused; nothing in-flight work references is evicted, and
// each cache evicts least recently used first. With it off only the caches' own bounds act (at the
// simulation's scale: 600 builds, 6 GiB of textures) and the driver refuses.
struct LoadResult {
    std::uint64_t driverRefusals = 0;
    VkDeviceSize peak = 0;
    VkDeviceSize target = 0;
    std::uint64_t poolEvictions = 0;
    std::uint64_t textureEvictions = 0;
    std::uint64_t buildEvictions = 0;
    std::uint64_t refusedShadows = 0;
    std::uint64_t notRetained = 0;
};

LoadResult simulateLoad(bool enabled) {
    VramBudget budget(settings(enabled));
    budget.SetClock([] { return std::uint64_t{0}; });
    budget.SetBackoffMs(0);
    constexpr VkDeviceSize heap = 2 * Gib;
    budget.SetHeap(heap);
    const auto device = fakeDevice(0x10);
    std::uintptr_t nextHandle = 1;
    LoadResult result;
    result.target = budget.Target();
    struct Entry {
        std::uintptr_t handle = 0;
        VkDeviceSize bytes = 0;
        std::uint64_t serial = 0;
        // Shared with the work that used it (the reference a batch keeps until it completed).
        std::shared_ptr<int> held = std::make_shared<int>(0);
    };
    std::list<Entry> textures, builds;
    std::deque<Entry> pool;
    std::deque<std::pair<int, std::shared_ptr<int>>> inFlight;
    std::uint64_t serial = 0;
    const auto allocate = [&](VramClass type, VkDeviceSize bytes) -> std::uintptr_t {
        // Inside the allocation: the Inline reclaimers.
        budget.Relieve(VramReclaimLevel::Inline, bytes);
        if (budget.Tracked() + bytes > heap) {
            ++result.driverRefusals;
            return 0;
        }
        const auto handle = nextHandle++;
        budget.NoteAllocation(device, fakeMemory(handle), type, bytes);
        result.peak = std::max(result.peak, budget.Tracked());
        return handle;
    };
    const auto release = [&](const Entry& entry) {
        // Back into the pool, unless over the target (then the driver gets it back).
        if (budget.Over()) {
            budget.Forget(device, fakeMemory(entry.handle));
            budget.CountNotRetained(entry.bytes);
        } else {
            budget.Reclass(device, fakeMemory(entry.handle), VramClass::Pool);
            pool.push_back(entry);
        }
    };
    // Cold entries (no work holds them) from the least recently used end; one call evicts in
    // ascending use order.
    const auto evictCold = [&](std::list<Entry>& cache, VkDeviceSize want, std::uint64_t& evicted, bool toPool) -> VkDeviceSize {
        VkDeviceSize freed = 0;
        std::uint64_t lastSerial = 0;
        for (auto it = cache.begin(); it != cache.end() && freed < want;) {
            if (it->held.use_count() != 1) {
                ++it;
                continue;
            }
            Require(it->serial > lastSerial, "load: a cache did not evict least recently used first");
            lastSerial = it->serial;
            freed += it->bytes;
            ++evicted;
            if (toPool) release(*it);
            else budget.Forget(device, fakeMemory(it->handle));
            it = cache.erase(it);
        }
        return freed;
    };
    budget.AddReclaimer("pool", VramReclaimLevel::Inline, 0, [&](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
        VkDeviceSize freed = 0;
        while (freed < want && !pool.empty()) {
            freed += pool.front().bytes;
            budget.Forget(device, fakeMemory(pool.front().handle));
            pool.pop_front();
            ++evicted;
            ++result.poolEvictions;
        }
        return freed;
    });
    budget.AddReclaimer("textures", VramReclaimLevel::SafePoint, 20, [&](VkDeviceSize want, std::uint64_t& evicted) {
        const auto freed = evictCold(textures, want, evicted, false);
        result.textureEvictions += evicted;
        return freed;
    });
    budget.AddReclaimer("builds", VramReclaimLevel::SafePoint, 30, [&](VkDeviceSize want, std::uint64_t& evicted) {
        const auto freed = evictCold(builds, want, evicted, true);
        result.buildEvictions += evicted;
        return freed;
    });
    for (int i = 0; i < 4; ++i) allocate(VramClass::Targets, 128 * Mib);
    std::uint32_t random = 12345;
    const auto next = [&] {
        random = random * 1664525u + 1013904223u;
        return random >> 8u;
    };
    const auto use = [&](int step, const std::shared_ptr<int>& held) { inFlight.emplace_back(step, held); };
    for (int step = 0; step < 2000; ++step) {
        // Batches complete: work older than four steps lets go.
        while (!inFlight.empty() && inFlight.front().first < step - 4) inFlight.pop_front();
        // A safe point per step (a resource cache insert, a present).
        budget.Relieve(VramReclaimLevel::SafePoint);
        const VkDeviceSize textureBytes = (16 + next() % 49) * Mib;
        if (const auto handle = allocate(VramClass::Textures, textureBytes)) {
            textures.push_back({handle, textureBytes, ++serial});
            use(step, textures.back().held);
        }
        if (!textures.empty() && next() % 4 == 0) {
            auto it = std::next(textures.begin(), static_cast<std::ptrdiff_t>(next() % textures.size()));
            it->serial = ++serial;
            textures.splice(textures.end(), textures, it);
            use(step, textures.back().held);
        }
        // The build's shadow has a fallback (bound in place), so it is refused over the target.
        const VkDeviceSize shadowBytes = (2 + next() % 7) * Mib;
        if (!budget.Admit(VramClass::Buffers, shadowBytes)) {
            ++result.refusedShadows;
        } else if (const auto handle = allocate(VramClass::Buffers, shadowBytes)) {
            builds.push_back({handle, shadowBytes, ++serial});
            use(step, builds.back().held);
        }
        // A scratch buffer used and released within the step.
        Entry scratch;
        scratch.bytes = 8 * Mib;
        scratch.handle = allocate(VramClass::Buffers, scratch.bytes);
        if (scratch.handle != 0) release(scratch);
        // The caches' own bounds.
        for (auto it = builds.begin(); builds.size() > 600 && it != builds.end();) {
            if (it->held.use_count() != 1) {
                ++it;
                continue;
            }
            release(*it);
            it = builds.erase(it);
        }
        VkDeviceSize textureTotal = 0;
        for (const auto& entry : textures) textureTotal += entry.bytes;
        for (auto it = textures.begin(); textureTotal > 6 * Gib && it != textures.end();) {
            if (it->held.use_count() != 1) {
                ++it;
                continue;
            }
            textureTotal -= it->bytes;
            budget.Forget(device, fakeMemory(it->handle));
            it = textures.erase(it);
        }
        // What in-flight work references is still there.
        for (const auto& [at, held] : inFlight) Require(held.use_count() > 1, "load: an entry in-flight work references was evicted");
    }
    result.notRetained = budget.NotRetained();
    return result;
}

void smallBudgetTests() {
    const auto on = simulateLoad(true);
    Require(on.target == 2 * Gib - 256 * Mib, "2 GiB load: the target is not the heap minus 256 MiB");
    Require(on.driverRefusals == 0, "2 GiB load: " + std::to_string(on.driverRefusals) + " allocations refused with the budget on");
    // Over by at most one step's mandatory allocations (a texture and a scratch buffer) between
    // two safe points.
    Require(on.peak <= on.target + 64 * Mib + 8 * Mib + 8 * Mib, "2 GiB load: the heap went " + std::to_string((on.peak - on.target) / Mib) + " MiB past the target");
    Require(on.textureEvictions != 0 && on.buildEvictions != 0 && on.poolEvictions != 0, "2 GiB load: a class was never reclaimed (textures " + std::to_string(on.textureEvictions) + ", builds " + std::to_string(on.buildEvictions) + ", pool " + std::to_string(on.poolEvictions) + ")");
    const auto off = simulateLoad(false);
    Require(off.driverRefusals != 0 && off.textureEvictions == 0 && off.buildEvictions == 0 && off.poolEvictions == 0 && off.refusedShadows == 0, "2 GiB load: with the budget off the load did not run out of memory (the reference failure)");
    std::printf("vram 2 GiB load: ok (budget on: 0 refused, peak %llu MiB of target %llu, evicted %llu textures, %llu builds, %llu pool slots, %llu shadows refused, %llu releases not retained; off: %llu refused)\n", static_cast<unsigned long long>(on.peak / Mib), static_cast<unsigned long long>(on.target / Mib), static_cast<unsigned long long>(on.textureEvictions), static_cast<unsigned long long>(on.buildEvictions), static_cast<unsigned long long>(on.poolEvictions), static_cast<unsigned long long>(on.refusedShadows), static_cast<unsigned long long>(on.notRetained), static_cast<unsigned long long>(off.driverRefusals));
}

void classScopeTests() {
    Require(CurrentVramClass() == VramClass::Buffers, "scope: the default class is not buffers");
    {
        const VramClassScope resident(VramClass::Resident);
        Require(CurrentVramClass() == VramClass::Resident, "scope: the scope did not set the class");
        {
            const VramClassScope other(VramClass::Other);
            Require(CurrentVramClass() == VramClass::Other, "scope: a nested scope did not set the class");
        }
        Require(CurrentVramClass() == VramClass::Resident, "scope: a nested scope did not restore the class");
    }
    Require(CurrentVramClass() == VramClass::Buffers, "scope: the scope did not restore the default");
    std::printf("vram class scope: ok\n");
}

}

int main() {
    try {
        accountingTests();
        targetTests();
        usedTests();
        excessTests();
        reclaimOrderTests();
        admitTests();
        smallBudgetTests();
        classScopeTests();
        std::printf("VramBudget tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VramBudget tests failed: %s\n", error.what());
        return 1;
    }
}
