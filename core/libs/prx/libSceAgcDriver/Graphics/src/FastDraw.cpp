#include "prx/libSceAgcDriver/Graphics/include/FastDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ScratchLease.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <tuple>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(FastDecline::Count)> FastDeclineNames{
    "indirect (APS5_FAST_DRAW_INDIRECT=0)", "debug mode", "snapshot retry", "no device", "decode", "shape", "no source", "walk: pending block", "walk: unmapped", "walk: queued label", "walk: no program", "walk: incomplete plan", "walk: bindless", "walk: other", "no variant", "push constants", "CPU-side indirect", "device replaced", "known rejection", "verify",
    "not recordable", "no plumbing", "written element", "storage image", "address role", "deferred words", "unsupported binding", "over maxPushDescriptors", "aliases the target", "no import", "misaligned", "undecodable T#", "image shape", "no sampler cache", "non-resident target", "reads a target", "indirect records path", "rewritten records", "ring full", "imports retired", "thrown"};

bool overlaps(std::uint64_t first, std::uint64_t firstBytes, std::uint64_t second, std::uint64_t secondBytes) {
    return first < second + secondBytes && second < first + firstBytes;
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}

const char* FastDeclineName(FastDecline decline) {
    return decline < FastDecline::Count ? FastDeclineNames[static_cast<std::size_t>(decline)] : "?";
}

bool HostImportResolver(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, VkDeviceSize& offset) {
    const auto* import = HostImportMemoized(context, address, bytes);
    if (import == nullptr) return false;
    buffer = import->buffer;
    offset = address - import->base;
    return true;
}

bool HostImportPeekResolver(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, VkDeviceSize& offset) {
    std::uint64_t base = 0;
    if (!HostImportExisting(context, address, bytes, buffer, base)) return false;
    offset = address - base;
    return true;
}

std::optional<FastDecline> FastStructuralDecline(std::span<const CompiledShader> shaders) {
    using Role = ShaderRecompiler::DescriptorRole;
    using Kind = ShaderRecompiler::DescriptorKind;
    static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return FastDecline::Unsupported;
        for (const auto& binding : shader.program->bindings) {
            if (binding.descriptorSet != 0 || binding.count == 0 || binding.readOnly) return FastDecline::Unsupported;
            switch (binding.role) {
                case Role::GuestBuffers:
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const bool written = allWritten || element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                        if (written || (element < binding.bufferAtomic.size() && binding.bufferAtomic[element])) return FastDecline::Written;
                    }
                    break;
                case Role::ShaderData:
                case Role::FlattenedSrt:
                    if (!binding.deferredWords.empty()) return FastDecline::DeferredWords;
                    break;
                case Role::GuestImages:
                    if (binding.kind == Kind::StorageImage || std::any_of(binding.imageWritten.begin(), binding.imageWritten.end(), [](bool written) { return written; })) return FastDecline::StorageImage;
                    break;
                case Role::GuestSamplers:
                    break;
                default:
                    return FastDecline::AddressRole;
            }
        }
    }
    return std::nullopt;
}

