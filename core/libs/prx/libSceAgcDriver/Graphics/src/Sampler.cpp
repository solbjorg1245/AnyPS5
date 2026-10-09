#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <cstdlib>

namespace AgcDriver::Graphics {

    Sampler::Sampler(const Context& context, const GuestSamplerResource& descriptor) : context(context) {
        Require(!descriptor.anisotropyEnable || context.samplerAnisotropy, "guest sampler descriptor requests anisotropic filtering which the device does not support");
        Require(descriptor.maxAnisotropy <= context.limits.maxSamplerAnisotropy, "guest sampler descriptor requests an anisotropy ratio beyond the device limit");
        Require(descriptor.lodBias >= -context.limits.maxSamplerLodBias && descriptor.lodBias <= context.limits.maxSamplerLodBias, "guest sampler descriptor requests a LOD bias beyond the device limit");

        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = descriptor.magFilter;
        info.minFilter = descriptor.minFilter;
        info.mipmapMode = descriptor.mipmapMode;
        info.addressModeU = descriptor.addressModeU;
        info.addressModeV = descriptor.addressModeV;
        info.addressModeW = descriptor.addressModeW;
        info.mipLodBias = descriptor.lodBias;
        info.anisotropyEnable = descriptor.anisotropyEnable ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = descriptor.maxAnisotropy;
        info.compareEnable = descriptor.compareEnable ? VK_TRUE : VK_FALSE;
        info.compareOp = descriptor.compareOp;
        info.minLod = descriptor.minLod;
        info.maxLod = descriptor.maxLod;
        info.borderColor = descriptor.borderColor;
        info.unnormalizedCoordinates = VK_FALSE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &info, nullptr, &sampler), "vkCreateSampler");
    }

    Sampler::~Sampler() {
        release();
    }

    void Sampler::release() noexcept {
        if (sampler) context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, sampler, nullptr);
    }

    VkSampler Sampler::Handle() const {
        return sampler;
    }

    namespace {

    std::uint64_t nextInstance() {
        static std::atomic<std::uint64_t> instances{0};
        return instances.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    bool samplerMemoEnabled() {
        static const bool enabled = std::getenv("APS5_NO_SAMPLER_MEMO") == nullptr;
        return enabled;
    }

    // A thread's last samplers by cache instance and key (GetMemoized). Held weakly: the memo must
    // not keep a device's samplers past the device.
    struct SamplerMemo {
        struct Entry {
            std::uint64_t instance = 0;
            std::uint64_t removals = 0;
            std::array<std::uint32_t, 5> key{};
            std::weak_ptr<Sampler> sampler;
        };
        static constexpr std::size_t Entries = 8;
        std::array<Entry, Entries> entries{};
        std::size_t next = 0;
    };
    struct SamplerMemoTag {};

    }

    SamplerCache::SamplerCache(std::size_t capacity) : capacity(std::max<std::size_t>(capacity, 1)), instance(nextInstance()) {}

    std::shared_ptr<Sampler> SamplerCache::GetMemoized(const Context& context, std::span<const std::uint32_t> words, bool compareEnable) {
        if (!samplerMemoEnabled() || words.size() != 4) return Get(context, words, compareEnable);
        const std::array<std::uint32_t, 5> key{words[0], words[1], words[2], words[3], compareEnable ? 1u : 0u};
        auto& memo = HostThreadLocal<SamplerMemo, SamplerMemoTag>();
        // Read before Get answers: an eviction from here on makes the noted entry miss.
        const auto dropped = Removals();
        for (const auto& entry : memo.entries) {
            if (entry.instance != instance || entry.removals != dropped || entry.key != key) continue;
            if (auto sampler = entry.sampler.lock()) {
                // As recent as Get's hit would have made it (the eviction's order).
                sampler->cacheUse.store(clock.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                // Counted for the APS5_PROFILE_DRAW line only (the stamp above is the one shared write per hit).
                static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                if (profile) memoHits.fetch_add(1, std::memory_order_relaxed);
                return sampler;
            }
        }
        auto sampler = Get(context, words, compareEnable);
        memo.entries[memo.next] = {instance, dropped, key, sampler};
        memo.next = (memo.next + 1) % SamplerMemo::Entries;
        return sampler;
    }

    std::shared_ptr<Sampler> SamplerCache::Get(const Context& context, std::span<const std::uint32_t> words, bool compareEnable) {
        Require(words.size() == 4, "guest sampler descriptor must contain 4 dwords");
        const std::array<std::uint32_t, 5> key{words[0], words[1], words[2], words[3], compareEnable ? 1u : 0u};
        std::lock_guard lock(mutex);
        const auto now = clock.fetch_add(1, std::memory_order_relaxed) + 1;
        if (const auto found = entries.find(key); found != entries.end()) {
            ++hits;
            found->second.sampler->cacheUse.store(now, std::memory_order_relaxed);
            return found->second.sampler;
        }
        ++misses;
        auto resource = DecodeSamplerResource(words);
        resource.compareEnable = compareEnable;
        auto sampler = std::make_shared<Sampler>(context, resource);
        sampler->cacheUse.store(now, std::memory_order_relaxed);
        // The cap keeps live samplers well below the device's limit (NVIDIA: ~4000); a set in flight
        // still holds the evicted sampler through its own shared_ptr.
        while (entries.size() >= capacity) {
            const auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.second.sampler->cacheUse.load(std::memory_order_relaxed) < right.second.sampler->cacheUse.load(std::memory_order_relaxed); });
            removals.fetch_add(1, std::memory_order_release);
            entries.erase(oldest);
        }
        entries.emplace(key, Entry{sampler});
        return sampler;
    }

}
