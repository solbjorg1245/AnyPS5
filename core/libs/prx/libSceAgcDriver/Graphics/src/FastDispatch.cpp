#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastRing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

using Decline = FastDispatchDecline;
using Role = ShaderRecompiler::DescriptorRole;
using Kind = ShaderRecompiler::DescriptorKind;

// A guest buffer element to bind in place: its range, the buffer info it fills, where its offset
// adjustment goes (a push constant byte, else a byte of the shader data words), whether the shader
// may store to it.
struct BufferElement {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::size_t info = 0;
    std::uint32_t adjustmentByte = 0;
    bool written = false;
};

// A data binding (flattened SRT or shader data words) to place in the ring.
struct DataBinding {
    std::size_t info = 0;
    std::size_t binding = 0;
    bool shaderData = false;
    std::byte* mapped = nullptr;
};

// Per-thread scratch: nothing a fast dispatch resolves allocates once the vectors have grown.
struct Scratch {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<BufferElement> elements;
    std::vector<DataBinding> data;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    std::vector<std::shared_ptr<void>> keep;
    std::vector<StorageTexture*> dirty;
    std::vector<StorageImageElement> storage;
    // (byte, adjustment) patches of the shader data words.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dataPatches;
};
struct ScratchTag {};

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

bool overlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t begin, std::uint64_t end) {
    return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return begin < range.second && range.first < end; });
}

// APS5_ALL_BUFFERS_WRITTEN=1: every element counts as written, as in ShaderResources::addGuestBuffer.
bool allBuffersWritten() {
    static const bool all = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    return all;
}

// A guest buffer element's checks that need neither the lock nor an import, a function of its V#
// words and the configuration (FastDispatchPrecheck before the lock, step 1 again under it): the
// adjustment slot ShaderResources requires of every element of a shader with push constants
// ("guest buffer offset lies outside the shader's push constants"), the reserved bit and type a
// build rejects, the range limit, a written element a build would stage. An empty V# leaves
// `element.bytes` 0: the device's placeholder binds.
std::optional<Decline> checkBufferElement(const Context& context, const ShaderRecompiler::RecompileResult& shader, const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t index, BufferElement& element) {
    element = {};
    element.adjustmentByte = shader.memoryOffsetDword * 4u + index;
    if (!shader.pushConstants.empty() && element.adjustmentByte >= shader.pushConstants.size()) return Decline::Invalid;
    const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(index) * 4u, 4u);
    if ((words[1] & 0x40000000u) != 0) return Decline::Invalid;
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
    if (descriptor.Type() != 0u) return Decline::Invalid;
    const auto address = descriptor.Base48();
    const auto bytes = descriptor.GetSize();
    if (bytes == 0 || address == 0) {
        if (context.emptyBuffer == VK_NULL_HANDLE) return Decline::Invalid;
        return std::nullopt;
    }
    if (bytes > context.limits.maxStorageBufferRange || bytes > std::numeric_limits<std::uint64_t>::max() - address) return Decline::Invalid;
    // An element the recompiler did not classify counts as written.
    element.written = index >= binding.bufferWritten.size() || binding.bufferWritten[index] || allBuffersWritten();
    const bool atomic = index < binding.bufferAtomic.size() && binding.bufferAtomic[index];
    if (element.written && DeviceStagingWanted(bytes, atomic)) return Decline::Staged;
    element.address = address;
    element.bytes = bytes;
    return std::nullopt;
}

}

