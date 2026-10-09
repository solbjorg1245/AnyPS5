#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VideoMemory.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::Graphics {

namespace {

// APS5_BUFFER_POOL_SHARED=1: one tier for every size, as before the split.
bool sharedTiers() {
    static const bool shared = std::getenv("APS5_BUFFER_POOL_SHARED") != nullptr;
    return shared;
}

// APS5_NO_OOM_RECLAIM=1: an allocation refused for want of video memory fails at once, as before.
bool outOfMemoryReclaim() {
    static const bool enabled = std::getenv("APS5_NO_OOM_RECLAIM") == nullptr;
    return enabled;
}

struct OutOfMemoryCounters {
    std::atomic<std::uint64_t> refused{0}, reclaims{0}, reclaimedBytes{0}, madeAfter{0};
};

OutOfMemoryCounters& outOfMemoryCounters() {
    static OutOfMemoryCounters counters;
    return counters;
}

}

BufferPool::BufferPool(const Context& context) : device(context.device), unmap(context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")), destroyBuffer(context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")) {
    smallTier.budget = smallBudget;
    largeTier.budget = budget;
    deviceTier.budget = DeviceBudget();
}

BufferPool::~BufferPool() {
    for (const auto* tier : {&smallTier, &largeTier, &deviceTier}) {
        for (const auto& [key, slots] : tier->free) {
            for (const auto& slot : slots) destroy(slot.allocation);
        }
    }
}

VkDeviceSize BufferPool::DeviceBudget() {
    static const VkDeviceSize deviceBudget = [] {
        const char* value = std::getenv("APS5_STAGING_POOL_MIB");
        return (value != nullptr ? std::strtoull(value, nullptr, 10) : 2048ull) << 20u;
    }();
    return deviceBudget;
}

void BufferPool::destroy(const BufferAllocation& allocation) noexcept {
    // Device-local allocations (see DeviceBuffer) are never mapped.
    if (allocation.mapping != nullptr) unmap(device, allocation.memory);
    ForgetDeviceAddress(allocation.address);
    destroyBuffer(device, allocation.buffer, nullptr);
    freeMemory(device, allocation.memory, nullptr);
}

std::size_t BufferPool::Capacity(std::size_t bytes) {
    static const bool exact = std::getenv("APS5_NO_BUFFER_CLASSES") != nullptr;
    constexpr std::size_t smallest = 256;
    if (exact || bytes >= classLimit) return bytes;
    return std::max(smallest, std::bit_ceil(bytes));
}

BufferPool::Tier& BufferPool::tierFor(std::size_t capacity, VkMemoryPropertyFlags properties) {
    if ((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 && DeviceBudget() != 0) return deviceTier;
    return !sharedTiers() && capacity < classLimit ? smallTier : largeTier;
}

std::optional<BufferAllocation> BufferPool::Take(std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto capacity = Capacity(bytes);
    std::optional<BufferAllocation> result;
    // Device-tier slots from before the video memory guard's last recycle met here: destroyed after
    // the mutex is released, as evictions are.
    std::vector<BufferAllocation> stale;
    {
        std::lock_guard lock(mutex);
        ++clock;
        if (profile) {
            static auto lastReport = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::seconds(10)) {
                lastReport = now;
                std::fprintf(stderr, "[bufferpool] small: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB); large: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB)%s; device: %llu hits, %llu misses, %llu evictions, %zu retained (%.0f MiB of %llu); out of video memory: %llu allocations refused, %llu releases (%.0f MiB), %llu made after one; video memory guard: trimmed %llu (%.0f MiB), recycled %llu (%.0f MiB)\n", static_cast<unsigned long long>(smallTier.hits), static_cast<unsigned long long>(smallTier.misses), static_cast<unsigned long long>(smallTier.evictions), smallTier.slots, smallTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(largeTier.hits), static_cast<unsigned long long>(largeTier.misses), static_cast<unsigned long long>(largeTier.evictions), largeTier.slots, largeTier.retainedBytes / 1048576.0, sharedTiers() ? " (shared)" : "", static_cast<unsigned long long>(deviceTier.hits), static_cast<unsigned long long>(deviceTier.misses), static_cast<unsigned long long>(deviceTier.evictions), deviceTier.slots, deviceTier.retainedBytes / 1048576.0, static_cast<unsigned long long>(deviceTier.budget >> 20u), static_cast<unsigned long long>(outOfMemoryCounters().refused.load(std::memory_order_relaxed)), static_cast<unsigned long long>(outOfMemoryCounters().reclaims.load(std::memory_order_relaxed)), outOfMemoryCounters().reclaimedBytes.load(std::memory_order_relaxed) / 1048576.0, static_cast<unsigned long long>(outOfMemoryCounters().madeAfter.load(std::memory_order_relaxed)), static_cast<unsigned long long>(guardTrimmed), guardTrimmedBytes / 1048576.0, static_cast<unsigned long long>(guardRecycled), guardRecycledBytes / 1048576.0);
            }
        }
        auto& tier = tierFor(capacity, properties);
        const bool recycling = &tier == &deviceTier && VideoMemory::GuardEnabled();
        const auto epoch = recycling ? VideoMemory::Epoch() : 0;
        const SlotKey key{capacity, usage, properties};
        while (!result) {
            const auto found = tier.free.find(key);
            if (found == tier.free.end()) break;
            // The most recently retained slot: the likeliest to be warm.
            const auto slot = found->second.back().allocation;
            const bool old = recycling && slot.epoch < epoch;
            // Before anything changes: a throw here leaves the slot retained.
            if (old) stale.push_back(slot);
            found->second.pop_back();
            if (found->second.empty()) tier.free.erase(found);
            tier.retainedBytes -= slot.allocationBytes;
            --tier.slots;
            if (!old) {
                result = slot;
                break;
            }
            // Made before the last recycle (possibly paged out to system memory during the
            // pressure episode before it): not handed out; the next one of the key may be newer.
            ++guardRecycled;
            guardRecycledBytes += slot.allocationBytes;
            ++tier.evictions;
        }
        if (result) ++tier.hits;
        else ++tier.misses;
    }
    for (const auto& gone : stale) destroy(gone);
    return result;
}

