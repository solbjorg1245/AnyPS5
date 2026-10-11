#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VideoMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace AgcDriver::Graphics {

Buffer::Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) : context(context), size(size), capacity(BufferPool::Capacity(size)), usage(PoolPoisonUsage(usage, properties)), properties(properties) {
    Require(size != 0, "zero-sized GPU buffer");
    // APS5_POISON_POOL may add TRANSFER_DST: the pool key and the VkBuffer take the member's.
    usage = this->usage;
    const bool addressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    Require(!addressable || context.bufferDeviceAddress, "buffer device address is not enabled");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, usage, properties)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        mapping = allocation->mapping;
        deviceAddress = allocation->address;
        allocationBytes = allocation->allocationBytes;
        epoch = allocation->epoch;
        memoryType = allocation->memoryType;
        ready = true;
        pooled = true;
        poisonOnHandOut();
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
        NoteImportHandle(buffer, 0, true);
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        if (addressable) allocation.pNext = &flags;
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        // The CPU reads most of these buffers back (write-back, diffs), which is very slow from
        // write-combined memory, so the default host properties prefer cached host memory.
        constexpr VkMemoryPropertyFlags hostDefault = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (properties == hostDefault) {
            try {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            } catch (const std::runtime_error&) {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault);
            }
        } else {
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, properties);
        }
        epoch = VideoMemory::Epoch();
        memoryType = allocation.memoryTypeIndex;
        // Device-local buffers under the thread's class (staging shadows unless a scope says
        // otherwise, see VramClassScope).
        const bool deviceOnly = (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0;
        Check(AllocateDeviceMemory(context, allocation, &memory, deviceOnly ? CurrentVramClass() : VramClass::Other), "vkAllocateMemory buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory");
        initializeAddress(usage);
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "vkMapMemory");
        ready = true;
        poisonOnHandOut();
    } catch (...) {
        release();
        throw;
    }
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (poisonPending.load(std::memory_order_relaxed)) NotePoolPoisonOwed(capacity);
    if (ready && cache && !discard.load(std::memory_order_relaxed)) {
        cache->Put({buffer, memory, mapping, deviceAddress, allocationBytes, capacity, usage, properties, epoch, memoryType});
        return;
    }
    if (mapping) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    ForgetDeviceAddress(deviceAddress);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) FreeDeviceMemory(context, memory);
}

VkBuffer Buffer::Handle() const {
    return buffer;
}

std::span<std::byte> Buffer::Bytes() {
    Require(mapping != nullptr, "device-local buffer has no host mapping");
    return {static_cast<std::byte*>(mapping), size};
}

void Buffer::Invalidate() {
    Require(mapping != nullptr, "cannot invalidate an unmapped GPU buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    Check(context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

DeviceBuffer::DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage) : context(context), size(size), capacity(BufferPool::Capacity(size)), usage(PoolPoisonUsage(usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    Require(size != 0, "zero-sized device buffer");
    // APS5_POISON_POOL may add TRANSFER_DST: the pool key and the VkBuffer take the member's.
    usage = this->usage;
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        allocationBytes = allocation->allocationBytes;
        epoch = allocation->epoch;
        memoryType = allocation->memoryType;
        pooled = true;
        poisonPending.store(PoisonPool(), std::memory_order_relaxed);
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer device");
        NoteImportHandle(buffer, 0, true);
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        epoch = VideoMemory::Epoch();
        memoryType = allocation.memoryTypeIndex;
        // Also the allocation whose own refusal started the episode (AllocateDeviceMemory).
        const bool pressuredBefore = VideoMemory::UnderPressure();
        Check(AllocateDeviceMemory(context, allocation, &memory, VramClass::Buffers), "vkAllocateMemory device buffer");
        madeUnderPressure = pressuredBefore || VideoMemory::UnderPressure();
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory device");
        poisonPending.store(PoisonPool(), std::memory_order_relaxed);
    } catch (...) {
        release();
        throw;
    }
}

DeviceBuffer::~DeviceBuffer() {
    release();
}

void DeviceBuffer::release() noexcept {
    if (poisonPending.load(std::memory_order_relaxed)) NotePoolPoisonOwed(capacity);
    if (buffer && memory && cache) {
        cache->Put({buffer, memory, nullptr, 0, allocationBytes, capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, epoch, memoryType});
        return;
    }
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) FreeDeviceMemory(context, memory);
}

VkBuffer DeviceBuffer::Handle() const {
    return buffer;
}

std::size_t DeviceBuffer::Size() const {
    return size;
}

