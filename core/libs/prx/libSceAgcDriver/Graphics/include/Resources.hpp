#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <atomic>
#include <span>
#include <string>

namespace AgcDriver::Graphics {

// Device address ranges of live buffers and of the last few thousand destroyed ones, for the
// device-lost report: DescribeDeviceAddress names the buffers a faulting GPU address lies in (or
// lies nearest to), with their kind, guest base and age.
void NoteDeviceAddress(VkDeviceAddress address, std::uint64_t bytes, const char* kind, std::uint64_t guestBase = 0);
void ForgetDeviceAddress(VkDeviceAddress address);
std::string DescribeDeviceAddress(VkDeviceAddress address);
// Whether [address, address + bytes) lies inside one live registered buffer.
bool DeviceAddressLive(VkDeviceAddress address, std::uint64_t bytes);

// Debug aid APS5_CHECK_STALE_IMPORTS=1: the VkBuffer handles of destroyed host imports are kept
// (until a new import gets the same handle), and work about to bind one reports it
// (ReportDestroyedImport: a descriptor or address that outlived its import reads freed memory,
// the 'read invalid' device losses inside destroyed imports, t167-t177).
bool CheckStaleImports();
// Prints the stack slots that point into the driver's code (offsets from its image base).
void ReportDriverStack(const char* tag);
void NoteImportHandle(VkBuffer buffer, std::uint64_t guestBase, bool live);
bool ReportDestroyedImport(VkBuffer buffer, const char* where, std::uint64_t detail, std::uint64_t* guestBase = nullptr);

// Debug aid APS5_GPU_CHECKPOINTS=1 (VK_NV_device_diagnostic_checkpoints): every recorded draw and
// dispatch is preceded by a checkpoint whose marker indexes a ring of descriptions (kind, guest
// program addresses, render target or dispatch size); a lost device prints the last checkpoint
// each pipeline stage reached, naming the work that faulted. SetCheckpointWork gives the calling
// thread's next draw its programs (the driver sets them per draw packet).
bool CheckpointsRequested();
void InstallCheckpoints(PFN_vkCmdSetCheckpointNV set, PFN_vkGetQueueCheckpointDataNV get, VkQueue queue);
void SetCheckpointWork(std::uint64_t first, std::uint64_t second);
void RecordCheckpoint(VkCommandBuffer commands, char kind, std::uint64_t first, std::uint64_t second, std::uint64_t detail);
void RecordDrawCheckpoint(VkCommandBuffer commands, std::uint64_t detail);
void ReportCheckpoints();

// vkAllocateMemory for the driver's buffers and images. VK_ERROR_OUT_OF_DEVICE_MEMORY (video memory
// exhausted: t421-t424 met it at the Boletaria load's peak, ~12.8k device buffers in use, and every
// draw after it failed a texture allocation for ~3-9 ms, one present per 10 s, while the buffer
// pool kept ~2 GiB of device-local allocations no work used) first releases those
// (BufferPool::ReleaseDevice) and, when that freed anything, allocates once more. The failure is
// returned as before when nothing was retained. APS5_NO_OOM_RECLAIM=1 returns it at once (old).
VkResult AllocateDeviceMemory(const Context& context, const VkMemoryAllocateInfo& allocation, VkDeviceMemory* memory);

class Buffer {
public:
    Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    VkBuffer Handle() const;
    VkDeviceAddress DeviceAddress() const;
    // Host-visible buffers only: a device-local one (properties without HOST_VISIBLE, the staging
    // shadows of GuestBufferMemory) has no mapping and its bytes move by GPU copies alone.
    std::span<std::byte> Bytes();
    bool Mapped() const { return mapping != nullptr; }
    void Invalidate();
    // The video memory guard's epoch when its memory was allocated (VideoMemory::Epoch).
    std::uint64_t Epoch() const { return epoch; }
    // Destroyed instead of returned to the buffer pool once the last reference goes (the video
    // memory guard's trim of resident copies: their memory is to be freed, not retained).
    void DiscardOnRelease() noexcept { discard.store(true, std::memory_order_relaxed); }

private:
    void initializeAddress(VkBufferUsageFlags usage);
    void release() noexcept;
    Context context;
    VkDeviceAddress deviceAddress = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    std::size_t size;
    // The VkBuffer's own size, `size` rounded up to its pool class (see BufferPool::Capacity).
    std::size_t capacity;
    // Fully made (or taken from the pool), so release returns it to the pool instead of destroying
    // the handles a failed construction left.
    bool ready = false;
    VkDeviceSize allocationBytes = 0;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties;
    std::shared_ptr<BufferPool> cache;
    // The video memory guard's epoch at the allocation and its memory type (BufferAllocation).
    std::uint64_t epoch = 0;
    std::uint32_t memoryType = ~0u;
    std::atomic<bool> discard{false};
};

// Device-local scratch memory for GPU-side layout conversion. The detiler reads and writes scattered
// elements, which crawls across PCIe, so guest bytes move between host and device buffers with DMA
// copies and are only swizzled in video memory.
class DeviceBuffer {
public:
    DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage);
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    VkBuffer Handle() const;
    std::size_t Size() const;
    // Taken from the buffer pool (else allocated by this constructor), the video memory guard's
    // epoch when its memory was allocated, whether that happened during a pressure episode, and
    // the memory type (the [retile] line attributes the write-back scratch with them).
    bool Pooled() const { return pooled; }
    std::uint64_t Epoch() const { return epoch; }
    bool MadeUnderPressure() const { return madeUnderPressure; }
    std::uint32_t MemoryType() const { return memoryType; }

private:
    void release() noexcept;
    Context context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::size_t size;
    std::size_t capacity;
    VkDeviceSize allocationBytes = 0;
    VkBufferUsageFlags usage;
    std::shared_ptr<BufferPool> cache;
    bool pooled = false;
    bool madeUnderPressure = false;
    std::uint64_t epoch = 0;
    std::uint32_t memoryType = ~0u;
};

// Records a whole-range buffer copy.
void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes);

// Records a global memory barrier.
void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);

// Resolves every DeviceFunctions entry point of the context's device (once, at device setup).
void FillDeviceFunctions(const Context& context, DeviceFunctions& functions);

class RenderTarget {
public:
    RenderTarget(const Context& context, const ColorTarget& target, bool blending);
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    VkImage Image() const;
    VkImageView View() const;

private:
    void release() noexcept;
    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

class CommandBatch {
public:
    explicit CommandBatch(const Context& context);
    ~CommandBatch();
    CommandBatch(const CommandBatch&) = delete;
    CommandBatch& operator=(const CommandBatch&) = delete;
    VkCommandBuffer Handle() const;
    void SubmitAndWait();
    void Submit();
    void Wait();

private:
    void release() noexcept;
    Context context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool submitted = false;
};

}

#endif
