#include "prx/libSceAgcDriver/Graphics/include/Upscale.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastLayouts.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/shaders/UpscaleBilinear_spv.h"
#include "prx/libSceAgcDriver/Graphics/shaders/UpscaleTemporal_spv.h"
#include <array>

namespace AgcDriver::Graphics {

namespace {

// The record, as RecordFastDispatch's: the leading barrier (unless the last trailing one covers it),
// the dispatch timed under `key`, the trailing barrier. The caller keeps the images and marks the output.
void recordPass(const Context& context, VkCommandBuffer commands, VkAccessFlags covered, VkPipeline pipeline, const FastLayout& layout, std::span<const VkWriteDescriptorSet> writes, const void* push, std::uint32_t pushBytes, const std::array<std::uint32_t, 2>& groups, std::uint64_t key) {
    auto& recorder = *context.recorder;
    using CommandClass = Recorder::CommandClass;
    constexpr VkAccessFlags shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if ((covered & shaderAccess) == shaderAccess && Recorder::MergeBarriers()) {
        Recorder::CountMerged(CommandClass::DispatchLeading);
    } else {
        const auto leadTiming = recorder.BeginGpuTiming(CommandClass::DispatchLeading);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, shaderAccess);
        Recorder::CountBarriers(CommandClass::DispatchLeading);
        recorder.EndGpuTiming(leadTiming);
    }
    // Ahead of the bind: BeginGpuTiming may record queued copy-backs, which bind a pipeline of their own.
    const auto gpuTiming = recorder.BeginGpuTiming(key);
    context.Resolved(&DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    PushDescriptors(context, commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, writes);
    context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, layout.pipeline, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
    context.Resolved(&DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, groups[0], groups[1], 1);
    recorder.EndGpuTiming(gpuTiming);
    constexpr VkAccessFlags dispatchedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    const auto trailingTiming = recorder.BeginGpuTiming(CommandClass::DispatchTrailing);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, dispatchedAccess);
    Recorder::CountBarriers(CommandClass::DispatchTrailing);
    recorder.EndGpuTiming(trailingTiming);
    recorder.MarkCovered(dispatchedAccess);
}

}

BilinearUpscaler::BilinearUpscaler(const Context& context) : context(context) {
    // Kept for the device's lifetime: it must not keep the buffer pool alive (as FastLayouts).
    this->context.bufferPool.reset();
    // Binding 0: MainColor with the sampler; 1: the output. FastLayouts owns the layouts.
    constexpr std::uint32_t compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const std::array<std::uint32_t, 8> key{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, compute, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, compute};
    layout = context.fastLayouts != nullptr ? context.fastLayouts->Get(key, compute) : nullptr;
    if (layout == nullptr) return;
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler upscale");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(UPSCALE_BILINEAR_SPV);
        moduleInfo.pCode = UPSCALE_BILINEAR_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule upscale");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout->pipeline;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines upscale");
    } catch (...) {
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        release();
        throw;
    }
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
}

BilinearUpscaler::~BilinearUpscaler() {
    release();
}

void BilinearUpscaler::release() noexcept {
    if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    if (sampler) context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, sampler, nullptr);
}

