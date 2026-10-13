#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RecordCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/PortSettings.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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
        PoisonPooled(context, commands.handle, *buffer, PoisonSite::Depth);
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
            handle = recorder != nullptr ? recorder->CommandsTouchingNothing(Recorder::CommandSite::Depth) : batch->Handle();
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
        Check(AllocateDeviceMemory(context, allocation, &memory, VramClass::Targets), "vkAllocateMemory depth target");
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
            if (old.memory) FreeDeviceMemory(context, old.memory);
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

std::size_t ReadBackDepthSurface(const Context& context, std::uint64_t address, std::uint32_t slice, const std::function<void(const ImageReadback&)>& consume) {
    std::lock_guard lock(surfacesMutex());
    const auto matches = [&](const std::unique_ptr<DepthSurface>& surface) { return surface->context.device == context.device && address != 0 && surface->target.address == address && slice < surface->layers; };
    const auto aliases = static_cast<std::uint32_t>(std::count_if(surfaces().begin(), surfaces().end(), matches));
    std::size_t read = 0;
    for (const auto& surface : surfaces()) {
        if (!matches(surface)) continue;
        const auto& target = surface->target;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const std::uint32_t texelBytes = d16 ? 2u : 4u;
        Buffer buffer(context, static_cast<std::size_t>(target.extent.width) * target.extent.height * texelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, slice, 1};
        region.imageExtent = {target.extent.width, target.extent.height, 1};
        CommandBatch batch(context);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(batch.Handle(), surface->image, VK_IMAGE_LAYOUT_GENERAL, buffer.Handle(), 1, &region);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        ImageReadback readback;
        readback.address = address;
        readback.guestBytes = static_cast<std::uint64_t>(target.extent.width) * target.extent.height * texelBytes;
        readback.format = d16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
        readback.width = target.extent.width;
        readback.height = target.extent.height;
        readback.firstLayer = slice;
        readback.rowTexels = target.extent.width;
        readback.texelBytes = texelBytes;
        readback.alias = static_cast<std::uint32_t>(read);
        readback.aliases = aliases;
        readback.bytes = buffer.Bytes();
        consume(readback);
        ++read;
    }
    return read;
}

std::uint64_t DepthSurfaceSerial() {
    return depthSerial().load(std::memory_order_acquire);
}

bool DepthSurfaceAt(std::uint64_t address, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) { return surface->Covers(address, width, height) && !surface->overwritten; });
}

