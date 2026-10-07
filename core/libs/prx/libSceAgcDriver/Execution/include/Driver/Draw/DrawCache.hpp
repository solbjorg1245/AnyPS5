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

// The shader user words the draw key keeps out of its base key (DrawKey::base): the 64-bit pointer
// pairs the title moves per frame, vertex/geometry-front user SGPRs 0-1, 4-5 and 8-9 and pixel user
// SGPRs 0-1 (the SRT base and its secondary tables in a ring of frame allocations: ~8k new keys
// per 10 s in Boletaria, PROGRESS t263). A new key whose base key has an entry is tried as that
// entry relocated by the pairs' delta (DrawRelocation.cpp).
inline constexpr std::array<std::uint32_t, 8> DrawPointerRegisters{0x08c, 0x08d, 0x090, 0x091, 0x094, 0x095, 0x00c, 0x00d};
// Candidate keys kept per base key (the objects drawn with one pipeline state).
inline constexpr std::size_t DrawBaseCandidates = 16;
struct DrawKey {
    std::uint64_t key = 0;
    std::uint64_t base = 0;
    std::array<std::uint32_t, DrawPointerRegisters.size()> words{};
    std::uint32_t present = 0;
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
    // The key's base and pointer words (DrawKey), for the relocation of a later key.
    std::uint64_t baseKey = 0;
    std::array<std::uint32_t, DrawPointerRegisters.size()> pointerWords{};
    std::uint32_t pointerPresent = 0;
};

// A draw whose key is new but whose base key has an entry (Driver::findRelocationCandidate): the
// candidate entry and its key, the new key and the entry's decode with the live pointer words, per
// program the delta its pointer pairs moved by (0: its variants compare in place) and the stages
// lookupDraw matched through a shifted variant (for the rekey and the learn step).
struct DrawRelocationCandidate {
    std::shared_ptr<DrawEntry> entry;
    std::uint64_t key = 0;
    std::vector<std::uint64_t> deltas;
};
struct DrawRelocation {
    // The fitting entries under the base key, oldest first (the objects drawn with one pipeline
    // state take turns: a rekey puts the entry at the back).
    std::vector<DrawRelocationCandidate> candidates;
    // The candidate lookupDraw validates against (chooseRelocationCandidate: a rule on every moved
    // stage and the shifted words in place) and, after a miss, the one the fresh variants learned
    // their rule from: the entry the new key takes over. Null: a plain miss.
    std::shared_ptr<DrawEntry> entry;
    std::uint64_t key = 0;
    DrawKey target;
    std::shared_ptr<const DrawDecode> decode;
    std::vector<std::uint64_t> deltas;
    std::vector<bool> relocated;
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
    // Variants inserted with don't-care bits (DispatchVariant::ignoredBits) and their positions.
    std::uint64_t ignoredInserts = 0, ignoredPositionsInserted = 0;
    // Variants inserted with buffer base slots (DispatchVariant::baseSlots), and the data hits'
    // stages that refreshed a base word.
    std::uint64_t baseInserts = 0, baseSlotsInserted = 0, baseStages = 0;
    // Why guest-buffer elements got no base slot at insert (BufferBaseCounts).
    std::uint64_t baseWritten = 0, baseUnlocated = 0, baseUnread = 0, baseAmbiguous = 0, baseData = 0;
    // Draw relocation (DrawRelocation.cpp): absent keys with a candidate entry under the base key
    // and without, candidates examined and refused (pointer pairs of one program disagreeing),
    // hits and partial hits through a candidate (stages shifted, compared in place), stages whose
    // variants had no rule, whose shifted variant differed or fell out of order, rekeys, and the
    // learn step's attempts, successes and verdicts.
    std::uint64_t relocationCandidates = 0, relocationNoCandidate = 0, relocationTried = 0, relocationDeltas = 0, relocationUnchosen = 0, relocationQuickRejected = 0, relocationLearnedFrom = 0, relocatedHits = 0, relocatedPartial = 0, relocatedStages = 0, relocatedInPlace = 0, relocationNoRule = 0, relocationDiffering = 0, relocationUnordered = 0, rekeys = 0, relocationLearnAttempts = 0, relocationLearned = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(RelocationVerdict::Count)> relocationVerdicts{};
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