std::optional<Upscale::Skip> BilinearUpscaler::Record(const Context& context, const Upscale::Plan& plan) {
    using Skip = Upscale::Skip;
    if (pipeline == VK_NULL_HANDLE) return Skip::NoPushDescriptors;
    Require(context.recorder != nullptr, "the upscale pass needs the device's recorder");
    // MainColor filtered linearly; the output a float or normalized format whose storage view is the
    // format itself (an sRGB one's would store without its encoding).
    const auto filters = [&](std::uint32_t guestFormat) {
        VkFormatProperties properties{};
        context.formatProperties(context.physical, ResolveTextureFormat(guestFormat), &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    };
    if (!filters(plan.color.format)) return Skip::ColorFormat;
    if (!StorageFormatAvailable(context, plan.output.format) || !filters(plan.output.format) || StorageFormatForGuest(context, plan.output.format) != ResolveTextureFormat(plan.output.format)) return Skip::OutputFormat;
    // 1. The lookups, as RecordFastDispatch's: they may record uploads, snapshots and flushes, so they
    // come before the command buffer is taken.
    bool firstLayer = false, undecodable = false;
    auto color = FastSampledTexture(context, plan.colorWords, ShaderRecompiler::DescriptorImageShape::Image2D, false, firstLayer, undecodable);
    if (color == nullptr) return Skip::ColorUnresolved;
    ShaderRecompiler::DescriptorBinding target{ShaderRecompiler::DescriptorKind::StorageImage, ShaderRecompiler::DescriptorRole::GuestImages, 0, 1, 1, {plan.outputWords.begin(), plan.outputWords.end()}};
    target.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    target.imageWritten = {true};
    const auto output = ResolveStorageImage(context, target, 0, nullptr);
    const std::array<VkDescriptorImageInfo, 2> images{{{sampler, firstLayer ? color->FirstLayerView() : color->View(), color->Layout()},
        {VK_NULL_HANDLE, output.firstLayer ? output.image->FirstLayerView(output.mip) : output.image->View(output.mip), VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, 0, 1, binding == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[binding], nullptr, nullptr};
    }
    // UpscaleBilinear.comp's Parameters (std430).
    struct Push {
        std::uint32_t width, height;
        std::array<float, 2> scale, low, high;
    };
    const Push push{plan.output.width, plan.output.height, plan.scale, plan.low, plan.high};
    // 2. The record (recordPass, under TimingKey), then the output's mark.
    auto& recorder = *context.recorder;
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered, Recorder::CommandSite::Dispatch);
    recorder.Keep(color);
    recorder.Keep(output.image);
    recordPass(context, commands, covered, pipeline, *layout, writes, &push, static_cast<std::uint32_t>(sizeof(push)), plan.groups, Upscale::TimingKey);
    // As MarkGpuWrites marks a written storage image: its results pending over its surface. Nothing is
    // bound in place, so there are no pending reads, pending writes or written ranges to note.
    output.image->MarkDirty();
    return std::nullopt;
}

// A device-local RGBA16F image the temporal pass samples and stores (layout GENERAL after its first
// pass's barrier), with its view.
struct TemporalUpscaler::HistoryImage {
    HistoryImage(const Context& owner, std::uint32_t width, std::uint32_t height) : context(owner), size{width, height} {
        context.bufferPool.reset();
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            info.extent = {width, height, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage upscale history");
            VkMemoryRequirements requirements{};
            context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(AllocateDeviceMemory(owner, allocation, &memory, VramClass::Targets), "vkAllocateMemory upscale history");
            Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory upscale history");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = info.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView upscale history");
        } catch (...) {
            release();
            throw;
        }
    }
    ~HistoryImage() { release(); }
    HistoryImage(const HistoryImage&) = delete;
    HistoryImage& operator=(const HistoryImage&) = delete;
    void release() noexcept {
        if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) FreeDeviceMemory(context, memory);
    }
    Context context;
    std::array<std::uint32_t, 2> size;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

TemporalUpscaler::TemporalUpscaler(const Context& context) : context(context) {
    this->context.bufferPool.reset();
    // Bindings 0-2: MainColor, Motion, MainDepth (fetched); 3: the last history (filtered); 4: the next
    // history; 5: the output.
    constexpr std::uint32_t compute = VK_SHADER_STAGE_COMPUTE_BIT, sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const std::array<std::uint32_t, 24> key{0, sampled, 1, compute, 1, sampled, 1, compute, 2, sampled, 1, compute, 3, sampled, 1, compute, 4, storage, 1, compute, 5, storage, 1, compute};
    layout = context.fastLayouts != nullptr ? context.fastLayouts->Get(key, compute) : nullptr;
    if (layout == nullptr) return;
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &nearest), "vkCreateSampler upscale");
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &linear), "vkCreateSampler upscale");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(UPSCALE_TEMPORAL_SPV);
        moduleInfo.pCode = UPSCALE_TEMPORAL_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule upscale");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout->pipeline;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines upscale");
    } catch (...) {
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        release();
        throw;
    }
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
}

TemporalUpscaler::~TemporalUpscaler() {
    release();
}

void TemporalUpscaler::release() noexcept {
    history = {};
    if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    if (linear) context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, linear, nullptr);
    if (nearest) context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, nearest, nullptr);
}

