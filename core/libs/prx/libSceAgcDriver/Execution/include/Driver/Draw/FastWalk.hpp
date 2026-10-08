#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTWALK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTWALK_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace ShaderRecompiler {
struct SourceHandle;
struct ResourceSnapshot;
struct ResourceSpecialization;
}

namespace AgcDriver::DriverDetail {

// F2 of the draw fast path (docs/design/draw-fastpath.md section 2.3), shadow mode:
// APS5_FAST_WALK=N walks the stages of every Nth draw again with the direct reader (live guest
// words: no page copies, no snapshot; a read in a pending 64 KiB block (Recorder::BlockPending),
// over a queued or deferred label of the thread, in a page with storage-image results or a unit
// shadow pending or in an unmapped page declines), fetches the vertex V#s the
// same way, populates the variant the walk's specialization selects and compares it with the
// result the old path bound (its lookup or capture): bindings word by word, push constants,
// variant identity, vertex V#s. Counted on the [fastpath] walk line every 10 s under
// APS5_PROFILE_DRAW; the first mismatches are printed. 0 (unset): off, nothing runs.
std::uint32_t FastWalkEvery();

// What the old path decided for the draw, as Driver::draw holds it after its stage loop.
struct FastWalkDraw {
    std::span<const DrawProgram> programs;
    std::span<const ShaderRecompiler::ProgramRole> roles;
    const Graphics::State& graphics;
    const ShaderRecompiler::ShaderPixelStageInfo& pixel;
    std::span<const std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos;
    std::span<const ShaderRecompiler::LinkedProgram> linked;
    const Pm4::DrawParameters& drawParameters;
    const VulkanDevice& device;
    std::span<const ShaderRecompiler::RecompileResult* const> results;
    std::span<const std::uint32_t> pushOffsets;
    // The old path bound stored results by a heuristic (a data hit, a relocated entry, a partial
    // hit): its mismatches are counted apart.
    bool heuristic = false;
};

// Every FastWalkEvery()th call walks and compares; never throws.
void ShadowWalkDraw(const FastWalkDraw& draw);

// The direct reader's parts for the fast draw (F3b, FastDraw.cpp), the shadow walk's steps over
// the shared reader (FastRead.hpp: FastSrtRead, the decline reasons, CompareWalkedResult):
// why a stage's walk declined: the walk's own statuses, then the direct reader's reasons.
using FastWalkDecline = WalkDecline;
// The vertex fetch of a vertex-family stage through the direct reader: the header part memoized per
// program (DecodeVertexFetchPlan) and the live attribute and V# words, the stage info
// DecodeVertexStageInfo would build. `programs` are the draw's: their registered regions are read first.
std::optional<FastWalkDecline> FastResolveVertex(std::span<const DrawProgram> programs, const DrawProgram& program, ShaderRecompiler::ShaderVertexStageInfo& info);
// The express walk of one stage over its live user words through the direct reader
// (ShaderRecompiler::WalkResources).
std::optional<FastWalkDecline> FastWalkStage(std::span<const DrawProgram> programs, const ShaderRecompiler::SourceHandle& handle, const DrawProgram& program, ShaderRecompiler::ResourceSnapshot& snapshot, ShaderRecompiler::ResourceSpecialization& specialization);

}

#endif