std::optional<FastDecline> FastBindings::Build(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget* target, FastBufferResolver resolve, bool flush) {
    using Role = ShaderRecompiler::DescriptorRole;
    using Kind = ShaderRecompiler::DescriptorKind;
    bindings.clear();
    buffers.clear();
    images.clear();
    data.clear();
    words.clear();
    writes.clear();
    layoutKey.clear();
    occupied.clear();
    inPlaceReads.clear();
    sites.clear();
    objects.clear();
    viewed.clear();
    // APS5_ALL_BUFFERS_WRITTEN=1 (ShaderResources::addGuestBuffer): every element counts as written.
    static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    static const bool noSamplerCache = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
    const auto alignment = context.limits.minStorageBufferOffsetAlignment;
    if (alignment == 0) return FastDecline::Unsupported;
    // The info arrays are sized first: the writes point into them.
    std::size_t bufferCount = 0;
    std::size_t imageCount = 0;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return FastDecline::Unsupported;
        for (const auto& binding : shader.program->bindings) (binding.kind == Kind::StorageBuffer ? bufferCount : imageCount) += binding.count;
    }
    buffers.reserve(bufferCount);
    images.reserve(imageCount);
    push = AssemblePushConstants(shaders);
    pushStages = PushConstantStages(shaders);
    // A stage's adjustments for its ShaderData words (byte, adjustment, element in buffers),
    // applied once its bindings are planned: the ShaderData binding may follow the guest buffers.
    thread_local std::vector<std::tuple<std::uint32_t, std::uint32_t, std::size_t>> dataPatches;
    for (const auto& shader : shaders) {
        const auto& program = *shader.program;
        const auto flags = static_cast<std::uint32_t>(VulkanStage(shader.stage));
        std::int64_t shaderData = -1;
        dataPatches.clear();
        for (const auto& binding : program.bindings) {
            if (binding.descriptorSet != 0 || binding.count == 0 || binding.readOnly) return FastDecline::Unsupported;
            if (std::find(occupied.begin(), occupied.end(), binding.binding) != occupied.end()) return FastDecline::Unsupported;
            occupied.push_back(binding.binding);
            Binding item;
            item.binding = binding.binding;
            item.count = binding.count;
            switch (binding.role) {
                case Role::GuestBuffers: {
                    if (binding.kind != Kind::StorageBuffer || binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return FastDecline::Unsupported;
                    item.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    item.first = buffers.size();
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        // An element the recompiler did not classify counts as written (addGuestBuffer).
                        const bool written = allWritten || element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                        const bool atomic = element < binding.bufferAtomic.size() && binding.bufferAtomic[element];
                        if (written || atomic) return FastDecline::Written;
                        const auto* sharp = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                        if ((sharp[1] & 0x40000000u) != 0) return FastDecline::Unsupported;
                        const ShaderRecompiler::ShaderBufferResource descriptor{{sharp[0], sharp[1], sharp[2], sharp[3]}};
                        if (descriptor.Type() != 0u) return FastDecline::Unsupported;
                        const auto address = descriptor.Base48();
                        const auto size = descriptor.GetSize();
                        if (size == 0 || address == 0) {
                            if (context.emptyBuffer == VK_NULL_HANDLE) return FastDecline::Unsupported;
                            buffers.push_back({context.emptyBuffer, 0, EmptyBufferBytes});
                            continue;
                        }
                        if (size > context.limits.maxStorageBufferRange) return FastDecline::Unsupported;
                        if (target != nullptr && overlaps(address, size, target->address, target->bytes)) return FastDecline::Aliases;
                        VkBuffer handle = VK_NULL_HANDLE;
                        VkDeviceSize offset = 0;
                        if (!resolve(context, address, static_cast<std::size_t>(size), handle, offset)) return FastDecline::NoImport;
                        // GuestBufferMemory::Descriptor: bound from the aligned offset below, the
                        // difference added by the shader (push constants or shader data, buildComplete).
                        const auto adjustment = static_cast<std::uint32_t>(offset % alignment);
                        if (adjustment % 4 != 0 || size + adjustment > context.limits.maxStorageBufferRange) return FastDecline::Misaligned;
                        const auto position = program.memoryOffsetDword * 4u + element;
                        if (!program.pushConstants.empty()) {
                            const bool inside = position < program.pushConstants.size() && shader.pushConstantOffset + position < push.size();
                            if (adjustment != 0) {
                                if (!inside) return FastDecline::Unsupported;
                                push[shader.pushConstantOffset + position] = static_cast<std::byte>(adjustment);
                            }
                            if (inside) sites.push_back({buffers.size(), -1, shader.pushConstantOffset + position});
                        } else {
                            dataPatches.emplace_back(position, adjustment, buffers.size());
                        }
                        // Storage results pending in the range are stored into the import first, as
                        // UploadFinish does for a region it binds in place.
                        if (flush) StorageTexture::FlushPending(address, static_cast<std::size_t>(size), nullptr, "imported buffer region");
                        buffers.push_back({handle, offset - adjustment, size + adjustment});
                        inPlaceReads.emplace_back(address, address + size);
                    }
                    break;
                }
                case Role::ShaderData:
                case Role::FlattenedSrt: {
                    if (binding.kind != Kind::StorageBuffer || binding.count != 1 || binding.guestDescriptor.empty()) return FastDecline::Unsupported;
                    if (!binding.deferredWords.empty()) return FastDecline::DeferredWords;
                    const auto bytes = binding.guestDescriptor.size() * sizeof(std::uint32_t);
                    if (bytes > context.limits.maxStorageBufferRange) return FastDecline::Unsupported;
                    item.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    item.first = buffers.size();
                    item.data = static_cast<std::int64_t>(data.size());
                    if (binding.role == Role::ShaderData) shaderData = item.data;
                    data.push_back({words.size(), binding.guestDescriptor.size()});
                    words.insert(words.end(), binding.guestDescriptor.begin(), binding.guestDescriptor.end());
                    // The ring region is filled in by Writes.
                    buffers.push_back({VK_NULL_HANDLE, 0, bytes});
                    break;
                }
                case Role::GuestImages: {
                    if (binding.kind == Kind::StorageImage) return FastDecline::StorageImage;
                    if (binding.kind != Kind::SampledImage || binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 8u || !binding.imageShape.has_value()) return FastDecline::Unsupported;
                    if (std::any_of(binding.imageWritten.begin(), binding.imageWritten.end(), [](bool written) { return written; })) return FastDecline::StorageImage;
                    if (context.detiler == nullptr || context.textureCache == nullptr) return FastDecline::Unsupported;
                    item.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    item.first = images.size();
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto words8 = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                        const bool depthCompare = !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element);
                        bool firstLayer = false;
                        bool undecodable = false;
                        auto texture = FastSampledTexture(context, words8, *binding.imageShape, depthCompare, firstLayer, undecodable);
                        if (texture == nullptr) return undecodable ? FastDecline::Undecodable : FastDecline::ImageShape;
                        images.push_back({VK_NULL_HANDLE, firstLayer ? texture->FirstLayerView() : texture->View(), texture->Layout()});
                        if (const auto* source = texture->StorageSource()) viewed.push_back(source);
                        objects.push_back(std::move(texture));
                    }
                    break;
                }
                case Role::GuestSamplers: {
                    if (binding.kind != Kind::Sampler || binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u || binding.samplerDepthCompare.size() != binding.count) return FastDecline::Unsupported;
                    if (context.samplerCache == nullptr || noSamplerCache) return FastDecline::NoSampler;
                    item.type = VK_DESCRIPTOR_TYPE_SAMPLER;
                    item.first = images.size();
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        auto sampler = context.samplerCache->GetMemoized(context, std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4u, 4u), binding.samplerDepthCompare[element]);
                        images.push_back({sampler->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                        objects.push_back(std::move(sampler));
                    }
                    break;
                }
                default:
                    return FastDecline::AddressRole;
            }
            bindings.push_back(item);
            layoutKey.insert(layoutKey.end(), {item.binding, static_cast<std::uint32_t>(item.type), item.count, flags});
        }
        // buildComplete: an adjustment without push constants goes into the stage's shader data.
        for (const auto& [byte, adjustment, buffer] : dataPatches) {
            if (adjustment == 0) {
                // Nothing to patch; the site is still where verification excuses a copied element.
                if (shaderData >= 0 && byte < data[static_cast<std::size_t>(shaderData)].words * sizeof(std::uint32_t)) sites.push_back({buffer, shaderData, byte});
                continue;
            }
            if (shaderData < 0) return FastDecline::Misaligned;
            const auto& part = data[static_cast<std::size_t>(shaderData)];
            if (byte >= part.words * sizeof(std::uint32_t)) return FastDecline::Unsupported;
            reinterpret_cast<std::byte*>(words.data() + part.first)[byte] = static_cast<std::byte>(adjustment);
            sites.push_back({buffer, shaderData, byte});
        }
    }
    return std::nullopt;
}

