#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTREAD_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace ShaderRecompiler {
// Optimization/ResourceProgram.hpp (WalkResources' outcome).
enum class WalkStatus : std::uint8_t;
}

namespace AgcDriver::DriverDetail {

// The pieces of the fast paths' walk (docs/design/draw-fastpath.md 2.3) that the F2 shadow walk
// (FastWalk.cpp), the fast dispatch (F5, FastDispatch.cpp) and the fast draw (F3b) share: the
// direct reader, the walk's decline reasons and the compare of a walked result with an old one.

// Why a stage's walk declined: the walk's own statuses, then the direct reader's reasons.
enum class WalkDecline : std::uint8_t { NoSource, IncompletePlan, NoProgram, Bindless, UnsupportedRoot, OpFailed, Pending, Unmapped, QueuedLabel, Boundary, Failed, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(WalkDecline::Count)> WalkDeclineNames{"no source", "incomplete plan", "no program", "bindless", "unsupported root", "op failed", "pending block", "unmapped", "queued label", "boundary", "failed"};

// What differed between the walk's populated variant and the old path's result.
enum class WalkMismatch : std::uint8_t { Specialization, Variant, Layout, Buffer, Image, Sampler, Flat, Data, Other, Push, Vertex, Count };
inline constexpr std::array<const char*, static_cast<std::size_t>(WalkMismatch::Count)> WalkMismatchNames{"specialization", "variant", "layout", "buffer", "image", "sampler", "flat", "data", "other binding", "push", "vertex"};

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
};

// FastSrtRead (design section 2.3), an SrtRuntime reader over a FastReader: the null page reads
// zero; a read in a pending block (Recorder::BlockPending), over a queued label of this thread or
// in a page not mapped declines; otherwise a plain load of the live word (guest addresses are host
// pointers). No page copy, no flush hook, no snapshot.
bool FastSrtRead(void* context, std::uint64_t address, std::uint32_t* value);

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
// variant identity, layout, every binding word (a FlattenedSrt word the old capture left to the GPU
// is skipped and counted in `deferredSkipped`; T# words differing only in the streaming-feedback
// bits count in `feedbackOnly`) and the push constants.
std::uint32_t CompareWalkedResults(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, WalkDifference& first, std::uint64_t& feedbackOnly, std::uint64_t& deferredSkipped);

}

#endif