std::map<BufferPool::SlotKey, std::deque<BufferPool::Slot>>::iterator BufferPool::oldestGroup(Tier& tier) {
    // Each group is oldest first, so the tier's oldest slot is the oldest group front.
    return std::min_element(tier.free.begin(), tier.free.end(), [](const auto& left, const auto& right) { return left.second.front().lastUse < right.second.front().lastUse; });
}

void BufferPool::evictOldest(Tier& tier, std::vector<BufferAllocation>& evicted) {
    const auto oldest = oldestGroup(tier);
    // First: a throw here leaves the slot retained and counted.
    evicted.push_back(oldest->second.front().allocation);
    tier.retainedBytes -= oldest->second.front().allocation.allocationBytes;
    oldest->second.pop_front();
    if (oldest->second.empty()) tier.free.erase(oldest);
    --tier.slots;
    ++tier.evictions;
}

std::size_t BufferPool::MaxSlots() {
    // APS5_BUFFER_POOL_SLOTS=<n> bounds the retained allocations; 64 is the capacity the pool had
    // before least-recently-used retention, for A/B runs.
    static const std::size_t slots = [] {
        const char* value = std::getenv("APS5_BUFFER_POOL_SLOTS");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : defaultSlots;
    }();
    return slots;
}

void BufferPool::Put(const BufferAllocation& allocation) noexcept {
    // Evicted allocations are destroyed after the mutex is released (see evictOldest). The vector
    // may throw on growth; a Put that cannot retain simply destroys, as noexcept requires.
    std::vector<BufferAllocation> evicted;
    try {
        std::lock_guard lock(mutex);
        auto& tier = tierFor(allocation.bytes, allocation.properties);
        if (&tier == &deviceTier && allocation.epoch < VideoMemory::Epoch() && VideoMemory::GuardEnabled()) {
            // Made before the last video memory pressure episode ended: not reused (see Take).
            ++guardRecycled;
            guardRecycledBytes += allocation.allocationBytes;
            evicted.push_back(allocation);
        } else if (allocation.allocationBytes > tier.budget) {
            evicted.push_back(allocation);
        } else {
            const auto maxSlots = MaxSlots();
            while (!tier.free.empty() && (tier.retainedBytes + allocation.allocationBytes > tier.budget || tier.slots >= maxSlots)) evictOldest(tier, evicted);
            tier.free[SlotKey{allocation.bytes, allocation.usage, allocation.properties}].push_back({allocation, ++clock, &tier == &deviceTier && VideoMemory::GuardEnabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}});
            ++tier.slots;
            tier.retainedBytes += allocation.allocationBytes;
        }
    } catch (...) {
        destroy(allocation);
    }
    for (const auto& gone : evicted) destroy(gone);
}