VkDeviceSize FastBindings::DataBytes(VkDeviceSize alignment) const {
    VkDeviceSize bytes = 0;
    for (const auto& part : data) bytes += alignUp(part.words * sizeof(std::uint32_t), alignment);
    return bytes;
}

std::span<const VkWriteDescriptorSet> FastBindings::Writes(const FastRing::Region* region, VkDeviceSize alignment) {
    writes.clear();
    VkDeviceSize cursor = 0;
    for (const auto& item : bindings) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstBinding = item.binding;
        write.descriptorCount = item.count;
        write.descriptorType = item.type;
        if (item.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (item.data >= 0) {
                Require(region != nullptr, "fast draw data words without a ring region");
                const auto& part = data[static_cast<std::size_t>(item.data)];
                const auto bytes = static_cast<VkDeviceSize>(part.words * sizeof(std::uint32_t));
                Require(cursor + bytes <= region->bytes, "fast draw data words exceed their ring region");
                std::memcpy(region->data + cursor, words.data() + part.first, static_cast<std::size_t>(bytes));
                buffers[item.first] = {region->buffer, region->offset + cursor, bytes};
                cursor += alignUp(bytes, alignment);
            }
            write.pBufferInfo = buffers.data() + item.first;
        } else {
            write.pImageInfo = images.data() + item.first;
        }
        writes.push_back(write);
    }
    return writes;
}