namespace {

bool depthProbeOn() {
    static const bool on = std::getenv("APS5_ASPECT_PROBE") != nullptr && std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return on;
}

struct ProbedViewport {
    VkViewport viewport;
    VkRect2D scissor;
    std::uint64_t draws;
};

struct DepthViewportWindow {
    VkExtent2D extent{};
    std::uint64_t draws = 0;
    std::vector<ProbedViewport> viewports;
};

HostMutex& probeMutex() {
    static HostMutex mutex;
    return mutex;
}

DepthViewportWindow& probeWindow() {
    static auto* window = new DepthViewportWindow();
    return *window;
}

// A distinct write of a watched target: a draw ('D') or a dispatch ('C') into a display buffer, a
// draw ('U') or a dispatch ('V') into the UI target, of one program (0 for 'U': UI draws are grouped
// by state only), viewport, scissor, blend and extent, with its count and the span of its order
// within the frames. In the UI phase (see PhaseTarget) also a dispatch ('W') or a storage write
// outside a dispatch ('X': a copy, clear or fill; draws' own target writes are left out) of another
// image of the UI target's extent.
struct DisplayWrite {
    char kind;
    std::uint64_t program;
    VkViewport viewport;
    VkRect2D scissor;
    bool blend;
    VkExtent2D extent;
    // 'U': the fragment program reads the fragment position (readsFragCoord) and why the draw was or
    // was not remapped (uiRemap's code). 'W', 'X': the image written.
    bool fragCoord = false;
    char remap = 0;
    std::uint64_t address = 0;
    std::uint64_t count = 1;
    std::uint32_t firstRank = 0;
    std::uint32_t lastRank = 0;
    std::uint64_t firstWork = 0;
    std::uint64_t lastWork = 0;
};

// A texture the display buffer's composite samples (its fragment program's sampled images).
struct CompositeSample {
    std::uint64_t address;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
};

// uiRemap's codes, in the order PhaseTarget counts them.
constexpr std::array<char, 6> RemapCodes{'r', 'm', 'o', 's', 'b', 'a'};

const char* remapName(char code) {
    switch (code) {
        case 'r': return "remapped";
        case 'm': return "moved at the composite";
        case 'o': return "UI_RECT off";
        case 's': return "not slot 0";
        case 'b': return "blend off";
        case 'a': return "whole target";
        default: return "not the UI target";
    }
}

// A colour target written in the UI phase (from the UI target's first draw after a present or a
// composite to the display buffer's composite): its draws, the blended ones and, for the UI target,
// how many had each uiRemap code (RemapCodes order).
struct PhaseTarget {
    std::uint64_t address;
    VkExtent2D extent;
    VkFormat format;
    std::uint64_t draws = 0;
    std::uint64_t blended = 0;
    std::array<std::uint64_t, RemapCodes.size()> remaps{};
};

// A texture the UI target's draws sample, with the draws that do.
struct UiSample {
    std::uint64_t address;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint64_t draws = 0;
};

struct MoveRefusal {
    const char* why = nullptr;
    std::uint64_t count = 0;
};

struct DisplayWriteWindow {
    // Kept across windows: the registered buffers, the current frame's counters and the last
    // composite's samples.
    std::array<std::uint64_t, 4> buffers{};
    std::size_t nextBuffer = 0;
    std::uint64_t work = 0;
    std::uint32_t rank = 0;
    std::array<CompositeSample, 4> samples{};
    std::size_t sampleCount = 0;
    std::size_t displaySized = 0;
    // The UI phase is open; the UI target was drawn since the last composite (the move's condition),
    // with the surface bytes and extent of its last draw.
    bool uiPhase = false;
    bool uiWritten = false;
    std::uint64_t uiBytes = 0;
    VkExtent2D uiExtent{};
    // Since the last digest.
    std::uint64_t presents = 0;
    std::uint64_t frameWork = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t uiDraws = 0;
    std::uint64_t uiDispatches = 0;
    std::uint64_t remapped = 0;
    std::uint64_t unlisted = 0;
    std::vector<DisplayWrite> writes;
    std::uint64_t phaseWrites = 0;
    std::uint64_t uiPhases = 0;
    std::uint64_t uiFragCoord = 0;
    std::uint64_t moved = 0;
    // Moves whose margins took the UI target's clear colour, or transparent black (none seen yet).
    std::uint64_t marginsCleared = 0;
    std::uint64_t marginsTransparent = 0;
    std::uint64_t notMoved = 0;
    std::array<MoveRefusal, 8> refusals{};
    std::uint64_t phaseUnlisted = 0;
    std::vector<PhaseTarget> phaseTargets;
    std::vector<UiSample> uiSamples;
};

DisplayWriteWindow& displayWindow() {
    static auto* window = new DisplayWriteWindow();
    return *window;
}

// APS5_NATIVE_ASPECT_UI_RECT (on by default inside NativeAspect = on, PortSettings::NativeAspectUiRect:
// APS5_NO_NATIVE_ASPECT_UI_RECT=1 goes back to part 1, the 16:9 UI stretched with the 3D; =1 or =0
// forces it on or off; nothing happens without a presentation aspect): the blended draws into the UI
// target are drawn into the part of it that the presenter shows undistorted (ComputeUndistortedRect),
// so the 16:9 UI keeps its proportions in a centred 16:9 region of a wider (or taller) window. The UI
// target is the one texture at the display buffer's size that the display buffer's composite samples
// (learned from the first composite; until then, or when that is not one texture, nothing is
// remapped), or APS5_NATIVE_ASPECT_UI_TARGET=<hex address>. The composite and every other write of
// the display buffer keep their state.
bool uiRectOn() {
    static const bool on = PortSettings::NativeAspectUiRect(GetPortSettings_nid_no_patch(), std::getenv("APS5_NATIVE_ASPECT_UI_RECT"), std::getenv("APS5_NO_NATIVE_ASPECT_UI_RECT"));
    return on;
}

// APS5_NATIVE_ASPECT_UI_BLIT (on inside APS5_NATIVE_ASPECT_UI_RECT; APS5_NO_NATIVE_ASPECT_UI_BLIT=1
// goes back to the per-draw remap): the UI draws keep their state and the display buffer's
// composite first moves the UI target's content into the undistorted rect (moveUiTarget). The
// per-draw remap moves a draw's geometry but not what its fragment program derives from the fragment
// position: Demon's Souls' item icons clip against a screen-space rect, so remapped they fall
// outside it and vanish (only where the moved and the original rect overlap does a sliver stay).
bool uiBlitOn() {
    static const bool on = [] {
        if (!uiRectOn()) return false;
        const char* value = std::getenv("APS5_NO_NATIVE_ASPECT_UI_BLIT");
        return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0;
    }();
    return on;
}

// APS5_NATIVE_ASPECT_UI_EDGE_STRETCH=1 (part 3's margins): the move stretches the UI target's edge
// columns (rows) over the margins, so a full-screen dim reaches the window's edges. Off by default: a
// movie drawn through the UI target (the intro's Bink, t583) smears its edge columns across the
// margins as streaks, and a UI line touching the rect's edge (the menu's separator) is drawn out to
// the window's edges. The margins instead take the UI target's own clear colour (the last fixed DCC
// clear it was key-filled or uploaded with; transparent black until one is seen): "no UI" in the
// game's own alpha convention, so the composite shows what lies under the UI there, the 3D beside the
// HUD and the empty scene (black) beside a movie.
bool uiEdgeStretchOn() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_NATIVE_ASPECT_UI_EDGE_STRETCH");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return on;
}

std::atomic<std::uint64_t>& uiTarget() {
    static std::atomic<std::uint64_t> target{[] {
        const char* value = std::getenv("APS5_NATIVE_ASPECT_UI_TARGET");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ull;
    }()};
    return target;
}

bool watchOn() {
    return depthProbeOn() || uiRectOn();
}

// The dispatch running on this thread (AspectProbeDispatch) and the storage images it has noted.
struct ProbeDispatchSubject {
    bool active = false;
    std::uint64_t program = 0;
    std::array<std::uint64_t, 4> noted{};
    std::size_t notedCount = 0;
};

