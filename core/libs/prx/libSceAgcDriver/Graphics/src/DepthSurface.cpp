#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

// A depth/stencil surface is a host image of as many array layers as the slices drawn or sampled
// so far (DB_DEPTH_VIEW SLICE_START picks a draw's layer; a 2D-array T# samples a range of them):
// shadow cascades and light shadow atlases are arrays of 1024x1024 slices. The registers never give
// the slice count, so the image grows when a draw or a T# reaches past it (see grow).
class DepthSurface {
public:
    DepthSurface(const Context& context, const DepthTarget& target) : context(context), target(target) {
        this->context.bufferPool.reset();
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be an attachment on this device");
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be sampled on this device");
        Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "depth target exceeds framebuffer limits");
        try {
            allocate(target.slice + 1u);
            Commands commands(context);
            prepare(commands.handle, 0);
            RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            commands.Finish(2);
            std::fprintf(stderr, "[gpu] depth surface 0x%llx (stencil 0x%llx, %ux%u, vk format %d) created\n", static_cast<unsigned long long>(target.address), static_cast<unsigned long long>(target.stencilAddress), target.extent.width, target.extent.height, static_cast<int>(target.format));
        } catch (...) {
            release();
            throw;
        }
    }
    ~DepthSurface() { release(); }
    DepthSurface(const DepthSurface&) = delete;
    DepthSurface& operator=(const DepthSurface&) = delete;

    // The attachment view of one array layer.
    VkImageView LayerView(std::uint32_t layer) {
        if (layer >= layers) grow(layer + 1u);
        auto& view = layerViews[layer];
        if (view == VK_NULL_HANDLE) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = target.format;
            viewInfo.subresourceRange = {aspects(), 0, 1, layer, 1};
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView depth");
        }
        return view;
    }

    std::shared_ptr<Texture> Sampled(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
        std::array<std::uint32_t, 12> key{};
        std::copy_n(words.begin(), std::min<std::size_t>(words.size(), 8), key.begin());
        key[8] = components.r;
        key[9] = components.g;
        key[10] = components.b;
        key[11] = components.a;
        if (const auto found = textures.find(key); found != textures.end()) return found->second;
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
        const auto format = ResolveTextureFormat(resource.format);
        const bool depthBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
        // A 2D T# samples its base array slice, a 2D-array T# its slices up to the last one.
        const bool array = resource.dimension == TextureDimension::k2DArray;
        const auto lastLayer = array ? resource.depthOrLastArray : resource.baseArray;
        if ((format != expected && !depthBits) || (resource.dimension != TextureDimension::k2D && !array) || resource.width != target.extent.width || resource.height != target.extent.height || resource.baseLevel != 0 || resource.lastLevel != 0 || resource.baseArray > lastLayer || lastLayer >= MaxLayers) {
            char text[448];
            std::snprintf(text, sizeof(text), "AGC graphics: sampling the %s plane of depth surface 0x%llx (%ux%u, vk format %d) as a %ux%u texture of guest format %u (vk %d), tile mode %u, dimension %d, levels %u-%u, slices %u-%u is not implemented (T# %08x %08x %08x %08x %08x %08x %08x %08x)",
                          stencil ? "stencil" : "depth", static_cast<unsigned long long>(target.address), target.extent.width, target.extent.height, static_cast<int>(target.format), resource.width, resource.height, resource.format, static_cast<int>(format),
                          static_cast<unsigned>(resource.tileMode), static_cast<int>(resource.dimension), resource.baseLevel, resource.lastLevel, resource.baseArray, lastLayer, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
            throw std::runtime_error(text);
        }
        if (lastLayer >= layers) grow(lastLayer + 1u);
        auto texture = std::make_shared<Texture>(context, image, target.format, stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, components, resource.baseArray, lastLayer - resource.baseArray + 1u, array);
        textures.emplace(key, texture);
        return texture;
    }

    // Whether Sampled hands out `texture` (some key of the current image maps to it).
    bool Serves(const Texture* texture) const {
        return std::any_of(textures.begin(), textures.end(), [&](const auto& entry) { return entry.second.get() == texture; });
    }

    // Takes over depth a compute pass left in a storage image of this memory (Demon's Souls downsamples
    // its depth with compute into the half-size target it then depth-tests against): the image's
    // first slice becomes slice 0, through a device buffer, behind the work recorded so far. Only
    // results still pending on the GPU are found; depth already stored to guest memory is not read
    // (reported when `written` says a write was noted).
    void TakeWrittenDepth(bool written) {
        overwritten = false;
        if (target.address == 0) return;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const std::uint32_t texelBytes = d16 ? 2u : 4u;
        const auto bytes = static_cast<std::uint64_t>(target.extent.width) * target.extent.height * texelBytes;
        const auto source = StorageTexture::FindPending(target.address, bytes);
        if (source == nullptr || source->Descriptor().width != target.extent.width || source->Descriptor().height != target.extent.height || BytesPerElement(source->Descriptor().format) != texelBytes) {
            static std::atomic<int> reports{0};
            if (written && reports.fetch_add(1) < 4) std::fprintf(stderr, "[gpu] depth surface 0x%llx: guest memory written by a compute pass is not taken over (%s)\n", static_cast<unsigned long long>(target.address), source == nullptr ? "results no longer pending" : "another texel layout");
            return;
        }
        auto buffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Commands commands(context);
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {target.extent.width, target.extent.height, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands.handle, source->Image(), VK_IMAGE_LAYOUT_GENERAL, buffer->Handle(), 1, &region);
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands.handle, buffer->Handle(), image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
        if (commands.recorder != nullptr) commands.recorder->Keep(buffer);
        commands.Finish(3);
    }

    // A storage image wrote this surface's memory (NoteDepthSurfaceWrite): that image, not this one,
    // holds the newest depth until the next use as an attachment takes it over (TakeWrittenDepth).
    bool overwritten = false;

    // The HTILE bytes of one slice: 4 bytes per 8x8 tile in 32 KiB blocks of 1024x512 pixels (the
    // toolkit fills 288 KiB for a 2560x1440 surface and 64 KiB per 1024x1024 shadow slice).
    std::uint64_t HtileSliceBytes() const {
        return static_cast<std::uint64_t>((target.extent.width + 1023u) / 1024u) * ((target.extent.height + 511u) / 512u) * 0x8000u;
    }

    // Marks the slices whose HTILE overlaps [begin, end) as cleared (NoteDepthMetadataClear).
    void NoteMetadataWrite(std::uint64_t begin, std::uint64_t end) {
        if (htileAddress == 0) return;
        const auto sliceBytes = HtileSliceBytes();
        // A single-slice surface owns its whole (possibly further padded) block; array slices follow
        // one another.
        const auto htileEnd = htileAddress + sliceBytes * std::max(layers, 1u);
        if (end <= htileAddress || begin >= htileEnd) return;
        const auto first = static_cast<std::uint32_t>((std::max(begin, htileAddress) - htileAddress) / sliceBytes);
        const auto last = static_cast<std::uint32_t>((std::min(end, htileEnd) - 1u - htileAddress) / sliceBytes);
        for (std::uint32_t slice = first; slice <= last; ++slice) clearedSlices.insert(slice);
    }

    // Clears the slices whose HTILE a shader zeroed to the clear values, behind the work recorded so
    // far (before the draw or sampling about to use the surface).
    void ApplyMetadataClears(float depth, std::uint8_t stencil) {
        if (clearedSlices.empty()) return;
        Commands commands(context);
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear{depth, stencil};
        std::vector<VkImageSubresourceRange> ranges;
        for (const auto slice : clearedSlices) {
            if (slice >= layers) break;
            if (!ranges.empty() && ranges.back().baseArrayLayer + ranges.back().layerCount == slice) ++ranges.back().layerCount;
            else ranges.push_back({aspects(), 0, 1, slice, 1});
        }
        if (!ranges.empty()) context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands.handle, image, VK_IMAGE_LAYOUT_GENERAL, &clear, static_cast<std::uint32_t>(ranges.size()), ranges.data());
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
        commands.Finish(2);
        static std::atomic<int> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "[gpu] depth surface 0x%llx (%ux%u): HTILE 0x%llx written by a shader, %zu slice(s) from %u cleared to depth %g stencil %u\n", static_cast<unsigned long long>(target.address != 0 ? target.address : target.stencilAddress), target.extent.width, target.extent.height, static_cast<unsigned long long>(htileAddress), clearedSlices.size(), *clearedSlices.begin(), depth, stencil);
        clearedSlices.clear();
    }

    // The HTILE base the last draw named, and the clear values it held (for sampling).
    std::uint64_t htileAddress = 0;
    float clearDepth = 0;
    std::uint8_t clearStencil = 0;
    std::set<std::uint32_t> clearedSlices;

    // Whether a resource of this extent at `address` is a plane of this surface. The memory of a depth
    // surface is often handed to other resources later in the frame (transient allocations: the
    // shadow cascades share theirs with a 1280x720 color target); those are not its planes.
    bool Covers(std::uint64_t address, std::uint32_t width, std::uint32_t height) const {
        const bool plane = (target.address != 0 && target.address == address) || (target.stencilAddress != 0 && target.stencilAddress == address);
        return plane && target.extent.width == width && target.extent.height == height;
    }

    static constexpr std::uint32_t MaxLayers = 2048;
    const Context context;
    const DepthTarget target;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t layers = 0;