bool FastBindings::ReadsImage(const StorageTexture* image) const {
    return image != nullptr && std::find(viewed.begin(), viewed.end(), image) != viewed.end();
}

void FastBindings::KeepObjects(std::vector<std::shared_ptr<void>>& hold) const {
    hold.insert(hold.end(), objects.begin(), objects.end());
}

std::span<const std::uint32_t> FastBindings::DataWords(const Binding& binding) const {
    if (binding.data < 0) return {};
    const auto& part = data[static_cast<std::size_t>(binding.data)];
    return std::span<const std::uint32_t>(words).subspan(part.first, part.words);
}

namespace {

struct BatchHold {
    std::vector<std::shared_ptr<void>> objects;
};

// The open batch's hold, by recorder and batch serial: the batch's Keep holds it, so a completed
// batch's entry has expired. Under GuestMemory::GpuMutex.
struct HoldState {
    const Recorder* recorder = nullptr;
    std::uint64_t serial = 0;
    std::weak_ptr<BatchHold> hold;
};

}

std::vector<std::shared_ptr<void>>& FastBatchHold(const Context& context, Recorder& recorder) {
    static HoldState state;
    const auto serial = recorder.Submissions() + 1;
    auto hold = state.hold.lock();
    if (hold == nullptr || state.recorder != &recorder || state.serial != serial) {
        hold = std::make_shared<BatchHold>();
        hold->objects.reserve(1024);
        // The ring regions of this batch are reused once the batch completed and dropped this.
        if (context.fastRing != nullptr) hold->objects.push_back(context.fastRing->Retirement(serial));
        recorder.Keep(hold);
        state = {&recorder, serial, hold};
    }
    // The batch's Keep holds the object until the batch completed, which cannot happen while it is
    // still the open batch.
    return hold->objects;
}

namespace {

struct VerifyCounters {
    HostMutex mutex;
    FastVerifyCounts counts;
};

VerifyCounters& verifyCounters() {
    static VerifyCounters counters;
    return counters;
}

}

bool& ThreadFastVerifyArmed() {
    thread_local bool armed = false;
    return armed;
}

