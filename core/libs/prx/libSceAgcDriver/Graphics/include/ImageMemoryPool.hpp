#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORYPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_IMAGEMEMORYPOOL_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace AgcDriver::Graphics {

// First-fit free list over [0, capacity) with coalescing; the bookkeeping of one memory block.
// Pure arithmetic (no Vulkan), so it is unit tested on its own.
class RangeAllocator {
public:
    explicit RangeAllocator(std::uint64_t capacity) : capacity(capacity) { free.emplace(0, capacity); }

    // Offset of `size` bytes aligned to `alignment` (a power of two or any positive value), or nullopt.
    std::optional<std::uint64_t> Allocate(std::uint64_t size, std::uint64_t alignment) {
        if (size == 0) size = 1;
        if (alignment == 0) alignment = 1;
        for (auto it = free.begin(); it != free.end(); ++it) {
            const auto start = it->first;
            const auto end = start + it->second;
            const auto aligned = (start + alignment - 1) / alignment * alignment;
            if (aligned < start || aligned > end || end - aligned < size) continue;
            free.erase(it);
            if (aligned > start) free.emplace(start, aligned - start);
            if (aligned + size < end) free.emplace(aligned + size, end - (aligned + size));
            used += size;
            return aligned;
        }
        return std::nullopt;
    }

    // Returns a range from Allocate (same offset and size), merging it with free neighbours.
    void Free(std::uint64_t offset, std::uint64_t size) {
        if (size == 0) size = 1;
        used -= size;
        auto next = free.lower_bound(offset);
        if (next != free.end() && offset + size == next->first) {
            size += next->second;
            next = free.erase(next);
        }
        if (next != free.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                offset = previous->first;
                size += previous->second;
                free.erase(previous);
            }
        }
        free.emplace(offset, size);
    }

    std::uint64_t Capacity() const { return capacity; }
    std::uint64_t Used() const { return used; }
    bool Empty() const { return used == 0; }
    // Number of separate free ranges (1 when fully coalesced).
    std::size_t FreeRanges() const { return free.size(); }
    std::uint64_t LargestFree() const {
        std::uint64_t best = 0;
        for (const auto& [offset, size] : free) best = size > best ? size : best;
        return best;
    }

private:
    std::uint64_t capacity;
    std::uint64_t used = 0;
    std::map<std::uint64_t, std::uint64_t> free;
};

// Sub-allocates the memory of sampled-texture images (optimal tiling only, so bufferImageGranularity
// never separates neighbours) from large device blocks: a vkAllocateMemory/vkFreeMemory per texture
// is a kernel GPU allocation each on Windows. Images the driver wants dedicated memory for, and very
// large ones, keep their own allocation. APS5_NO_TEXTURE_SUBALLOC=1 disables it.
class ImageMemoryPool {
public:
    struct Allocation {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        // False: `memory` is this image's own VkDeviceMemory (free with vkFreeMemory).
        bool pooled = false;
    };

    explicit ImageMemoryPool(const Context& context);
    ~ImageMemoryPool();
    ImageMemoryPool(const ImageMemoryPool&) = delete;
    ImageMemoryPool& operator=(const ImageMemoryPool&) = delete;

    // Memory for `image` (not yet bound), bound at the result's offset. Throws on failure.
    Allocation AllocateAndBind(VkImage image, VkMemoryPropertyFlags properties);
    // Returns the memory of an image that was destroyed (callers destroy the VkImage first).
    void Release(const Allocation& allocation) noexcept;

    static bool Enabled();
    static constexpr VkDeviceSize blockBytes = 256ull << 20u;
    static constexpr VkDeviceSize maxPooledBytes = 64ull << 20u;

    static std::uint64_t PooledImages();
    static std::uint64_t DedicatedImages();

private:
    struct Block {
        VkDeviceMemory memory;
        std::uint32_t type;
        RangeAllocator ranges;
        // In the video-memory budget's heap: its free space is accounted as slack, its used part
        // as textures (VramBudget).
        bool budgeted = false;
    };
    VkDevice device;
    const Context context;
    HostMutex mutex;
    std::vector<std::unique_ptr<Block>> blocks;
};

// The device's pool; shared by the textures using it, which keep it alive.
std::shared_ptr<ImageMemoryPool> GetImageMemoryPool(const Context& context);

}

#endif