private:
    // Commands recorded into the active recorder (behind the work drawn so far), or into a batch of
    // their own when there is none.
    struct Commands {
        explicit Commands(const Context& context) : recorder(Recorder::Active()) {
            if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
            handle = recorder != nullptr ? recorder->Commands() : batch->Handle();
        }
        void Finish(std::uint32_t barriers) {
            if (batch) batch->SubmitAndWait();
            else Recorder::CountBarriers(Recorder::CommandClass::Draw, barriers);
        }
        Recorder* recorder;
        std::unique_ptr<CommandBatch> batch;
        VkCommandBuffer handle = VK_NULL_HANDLE;
    };

    // An image a grow replaced, with its views and textures: framebuffers and resource sets may
    // still name them, and their view handles must not be recycled while they do, so they live on
    // with the surface (until device teardown).
    struct Retired {
        VkImage image;
        VkDeviceMemory memory;
        std::map<std::uint32_t, VkImageView> views;
        std::map<std::array<std::uint32_t, 12>, std::shared_ptr<Texture>> textures;
    };

    VkImageAspectFlags aspects() const {
        return VK_IMAGE_ASPECT_DEPTH_BIT | (target.stencilAddress != 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    }

    void allocate(std::uint32_t count) {
        Require(count <= MaxLayers, "depth surface slice exceeds the supported array size");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = count;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage depth");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory depth target");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory depth");
        layers = count;
    }

    // Moves every layer of the new image to the general layout and clears layers from `cleared` on
    // to the surface's clear value; the caller records the barrier that orders them before use.
    void prepare(VkCommandBuffer commands, std::uint32_t cleared) {
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = image;
        toGeneral.subresourceRange = {aspects(), 0, 1, 0, layers};
        const auto barrier = context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier");
        barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        if (cleared >= layers) return;
        const VkClearDepthStencilValue clear{target.clearDepth, target.clearStencil};
        const VkImageSubresourceRange range{aspects(), 0, 1, cleared, layers - cleared};
        context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    }

    // A draw or T# reached past the last layer: a new image with room for `needed` layers (at least
    // twice as many as before, so a slice-by-slice first frame grows a few times only) takes over,
    // the layers so far copied over on the device behind the work recorded on the old one.
    void grow(std::uint32_t needed) {
        const auto previous = layers;
        retired.push_back({image, memory, std::move(layerViews), std::move(textures)});
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        layerViews.clear();
        textures.clear();
        allocate(std::min(std::max(needed, previous * 2u), MaxLayers));
        Commands commands(context);
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        prepare(commands.handle, previous);
        std::vector<VkImageCopy> regions;
        for (const auto aspect : {VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
            if ((aspects() & aspect) == 0) continue;
            VkImageCopy region{};
            region.srcSubresource = {static_cast<VkImageAspectFlags>(aspect), 0, 0, previous};
            region.dstSubresource = region.srcSubresource;
            region.extent = {target.extent.width, target.extent.height, 1};
            regions.push_back(region);
        }
        context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands.handle, retired.back().image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, static_cast<std::uint32_t>(regions.size()), regions.data());
        RecordMemoryBarrier(context, commands.handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
        commands.Finish(3);
        std::fprintf(stderr, "[gpu] depth surface 0x%llx (%ux%u) grew from %u to %u slices\n", static_cast<unsigned long long>(target.address != 0 ? target.address : target.stencilAddress), target.extent.width, target.extent.height, previous, layers);
    }

    std::map<std::uint32_t, VkImageView> layerViews;
    std::map<std::array<std::uint32_t, 12>, std::shared_ptr<Texture>> textures;
    std::vector<Retired> retired;

    void release() noexcept {
        const auto discard = [&](Retired& old) {
            old.textures.clear();
            for (const auto& [layer, view] : old.views) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
            if (old.image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, old.image, nullptr);
            if (old.memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, old.memory, nullptr);
        };
        Retired current{image, memory, std::move(layerViews), std::move(textures)};
        discard(current);
        for (auto& old : retired) discard(old);
        retired.clear();
        layerViews.clear();
        textures.clear();
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

bool sameSurface(const DepthTarget& a, const DepthTarget& b) {
    return a.address == b.address && a.stencilAddress == b.stencilAddress && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.format == b.format;
}

HostMutex& surfacesMutex() {
    static HostMutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<DepthSurface>>& surfaces() {
    static auto* list = new std::vector<std::unique_ptr<DepthSurface>>();
    return *list;
}

// See DepthSurfaceSerial: bumped under surfacesMutex wherever the list or an `overwritten` flag moves.
std::atomic<std::uint64_t>& depthSerial() {
    static std::atomic<std::uint64_t> serial{1};
    return serial;
}

void bumpDepthSerial() {
    depthSerial().fetch_add(1, std::memory_order_release);
}

}

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (surface->context.device != context.device || !sameSurface(surface->target, target)) continue;
        if (surface->overwritten) {
            surface->TakeWrittenDepth(true);
            bumpDepthSerial();
        }
        surface->htileAddress = target.htileAddress;
        surface->clearDepth = target.clearDepth;
        surface->clearStencil = target.clearStencil;
        const auto view = surface->LayerView(target.slice);
        surface->ApplyMetadataClears(target.clearDepth, target.clearStencil);
        return view;
    }
    surfaces().push_back(std::make_unique<DepthSurface>(context, target));
    bumpDepthSerial();
    auto& surface = *surfaces().back();
    // A compute pass may have written the memory before any draw used it as depth.
    surface.TakeWrittenDepth(false);
    surface.htileAddress = target.htileAddress;
    surface.clearDepth = target.clearDepth;
    surface.clearStencil = target.clearStencil;
    return surface.LayerView(target.slice);
}