void VerifyFastBindings(const Context& context, const ShaderResources& resources, std::span<const CompiledShader> shaders, const ColorTarget& target, FastBufferResolver resolve) {
    ThreadFastVerifyArmed() = false;
    FastVerifyCounts local;
    local.compared = 1;
    ScratchLease<FastBindings> fast;
    std::optional<FastDecline> declined;
    // The old set's side first (it reads the old build's in-place regions), then the dry build.
    std::vector<ShaderResources::BoundBinding> bound;
    auto block = AssemblePushConstants(shaders);
    try {
        bound = resources.BoundDescriptors();
        resources.PatchPushConstants(block);
        declined = fast->Build(context, shaders, &target, resolve, false);
    } catch (const std::exception&) {
        declined = FastDecline::Thrown;
    }
    char first[192] = "";
    const auto note = [&](std::uint64_t FastVerifyCounts::*kind, std::size_t binding, std::size_t element) {
        if (first[0] == '\0') std::snprintf(first, sizeof(first), "binding %zu element %zu", binding, element);
        ++(local.*kind);
    };
    if (declined) {
        local.declined = 1;
    } else {
        const auto fastBindings = fast->Bindings();
        // The fast elements whose old counterpart is a copy: their adjustment bytes are excused.
        std::vector<std::size_t> copiedBuffers;
        const auto excused = [&](const FastBindings::AdjustmentSite& site) { return std::find(copiedBuffers.begin(), copiedBuffers.end(), site.buffer) != copiedBuffers.end(); };
        if (bound.size() != fastBindings.size()) {
            note(&FastVerifyCounts::layout, bound.size(), fastBindings.size());
        } else {
            for (std::size_t index = 0; index < bound.size(); ++index) {
                const auto& old = bound[index];
                const auto& item = fastBindings[index];
                if (old.binding != item.binding || old.type != item.type || old.elements.size() != item.count) {
                    note(&FastVerifyCounts::layout, index, 0);
                    continue;
                }
                for (std::size_t element = 0; element < old.elements.size(); ++element) {
                    const auto& was = old.elements[element];
                    if (item.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && item.data >= 0) {
                        continue;
                    } else if (item.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        const auto& is = fast->Buffers()[item.first + element];
                        if (was.buffer.buffer == is.buffer && was.buffer.offset == is.offset && was.buffer.range == is.range) continue;
                        // The old path copies a range it does not bind in place (no import, or a
                        // region start off the alignment); one it binds in place must be the same.
                        if (was.guest && !was.inPlace && is.buffer != context.emptyBuffer) {
                            note(&FastVerifyCounts::copied, index, element);
                            copiedBuffers.push_back(item.first + element);
                        } else {
                            note(&FastVerifyCounts::buffer, index, element);
                        }
                    } else if (item.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                        if (was.view != fast->Images()[item.first + element].imageView) note(&FastVerifyCounts::image, index, element);
                    } else if (item.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                        if (was.sampler != fast->Images()[item.first + element].sampler) note(&FastVerifyCounts::sampler, index, element);
                    }
                }
            }
            // The data words, once every copied element is known (a stage's ShaderData binding may
            // come before its guest buffers).
            std::vector<std::byte> expected;
            for (std::size_t index = 0; index < bound.size(); ++index) {
                const auto& item = fastBindings[index];
                if (item.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || item.data < 0 || bound[index].elements.size() != 1) continue;
                const auto& was = bound[index].elements[0];
                const auto words = std::as_bytes(fast->DataWords(item));
                if (was.data.size() < words.size()) {
                    note(&FastVerifyCounts::data, index, 0);
                    continue;
                }
                expected.assign(was.data.begin(), was.data.begin() + static_cast<std::ptrdiff_t>(words.size()));
                for (const auto& site : fast->AdjustmentSites()) {
                    if (site.data == item.data && site.byte < expected.size() && excused(site)) expected[site.byte] = words[site.byte];
                }
                if (std::memcmp(expected.data(), words.data(), words.size()) != 0) note(&FastVerifyCounts::data, index, 0);
            }
        }
        for (const auto& site : fast->AdjustmentSites()) {
            if (site.data < 0 && site.byte < block.size() && excused(site)) block[site.byte] = fast->PushBytes()[site.byte];
        }
        if (block != fast->PushBytes()) note(&FastVerifyCounts::push, 0, 0);
        if (local.layout + local.buffer + local.data + local.image + local.sampler + local.push == 0) local.matched = 1;
    }
    fast->Release();
    if (local.layout + local.buffer + local.data + local.image + local.sampler + local.push != 0) {
        static std::atomic<std::uint32_t> printed{0};
        if (printed.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[fastpath] draw verify: bindings differ from the old path's set (layout %llu, buffer %llu, data %llu, image %llu, sampler %llu, push %llu), first at %s\n", static_cast<unsigned long long>(local.layout), static_cast<unsigned long long>(local.buffer), static_cast<unsigned long long>(local.data), static_cast<unsigned long long>(local.image), static_cast<unsigned long long>(local.sampler), static_cast<unsigned long long>(local.push), first);
    }
    auto& counters = verifyCounters();
    std::lock_guard lock(counters.mutex);
    auto& total = counters.counts;
    total.compared += local.compared;
    total.matched += local.matched;
    total.declined += local.declined;
    total.layout += local.layout;
    total.buffer += local.buffer;
    total.copied += local.copied;
    total.data += local.data;
    total.image += local.image;
    total.sampler += local.sampler;
    total.push += local.push;
}

FastVerifyCounts TakeFastVerifyCounts() {
    auto& counters = verifyCounters();
    std::lock_guard lock(counters.mutex);
    const auto counts = counters.counts;
    counters.counts = {};
    return counts;
}

}
