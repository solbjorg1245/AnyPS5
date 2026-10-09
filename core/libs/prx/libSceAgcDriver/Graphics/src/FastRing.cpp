#include "prx/libSceAgcDriver/Graphics/include/FastRing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

// Raises `value` to `serial`: batches finish in order, but the recorder may release their kept
// retirement objects on its release thread after a later Complete already ran.
void raiseTo(std::atomic<std::uint64_t>& value, std::uint64_t serial) {
    auto current = value.load(std::memory_order_relaxed);
    while (current < serial && !value.compare_exchange_weak(current, serial, std::memory_order_release, std::memory_order_relaxed)) {}
}

struct Retire {
    Retire(std::shared_ptr<std::atomic<std::uint64_t>> counter, std::uint64_t value) : completed(std::move(counter)), serial(value) {}
    Retire(const Retire&) = delete;
    Retire& operator=(const Retire&) = delete;
    ~Retire() { raiseTo(*completed, serial); }
    std::shared_ptr<std::atomic<std::uint64_t>> completed;
    std::uint64_t serial;
};

}

VkDeviceSize FastRing::ConfiguredBytes() {
    static const VkDeviceSize bytes = [] {
        const char* value = std::getenv("APS5_FAST_RING_MIB");
        // At most 4 GiB (maxMemoryAllocationSize on most drivers); larger values would also overflow the shift.
        return std::min<VkDeviceSize>(value != nullptr ? std::strtoull(value, nullptr, 10) : 64, 4096) << 20u;
    }();
    return bytes;
}

bool FastRing::DrainWhenFull() {
    static const bool drain = [] {
        const char* value = std::getenv("APS5_FAST_RING_SYNC");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return drain;
}

void ReclaimFastRing(FastRing& ring, Recorder& recorder) {
    recorder.Reap();
    const auto inFlight = static_cast<std::uint64_t>(recorder.InFlightBatches());
    const auto submitted = recorder.Submissions();
    if (submitted > inFlight) ring.Complete(submitted - inFlight);
}

FastRing::FastRing(const Context& context, VkDeviceSize bytes) : context(context), alignment(std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 1)), completed(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    // The ring lives as long as the device's other caches; it must not keep the buffer pool alive.
    this->context.bufferPool.reset();
    capacity = bytes / alignment * alignment;
    Require(capacity != 0, "fast ring is smaller than one storage buffer offset alignment");
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer fast ring");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        // The CPU only writes the ring and the GPU reads it once: uncached host memory is right.
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory fast ring");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory fast ring");
        void* data = nullptr;
        Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &data), "vkMapMemory fast ring");
        mapping = static_cast<std::byte*>(data);
    } catch (...) {
        release();
        throw;
    }
}

FastRing::~FastRing() {
    release();
}

void FastRing::release() noexcept {
    if (mapping != nullptr) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    mapping = nullptr;
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

void FastRing::reclaim() {
    const auto finished = completed->load(std::memory_order_acquire);
    while (!marks.empty() && marks.front().first <= finished) {
        tail = marks.front().second;
        marks.pop_front();
    }
    // Nothing in use: the next region starts at the beginning, so a request of the whole ring fits.
    if (marks.empty()) {
        head = (head + capacity - 1) / capacity * capacity;
        tail = head;
    }
}

std::optional<FastRing::Region> FastRing::Allocate(VkDeviceSize bytes, std::uint64_t serial) {
    Require(bytes != 0 && serial != 0, "empty fast ring request");
    Require(serial >= lastSerial, "fast ring serials must not go backwards");
    if (bytes > capacity) {
        oversize.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    reclaim();
    const auto offset = head % capacity;
    auto start = (offset + alignment - 1) / alignment * alignment;
    // A region never straddles the end: the rest of the buffer becomes padding of this region.
    const bool wrap = start + bytes > capacity;
    if (wrap) start = 0;
    const auto advance = (wrap ? capacity - offset : start - offset) + bytes;
    if (head + advance - tail > capacity) {
        full.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    head += advance;
    lastSerial = serial;
    if (marks.empty() || marks.back().first != serial) marks.emplace_back(serial, head);
    else marks.back().second = head;
    allocations.fetch_add(1, std::memory_order_relaxed);
    allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (wrap) wraps.fetch_add(1, std::memory_order_relaxed);
    // APS5_POISON_POOL: the region's leftovers (an earlier batch's words) become the poison word.
    if (PoisonPool()) {
        FillPoolPoison({mapping + start, static_cast<std::size_t>(bytes)}, PoolPoisonWord());
        NoteRingPoison(bytes);
    }
    return Region{buffer, start, bytes, mapping + start};
}

void FastRing::Complete(std::uint64_t serial) {
    raiseTo(*completed, serial);
}

std::shared_ptr<void> FastRing::Retirement(std::uint64_t serial) const {
    return std::make_shared<Retire>(completed, serial);
}

FastRing::Stats FastRing::Counters() const {
    return {allocations.load(std::memory_order_relaxed), allocatedBytes.load(std::memory_order_relaxed), wraps.load(std::memory_order_relaxed), full.load(std::memory_order_relaxed), oversize.load(std::memory_order_relaxed)};
}

}