ProbeDispatchSubject& probeDispatchSubject() {
    static thread_local ProbeDispatchSubject subject;
    return subject;
}

bool isDisplayBuffer(const DisplayWriteWindow& window, std::uint64_t address) {
    return address != 0 && std::find(window.buffers.begin(), window.buffers.end(), address) != window.buffers.end();
}

void noteDisplayWrite(DisplayWriteWindow& window, const DisplayWrite& write) {
    const auto rank = window.rank++;
    switch (write.kind) {
        case 'D': ++window.draws; break;
        case 'C': ++window.dispatches; break;
        case 'U': ++window.uiDraws; break;
        case 'V': ++window.uiDispatches; break;
        default: ++window.phaseWrites; break;
    }
    if (!depthProbeOn()) return;
    for (auto& entry : window.writes) {
        if (entry.kind == write.kind && entry.program == write.program && entry.blend == write.blend && entry.extent.width == write.extent.width && entry.extent.height == write.extent.height
            && entry.fragCoord == write.fragCoord && entry.remap == write.remap && entry.address == write.address
            && std::memcmp(&entry.viewport, &write.viewport, sizeof(VkViewport)) == 0 && std::memcmp(&entry.scissor, &write.scissor, sizeof(VkRect2D)) == 0) {
            ++entry.count;
            entry.firstRank = std::min(entry.firstRank, rank);
            entry.lastRank = std::max(entry.lastRank, rank);
            entry.firstWork = std::min(entry.firstWork, window.work);
            entry.lastWork = std::max(entry.lastWork, window.work);
            return;
        }
    }
    if (window.writes.size() >= 24) {
        ++window.unlisted;
        return;
    }
    auto entry = write;
    entry.firstRank = entry.lastRank = rank;
    entry.firstWork = entry.lastWork = window.work;
    window.writes.push_back(entry);
}

// Every texture a draw's fragment program samples (from its descriptor words), handed to `visit`.
template <typename Visit>
void forSampledImages(std::span<const CompiledShader> shaders, const Visit& visit) {
    for (const auto& shader : shaders) {
        if (shader.stage != ShaderRecompiler::ShaderStage::Fragment || shader.program == nullptr) continue;
        for (const auto& binding : shader.program->bindings) {
            if (binding.kind != ShaderRecompiler::DescriptorKind::SampledImage || binding.guestDescriptor.size() < static_cast<std::size_t>(binding.count) * 8u) continue;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                GuestTextureResource resource{};
                try {
                    resource = DecodeTextureResource(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u));
                } catch (const std::exception&) {
                    continue;
                }
                if (resource.baseAddress != 0) visit(resource);
            }
        }
    }
}

// The composite's sampled textures; the UI target is learned when exactly one of them, other than a
// display buffer, has the display buffer's extent.
void learnComposite(DisplayWriteWindow& window, std::span<const CompiledShader> shaders, VkExtent2D extent) {
    window.sampleCount = 0;
    window.displaySized = 0;
    std::uint64_t candidate = 0;
    forSampledImages(shaders, [&](const GuestTextureResource& resource) {
        if (window.sampleCount < window.samples.size()) window.samples[window.sampleCount++] = {resource.baseAddress, resource.width, resource.height, resource.format};
        if (resource.width == extent.width && resource.height == extent.height && !isDisplayBuffer(window, resource.baseAddress) && resource.baseAddress != candidate) {
            ++window.displaySized;
            candidate = resource.baseAddress;
        }
    });
    if (!uiRectOn()) return;
    static const bool named = uiTarget().load(std::memory_order_relaxed) != 0;
    static bool reported = false;
    if (named) {
        if (!reported) std::fprintf(stderr, "[aspect] UI rect (APS5_NATIVE_ASPECT_UI_RECT): UI target 0x%llx from APS5_NATIVE_ASPECT_UI_TARGET\n", static_cast<unsigned long long>(uiTarget().load(std::memory_order_relaxed)));
    } else if (window.displaySized == 1) {
        // Followed from composite to composite: a change (a menu or movie composite) is printed.
        if (uiTarget().exchange(candidate, std::memory_order_relaxed) == candidate) return;
        std::fprintf(stderr, "[aspect] UI rect (APS5_NATIVE_ASPECT_UI_RECT): UI target 0x%llx %ux%u learned from the display buffer's composite\n", static_cast<unsigned long long>(candidate), extent.width, extent.height);
    } else if (!reported) {
        std::fprintf(stderr, "[aspect] UI rect (APS5_NATIVE_ASPECT_UI_RECT): a composite samples %zu textures at the display buffer's %ux%u: no UI target from it (APS5_NATIVE_ASPECT_UI_TARGET=<hex> names one)\n", window.displaySized, extent.width, extent.height);
    } else {
        return;
    }
    reported = true;
    std::fflush(stderr);
}