namespace {

// APS5_POISON_POOL's counts for the [pool-poison] line: one 10 s window each, reset by the report.
struct PoolPoisonCounters {
    std::atomic<std::uint64_t> host{0}, hostBytes{0}, hostReused{0};
    std::atomic<std::uint64_t> device{0}, deviceBytes{0}, deviceReused{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(PoisonSite::Count)> sites{};
    std::atomic<std::uint64_t> ring{0}, ringBytes{0}, images{0}, imageBytes{0}, imagesPooled{0};
    std::atomic<std::uint64_t> owed{0}, owedBytes{0};
};

PoolPoisonCounters& poolPoison() {
    static PoolPoisonCounters counters;
    return counters;
}

void recordPoolPoison(const Context& context, VkCommandBuffer commands, VkBuffer buffer, std::size_t capacity, bool reused, PoisonSite site) {
    // A buffer just taken from the pool: no earlier work of it is in flight (it was released once
    // its batch completed), so the fill needs no leading barrier.
    context.Resolved(&DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, buffer, 0, VK_WHOLE_SIZE, PoolPoisonWord());
    constexpr VkAccessFlags later = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, later);
    auto& counters = poolPoison();
    counters.device.fetch_add(1, std::memory_order_relaxed);
    counters.deviceBytes.fetch_add(capacity, std::memory_order_relaxed);
    if (reused) counters.deviceReused.fetch_add(1, std::memory_order_relaxed);
    counters.sites[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

}

bool PoisonPool() {
    static const bool poison = [] {
        const char* value = std::getenv("APS5_POISON_POOL");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return poison;
}

std::uint32_t PoolPoisonWordFrom(const char* value) {
    if (value != nullptr && std::strcmp(value, "geometry") == 0) return 0x47ff00ffu;
    if (value != nullptr && value[0] == '0' && (value[1] == 'x' || value[1] == 'X') && value[2] != '\0') return static_cast<std::uint32_t>(std::strtoul(value + 2, nullptr, 16));
    return 0x40ff00ffu;
}

std::uint32_t PoolPoisonWord() {
    static const std::uint32_t word = PoolPoisonWordFrom(std::getenv("APS5_POISON_POOL"));
    return word;
}

VkBufferUsageFlags PoolPoisonUsage(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    const bool deviceOnly = (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0;
    return deviceOnly && PoisonPool() ? usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT : usage;
}

void FillPoolPoison(std::span<std::byte> bytes, std::uint32_t word) noexcept {
    std::array<std::byte, 4> pattern{};
    std::memcpy(pattern.data(), &word, pattern.size());
    for (std::size_t offset = 0; offset < bytes.size(); offset += pattern.size()) std::memcpy(bytes.data() + offset, pattern.data(), std::min(pattern.size(), bytes.size() - offset));
}

void NotePoolPoisonOwed(std::size_t bytes) noexcept {
    auto& counters = poolPoison();
    counters.owed.fetch_add(1, std::memory_order_relaxed);
    counters.owedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void Buffer::poisonOnHandOut() noexcept {
    if (!PoisonPool()) return;
    if (mapping == nullptr) {
        poisonPending.store(true, std::memory_order_relaxed);
        return;
    }
    // The whole VkBuffer (its size class too): the mapping covers the allocation.
    FillPoolPoison({static_cast<std::byte*>(mapping), capacity}, PoolPoisonWord());
    auto& counters = poolPoison();
    counters.host.fetch_add(1, std::memory_order_relaxed);
    counters.hostBytes.fetch_add(capacity, std::memory_order_relaxed);
    if (pooled) counters.hostReused.fetch_add(1, std::memory_order_relaxed);
}

void PoisonPooled(const Context& context, VkCommandBuffer commands, Buffer& buffer, PoisonSite site) {
    if (!PoisonPool() || !buffer.TakePoison()) return;
    recordPoolPoison(context, commands, buffer.Handle(), buffer.Capacity(), buffer.Pooled(), site);
}

void PoisonPooled(const Context& context, VkCommandBuffer commands, DeviceBuffer& buffer, PoisonSite site) {
    if (!PoisonPool() || !buffer.TakePoison()) return;
    recordPoolPoison(context, commands, buffer.Handle(), buffer.Capacity(), buffer.Pooled(), site);
}

void NoteRingPoison(std::uint64_t bytes) {
    poolPoison().ring.fetch_add(1, std::memory_order_relaxed);
    poolPoison().ringBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteImagePoison(std::uint64_t bytes, bool pooled) {
    auto& counters = poolPoison();
    counters.images.fetch_add(1, std::memory_order_relaxed);
    counters.imageBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (pooled) counters.imagesPooled.fetch_add(1, std::memory_order_relaxed);
}

void ReportPoolPoison() {
    if (!PoisonPool()) return;
    auto& c = poolPoison();
    const auto take = [](std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.exchange(0, std::memory_order_relaxed)); };
    const auto host = take(c.host), hostBytes = take(c.hostBytes), hostReused = take(c.hostReused);
    const auto device = take(c.device), deviceBytes = take(c.deviceBytes), deviceReused = take(c.deviceReused);
    std::array<unsigned long long, static_cast<std::size_t>(PoisonSite::Count)> sites{};
    for (std::size_t site = 0; site < sites.size(); ++site) sites[site] = take(c.sites[site]);
    const auto ring = take(c.ring), ringBytes = take(c.ringBytes), images = take(c.images), imageBytes = take(c.imageBytes), imagesPooled = take(c.imagesPooled);
    const auto owed = take(c.owed), owedBytes = take(c.owedBytes);
    std::fprintf(stderr, "[pool-poison] (10 s) %llu buffers poisoned (%.1f MiB) by pool: host %llu (%.1f MiB, %llu reused slots), device %llu (%.1f MiB, %llu reused slots; staging %llu, resident %llu, texture upload %llu, storage upload %llu, write-back %llu, draw target %llu, indirect %llu, depth %llu, pattern %llu); fast ring %llu regions (%.1f MiB); sampled images %llu cleared (%.1f MiB, %llu suballocated); device buffers released without their fill %llu (%.1f MiB); word 0x%08x\n", host + device, (hostBytes + deviceBytes) / 1048576.0, host, hostBytes / 1048576.0, hostReused, device, deviceBytes / 1048576.0, deviceReused, sites[0], sites[1], sites[2], sites[3], sites[4], sites[5], sites[6], sites[7], sites[8], ring, ringBytes / 1048576.0, images, imageBytes / 1048576.0, imagesPooled, owed, owedBytes / 1048576.0, static_cast<unsigned>(PoolPoisonWord()));
}

void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes) {
    const VkBufferCopy region{sourceOffset, destinationOffset, bytes};
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, source, destination, 1, &region);
}

void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void FillDeviceFunctions(const Context& context, DeviceFunctions& functions) {
    functions.cmdPipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    functions.cmdCopyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    functions.cmdUpdateBuffer = context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer");
    functions.cmdFillBuffer = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    functions.cmdBindPipeline = context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    functions.cmdBindDescriptorSets = context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    functions.cmdPushConstants = context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    functions.cmdDispatch = context.Function<PFN_vkCmdDispatch>("vkCmdDispatch");
    functions.cmdDispatchIndirect = context.Function<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect");
    functions.cmdBeginRenderPass = context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    functions.cmdEndRenderPass = context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    functions.cmdSetViewport = context.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport");
    functions.cmdSetScissor = context.Function<PFN_vkCmdSetScissor>("vkCmdSetScissor");
    functions.cmdBindVertexBuffers = context.Function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    functions.cmdBindIndexBuffer = context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
    functions.cmdDraw = context.Function<PFN_vkCmdDraw>("vkCmdDraw");
    functions.cmdDrawIndexed = context.Function<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
    functions.cmdDrawIndirect = context.Function<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect");
    functions.cmdDrawIndexedIndirect = context.Function<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect");
    functions.cmdCopyBufferToImage = context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    functions.cmdCopyImageToBuffer = context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    functions.cmdClearColorImage = context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    functions.updateDescriptorSets = context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    functions.allocateDescriptorSets = context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    functions.getFenceStatus = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    if (context.pushDescriptors) functions.cmdPushDescriptorSet = context.Function<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR");
}

RenderTarget::RenderTarget(const Context& context, const ColorTarget& target, bool blending) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (blending ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    Require((properties.optimalTilingFeatures & required) == required, "render-target format does not support required operations");
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties");
    Require(target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 && target.bytes <= supported.maxResourceSize, "render target exceeds device image limits");
    Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "render target exceeds framebuffer limits");
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(AllocateDeviceMemory(context, allocation, &memory, VramClass::Targets), "vkAllocateMemory render target");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = target.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    } catch (...) {
        release();
        throw;
    }
}

RenderTarget::~RenderTarget() {
    release();
}

void RenderTarget::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) FreeDeviceMemory(context, memory);
}

