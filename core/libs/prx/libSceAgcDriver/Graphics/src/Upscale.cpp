#include "prx/libSceAgcDriver/Graphics/include/Upscale.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastLayouts.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/shaders/UpscaleBilinear_spv.h"
#include <array>

namespace AgcDriver::Graphics {

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
    // 2. The record, as RecordFastDispatch's: the leading barrier (unless the last trailing one
    // covers it), the dispatch timed under TimingKey, the trailing barrier, the output's mark.
    auto& recorder = *context.recorder;
    VkAccessFlags covered = 0;
    const auto commands = recorder.Commands(&covered, Recorder::CommandSite::Dispatch);
    recorder.Keep(color);
    recorder.Keep(output.image);
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
    const auto gpuTiming = recorder.BeginGpuTiming(Upscale::TimingKey);
    context.Resolved(&DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    PushDescriptors(context, commands, VK_PIPELINE_BIND_POINT_COMPUTE, *layout, writes);
    context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, layout->pipeline, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<std::uint32_t>(sizeof(push)), &push);
    context.Resolved(&DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, plan.groups[0], plan.groups[1], 1);
    recorder.EndGpuTiming(gpuTiming);
    constexpr VkAccessFlags dispatchedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    const auto trailingTiming = recorder.BeginGpuTiming(CommandClass::DispatchTrailing);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, dispatchedAccess);
    Recorder::CountBarriers(CommandClass::DispatchTrailing);
    recorder.EndGpuTiming(trailingTiming);
    recorder.MarkCovered(dispatchedAccess);
    // As MarkGpuWrites marks a written storage image: its results pending over its surface. Nothing is
    // bound in place, so there are no pending reads, pending writes or written ranges to note.
    output.image->MarkDirty();
    return std::nullopt;
}

}