// Why a draw writing the UI target is or is not remapped: 'r' remapped into `rect` (of `extent`),
// 'm' left whole for the composite's move (APS5_NATIVE_ASPECT_UI_BLIT), 'o' UI_RECT off, 's' the UI
// target is not its first colour target, 'b' not blended (a clear or a copy keeps the whole target,
// so the columns outside the rect are cleared too), 'a' no presentation aspect or a rect that is the
// whole target; 0 for a draw not writing the UI target.
char uiRemap(const State& state, AspectFitRect& rect, VkExtent2D& extent) {
    const auto target = uiTarget().load(std::memory_order_relaxed);
    if (target == 0 || !state.hasColorTarget) return 0;
    const auto targets = state.colors.empty() ? std::size_t{1} : state.colors.size();
    std::size_t slot = 0;
    while (slot < targets && (state.colors.empty() ? state.color : state.colors[slot]).address != target) ++slot;
    if (slot == targets) return 0;
    if (!uiRectOn()) return 'o';
    if (slot != 0) return 's';
    const auto& color = state.colors.empty() ? state.color : state.colors[0];
    const auto& blend = state.blends.empty() ? state.blend : state.blends[0];
    if (blend.blendEnable != VK_TRUE) return 'b';
    const auto aspect = PresentationAspect_nid_postfix();
    if (color.extent.width == 0 || color.extent.height == 0 || aspect.width == 0 || aspect.height == 0) return 'a';
    rect = ComputeUndistortedRect_nid_postfix(color.extent.width, color.extent.height, aspect.width, aspect.height);
    extent = color.extent;
    if (rect.width == extent.width && rect.height == extent.height) return 'a';
    return uiBlitOn() ? 'm' : 'r';
}

// The undistorted rect a draw into the UI target is mapped into (uiRemap's 'r'), or false.
bool uiRect(const State& state, AspectFitRect& rect, VkExtent2D& extent) {
    return uiRectOn() && uiRemap(state, rect, extent) == 'r';
}

// Whether a fragment program reads the fragment position: its SPIR-V decorates a variable BuiltIn
// FragCoord (OpDecorate 71, BuiltIn 11, FragCoord 15). Memoized by variant, under probeMutex.
bool readsFragCoord(const ShaderRecompiler::RecompileResult& program) {
    static auto* memo = new std::map<std::uint64_t, bool>();
    if (program.variantId != 0) {
        if (const auto found = memo->find(program.variantId); found != memo->end()) return found->second;
    }
    const auto& words = program.spirv.Words();
    bool reads = false;
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16u;
        if (count == 0) break;
        if ((words[at] & 0xffffu) == 71u && count >= 4 && at + 3 < words.size() && words[at + 2] == 11u && words[at + 3] == 15u) {
            reads = true;
            break;
        }
        at += count;
    }
    if (program.variantId != 0 && memo->size() < 4096) memo->emplace(program.variantId, reads);
    return reads;
}

bool phaseTargetNoted(const DisplayWriteWindow& window, std::uint64_t address) {
    return std::any_of(window.phaseTargets.begin(), window.phaseTargets.end(), [&](const PhaseTarget& target) { return target.address == address; });
}

void notePhaseTarget(DisplayWriteWindow& window, const ColorTarget& color, bool blended, char remap) {
    auto found = std::find_if(window.phaseTargets.begin(), window.phaseTargets.end(), [&](const PhaseTarget& target) {
        return target.address == color.address && target.extent.width == color.extent.width && target.extent.height == color.extent.height;
    });
    if (found == window.phaseTargets.end()) {
        if (window.phaseTargets.size() >= 12) {
            ++window.phaseUnlisted;
            return;
        }
        window.phaseTargets.push_back({color.address, color.extent, color.format});
        found = window.phaseTargets.end() - 1;
    }
    ++found->draws;
    if (blended) ++found->blended;
    const auto code = std::find(RemapCodes.begin(), RemapCodes.end(), remap);
    if (code != RemapCodes.end()) ++found->remaps[static_cast<std::size_t>(code - RemapCodes.begin())];
}

void noteUiSamples(DisplayWriteWindow& window, std::span<const CompiledShader> shaders) {
    forSampledImages(shaders, [&](const GuestTextureResource& resource) {
        for (auto& sample : window.uiSamples) {
            if (sample.address == resource.baseAddress && sample.width == resource.width && sample.height == resource.height) {
                ++sample.draws;
                return;
            }
        }
        if (window.uiSamples.size() < 12) window.uiSamples.push_back({resource.baseAddress, resource.width, resource.height, resource.format, 1});
    });
}

// The UI move a composite asks for (takeUiMove): the UI target's address, surface bytes and extent,
// or why there is none.
struct UiMove {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    VkExtent2D extent{};
    const char* refusal = nullptr;
};

void noteMoveRefusal(DisplayWriteWindow& window, const char* why) {
    ++window.notMoved;
    for (auto& refusal : window.refusals) {
        if (refusal.why == nullptr) refusal.why = why;
        if (std::strcmp(refusal.why, why) == 0) {
            ++refusal.count;
            return;
        }
    }
}

// At the display buffer's composite (APS5_NATIVE_ASPECT_UI_BLIT): the move to record when the
// composite samples the UI target and the target was drawn since the last composite. A target not
// drawn again still holds the moved content, which must not move twice.
UiMove takeUiMove(DisplayWriteWindow& window) {
    const auto ui = uiTarget().load(std::memory_order_relaxed);
    const auto samples = window.samples.begin() + static_cast<std::ptrdiff_t>(window.sampleCount);
    if (ui == 0 || std::none_of(window.samples.begin(), samples, [&](const CompositeSample& sample) { return sample.address == ui; })) return {};
    if (!std::exchange(window.uiWritten, false)) return {0, 0, {}, "not drawn since the last composite"};
    return {ui, window.uiBytes, window.uiExtent, nullptr};
}