std::optional<Upscale::Skip> TemporalUpscaler::Record(const Context& context, const Upscale::TemporalPlan& plan) {
    using Skip = Upscale::Skip;
    using ShaderRecompiler::DescriptorImageShape;
    if (pipeline == VK_NULL_HANDLE) return Skip::NoPushDescriptors;
    Require(context.recorder != nullptr, "the upscale pass needs the device's recorder");
    // The inputs are fetched, not filtered; the output as the bilinear pass needs it.
    const auto outputFormat = plan.base.output.format;
    if (!StorageFormatAvailable(context, outputFormat) || StorageFormatForGuest(context, outputFormat) != ResolveTextureFormat(outputFormat)) return Skip::OutputFormat;
    // 1. The lookups (BilinearUpscaler::Record's), then the history images: made again when the
    // output size changes, the old ones kept by the recorder for the batches still reading them.
    bool colorFirst = false, motionFirst = false, depthFirst = false, undecodable = false;
    auto color = FastSampledTexture(context, plan.base.colorWords, DescriptorImageShape::Image2D, false, colorFirst, undecodable);
    if (color == nullptr) return Skip::ColorUnresolved;
    auto motion = FastSampledTexture(context, plan.motionWords, DescriptorImageShape::Image2D, false, motionFirst, undecodable);
    auto depth = FastSampledTexture(context, plan.depthWords, DescriptorImageShape::Image2D, false, depthFirst, undecodable);
    if (motion == nullptr || depth == nullptr) return Skip::InputUnresolved;
    ShaderRecompiler::DescriptorBinding target{ShaderRecompiler::DescriptorKind::StorageImage, ShaderRecompiler::DescriptorRole::GuestImages, 0, 1, 1, {plan.base.outputWords.begin(), plan.base.outputWords.end()}};
    target.imageShape = DescriptorImageShape::Image2D;
    target.imageWritten = {true};
    const auto output = ResolveStorageImage(context, target, 0, nullptr);
    auto& recorder = *context.recorder;
    const std::array<std::uint32_t, 2> size{plan.base.output.width, plan.base.output.height};
    const bool fresh = history[0] == nullptr || history[0]->size != size;
    if (fresh) {
        try {
            for (auto& image : history) {
                if (image != nullptr) recorder.Keep(std::move(image));
                image = std::make_shared<HistoryImage>(context, size[0], size[1]);
            }
        } catch (const std::exception&) {
            history = {};
            return Skip::History;
        }
        newest = 0;
    }
    const std::array<VkDescriptorImageInfo, 6> images{{{nearest, colorFirst ? color->FirstLayerView() : color->View(), color->Layout()},
        {nearest, motionFirst ? motion->FirstLayerView() : motion->View(), motion->Layout()}, {nearest, depthFirst ? depth->FirstLayerView() : depth->View(), depth->Layout()},
        {linear, history[newest]->view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, history[newest ^ 1]->view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, output.firstLayer ? output.image->FirstLayerView(output.mip) : output.image->View(output.mip), VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, 0, 1, binding < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[binding], nullptr, nullptr};
    }
    // UpscaleTemporal.comp's Parameters (std430); exposure 1.0 (+0x118 is 1.0 so far).
    struct Push {
        std::uint32_t width, height, renderWidth, renderHeight;
        std::array<float, 2> jitter, motionScale;
        float exposure, currentWeight;
        std::uint32_t flags;
    };
    const Push push{size[0], size[1], plan.base.render[0], plan.base.render[1], plan.jitter, plan.motionScale, 1.0f, Upscale::PortCurrentWeight, (plan.reset || fresh ? 1u : 0u) | (plan.smallestNearest ? 2u : 0u)};
    // 2. The command buffer (after the lookups and the images, which may record or reclaim). On a
    // reset the history images go from undefined to GENERAL (their content is not read), where they
    // stay: the passes' global barriers order their reads and writes. Then the record (recordPass,
    // under PortTimingKey) and the output's mark.
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered, Recorder::CommandSite::Dispatch);
    recorder.Keep(color);
    recorder.Keep(motion);
    recorder.Keep(depth);
    recorder.Keep(output.image);
    if (fresh || plan.reset) {
        std::array<VkImageMemoryBarrier, 2> barriers{};
        for (std::size_t index = 0; index < barriers.size(); ++index) {
            barriers[index] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, history[index]->image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        }
        context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()), barriers.data());
    }
    recordPass(context, commands, covered, pipeline, *layout, writes, &push, static_cast<std::uint32_t>(sizeof(push)), plan.base.groups, Upscale::PortTimingKey);
    output.image->MarkDirty();
    newest ^= 1;
    return std::nullopt;
}

}
