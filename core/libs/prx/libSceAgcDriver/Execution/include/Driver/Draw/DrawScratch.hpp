#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSCRATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSCRATCH_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

// The vectors Driver::draw builds for one draw, kept with their capacity on the drawing thread
// between draws. Built fresh, they were ~25 of the ~70 heap allocations a steady-state draw made
// (RtlAllocateHeap/RtlSizeHeap/operator new were ~8% of the draw thread). Nothing in here outlives
// the draw by design: the next draw resets every member before use, so a shared_ptr kept here only
// delays a release by one draw. A nested draw (the label re-entry after a capture) and
// APS5_NO_DRAW_SCRATCH=1 (the old path) use a private DrawScratch instead.
struct DrawScratch {
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos;
    std::vector<std::vector<Graphics::DecodeRead>> decodeReads;
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> results;
    std::vector<Graphics::CompiledShader> stages;
    std::vector<const ShaderRecompiler::RecompileResult*> programResults;
    std::vector<StageCapture> stageCaptures;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::shared_ptr<DispatchVariant>> fresh;
    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
    std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions;
    DrawStageHits hits;
    std::vector<bool> recompiled;
    std::vector<std::uint32_t> pushOffsets;
    std::vector<std::size_t> resultIndex;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    std::vector<Pm4::DrawArguments> records;
    // The draw relocation's candidate list and state (DrawRelocation.cpp), reset per draw.
    DrawRelocation relocation;
    // A recording-pipe slot's capture memory (the snapshots point into its pages), so it outlives
    // the draw that handed the slot over; other leases keep it on draw()'s stack.
    std::optional<ShaderMemory> shaderMemory;
    unsigned depth = 0;
};

struct Submission;

// A draw handed to the recording pipe (RecordPipe.cpp): what its locked half reads besides the
// slot's DrawScratch (graphics is decode->state), and what A needs to redo it if it turns stale.
struct RecordPipeDraw {
    std::shared_ptr<const DrawDecode> decode;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<VulkanDevice> device;
    std::shared_ptr<const DrawRecipe> recipe;
    Pm4::DrawParameters parameters{};
    std::uint64_t key = 0;
    std::uint32_t opcode = 0;
    bool retryable = false, hit = false;
    std::span<const std::uint32_t> packet;
    const Submission* submission = nullptr;
};

// Lends the calling thread its DrawScratch while no other lease on this thread holds it; `slot`, the
// recording pipe's next free slot (RecordPipe.cpp), is taken first when no lease holds it.
class DrawScratchLease {
public:
    explicit DrawScratchLease(DrawScratch* slot = nullptr);
    ~DrawScratchLease();
    DrawScratchLease(const DrawScratchLease&) = delete;
    DrawScratchLease& operator=(const DrawScratchLease&) = delete;
    DrawScratch& operator*() const { return *scratch; }
    DrawScratch* operator->() const { return scratch; }
    bool Slot() const { return slot; }

private:
    DrawScratch* scratch = nullptr;
    bool slot = false;
    std::unique_ptr<DrawScratch> owned;
};

}

#endif