VkImage RenderTarget::Image() const {
    return image;
}

VkImageView RenderTarget::View() const {
    return view;
}

CommandBatch::CommandBatch(const Context& context) : context(context) {
    // Records into the device's one command pool and submits to its queue: device-lock work only.
    GuestMemory::AssertGpuLockHeld("CommandBatch");
    try {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = context.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &fence), "vkCreateFence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        // Work recorded so far goes first in queue order, so this batch sees its results.
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) recorder->Submit();
    } catch (...) {
        release();
        throw;
    }
}

CommandBatch::~CommandBatch() {
    release();
}

void CommandBatch::release() noexcept {
    if (pending) {
        auto result = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, fence);
        if (result == VK_NOT_READY) result = context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
    }
    if (commands) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (fence) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
}

VkCommandBuffer CommandBatch::Handle() const {
    return commands;
}

void CommandBatch::SubmitAndWait() {
    Submit();
    Wait();
}

void CommandBatch::Submit() {
    PerformanceTimer timing("Graphics.Submit");
    Require(!submitted, "command batch has already been submitted");
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    timing.Mark("command_end");
    Check(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submission, fence), "vkQueueSubmit graphics");
    timing.Mark("queue_submit");
    pending = true;
    submitted = true;
}

void CommandBatch::Wait() {
    Require(submitted, "command batch has not been submitted");
    if (!pending) return;
    PerformanceTimer timing("Graphics.Wait");
    const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    timing.Mark("fence_wait");
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) pending = false;
    Check(result, "vkWaitForFences graphics");
    // Recorded batches preceded this one, so their completions (write-backs) can run now.
    if (auto* recorder = Recorder::Active()) recorder->Reap();
}

}