VkImageBlit blitRegion(const AspectFitBlit& blit) {
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[0] = {blit.source.x, blit.source.y, 0};
    region.srcOffsets[1] = {blit.source.x + static_cast<std::int32_t>(blit.source.width), blit.source.y + static_cast<std::int32_t>(blit.source.height), 1};
    region.dstOffsets[0] = {blit.destination.x, blit.destination.y, 0};
    region.dstOffsets[1] = {blit.destination.x + static_cast<std::int32_t>(blit.destination.width), blit.destination.y + static_cast<std::int32_t>(blit.destination.height), 1};
    return region;
}

// Records the move into the active recorder (outside probeMutex: the blit marks the image written,
// which notes the write here), counts it and prints the first move and each reason a move was
// refused once.
void moveUiTarget(UiMove move) {
    AspectFitRect rect{};
    std::uint32_t blitCount = 0;
    // The margins: 's' edge stretch (APS5_NATIVE_ASPECT_UI_EDGE_STRETCH), 'c' filled with the UI
    // target's clear colour, 't' transparent black (no clear colour seen).
    char margins = 's';
    VkClearColorValue fill{};
    if (move.refusal == nullptr) {
        try {
            const auto aspect = PresentationAspect_nid_postfix();
            if (aspect.width == 0 || aspect.height == 0) {
                move.refusal = "no presentation aspect";
            } else if (auto image = StorageTexture::FindPending(move.address, move.bytes); image == nullptr && (image = StorageTexture::FindLive(move.address, move.bytes)) == nullptr) {
                move.refusal = "no image";
            } else if (image->Descriptor().width != move.extent.width || image->Descriptor().height != move.extent.height) {
                move.refusal = "image extent";
            } else {
                rect = ComputeUndistortedRect_nid_postfix(move.extent.width, move.extent.height, aspect.width, aspect.height);
                std::array<AspectFitBlit, 3> blits{};
                blitCount = ComputeRectMoveBlits_nid_postfix(move.extent.width, move.extent.height, rect, blits.data());
                if (blitCount == 0) {
                    move.refusal = "whole target";
                } else {
                    std::array<VkImageBlit, 3> regions{};
                    std::array<VkFilter, 3> filters{};
                    for (std::uint32_t index = 0; index < blitCount; ++index) {
                        regions[index] = blitRegion(blits[index]);
                        filters[index] = blits[index].nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                    }
                    // The margin blits (all after the first) carry the fill once the copy is cleared to it.
                    if (!uiEdgeStretchOn()) margins = image->LastClearColor(fill) ? 'c' : 't';
                    static_cast<void>(image->BlitSelf(std::span<const VkImageBlit>(regions.data(), blitCount), std::span<const VkFilter>(filters.data(), blitCount), move.refusal, margins == 's' ? nullptr : &fill, 1));
                }
            }
        } catch (const std::exception&) {
            move.refusal = "rect";
        }
    }
    static auto* reported = new std::set<std::string>();
    bool first = false;
    {
        std::lock_guard lock(probeMutex());
        auto& window = displayWindow();
        if (move.refusal == nullptr) {
            ++window.moved;
            if (margins == 'c') ++window.marginsCleared;
            else if (margins == 't') ++window.marginsTransparent;
        } else {
            noteMoveRefusal(window, move.refusal);
        }
        first = reported->insert(move.refusal == nullptr ? std::string(1, margins) : std::string(move.refusal)).second;
    }
    if (!first) return;
    if (move.refusal == nullptr) {
        char how[160];
        if (margins == 's') std::snprintf(how, sizeof(how), "its edge columns or rows stretched over the margins, APS5_NATIVE_ASPECT_UI_EDGE_STRETCH");
        else if (margins == 'c') std::snprintf(how, sizeof(how), "the margins filled with its clear colour %g,%g,%g,%g", fill.float32[0], fill.float32[1], fill.float32[2], fill.float32[3]);
        else std::snprintf(how, sizeof(how), "the margins transparent black, no clear colour seen");
        std::fprintf(stderr, "[aspect] UI move (APS5_NATIVE_ASPECT_UI_BLIT): UI target 0x%llx %ux%u moved into %d,%d %ux%u before the display buffer's composite (%u blits: the target linear, %s)\n",
            static_cast<unsigned long long>(move.address), move.extent.width, move.extent.height, rect.x, rect.y, rect.width, rect.height, blitCount, how);
    } else {
        std::fprintf(stderr, "[aspect] UI move (APS5_NATIVE_ASPECT_UI_BLIT): UI target 0x%llx %ux%u not moved: %s\n", static_cast<unsigned long long>(uiTarget().load(std::memory_order_relaxed)), move.extent.width, move.extent.height, move.refusal);
    }
    std::fflush(stderr);
}

