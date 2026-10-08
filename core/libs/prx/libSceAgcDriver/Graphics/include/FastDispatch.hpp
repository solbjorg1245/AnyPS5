#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTDISPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTDISPATCH_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastLayouts.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace AgcDriver::Graphics {

class Recorder;

// Why a dispatch the fast path (F5, docs/design/draw-fastpath.md 2.9 and 2.11; APS5_FAST_DISPATCH)
// looked at went to the old path, counted on the [fastpath] dispatches line: the driver's reasons
// (a debug mode, no device yet, pending blocks untracked, the walk declined, no compiled variant
// for the walk's specialization, a verify mismatch, an exception before recording), then the
// device's (a BDA or other unbound role, a layout over maxPushDescriptors, a V# a build would
// reject, a written element a build would stage, a sparse range, a range outside one host import, a
// misaligned element without an adjustment slot, an image lookup that threw, deferred words, the
// data ring full or missing, indirect arguments the old path reads on the CPU, workgroup limits).
enum class FastDispatchDecline : std::uint8_t { Debug, NoDevice, Untracked, Walk, VariantMiss, Verify, Exception, NoPush, Bda, Role, PushLimit, Invalid, Staged, Sparse, NoImport, Misaligned, Image, Deferred, Ring, IndirectCpu, Limits, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(FastDispatchDecline::Count)> FastDispatchDeclineNames{"debug mode", "no device", "untracked blocks", "walk", "variant miss", "verify mismatch", "exception", "no push descriptors", "BDA", "role", "push limit", "invalid", "staged", "sparse", "no import", "misaligned", "image", "deferred words", "ring", "indirect on the CPU", "limits"};

// The layout key a ShaderResources build makes for a compute shader's bindings (binding, type, count
// and stage flags per binding; ShaderResources::LayoutKey), into `key`; a binding the fast dispatch
// does not bind (BDA table, fault buffer, GDS, texel buffers) or that a build would reject gives
// the decline instead.
std::optional<FastDispatchDecline> FastComputeLayoutKey(const ShaderRecompiler::RecompileResult& shader, std::vector<std::uint32_t>& key);

// One fast dispatch for RecordFastDispatch: the shader populated from the fast walk, the pipeline
// built on its push layout (kept by the batch through `pipelineObjects`), the group counts or the
// GPU-indirect argument dwords (the caller has ruled out the CPU path's reasons).
struct FastDispatchCall {
    const ShaderRecompiler::RecompileResult* shader = nullptr;
    const FastLayout* layout = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::shared_ptr<void> pipelineObjects;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    std::uint64_t arguments = 0;
    std::uint64_t programAddress = 0;
};

// What one RecordFastDispatch spent and did (APS5_PROFILE_DRAW rows of [fastpath] dispatches):
// resolving the bindings, recording; whether the leading barrier was elided, whether a full ring
// made it sync. `recorded`: commands went into the batch, so a throw after it must not fall back.
struct FastDispatchTiming {
    std::uint64_t resolveNs = 0;
    std::uint64_t recordNs = 0;
    bool leadSkipped = false;
    bool ringSynced = false;
    bool recorded = false;
};

// F5's device half, under GuestMemory::GpuMutex: binds what ShaderResources would bind for a
// compute shader without building one, then records the dispatch as VulkanDevice::recordDispatch
// does. Guest buffer elements (read and written) are bound in place in their host imports, with
// the offset adjustment in the push constants or the shader data words; flattened SRT and shader
// data words go to the fast ring; images and samplers come from the texture, storage and sampler
// caches (ResolveSampledImage, ResolveStorageImage); all as one push of descriptors. Then the
// queued stores and copy-backs over its ranges, the leading barrier (elided after a covering
// command), the dispatch, the trailing barrier, and the marks MarkGpuWrites makes (pending reads
// of every in-place range, pending writes, MarkWritten stamps and so PendingBlocks of the written
// ones, dirty storage images); the batch keeps the pipeline, the images and the ring region.
// Nothing needs completion work: what a build would copy or stage declines. Returns the decline
// with nothing recorded, else nothing.
std::optional<FastDispatchDecline> RecordFastDispatch(const Context& context, Recorder& recorder, const FastDispatchCall& call, FastDispatchTiming& timing);

}

#endif
