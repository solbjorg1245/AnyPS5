#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SAMPLER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SAMPLER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>

namespace AgcDriver::Graphics {

class Sampler {
public:
    Sampler(const Context& context, const GuestSamplerResource& descriptor);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    VkSampler Handle() const;
    // The sampler cache's recency stamp (SamplerCache): the cache's clock at the last lookup that
    // answered this sampler, by Get or by a thread's memo (GetMemoized, which takes no lock), so the
    // cache evicts the least recently used sampler as if every lookup had reached Get.
    mutable std::atomic<std::uint64_t> cacheUse{0};

private:
    void release() noexcept;

    Context context;
    VkSampler sampler = VK_NULL_HANDLE;
};

// One VkSampler per distinct S# (its 4 words plus the shader's depth-compare use): samplers are
// immutable and DecodeSamplerResource is a pure function of the words, so equal keys mean equal
// samplers and nothing ever invalidates an entry. Holders keep a shared_ptr, so an entry evicted from
// the LRU-capped cache lives on while a descriptor set in flight references it. One cache per device.
// APS5_NO_SAMPLER_CACHE=1 creates a sampler per binding as before.
class SamplerCache {
public:
    explicit SamplerCache(std::size_t capacity = 1024);
    SamplerCache(const SamplerCache&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    std::shared_ptr<Sampler> Get(const Context& context, std::span<const std::uint32_t> words, bool compareEnable);
    // Get through the calling thread's memo of the samplers this cache answered it last (the fast
    // paths' S# memo, docs/design/draw-fastpath.md 2.6, F4): an entry answers an equal key while
    // this cache dropped no entry since it answered (Removals) and the sampler lives, i.e. while
    // Get would return the same object; a hit takes no lock. APS5_NO_SAMPLER_MEMO=1 calls Get.
    std::shared_ptr<Sampler> GetMemoized(const Context& context, std::span<const std::uint32_t> words, bool compareEnable);
    // APS5_PROFILE_DRAW counters: lookups served by an existing sampler (memo hits included), and
    // samplers created.
    std::uint64_t Hits() const { return hits + memoHits.load(std::memory_order_relaxed); }
    std::uint64_t Misses() const { return misses; }
    // Entries evicted so far (the memo's validity), and this cache's identity among every cache
    // the process made (a cache made at a destroyed one's address is another instance).
    std::uint64_t Removals() const { return removals.load(std::memory_order_acquire); }
    std::uint64_t Instance() const { return instance; }

private:
    struct Entry {
        // Its last use is the sampler's cacheUse.
        std::shared_ptr<Sampler> sampler;
    };
    HostMutex mutex;
    std::map<std::array<std::uint32_t, 5>, Entry> entries;
    // Ticks once per lookup (Get, under the mutex; a memo hit, without it).
    std::atomic<std::uint64_t> clock{0};
    std::size_t capacity;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::atomic<std::uint64_t> memoHits{0};
    std::atomic<std::uint64_t> removals{0};
    const std::uint64_t instance;
};

}

#endif