std::string displayWriteDigest() {
    DisplayWriteWindow window;
    {
        std::lock_guard lock(probeMutex());
        auto& live = displayWindow();
        window = live;
        live.presents = live.frameWork = live.draws = live.dispatches = live.uiDraws = live.uiDispatches = live.remapped = live.unlisted = 0;
        live.writes.clear();
        live.phaseWrites = live.uiPhases = live.uiFragCoord = live.moved = live.marginsCleared = live.marginsTransparent = live.notMoved = live.phaseUnlisted = 0;
        live.refusals = {};
        live.phaseTargets.clear();
        live.uiSamples.clear();
    }
    std::sort(window.writes.begin(), window.writes.end(), [](const auto& left, const auto& right) { return left.firstRank < right.firstRank; });
    char text[320];
    std::snprintf(text, sizeof(text), "; display buffer writes (APS5_ASPECT_PROBE): %llu presents, %.0f draws+dispatches per frame, %llu draws, %llu dispatches, %llu unlisted; buffers",
        static_cast<unsigned long long>(window.presents), window.presents != 0 ? static_cast<double>(window.frameWork) / static_cast<double>(window.presents) : 0.0,
        static_cast<unsigned long long>(window.draws), static_cast<unsigned long long>(window.dispatches), static_cast<unsigned long long>(window.unlisted));
    std::string result = text;
    for (const auto buffer : window.buffers) {
        if (buffer == 0) continue;
        std::snprintf(text, sizeof(text), " 0x%llx", static_cast<unsigned long long>(buffer));
        result += text;
    }
    result += "; composite samples";
    for (std::size_t index = 0; index < window.sampleCount; ++index) {
        const auto& sample = window.samples[index];
        std::snprintf(text, sizeof(text), " 0x%llx %ux%u f%u", static_cast<unsigned long long>(sample.address), sample.width, sample.height, sample.format);
        result += text;
    }
    std::snprintf(text, sizeof(text), " (%zu at display size); UI target 0x%llx: %llu draws, %llu dispatches, %llu remapped (APS5_NATIVE_ASPECT_UI_RECT %s)", window.displaySized,
        static_cast<unsigned long long>(uiTarget().load(std::memory_order_relaxed)), static_cast<unsigned long long>(window.uiDraws), static_cast<unsigned long long>(window.uiDispatches),
        static_cast<unsigned long long>(window.remapped), uiRectOn() ? "on" : "off");
    result += text;
    // The UI phase: the moves, the UI draws' FragCoord reads, every colour target written, the
    // textures the UI draws sample (those of the UI target's extent, and those a draw of the phase
    // wrote, flagged).
    std::snprintf(text, sizeof(text), "; UI move (APS5_NATIVE_ASPECT_UI_BLIT %s): %llu moved, %llu not moved", uiBlitOn() ? "on" : "off", static_cast<unsigned long long>(window.moved),
        static_cast<unsigned long long>(window.notMoved));
    result += text;
    for (const auto& refusal : window.refusals) {
        if (refusal.why == nullptr) break;
        std::snprintf(text, sizeof(text), " (%s: %llu)", refusal.why, static_cast<unsigned long long>(refusal.count));
        result += text;
    }
    std::snprintf(text, sizeof(text), "; UI margins (APS5_NATIVE_ASPECT_UI_EDGE_STRETCH %s): %llu the UI target's clear colour, %llu transparent black", uiEdgeStretchOn() ? "on" : "off",
        static_cast<unsigned long long>(window.marginsCleared), static_cast<unsigned long long>(window.marginsTransparent));
    result += text;
    std::snprintf(text, sizeof(text), "; UI draws reading FragCoord %llu of %llu; UI phase in %llu frames, %llu other storage writes of the UI extent in it; targets written", static_cast<unsigned long long>(window.uiFragCoord),
        static_cast<unsigned long long>(window.uiDraws), static_cast<unsigned long long>(window.uiPhases), static_cast<unsigned long long>(window.phaseWrites));
    result += text;
    for (const auto& target : window.phaseTargets) {
        std::snprintf(text, sizeof(text), " 0x%llx %ux%u vkf%d %llu draws (%llu blended", static_cast<unsigned long long>(target.address), target.extent.width, target.extent.height, static_cast<int>(target.format),
            static_cast<unsigned long long>(target.draws), static_cast<unsigned long long>(target.blended));
        result += text;
        for (std::size_t code = 0; code < RemapCodes.size(); ++code) {
            if (target.remaps[code] == 0) continue;
            std::snprintf(text, sizeof(text), ", %s %llu", remapName(RemapCodes[code]), static_cast<unsigned long long>(target.remaps[code]));
            result += text;
        }
        result += ")";
    }
    if (window.phaseUnlisted != 0) {
        std::snprintf(text, sizeof(text), " +%llu draws unlisted", static_cast<unsigned long long>(window.phaseUnlisted));
        result += text;
    }
    result += "; UI draws sample";
    for (const auto& sample : window.uiSamples) {
        const bool uiSized = sample.width == window.uiExtent.width && sample.height == window.uiExtent.height;
        std::snprintf(text, sizeof(text), " 0x%llx %ux%u f%u (%llu)%s%s", static_cast<unsigned long long>(sample.address), sample.width, sample.height, sample.format, static_cast<unsigned long long>(sample.draws),
            uiSized ? " UI-sized" : "", phaseTargetNoted(window, sample.address) ? " written in the UI phase" : "");
        result += text;
    }
    for (const auto& entry : window.writes) {
        if (entry.kind == 'D' || entry.kind == 'U') {
            std::snprintf(text, sizeof(text), "; #%u-%u @%llu-%llu %s fs %016llx %ux%u viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %ux%u blend %s (%llu)", entry.firstRank, entry.lastRank,
                static_cast<unsigned long long>(entry.firstWork), static_cast<unsigned long long>(entry.lastWork), entry.kind == 'D' ? "draw" : "UI draw", static_cast<unsigned long long>(entry.program),
                entry.extent.width, entry.extent.height, entry.viewport.x, entry.viewport.y, entry.viewport.width, entry.viewport.height, entry.scissor.offset.x, entry.scissor.offset.y,
                entry.scissor.extent.width, entry.scissor.extent.height, entry.blend ? "on" : "off", static_cast<unsigned long long>(entry.count));
            if (entry.kind == 'U') {
                result += text;
                std::snprintf(text, sizeof(text), " fragcoord %s, %s", entry.fragCoord ? "yes" : "no", remapName(entry.remap));
            }
        } else if (entry.kind == 'W' || entry.kind == 'X') {
            std::snprintf(text, sizeof(text), "; #%u-%u @%llu-%llu UI-phase %s cs 0x%llx writes 0x%llx %ux%u (%llu)", entry.firstRank, entry.lastRank, static_cast<unsigned long long>(entry.firstWork),
                static_cast<unsigned long long>(entry.lastWork), entry.kind == 'W' ? "dispatch" : "copy/clear", static_cast<unsigned long long>(entry.program), static_cast<unsigned long long>(entry.address),
                entry.extent.width, entry.extent.height, static_cast<unsigned long long>(entry.count));
        } else {
            std::snprintf(text, sizeof(text), "; #%u-%u @%llu-%llu %s cs 0x%llx storage %ux%u (%llu)", entry.firstRank, entry.lastRank, static_cast<unsigned long long>(entry.firstWork),
                static_cast<unsigned long long>(entry.lastWork), entry.kind == 'C' ? "dispatch" : "UI dispatch", static_cast<unsigned long long>(entry.program), entry.extent.width, entry.extent.height,
                static_cast<unsigned long long>(entry.count));
        }
        result += text;
    }
    return result;
}

}