std::optional<FastDispatchDecline> FastComputeLayoutKey(const ShaderRecompiler::RecompileResult& shader, std::vector<std::uint32_t>& key) {
    key.clear();
    const VkShaderStageFlags flags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (const auto& binding : shader.bindings) {
        if (binding.role == Role::BdaPagetable || binding.role == Role::FaultBuffer) return Decline::Bda;
        if (binding.descriptorSet != 0 || binding.count == 0) return Decline::Invalid;
        for (std::size_t i = 0; i < key.size(); i += 4) {
            if (key[i] == binding.binding) return Decline::Invalid;
        }
        VkDescriptorType type;
        switch (binding.role) {
            case Role::GuestBuffers:
            case Role::ShaderData:
            case Role::FlattenedSrt:
                if (binding.kind != Kind::StorageBuffer || binding.readOnly) return Decline::Invalid;
                if (binding.role != Role::GuestBuffers && binding.count != 1) return Decline::Invalid;
                type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            case Role::GuestImages:
                if (binding.kind == Kind::SampledImage) type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                else if (binding.kind == Kind::StorageImage) type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                else return Decline::Invalid;
                break;
            case Role::GuestSamplers:
                if (binding.kind != Kind::Sampler) return Decline::Invalid;
                type = VK_DESCRIPTOR_TYPE_SAMPLER;
                break;
            default: return Decline::Role;
        }
        key.insert(key.end(), {binding.binding, static_cast<std::uint32_t>(type), binding.count, flags});
    }
    return std::nullopt;
}

std::optional<FastDispatchDecline> FastDispatchPrecheck(const Context& context, const ShaderRecompiler::RecompileResult& shader) {
    for (const auto& binding : shader.bindings) {
        if (binding.role == Role::GuestBuffers) {
            if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return Decline::Invalid;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                BufferElement checked;
                if (const auto decline = checkBufferElement(context, shader, binding, element, checked)) return decline;
            }
        } else if (binding.role == Role::ShaderData || binding.role == Role::FlattenedSrt) {
            if (binding.guestDescriptor.empty() || binding.guestDescriptor.size() * sizeof(std::uint32_t) > context.limits.maxStorageBufferRange) return Decline::Invalid;
            if (!binding.deferredWords.empty()) return Decline::Deferred;
        }
    }
    return std::nullopt;
}

