#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHCACHE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

inline constexpr std::size_t MaxDispatchVariants = 8;

struct DispatchVariant {

    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::vector<std::uint32_t> words;
    std::uint64_t forgetSerial = 0;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;

    std::shared_ptr<const ShaderSnapshot> shader;

    std::atomic<std::shared_ptr<const Recipe>> recipe;

    static constexpr std::uint32_t NoFlatBinding = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> dataPositions;
    std::vector<std::uint32_t> dataSlots;
    std::uint32_t flatBinding = NoFlatBinding;
    // Don't-care bits (IgnoredWordBits): (position, mask) sorted by position, of the stored words a
    // compare may differ in without the variant missing: the sampled-image T#s' texture-streaming
    // feedback fields (word 5 bit 25, the mip-stats counter enable; word 6 bits 0-7, its counter
    // id), which the title toggles per frame and the port does not implement (~5-8k fragment stage
    // misses per 10 s in Boletaria). A hit binds the stored words, so the texture keys stay put.
    // Draw variants only; APS5_NO_TSHARP_MASK=1 compares them exactly.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ignoredBits;
    // Buffer base slots (BufferBaseWords): the read-only guest-buffer V#s' base words among the
    // stored words, refreshed on a hit like the data words and patched into their bindings'
    // descriptors (the Graphics side rebases the moved buffer in place). Draw variants only;
    // APS5_NO_VSHARP_BASES=1 leaves them out, so such a V# moving misses the stage.
    std::vector<WordPatchSlot> baseSlots;

    // Relocation rule (DispatchRelocation.cpp): the low-word positions of the 64-bit pointers
    // through which the walk reached `movedRuns` (run indices). A later dispatch whose pointers all
    // moved by one delta is validated against these runs shifted by it instead of captured again.
    std::vector<std::uint32_t> pointerPositions;
    std::vector<std::uint32_t> movedRuns;
    // (binding, word) of the compiled descriptors' addresses inside the moved runs, shifted with them.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shiftSlots;

    std::uint32_t pushOffset = 0;
    std::shared_ptr<const ShaderRecompiler::ShaderVertexStageInfo> vertexInfo;

    std::shared_ptr<ShaderMemory> memory;
    std::vector<ShaderRecompiler::MemoryRegion> captured;
    std::vector<std::pair<std::uint64_t, std::size_t>> spans;
    std::atomic<std::uint64_t> generation{0};
    // Draw data hits (DrawLookup.cpp): the one patched copy of `compiled` the variant's hits bind,
    // reused while no earlier draw still holds it (use_count 1) instead of a deep copy per hit (a
    // DescriptorBinding is seven vectors; ~130k copies per 10 s in Boletaria); only the flat
    // binding's data slots and the buffer base slots are rewritten. APS5_NO_PATCHED_RESULT_REUSE=1
    // copies per hit again.
    std::shared_ptr<ShaderRecompiler::RecompileResult> patched;
};

struct DispatchEntry {
    std::vector<std::shared_ptr<DispatchVariant>> variants;

    std::uint64_t touched = 0;

    std::list<std::uint64_t>::iterator order;
};

struct EntryCounters {
    std::uint64_t lookups = 0, absent = 0, equal = 0, differing = 0, inaccessible = 0, queuedLabel = 0, flushingImage = 0, publishMoved = 0, pendingMoved = 0, forgetMoved = 0, imagesFlushed = 0, runsSynced = 0, forgetSinceInsert = 0, replaced = 0, inserts = 0, unstable = 0, touches = 0;

    std::uint64_t runsValidated = 0, runsInserted = 0, retriesEqual = 0, retriesMoved = 0;
    double validateUs = 0;

    std::uint64_t differingClassified = 0, differingAddress = 0, differingData = 0, differingWalk = 0, differingMixed = 0, differingRunsChanged = 0, differingMatchedPrior = 0, differingWords = 0;
    std::array<std::uint64_t, 4> differingWordBuckets{};

    std::array<std::uint64_t, MaxDispatchVariants> variantHitsByRank{};
    std::uint64_t variantsCompared = 0, variantsInserted = 0, variantsEvicted = 0;

    std::uint64_t dataHits = 0, dataWordsRefreshed = 0, dataVerified = 0, dataInserts = 0, dataPositionsInserted = 0, dataLeavesUnmapped = 0, dataLeavesMismatched = 0, dataLeavesAliased = 0;
    std::array<std::uint64_t, MaxDispatchVariants> dataHitsByRank{};
    std::array<std::uint64_t, 9> relocationVerdicts{};
    std::uint64_t relocatedHits = 0, relocatedDiffering = 0, relocatedUnordered = 0;
    std::set<std::size_t> differingPositions;
    std::size_t differingFirstPosition = std::numeric_limits<std::size_t>::max(), differingLastPosition = 0;
    std::map<std::uint64_t, std::uint64_t> differingByProgram;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

struct DataMask {
    std::span<const std::uint32_t> positions;
    // Receives (position, live word) for a difference at a data position; null: such a difference
    // is a miss (positions empty), the mask then serves `ignored` alone.
    std::vector<std::pair<std::uint32_t, std::uint32_t>>* live;
    // (position, mask) sorted by position: a word differing only in the masked bits compares equal
    // (DispatchVariant::ignoredBits).
    std::span<const std::pair<std::uint32_t, std::uint32_t>> ignored;
    // Sorted patch slots (DispatchVariant::baseSlots): a difference within a slot's mask is a
    // refreshed word, reported through `live` like one at a data position.
    std::span<const WordPatchSlot> patches;
};

// Why learnRelocation took or refused a relocation rule (counted per verdict).
enum class RelocationVerdict { Learned, Shape, DataPositions, Deltas, NothingMoved, OtherWords, NoPointer, Compiled, Descriptors, Count };

enum class EntryOutcome { Equal, EqualData, Differing, Inaccessible, QueuedLabel, FlushingImage, PublishMoved, PendingMoved, ForgetMoved };

}

#endif
