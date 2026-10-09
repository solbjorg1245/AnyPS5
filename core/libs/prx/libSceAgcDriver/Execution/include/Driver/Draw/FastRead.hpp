#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTREAD_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
// Optimization/ResourceProgram.hpp (WalkResources' outcome).
enum class WalkStatus : std::uint8_t;
}

namespace AgcDriver::DriverDetail {

// The pieces of the fast paths' walk (docs/design/draw-fastpath.md 2.3) that the F2 shadow walk
// (FastWalk.cpp), the fast dispatch (F5, FastDispatch.cpp) and the fast draw (F3b, FastDraw.cpp)
// share: the direct reader, the walk's decline reasons and the compare of a walked result with an
// old one.

// Why a stage's walk declined: the walk's own statuses, then the direct reader's reasons.
enum class WalkDecline : std::uint8_t { NoSource, IncompletePlan, NoProgram, Bindless, UnsupportedRoot, OpFailed, Pending, Unmapped, QueuedLabel, PendingStorage, Boundary, Failed, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(WalkDecline::Count)> WalkDeclineNames{"no source", "incomplete plan", "no program", "bindless", "unsupported root", "op failed", "pending block", "unmapped", "queued label", "pending storage", "boundary", "failed"};

// What differed between the walk's populated variant and the old path's result. `Deferred`: a
// binding the old capture left words of to the GPU, under CompareWalkedResults' `deferredMismatch`
// (the reader missed the write pending over them; kept apart from a wrong word of the role).
enum class WalkMismatch : std::uint8_t { Specialization, Variant, Layout, Buffer, Image, Sampler, Flat, Data, Other, Push, Vertex, Deferred, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(WalkMismatch::Count)> WalkMismatchNames{"specialization", "variant", "layout", "buffer", "image", "sampler", "flat", "data", "other binding", "push", "vertex", "deferred"};

// Whether FastSrtRead tests a word in a pending 64 KiB block against the exact pending-write
// ranges (the default) or declines on the block alone (APS5_FAST_PENDING_BLOCKS=1, the F2 rule).
bool FastPendingExact();

// The direct reader's state for one walk: the registered regions the old path's ShaderMemory
// serves first (a draw's programs, or a dispatch's code and header), the page last found readable,
// and why a read declined.
struct FastReader {
    std::span<const DrawProgram> programs;
    std::span<const ShaderRecompiler::MemoryRegion> regions;
    std::uint64_t page = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t reads = 0;
    std::uint64_t queries = 0;
    std::optional<WalkDecline> declined;
    // The labels earlier packets of this thread's command buffer queued and nobody wrote yet.
    const std::vector<DeferredLabel>* labels = &deferredLabels().labels;
    // The exact test behind the pending-block prefilter (FastSrtRead); false declines on the block
    // alone (FastPendingExact). The pending-write snapshot (Recorder::PendingWriteSnapshot) last
    // loaded and the publish generation it was loaded at: reloaded when the generation moved, as
    // the old capture's PendingView reloads. This walk's reads that met a pending block, and those
    // of them the exact test let through (each walk's owner adds them to its own report line).
    bool exactPending = FastPendingExact();
    bool snapshotLoaded = false;
    std::uint64_t snapshotGeneration = 0;
    std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> snapshot;
    std::uint64_t pendingInBlocks = 0;
    std::uint64_t pendingPassed = 0;
};

// FastSrtRead (design section 2.3), an SrtRuntime reader over a FastReader. The null page reads
// zero. A read overlapping a pending GPU write declines: Recorder::BlockPending is the prefilter,
// then the exact ranges of the pending-write snapshot decide, which is the old capture's raw-read
// rule (classifyPendingWrite: no overlap, a raw read); APS5_FAST_PENDING_BLOCKS=1 declines on the
// block alone. A read over a queued label of this thread (noted or still deferred: the old path
// captures again after recording one over its reads), in a page not mapped, or in a page the flush
// hook would first store storage-image results or publish unit shadows into (the old capture's
// page read runs it) declines too. Otherwise a plain load of the live word (guest addresses are
// host pointers). No page copy, no flush hook.
bool FastSrtRead(void* context, std::uint64_t address, std::uint32_t* value);

// A tally of walks' reads that met a pending block and of those the exact test let through
// (FastReader::pendingInBlocks, pendingPassed): each report line ([fastpath] walk, draws,
// dispatches) sums its own walks' readers, so no read is counted on two lines.
struct FastPendingReads {
    std::uint64_t inBlocks = 0;
    std::uint64_t readPast = 0;
    void Add(const FastReader& reader) {
        inBlocks += reader.pendingInBlocks;
        readPast += reader.pendingPassed;
    }
    void Add(const FastPendingReads& other) {
        inBlocks += other.inBlocks;
        readPast += other.readPast;
    }
};

// The decline of a walk that ended with `status` (a read the reader declined names its reason).
WalkDecline WalkDeclineOf(ShaderRecompiler::WalkStatus status, const FastReader& reader);

// The first difference CompareWalkedResults found, for the report lines.
struct WalkDifference {
    WalkMismatch kind = WalkMismatch::Count;
    std::size_t binding = 0;
    std::size_t word = 0;
    std::uint32_t old = 0;
    std::uint32_t walked = 0;
};

// The mismatch kinds of a walked result against the old one, as a bit mask (1 << WalkMismatch):
// variant identity, layout, every binding word and the push constants. Not differences:
// - a FlattenedSrt word the old capture left to the GPU (counted in `deferredSkipped`);
// - the flat copy of a sampled T# word that differs only in that word's don't-care bits
//   (FlatTsharpFeedbackCopy, counted in `flatFeedback`): the old stage compare accepted it through
//   those bits, so its hit binds the stored word and the walk the live one (in the fast dispatch's
//   verify, whose old result is a fresh capture with no stage compare, it fires only when the
//   feedback bits changed in memory between the walk and the capture: such a dispatch also counts
//   a T# feedback-only binding);
// - T# words differing only in the streaming-feedback bits (the binding counts in `feedbackOnly`).
// `deferredMismatch` (the fast dispatch and the fast draw's verify, which bind the words they
// walked): a binding the old capture left words of to the GPU differs (WalkMismatch::Deferred),
// whatever its placeholder holds, since the reader missed the write pending over them. Walked
// results come from WalkResources, which claims no pure leaf, so they have no deferredWords of
// their own; the test does not look at them.
std::uint32_t CompareWalkedResults(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, WalkDifference& first, std::uint64_t& feedbackOnly, std::uint64_t& flatFeedback, std::uint64_t& deferredSkipped, bool deferredMismatch = false);

// CompareWalkedResults' kinds alone, without its counters or first difference (the fast draw's
// verify, FastDraw.cpp: `strictDeferred` is `deferredMismatch`).
std::uint32_t CompareWalkedResult(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, bool strictDeferred = false);
// The names of CompareWalkedResult's kinds by bit (WalkMismatchNames).
std::span<const char* const> FastWalkMismatchNames();

}

#endif
