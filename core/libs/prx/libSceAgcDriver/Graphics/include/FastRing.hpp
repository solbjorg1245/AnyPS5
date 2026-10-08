#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTRING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTRING_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace AgcDriver::Graphics {

// The fast draw path's data ring (docs/design/draw-fastpath.md 2.4): one persistently mapped
// HOST_VISIBLE|HOST_COHERENT storage buffer the flattened SRT and shader-data words of fast draws
// are written to, in place of a Buffer object per draw. Regions are handed out in order, aligned to
// minStorageBufferOffsetAlignment, and tagged with the serial of the Recorder batch that reads them
// (Recorder::Submissions() + 1 for the open batch; serials never go backwards). A region is reused
// only once Complete reported its batch finished: the batch keeps Retirement(serial) (Recorder::Keep),
// whose release completes it, and after a Recorder::Sync the caller completes Submissions() itself.
// Allocate runs under GuestMemory::GpuMutex; Complete and the counters may run on any thread.
class FastRing {
public:
    struct Region {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize bytes = 0;
        std::byte* data = nullptr;
    };
    // APS5_PROFILE_DRAW counters (cumulative): regions handed out and their bytes, wraps to the
    // start, requests the ring could not serve before batches complete (the caller reaps) and
    // requests larger than the ring.
    struct Stats {
        std::uint64_t allocations = 0;
        std::uint64_t bytes = 0;
        std::uint64_t wraps = 0;
        std::uint64_t full = 0;
        std::uint64_t oversize = 0;
    };

    FastRing(const Context& context, VkDeviceSize bytes);
    ~FastRing();
    FastRing(const FastRing&) = delete;
    FastRing& operator=(const FastRing&) = delete;

    // A region of `bytes` read by the batch `serial`, or none while the regions of unfinished
    // batches leave no room (counted as full; the caller reaps finished batches and asks again).
    std::optional<Region> Allocate(VkDeviceSize bytes, std::uint64_t serial);
    // Every batch up to `serial` finished: their regions may be reused.
    void Complete(std::uint64_t serial);
    // An object whose destruction completes `serial` (it does not keep the ring alive).
    std::shared_ptr<void> Retirement(std::uint64_t serial) const;
    VkDeviceSize Capacity() const { return capacity; }
    // Bytes held by batches not known to be finished (wrap padding included).
    VkDeviceSize InUse() const { return head - tail; }
    Stats Counters() const;
    // The ring size: APS5_FAST_RING_MIB (default 64, at most 4096; 0 = no ring).
    static VkDeviceSize ConfiguredBytes();

private:
    void reclaim();
    void release() noexcept;
    Context context;
    VkDeviceSize alignment = 1;
    VkDeviceSize capacity = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::byte* mapping = nullptr;
    // Monotonic byte positions: the next region starts at head % capacity, everything before tail
    // is free again.
    std::uint64_t head = 0;
    std::uint64_t tail = 0;
    std::uint64_t lastSerial = 0;
    // Per batch serial still in use, oldest first: the head position after its last region.
    std::deque<std::pair<std::uint64_t, std::uint64_t>> marks;
    // The newest finished serial, shared with the retirement objects batches keep.
    std::shared_ptr<std::atomic<std::uint64_t>> completed;
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> allocatedBytes{0};
    std::atomic<std::uint64_t> wraps{0};
    std::atomic<std::uint64_t> full{0};
    std::atomic<std::uint64_t> oversize{0};
};

}

#endif