void NoteDepthMetadataClear(std::uint64_t begin, std::uint64_t end) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) surface->NoteMetadataWrite(begin, end);
}

void NoteDepthSurfaceWrite(std::uint64_t address, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (surface->Covers(address, width, height) && !surface->overwritten) {
            surface->overwritten = true;
            bumpDepthSerial();
        }
    }
}

void ClearDepthSurfaces(VkDevice device) {
    std::lock_guard lock(surfacesMutex());
    if (std::erase_if(surfaces(), [&](const auto& surface) { return surface->context.device == device; }) != 0) bumpDepthSerial();
}

std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
    std::lock_guard lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return surface->context.device == context.device && surface->Covers(resource.baseAddress, resource.width, resource.height);
    });
    // Memory a compute pass wrote since the last depth draw is sampled from its storage image.
    if (found == list.rend() || (*found)->overwritten) return nullptr;
    auto texture = (*found)->Sampled(words, resource, components);
    (*found)->ApplyMetadataClears((*found)->clearDepth, (*found)->clearStencil);
    return texture;
}

bool DepthSurfaceServes(const Context& context, const GuestTextureResource& resource, const Texture* texture) {
    std::lock_guard lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return surface->context.device == context.device && surface->Covers(resource.baseAddress, resource.width, resource.height);
    });
    if (found == list.rend() || (*found)->overwritten || !(*found)->Serves(texture)) return false;
    (*found)->ApplyMetadataClears((*found)->clearDepth, (*found)->clearStencil);
    return true;
}

