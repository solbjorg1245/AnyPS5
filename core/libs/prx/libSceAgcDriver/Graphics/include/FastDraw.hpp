#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTDRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTDRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/FastLayouts.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastRing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Recorder;
class ShaderResources;
class StorageTexture;

// F3b of the draw fast path (docs/design/draw-fastpath.md 2.4-2.8, 2.11): APS5_FAST_DRAW=1 hands a
// VS+PS draw whose stages the driver walked live and populated (Execution/src/Driver/Draw/
// FastDraw.cpp) to DrawFast instead of Draw. No resource template, no snapshot, no descriptor set:
// the stages' bindings become one push-descriptor set on the push layout (FastLayouts), read-only
// V#s bind their host import in place, sampled images and samplers come from the texture and
// sampler caches, flattened SRT and shader-data words go into the data ring (FastRing). What the
// fast path does not cover goes back to the old path, counted per reason (FastDecline) on the
// [fastpath] draws line.

// Why a draw offered to the fast path went to the old path (draw-fastpath.md 2.11). The first group
// is the driver's (the front end), the second this file's; Verify is no decline: the draw was
// taken by the old path on purpose to be compared (APS5_FAST_DRAW_VERIFY).
enum class FastDecline : std::uint8_t {
    Indirect, DebugMode, Retry, NoDevice, Decode, Shape, NoSource, WalkPending, WalkUnmapped, WalkQueuedLabel, WalkNoProgram, WalkIncomplete, WalkBindless, WalkOther, NoVariant, PushOverflow, CpuIndirect, DeviceReplaced, Rejected, Verify,
    NotRecordable, NoPlumbing, Written, StorageImage, AddressRole, DeferredWords, Unsupported, OverLimit, Aliases, NoImport, Misaligned, Undecodable, ImageShape, NoSampler, NotResident, ReadsTarget, IndirectPath, Rewrites, RingFull, ImportsRetired, Thrown,
    Count
};
const char* FastDeclineName(FastDecline decline);

// A read-only guest range bound in place: the buffer that holds `address` and its offset there
// (HostImportResolver: the host import, as an in-place region of GuestBufferMemory binds it);
// false when nothing covers the range. Tests pass their own.
using FastBufferResolver = bool (*)(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, VkDeviceSize& offset);
bool HostImportResolver(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, VkDeviceSize& offset);
// Verification's resolver: an existing import only (HostImportExisting), nothing reconciled or
// made, so the comparison cannot retire an import the draw it checks binds.
bool HostImportPeekResolver(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, VkDeviceSize& offset);

// The push-descriptor bindings of a draw's stages, in ShaderResources' plan order (stage by stage,
// binding by binding), so LayoutKey() is the key a ShaderResources build of the same stages has. A
// thread's scratch (ScratchLease): Build resets every member.
class FastBindings {
public:
    // Plans and resolves every element of `shaders`' populated bindings, ShaderResources::
    // buildPrepare/buildComplete's rules with a decline where the old path would copy, write, throw
    // or bind something else:
    // - GuestBuffers: a written or atomic element declines; an empty V# binds the null buffer; a
    //   V# aliasing the target declines; the rest binds `resolve`'s buffer, its offset aligned down
    //   to minStorageBufferOffsetAlignment and the difference (the adjustment) patched where the
    //   shader reads it: the stage's push constants at memoryOffsetDword * 4 + element, or its
    //   ShaderData words (no ShaderData: declines);
    // - ShaderData and FlattenedSrt: their words, for the ring (deferred words decline);
    // - sampled images: FastSampledTexture; samplers: the device's sampler cache;
    // - storage images, BDA, fault and GDS roles, other kinds: decline.
    // `flush`: storage results pending over a buffer range are stored first (UploadFinish's rule
    // for an in-place region); false (verification) touches nothing.
    std::optional<FastDecline> Build(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget* target, FastBufferResolver resolve, bool flush);
    std::span<const std::uint32_t> LayoutKey() const { return layoutKey; }
    // The ring bytes the data bindings take, each from an offset aligned to `alignment`.
    VkDeviceSize DataBytes(VkDeviceSize alignment) const;
    // Copies the data words into `region` (null when DataBytes is 0) and returns one write per
    // binding (dstSet unset), valid until the next Build.
    std::span<const VkWriteDescriptorSet> Writes(const FastRing::Region* region, VkDeviceSize alignment);
    // The push constant block (AssemblePushConstants with the adjustment patches) and its stages.
    const std::array<std::byte, PipelinePushConstantBytes>& PushBytes() const { return push; }
    VkShaderStageFlags PushStages() const { return pushStages; }
    bool ReadsImage(const StorageTexture* image) const;
    // The storage images the sampled views read (the barrier validation's image list).
    std::span<const StorageTexture* const> Viewed() const { return viewed; }
    std::span<const std::pair<std::uint64_t, std::uint64_t>> InPlaceReads() const { return inPlaceReads; }
    // The textures and samplers the writes name, for the batch hold; Release drops them from the
    // scratch (a thread's scratch must not keep them past the draw, or past the device).
    void KeepObjects(std::vector<std::shared_ptr<void>>& hold) const;
    void Release() { objects.clear(); }
    // Verification: the resolved elements of each binding.
    struct Binding {
        std::uint32_t binding = 0;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        std::uint32_t count = 0;
        // First element in buffers() / images(); a data binding's first word in its words.
        std::size_t first = 0;
        std::int64_t data = -1;
    };
    std::span<const Binding> Bindings() const { return bindings; }
    std::span<const VkDescriptorBufferInfo> Buffers() const { return buffers; }
    std::span<const VkDescriptorImageInfo> Images() const { return images; }
    std::span<const std::uint32_t> DataWords(const Binding& binding) const;
    // Where an in-place element's adjustment byte is (whatever its value): byte `byte` of the push
    // block (data -1) or of data binding `data`'s words; `buffer` indexes Buffers().
    struct AdjustmentSite {
        std::size_t buffer = 0;
        std::int64_t data = -1;
        std::uint32_t byte = 0;
    };
    std::span<const AdjustmentSite> AdjustmentSites() const { return sites; }