void NoteDepthViewport(const State& state) {
    if (!depthProbeOn() || !state.depth) return;
    const auto extent = state.depth->extent;
    const auto area = static_cast<std::uint64_t>(extent.width) * extent.height;
    std::lock_guard lock(probeMutex());
    auto& window = probeWindow();
    const auto largest = static_cast<std::uint64_t>(window.extent.width) * window.extent.height;
    if (area < largest || (area == largest && extent.width != window.extent.width)) return;
    if (area > largest) {
        window = {};
        window.extent = extent;
    }
    ++window.draws;
    for (auto& entry : window.viewports) {
        if (std::memcmp(&entry.viewport, &state.viewport, sizeof(VkViewport)) == 0 && std::memcmp(&entry.scissor, &state.scissor, sizeof(VkRect2D)) == 0) {
            ++entry.draws;
            return;
        }
    }
    if (window.viewports.size() < 4) window.viewports.push_back({state.viewport, state.scissor, 1});
}

std::string DepthViewportDigest() {
    if (!depthProbeOn()) return {};
    DepthViewportWindow window;
    {
        std::lock_guard lock(probeMutex());
        window = probeWindow();
        probeWindow() = {};
    }
    char text[192];
    std::snprintf(text, sizeof(text), "; depth viewports (APS5_ASPECT_PROBE): largest surface %ux%u, %llu draws", window.extent.width, window.extent.height, static_cast<unsigned long long>(window.draws));
    std::string result = text;
    for (const auto& entry : window.viewports) {
        std::snprintf(text, sizeof(text), "; viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %ux%u (%llu draws)", entry.viewport.x, entry.viewport.y, entry.viewport.width, entry.viewport.height,
            entry.scissor.offset.x, entry.scissor.offset.y, entry.scissor.extent.width, entry.scissor.extent.height, static_cast<unsigned long long>(entry.draws));
        result += text;
    }
    return result + displayWriteDigest();
}

void NoteDisplayBufferPresent(std::uint64_t address) {
    if (!watchOn() || address == 0) return;
    std::lock_guard lock(probeMutex());
    auto& window = displayWindow();
    if (!isDisplayBuffer(window, address)) window.buffers[window.nextBuffer++ % window.buffers.size()] = address;
    ++window.presents;
    window.frameWork += window.work;
    window.work = 0;
    window.rank = 0;
    window.uiPhase = false;
}