std::size_t DumpDepthSurfaces(const Context& context, const std::string& prefix, std::span<const std::uint64_t> addresses) {
    std::lock_guard lock(surfacesMutex());
    std::size_t written = 0;
    for (const auto& surface : surfaces()) {
        if (surface->context.device != context.device) continue;
        const auto& target = surface->target;
        const auto listed = [&](std::uint64_t address) { return address != 0 && std::find(addresses.begin(), addresses.end(), address) != addresses.end(); };
        if (!addresses.empty() && !listed(target.address) && !listed(target.stencilAddress)) continue;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool stencil = target.stencilAddress != 0;
        const std::uint64_t texels = static_cast<std::uint64_t>(target.extent.width) * target.extent.height;
        const std::uint64_t depthBytes = texels * (d16 ? 2u : 4u);
        const std::uint64_t sliceBytes = depthBytes + (stencil ? texels : 0u);
        const auto slices = std::min(surface->layers, 8u);
        std::vector<VkBufferImageCopy> regions;
        for (std::uint32_t slice = 0; slice < slices; ++slice) {
            VkBufferImageCopy region{};
            region.bufferOffset = slice * sliceBytes;
            region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, slice, 1};
            region.imageExtent = {target.extent.width, target.extent.height, 1};
            if (target.address != 0) regions.push_back(region);
            if (stencil) {
                region.bufferOffset += depthBytes;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                regions.push_back(region);
            }
        }
        if (regions.empty()) continue;
        Buffer buffer(context, static_cast<std::size_t>(sliceBytes * slices), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(batch.Handle(), surface->image, VK_IMAGE_LAYOUT_GENERAL, buffer.Handle(), static_cast<std::uint32_t>(regions.size()), regions.data());
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        const auto save = [&](const char* plane, std::uint64_t address, std::uint32_t slice, VkFormat format, std::uint64_t offset, std::uint64_t bytes) {
            char name[96];
            std::snprintf(name, sizeof(name), "%s_%llx_%ux%u_l%u.raw", plane, static_cast<unsigned long long>(address), target.extent.width, target.extent.height, slice);
            std::FILE* file = std::fopen((prefix + name).c_str(), "wb");
            if (file == nullptr) return;
            const std::uint32_t header[3] = {target.extent.width, target.extent.height, static_cast<std::uint32_t>(format)};
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(buffer.Bytes().data() + offset, 1, static_cast<std::size_t>(bytes), file);
            std::fclose(file);
            ++written;
        };
        for (std::uint32_t slice = 0; slice < slices; ++slice) {
            if (target.address != 0) save("depth", target.address, slice, d16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT, slice * sliceBytes, depthBytes);
            if (stencil) save("stencil", target.stencilAddress, slice, VK_FORMAT_S8_UINT, slice * sliceBytes + depthBytes, texels);
        }
    }
    return written;
}

std::uint64_t DepthSurfaceSerial() {
    return depthSerial().load(std::memory_order_acquire);
}

bool DepthSurfaceAt(std::uint64_t address, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) { return surface->Covers(address, width, height) && !surface->overwritten; });
}

}
