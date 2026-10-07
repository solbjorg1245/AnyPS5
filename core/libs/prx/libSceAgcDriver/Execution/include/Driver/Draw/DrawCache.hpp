#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DrawProgram {
    ShaderRecompiler::ShaderBinary binary;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr = 8;
    std::vector<std::uint32_t> userData;
    std::array<ShaderRecompiler::MemoryRegion, 2> memory;

    std::shared_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
};

struct DrawDecode {
    Graphics::State state;
    ShaderRecompiler::ShaderPixelStageInfo pixel;
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::ProgramRole> roles;
};

struct DrawRecipeRecord {
    std::vector<std::weak_ptr<const DispatchVariant>> stages;
    std::shared_ptr<const DrawRecipe> recipe;
    bool Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const;
    bool Expired() const;
};

struct DrawEntry {

    std::shared_ptr<const DrawDecode> decode;

    std::vector<std::vector<std::shared_ptr<DispatchVariant>>> stages;

    std::atomic<std::shared_ptr<const std::vector<DrawRecipeRecord>>> recipes;
    std::uint64_t touched = 0;
    std::list<std::uint64_t>::iterator order;
};

enum class DrawMiss : std::size_t { FrontDiffering, FragmentDiffering, OtherDiffering, Layout, Gate, Stages, Count };

struct DrawEntryCounters {
    std::uint64_t lookups = 0, absent = 0, hits = 0, stageValidations = 0, stageEqual = 0, variantsCompared = 0, inserts = 0, variantsInserted = 0, variantsEvicted = 0, present = 0, unstable = 0, touches = 0, verifyHits = 0, verifyMismatches = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> misses{};
    std::array<std::uint64_t, MaxDispatchVariants> variantHitsByRank{};
    double validateUs = 0;

    std::uint64_t registerKeyLookups = 0, registerKeyHits = 0, decodeSkipped = 0, decodePartial = 0, facadeMismatches = 0, verifyDecodes = 0, verifyDecodeMismatches = 0;
    double keyUs = 0;
    // The validate phase split (APS5_PROFILE_DRAW): the stage compares (validateVariant), the
    // hit's patched results, and the patched copies made or reused.
    double compareUs = 0, patchUs = 0;
    std::uint64_t compareCalls = 0, patchedMade = 0, patchedReused = 0;
    std::uint64_t partialHits = 0, partialStagesKept = 0;
    // Data hits (DrawStageHits): hits with at least one stage's flat-SRT data words refreshed.
    std::uint64_t dataHits = 0, dataStages = 0, dataWordsRefreshed = 0, dataInserts = 0, dataPositionsInserted = 0, dataVerified = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

enum class DrawVerdict { Drawn, Nothing, Rejected };

// A draw hit's stage results. A stage variant whose stored words differ from guest memory only at
// pure flat-SRT leaves (DataWordPositions, as the dispatch cache's data hits) still matches: its
// regions then carry the live words (`liveWords`, which they point into) and its result is a copy
// with the flattened SRT patched (`results`; the variant's own result for an equal stage). Each
// moving per-object constant (a world matrix in the flattened SRT) otherwise made the vertex stage
// miss, and the miss captured and recompiled every stage of the draw again (~47k of ~118k draws
// per 10 s in Boletaria). APS5_NO_DATA_HITS=1 disables them with the dispatch ones;
// APS5_VERIFY_DATA_HITS=1 captures such draws again and aborts when the patched result differs.
struct DrawStageHits {
    std::vector<std::vector<std::uint32_t>> liveWords;
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> results;
    bool data = false;
    // A miss that keeps its matched stages (results and liveWords set for them, as on a hit): only
    // the stages without a match are captured again. APS5_NO_PARTIAL_DRAW_HITS=1 captures every
    // stage of a miss.
    bool partial = false;
};

struct StageCapture {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    std::uint64_t forgetSerial = 0;
    std::uint32_t pushOffset = 0;
    // The capture's read trace, for the inserted variant's data positions.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
};

}

#endif