    unsigned depth = 0;

private:
    struct Data {
        std::size_t first = 0;
        std::size_t words = 0;
    };
    std::vector<Binding> bindings;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<Data> data;
    std::vector<std::uint32_t> words;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::uint32_t> layoutKey;
    std::vector<std::uint32_t> occupied;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> inPlaceReads;
    std::vector<AdjustmentSite> sites;
    std::vector<std::shared_ptr<void>> objects;
    std::vector<const StorageTexture*> viewed;
    std::array<std::byte, PipelinePushConstantBytes> push{};
    VkShaderStageFlags pushStages = 0;
};

// The declines of FastBindings::Build that follow from the populated results alone (nothing
// resolved): a written or atomic guest buffer element, a storage image or written sampled image,
// deferred data words, an address, fault or GDS role, a binding outside set 0 or read-only. The
// driver asks before it takes the device lock; Build keeps its own checks.
std::optional<FastDecline> FastStructuralDecline(std::span<const CompiledShader> shaders);

// What DrawFast did: recorded, or the decline that left the draw to Draw (nothing recorded then).
// The parts' times in microseconds under APS5_PROFILE_DRAW.
struct FastDrawOutcome {
    bool recorded = false;
    FastDecline decline = FastDecline::Count;
    bool passContinued = false;
    double inputsUs = 0;
    double targetsUs = 0;
    double bindingsUs = 0;
    double pipelineUs = 0;
    double recordUs = 0;
};

// The fast draw (draw-fastpath.md 2.8), under GuestMemory::GpuMutex after the packet's labels:
// Draw's validation, index and vertex inputs and resident targets, the bindings (FastBindings),
// the pipeline on the push layout (CachedFastPipeline), then the record as recordDraw makes it:
// queued stores over the draw's reads flushed, the previous pass continued or a new one begun
// with its barrier, the GPU-side indirect records read in place (no rewrite), the push descriptors
// and constants, the draw, the pass left open; the objects go into the batch hold. A decline
// records nothing (an exception before the record is a Thrown decline: Draw then meets it).
FastDrawOutcome DrawFast(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders);

// The objects fast draws of the open batch use, kept by one Recorder::Keep per batch until it
// completed (draw-fastpath.md 2.8, in place of a Kept per draw): the batch's ring retirement
// first, then what each draw appends. Under GuestMemory::GpuMutex, with the batch open.
std::vector<std::shared_ptr<void>>& FastBatchHold(const Context& context, Recorder& recorder);

// APS5_FAST_DRAW_VERIFY: the thread's armed comparison. The driver arms it for a draw the fast path
// would have taken and lets Draw draw it; Draw then builds the resources afresh (no template, no
// recipe) and calls VerifyFastBindings, which disarms it.
bool& ThreadFastVerifyArmed();
// Compares the fast bindings of `shaders`, built dry, with the set `resources` binds: per element
// the buffer, offset and range, the data words, the image view and the sampler; and the patched
// push constant blocks. A buffer the old path copied (its element not served in place) counts
// apart, and only that element's adjustment byte is excused in the data words and push constants
// (the copy's adjustment differs from the in-place one); any other difference counts.
void VerifyFastBindings(const Context& context, const ShaderResources& resources, std::span<const CompiledShader> shaders, const ColorTarget& target, FastBufferResolver resolve = &HostImportPeekResolver);
struct FastVerifyCounts {
    std::uint64_t compared = 0, matched = 0, declined = 0, layout = 0, buffer = 0, copied = 0, data = 0, image = 0, sampler = 0, push = 0;
};
// The counts since the last call (the [fastpath] verify line).
FastVerifyCounts TakeFastVerifyCounts();

}

#endif