std::optional<FastDispatchDecline> RecordFastDispatch(const Context& context, Recorder& recorder, const FastDispatchCall& call, FastDispatchTiming& timing) {
    const auto& shader = *call.shader;
    const auto& layout = *call.layout;
    const auto resolveStart = std::chrono::steady_clock::now();
    auto& scratch = HostThreadLocal<Scratch, ScratchTag>();
    scratch.writes.clear();
    scratch.buffers.clear();
    scratch.images.clear();
    scratch.elements.clear();
    scratch.data.clear();
    scratch.reads.clear();
    scratch.written.clear();
    scratch.keep.clear();
    scratch.dirty.clear();
    scratch.dataPatches.clear();
    // The images and samplers resolved are the batch's once recorded (moved out below); on every
    // other return they go now, not at this thread's next fast dispatch (VRAM held by an idle
    // thread, objects outliving their device).
    struct Release {
        Scratch& scratch;
        ~Release() {
            scratch.keep.clear();
            scratch.storage.clear();
        }
    } release{scratch};
    // The infos are sized up front: the writes point into them.
    std::size_t bufferCount = 0;
    std::size_t imageCount = 0;
    for (const auto& binding : shader.bindings) (binding.kind == Kind::StorageBuffer ? bufferCount : imageCount) += binding.count;
    scratch.buffers.reserve(bufferCount);
    scratch.images.reserve(imageCount);
    const std::array<CompiledShader, 1> shaders{{{ShaderRecompiler::ShaderStage::Compute, &shader, 0}}};
    auto pushBytes = AssemblePushConstants(shaders);
    const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 1);
    // 1. What needs no import: V# decode and checks, the storage results over in-place ranges
    // flushed (UploadFinish's rule), the image and sampler lookups (they may record uploads and wait
    // for recorded work, so they come before the ring region and the imports).
    try {
        for (std::size_t index = 0; index < shader.bindings.size(); ++index) {
            const auto& binding = shader.bindings[index];
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstBinding = binding.binding;
            write.descriptorCount = binding.count;
            if (binding.role == Role::GuestBuffers) {
                if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return Decline::Invalid;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = scratch.buffers.data() + scratch.buffers.size();
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    BufferElement resolved;
                    if (const auto decline = checkBufferElement(context, shader, binding, element, resolved)) return decline;
                    if (resolved.bytes == 0) {
                        // An empty V# binds the device's placeholder, as a build does.
                        scratch.buffers.push_back({context.emptyBuffer, 0, EmptyBufferBytes});
                        continue;
                    }
                    if (!GuestMemory::DescribeCommitted(resolved.address, static_cast<std::size_t>(resolved.bytes)).whole) return Decline::Sparse;
                    resolved.info = scratch.buffers.size();
                    scratch.elements.push_back(resolved);
                    scratch.buffers.push_back({});
                    scratch.reads.emplace_back(resolved.address, resolved.address + resolved.bytes);
                    if (resolved.written) scratch.written.emplace_back(resolved.address, resolved.address + resolved.bytes);
                }
            } else if (binding.role == Role::ShaderData || binding.role == Role::FlattenedSrt) {
                if (binding.guestDescriptor.empty() || binding.guestDescriptor.size() * sizeof(std::uint32_t) > context.limits.maxStorageBufferRange) return Decline::Invalid;
                if (!binding.deferredWords.empty()) return Decline::Deferred;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = scratch.buffers.data() + scratch.buffers.size();
                scratch.data.push_back({scratch.buffers.size(), index, binding.role == Role::ShaderData});
                scratch.buffers.push_back({});
            } else if (binding.role == Role::GuestImages && binding.kind == Kind::SampledImage) {
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                write.pImageInfo = scratch.images.data() + scratch.images.size();
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    bool firstLayer = false;
                    auto texture = ResolveSampledImage(context, binding, element, firstLayer);
                    scratch.images.push_back({VK_NULL_HANDLE, firstLayer ? texture->FirstLayerView() : texture->View(), texture->Layout()});
                    scratch.keep.push_back(std::move(texture));
                }
            } else if (binding.role == Role::GuestImages && binding.kind == Kind::StorageImage) {
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.pImageInfo = scratch.images.data() + scratch.images.size();
                scratch.storage.clear();
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    scratch.storage.push_back(ResolveStorageImage(context, binding, element, scratch.storage.empty() ? nullptr : &scratch.storage.back()));
                    auto& resolved = scratch.storage.back();
                    scratch.images.push_back({VK_NULL_HANDLE, resolved.firstLayer ? resolved.image->FirstLayerView(resolved.mip) : resolved.image->View(resolved.mip), VK_IMAGE_LAYOUT_GENERAL});
                    if (resolved.written) scratch.dirty.push_back(resolved.image.get());
                    scratch.keep.push_back(resolved.image);
                }
            } else if (binding.role == Role::GuestSamplers) {
                if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u || binding.samplerDepthCompare.size() != binding.count) return Decline::Invalid;
                write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                write.pImageInfo = scratch.images.data() + scratch.images.size();
                static const bool noSamplerCache = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4u, 4u);
                    const bool compareEnable = binding.samplerDepthCompare.at(element);
                    std::shared_ptr<Sampler> sampler;
                    if (context.samplerCache != nullptr && !noSamplerCache) {
                        sampler = context.samplerCache->Get(context, words, compareEnable);
                    } else {
                        auto resource = DecodeSamplerResource(words);
                        resource.compareEnable = compareEnable;
                        sampler = std::make_shared<Sampler>(context, resource);
                    }
                    scratch.images.push_back({sampler->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                    scratch.keep.push_back(std::move(sampler));
                }
            } else {
                return Decline::Role;
            }
            scratch.writes.push_back(write);
        }
        for (const auto& element : scratch.elements) StorageTexture::FlushPending(element.address, static_cast<std::size_t>(element.bytes), nullptr, "imported buffer region");
    } catch (const std::exception&) {
        // The old path's build throws the same and reports it (a skipped dispatch).
        return Decline::Image;
    }
    // 2. The ring regions of the data words, tagged with the open batch's serial: a full ring
    // reaps the batches that already finished (their releases free their regions) and asks once
    // more, then declines (counted): no GPU wait under the mutex every queue takes.
    std::uint64_t serial = recorder.Submissions() + 1;
    if (!scratch.data.empty()) {
        if (context.fastRing == nullptr) return Decline::Ring;
        auto& ring = *context.fastRing;
        std::array<std::optional<FastRing::Region>, 2> regions;
        if (scratch.data.size() > regions.size()) return Decline::Invalid;
        for (std::size_t attempt = 0; attempt < 2; ++attempt) {
            serial = recorder.Submissions() + 1;
            bool full = false;
            for (std::size_t i = 0; i < scratch.data.size() && !full; ++i) {
                const auto& words = shader.bindings[scratch.data[i].binding].guestDescriptor;
                regions[i] = ring.Allocate(words.size() * sizeof(std::uint32_t), serial);
                full = !regions[i].has_value();
            }
            if (!full) break;
            if (attempt != 0) return Decline::Ring;
            timing.ringFull = true;
            recorder.Reap();
        }
        for (std::size_t i = 0; i < scratch.data.size(); ++i) {
            const auto& words = shader.bindings[scratch.data[i].binding].guestDescriptor;
            const auto& region = *regions[i];
            std::memcpy(region.data, words.data(), words.size() * sizeof(std::uint32_t));
            scratch.data[i].mapped = region.data;
            scratch.buffers[scratch.data[i].info] = {region.buffer, region.offset, region.bytes};
        }
        // The batch's release completes the serial, which frees its regions.
        recorder.Keep(ring.Retirement(serial));
    }
    // 3. The imports, with the batch open: an import a reconcile retires meanwhile goes to the batch
    // (retireImport keeps nothing while the recorder is idle), so every handle taken here outlives
    // the recorded dispatch.
    recorder.Keep(call.pipelineObjects);
    for (const auto& element : scratch.elements) {
        const auto* import = HostImportFor(context, element.address, static_cast<std::size_t>(element.bytes));
        if (import == nullptr) return Decline::NoImport;
        const auto offset = element.address - import->base;
        const auto adjustment = static_cast<std::uint32_t>(offset % alignment);
        if (adjustment % 4 != 0 || element.bytes + adjustment > context.limits.maxStorageBufferRange) return Decline::Misaligned;
        if (adjustment != 0) {
            ++timing.adjusted;
            // Where ShaderResources::buildComplete patches it: the push constant byte (its position
            // was checked with the V#), else the byte of the shader data words.
            if (!shader.pushConstants.empty()) pushBytes[element.adjustmentByte] = static_cast<std::byte>(adjustment);
            else scratch.dataPatches.emplace_back(element.adjustmentByte, adjustment);
        }
        scratch.buffers[element.info] = {import->buffer, offset - adjustment, element.bytes + adjustment};
    }
    if (!scratch.dataPatches.empty()) {
        const auto data = std::find_if(scratch.data.begin(), scratch.data.end(), [](const DataBinding& binding) { return binding.shaderData; });
        if (data == scratch.data.end()) return Decline::Misaligned;
        const auto range = scratch.buffers[data->info].range;
        for (const auto& [byte, adjustment] : scratch.dataPatches) {
            if (byte >= range) return Decline::Misaligned;
        }
        // The region was filled in step 2: patched in place (host-coherent memory).
        for (const auto& [byte, adjustment] : scratch.dataPatches) data->mapped[byte] = static_cast<std::byte>(adjustment);
    }
    // The indirect arguments' import, taken last: no reconcile can retire it before the record.
    VkBuffer argumentBuffer = VK_NULL_HANDLE;
    VkDeviceSize argumentOffset = 0;
    if (call.arguments != 0) {
        const auto* import = HostImportFor(context, call.arguments, 12);
        if (import == nullptr) return Decline::IndirectCpu;
        argumentBuffer = import->buffer;
        argumentOffset = call.arguments - import->base;
    }
    const bool indirect = argumentBuffer != VK_NULL_HANDLE;
    timing.resolveNs = nanosecondsSince(resolveStart);
    // 4. The record, as VulkanDevice::recordDispatch: queued stores and copy-backs over the ranges
    // first, then the barriers around the dispatch.
    const auto recordStart = std::chrono::steady_clock::now();
    const auto arguments = call.arguments;
    const auto touches = [&](std::uint64_t begin, std::uint64_t end) {
        return overlaps(scratch.reads, begin, end) || (indirect && begin < arguments + 12 && arguments < end);
    };
    if (recorder.HasQueuedKeyStores() && recorder.AnyQueuedKeyStore(touches)) recorder.FlushKeyStores();
    if (recorder.HasQueuedStores() && recorder.AnyQueuedStore(touches)) recorder.FlushStores();
    const bool keepCopyBacks = recorder.CoalescesCopyBacks();
    if (keepCopyBacks && recorder.HasDeferredCopies()) {
        recorder.FlushDeferredWhere([&](const Recorder::DeferredCopy& copy, bool claimed) {
            return touches(copy.address, copy.address + copy.bytes) || (!claimed && overlaps(scratch.written, copy.address, copy.address + copy.bytes));
        }, Recorder::FlushReason::Dispatch);
    }
    VkAccessFlags covered = 0;
    const auto commands = keepCopyBacks ? recorder.CommandsKeepingCopyBacks(&covered) : recorder.Commands(&covered);
    // The ring regions belong to the open batch (nothing above submits).
    if (!scratch.data.empty() && recorder.Submissions() + 1 != serial) return Decline::Ring;
    timing.recorded = true;
    for (auto& object : scratch.keep) recorder.Keep(std::move(object));
    using CommandClass = Recorder::CommandClass;
    if (indirect) {
        const auto argumentTiming = recorder.BeginGpuTiming(CommandClass::IndirectArguments);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        Recorder::CountBarriers(CommandClass::IndirectArguments);
        recorder.EndGpuTiming(argumentTiming, 12);
        recorder.NotePendingRead(arguments, 12, Recorder::ReadKind::Indirect);
    }
    constexpr VkAccessFlags shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if ((covered & shaderAccess) == shaderAccess && Recorder::MergeBarriers()) {
        timing.leadSkipped = true;
        Recorder::CountMerged(CommandClass::DispatchLeading);
    } else {
        const auto leadTiming = recorder.BeginGpuTiming(CommandClass::DispatchLeading);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, shaderAccess);
        Recorder::CountBarriers(CommandClass::DispatchLeading);
        recorder.EndGpuTiming(leadTiming);
    }
    context.Resolved(&DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, call.pipeline);
    PushDescriptors(context, commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, scratch.writes);
    if (layout.pushStages != 0) context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, layout.pipeline, VK_SHADER_STAGE_COMPUTE_BIT, 0, PipelinePushConstantBytes, pushBytes.data());
    const auto gpuTiming = recorder.BeginGpuTiming(call.programAddress != 0 ? call.programAddress : shader.variantId);
    RecordCheckpoint(commands, 'C', call.programAddress, shader.variantId, indirect ? ~std::uint64_t{0} : (static_cast<std::uint64_t>(call.x) << 42u) | (static_cast<std::uint64_t>(call.y) << 21u) | call.z);
    if (indirect) context.Resolved(&DeviceFunctions::cmdDispatchIndirect, "vkCmdDispatchIndirect")(commands, argumentBuffer, argumentOffset);
    else context.Resolved(&DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, call.x, call.y, call.z);
    recorder.EndGpuTiming(gpuTiming);
    const auto trailingTiming = recorder.BeginGpuTiming(CommandClass::DispatchTrailing);
    constexpr VkAccessFlags dispatchedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, dispatchedAccess);
    Recorder::CountBarriers(CommandClass::DispatchTrailing);
    recorder.EndGpuTiming(trailingTiming);
    recorder.MarkCovered(dispatchedAccess);
    // 5. The marks of ShaderResources::MarkGpuWrites for an upload served wholly in place: every
    // in-place range is a pending read (a CPU store into it waits for the batch), the written ones
    // pending writes (CPU reads wait, PendingBlocks decline walks over them) stamped as written, and
    // the written storage images hold results pending over their surfaces.
    recorder.NotePendingReads(scratch.reads, Recorder::ReadKind::DispatchElement);
    for (auto* image : scratch.dirty) image->MarkDirty();
    recorder.NotePendingWrites(scratch.written, Recorder::WriteKind::ShaderWrite);
    for (const auto& [begin, end] : scratch.written) GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin));
    timing.recordNs = nanosecondsSince(recordStart);
    return std::nullopt;
}

}