VkDeviceSize BufferPool::ReleaseDevice() noexcept {
    // Destroyed after the mutex is released, as evictions are. Reserved first: a throw leaves the
    // tier as it was.
    std::vector<BufferAllocation> released;
    try {
        std::lock_guard lock(mutex);
        released.reserve(deviceTier.slots);
        for (const auto& [key, slots] : deviceTier.free) {
            for (const auto& slot : slots) released.push_back(slot.allocation);
        }
        deviceTier.free.clear();
        deviceTier.evictions += released.size();
        deviceTier.slots = 0;
        deviceTier.retainedBytes = 0;
    } catch (...) {
        return 0;
    }
    VkDeviceSize bytes = 0;
    for (const auto& gone : released) {
        bytes += gone.allocationBytes;
        destroy(gone);
    }
    return bytes;
}

VkDeviceSize BufferPool::TrimDevice(VkDeviceSize bytes, VkDeviceSize floor, std::chrono::milliseconds idle) noexcept {
    std::vector<BufferAllocation> evicted;
    VkDeviceSize freed = 0;
    try {
        const auto cutoff = std::chrono::steady_clock::now() - idle;
        std::lock_guard lock(mutex);
        while (!deviceTier.free.empty() && freed < bytes && deviceTier.retainedBytes > floor) {
            // Least recently used first; one that came back within `idle` is in use, and so is
            // every newer one.
            if (oldestGroup(deviceTier)->second.front().returned > cutoff) break;
            const auto before = deviceTier.retainedBytes;
            evictOldest(deviceTier, evicted);
            freed += before - deviceTier.retainedBytes;
        }
        guardTrimmed += evicted.size();
        guardTrimmedBytes += freed;
    } catch (...) {
    }
    for (const auto& gone : evicted) destroy(gone);
    return freed;
}

VkDeviceSize BufferPool::DeviceRetainedBytes() {
    std::lock_guard lock(mutex);
    return deviceTier.retainedBytes;
}

VkDeviceSize BufferPool::DeviceLimit() {
    std::lock_guard lock(mutex);
    return deviceTier.budget;
}

BufferPool::OutOfMemoryCounts BufferPool::OutOfMemory() {
    const auto& counters = outOfMemoryCounters();
    return {counters.refused.load(std::memory_order_relaxed), counters.reclaims.load(std::memory_order_relaxed), counters.reclaimedBytes.load(std::memory_order_relaxed), counters.madeAfter.load(std::memory_order_relaxed)};
}

VkResult AllocateDeviceMemory(const Context& context, const VkMemoryAllocateInfo& allocation, VkDeviceMemory* memory) {
    const auto allocate = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory");
    // Device-local allocations made during a video memory pressure episode are counted ([vram]).
    const bool deviceLocal = allocation.memoryTypeIndex < context.memory.memoryTypeCount && (context.memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    auto result = allocate(context.device, &allocation, nullptr, memory);
    if (result == VK_SUCCESS && deviceLocal) VideoMemory::NoteDeviceAllocation(allocation.allocationSize);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY) return result;
    auto& counters = outOfMemoryCounters();
    counters.refused.fetch_add(1, std::memory_order_relaxed);
    // A pressure episode for the video memory guard (no action with it off).
    VideoMemory::NoteOutOfMemory();
    if (!outOfMemoryReclaim()) return result;
    // Nothing released: the same refusal again (each costs milliseconds), so no second try.
    const auto released = GetBufferPool(context)->ReleaseDevice();
    if (released == 0) return result;
    counters.reclaims.fetch_add(1, std::memory_order_relaxed);
    counters.reclaimedBytes.fetch_add(released, std::memory_order_relaxed);
    result = allocate(context.device, &allocation, nullptr, memory);
    if (result == VK_SUCCESS) {
        counters.madeAfter.fetch_add(1, std::memory_order_relaxed);
        if (deviceLocal) VideoMemory::NoteDeviceAllocation(allocation.allocationSize);
    }
    return result;
}

std::shared_ptr<BufferPool> GetBufferPool(const Context& context) {
    if (!context.bufferPool) context.bufferPool = std::make_shared<BufferPool>(context);
    return context.bufferPool;
}

}