void NoteDisplayBufferDraw(const State& state, std::span<const CompiledShader> shaders) {
    if (!watchOn()) return;
    RecordCensus::Touch(RecordCensus::UiProbe);
    const ShaderRecompiler::RecompileResult* fragment = nullptr;
    for (const auto& shader : shaders) {
        if (shader.stage == ShaderRecompiler::ShaderStage::Fragment && shader.program != nullptr) fragment = shader.program;
    }
    const std::uint64_t program = fragment != nullptr ? fragment->variantId : 0;
    AspectFitRect rect{};
    VkExtent2D extent{};
    const char remap = uiRemap(state, rect, extent);
    UiMove move{};
    {
        std::lock_guard lock(probeMutex());
        auto& window = displayWindow();
        ++window.work;
        if (remap == 'r') ++window.remapped;
        const auto ui = uiTarget().load(std::memory_order_relaxed);
        const auto targets = state.colors.empty() ? (state.hasColorTarget ? 1u : 0u) : state.colors.size();
        bool uiNoted = false;
        for (std::size_t slot = 0; slot < targets; ++slot) {
            const auto& color = state.colors.empty() ? state.color : state.colors[slot];
            const auto& blend = slot < state.blends.size() ? state.blends[slot] : state.blend;
            const bool blended = blend.blendEnable == VK_TRUE;
            if (!uiNoted && (isDisplayBuffer(window, color.address) || isDisplayBuffer(window, color.surfaceAddress))) {
                learnComposite(window, shaders, color.extent);
                noteDisplayWrite(window, {'D', program, state.viewport, state.scissor, blended, color.extent});
                // The composite ends the UI phase; the UI target drawn in it moves before the
                // composite reads it.
                window.uiPhase = false;
                if (uiBlitOn()) move = takeUiMove(window);
                break;
            }
            if (!uiNoted && ui != 0 && color.address == ui) {
                if (!window.uiPhase) ++window.uiPhases;
                window.uiPhase = true;
                window.uiWritten = true;
                window.uiBytes = color.bytes;
                window.uiExtent = color.extent;
                const bool fragCoord = depthProbeOn() && fragment != nullptr && readsFragCoord(*fragment);
                if (fragCoord) ++window.uiFragCoord;
                noteDisplayWrite(window, {'U', 0, state.viewport, state.scissor, blended, color.extent, fragCoord, remap});
                if (depthProbeOn()) noteUiSamples(window, shaders);
                uiNoted = true;
            }
            if (window.uiPhase && depthProbeOn()) notePhaseTarget(window, color, blended, ui != 0 && color.address == ui ? remap : '-');
        }
    }
    if (move.address != 0 || move.refusal != nullptr) moveUiTarget(move);
}

void NoteDisplayBufferStorageWrite(std::uint64_t address, std::uint32_t width, std::uint32_t height) {
    if (!depthProbeOn()) return;
    auto& subject = probeDispatchSubject();
    std::lock_guard lock(probeMutex());
    auto& window = displayWindow();
    const bool ui = address == uiTarget().load(std::memory_order_relaxed);
    const bool display = isDisplayBuffer(window, address);
    const bool uiExtent = window.uiPhase && width == window.uiExtent.width && height == window.uiExtent.height;
    if (!subject.active) {
        // Outside a dispatch: a draw's target (noted with the draw), the UI move, a copy, a clear or
        // a fill. Only the last three of other images of the UI target's extent are listed.
        if (ui || display || !uiExtent || phaseTargetNoted(window, address)) return;
        noteDisplayWrite(window, {'X', 0, VkViewport{}, VkRect2D{}, false, VkExtent2D{width, height}, false, 0, address});
        return;
    }
    if (!ui && !display && !uiExtent) return;
    const auto noted = subject.noted.begin() + static_cast<std::ptrdiff_t>(subject.notedCount);
    if (std::find(subject.noted.begin(), noted, address) != noted) return;
    if (subject.notedCount < subject.noted.size()) subject.noted[subject.notedCount++] = address;
    noteDisplayWrite(window, {ui ? 'V' : display ? 'C' : 'W', subject.program, VkViewport{}, VkRect2D{}, false, VkExtent2D{width, height}, false, 0, ui || display ? 0 : address});
}

VkViewport UiRectViewport(const State& state) {
    AspectFitRect rect{};
    VkExtent2D extent{};
    if (!uiRect(state, rect, extent)) return state.viewport;
    const auto scaleX = static_cast<float>(rect.width) / static_cast<float>(extent.width);
    const auto scaleY = static_cast<float>(rect.height) / static_cast<float>(extent.height);
    auto viewport = state.viewport;
    viewport.x = static_cast<float>(rect.x) + viewport.x * scaleX;
    viewport.y = static_cast<float>(rect.y) + viewport.y * scaleY;
    viewport.width *= scaleX;
    viewport.height *= scaleY;
    return viewport;
}

VkRect2D UiRectScissor(const State& state) {
    AspectFitRect rect{};
    VkExtent2D extent{};
    if (!uiRect(state, rect, extent)) return state.scissor;
    const auto mapped = MapRectInto_nid_postfix({state.scissor.offset.x, state.scissor.offset.y, state.scissor.extent.width, state.scissor.extent.height}, extent.width, extent.height, rect);
    return VkRect2D{{mapped.x, mapped.y}, {mapped.width, mapped.height}};
}

AspectProbeDispatch::AspectProbeDispatch(std::uint64_t program) {
    if (!depthProbeOn()) return;
    probeDispatchSubject() = {true, program, {}, 0};
    std::lock_guard lock(probeMutex());
    ++displayWindow().work;
}

AspectProbeDispatch::~AspectProbeDispatch() {
    probeDispatchSubject().active = false;
}

}
