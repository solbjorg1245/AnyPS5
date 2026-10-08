#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace AgcDriver::DriverDetail {

namespace {

static_assert(static_cast<int>(Graphics::RegisterBank::Context) == 0 && static_cast<int>(Graphics::RegisterBank::Shader) == 1 && static_cast<int>(Graphics::RegisterBank::UserConfig) == 2, "FastCensusRange banks follow Graphics::RegisterBank");

constexpr auto CensusRanges = [] {
    std::array<FastCensusRange, Graphics::DrawKeyRegisters.size()> ranges{};
    for (std::size_t i = 0; i < ranges.size(); ++i) ranges[i] = {static_cast<std::uint8_t>(Graphics::DrawKeyRegisters[i].bank), Graphics::DrawKeyRegisters[i].first, Graphics::DrawKeyRegisters[i].count};
    return ranges;
}();

constexpr std::size_t MaxStages = 6;
constexpr std::uint8_t NoPath = 0xff;
constexpr std::size_t IndirectPaths = static_cast<std::size_t>(Graphics::IndirectDrawPath::Count);
// Whether a stage binds a bindless image table: from this draw's capture, or for a draw-cache hit
// (no capture) learned from the capture its variant was compiled over.
enum class Bindless : std::uint8_t { No, Yes, Unknown };

// The calling thread's draw packet: Driver::draw fills it, the packet's end commits it.
struct PendingDraw {
    bool evaluated = false;
    // Keys, descriptors and reads were taken (not for the precheck's metadata pass).
    bool keyed = false;
    std::uint32_t reasons = 0;
    std::uint32_t descriptors = 0;
    FastCensusKeys keys;
    std::uint32_t reads = 0;
    std::uint32_t blockReads = 0;
    std::uint32_t exactReads = 0;
    std::uint32_t stages = 0;
    std::array<std::uint64_t, MaxStages> programs{};
    std::array<std::uint64_t, MaxStages> variants{};
    std::array<Bindless, MaxStages> bindless{};
    std::uint8_t cpuPath = NoPath;
};

PendingDraw& pendingDraw() {
    static thread_local PendingDraw pending;
    return pending;
}

// The calling thread's open run of eligible draws and its previous draw's keys.
struct ThreadRun {
    std::uint64_t length = 0;
    bool keyed = false;
    FastCensusKeys last;
};

ThreadRun& threadRun() {
    static thread_local ThreadRun run;
    return run;
}

struct Totals {
    HostMutex mutex;
    FastCensusTally tally;
    std::uint64_t dropped = 0;
    std::uint64_t unseen = 0;
    std::array<std::uint64_t, FastCensusDescriptorBucketNames.size()> descriptors{};
    std::uint32_t maxDescriptors = 0;
    std::uint64_t keyed = 0;
    std::unordered_set<std::uint64_t> full, state, stateNoTargets;
    std::uint64_t fullChanges = 0, stateChanges = 0, stateNoTargetChanges = 0;
    std::uint64_t reads = 0, blockReads = 0, exactReads = 0, blockDraws = 0;
    std::unordered_set<std::uint64_t> programVariants;
    std::unordered_map<std::uint64_t, std::uint32_t> variantsPerProgram;
    // variantId -> bindless, kept across windows (a hit's stage is resolved through it).
    std::unordered_map<std::uint64_t, bool> bindlessByVariant;
    std::uint64_t bindlessUnknown = 0;
    std::array<std::uint64_t, IndirectPaths> cpuPaths{};
    FastCensusRuns runs;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Totals& totals() {
    // Never destroyed: a queue thread may still commit during static teardown.
    static auto* const value = new Totals();
    return *value;
}

double share(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

void reportLocked(Totals& census) {
    const auto now = std::chrono::steady_clock::now();
    if (now - census.lastReport < std::chrono::seconds(10)) return;
    census.lastReport = now;
    const auto& tally = census.tally;
    char item[96];
    std::string declines;
    for (std::size_t reason = 0; reason < FastCensusReasons; ++reason) {
        if (tally.any[reason] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %llu/%llu", FastCensusReasonNames[reason], static_cast<unsigned long long>(tally.any[reason]), static_cast<unsigned long long>(tally.sole[reason]));
        declines += item;
    }
    std::string descriptors;
    for (std::size_t bucket = 0; bucket < census.descriptors.size(); ++bucket) {
        std::snprintf(item, sizeof(item), " %s %llu", FastCensusDescriptorBucketNames[bucket], static_cast<unsigned long long>(census.descriptors[bucket]));
        descriptors += item;
    }
    std::string paths;
    for (std::size_t path = 0; path < IndirectPaths; ++path) {
        if (census.cpuPaths[path] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %llu", Graphics::IndirectDrawPathName(static_cast<Graphics::IndirectDrawPath>(path)), static_cast<unsigned long long>(census.cpuPaths[path]));
        paths += item;
    }
    std::uint32_t maxVariants = 0;
    std::size_t manyVariants = 0;
    for (const auto& [program, count] : census.variantsPerProgram) {
        maxVariants = std::max(maxVariants, count);
        if (count > 8) ++manyVariants;
    }
    std::fprintf(stderr, "[fastpath] census (10 s): %llu draw packets, %llu eligible (%.1f%%), %llu dropped before the census point (nothing, rejected, thrown), %llu without a Graphics::Draw outcome (early end); declines any/sole:%s; descriptors per draw (push limit %u):%s, max %u; keys over %llu draws: %zu full (%llu changes from the previous draw), %zu state (%llu changes), %zu state without target bases (%llu changes); walk reads %llu, in a pending 64 KiB block %llu (%.1f%%, exactly pending %llu), draws with one %llu (%.1f%%); variants: %zu programs, %zu (program, variant) pairs, max %u per program, %zu programs over 8, hit stages of unknown bindless %llu; cpu indirect paths:%s\n",
        static_cast<unsigned long long>(tally.draws + census.dropped), static_cast<unsigned long long>(tally.eligible), share(tally.eligible, tally.draws), static_cast<unsigned long long>(census.dropped), static_cast<unsigned long long>(census.unseen), declines.c_str(),
        FastCensusPushLimit, descriptors.c_str(), census.maxDescriptors,
        static_cast<unsigned long long>(census.keyed), census.full.size(), static_cast<unsigned long long>(census.fullChanges), census.state.size(), static_cast<unsigned long long>(census.stateChanges), census.stateNoTargets.size(), static_cast<unsigned long long>(census.stateNoTargetChanges),
        static_cast<unsigned long long>(census.reads), static_cast<unsigned long long>(census.blockReads), share(census.blockReads, census.reads), static_cast<unsigned long long>(census.exactReads), static_cast<unsigned long long>(census.blockDraws), share(census.blockDraws, census.keyed),
        census.variantsPerProgram.size(), census.programVariants.size(), maxVariants, manyVariants, static_cast<unsigned long long>(census.bindlessUnknown), paths.c_str());
    const auto& runs = census.runs;
    std::string lengths;
    for (std::size_t bucket = 0; bucket < FastCensusRuns::Buckets; ++bucket) {
        std::snprintf(item, sizeof(item), " %s %llu", FastCensusRuns::BucketNames[bucket], static_cast<unsigned long long>(runs.runs[bucket]));
        lengths += item;
    }
    std::string ends;
    for (std::size_t why = 0; why < FastCensusBreaks; ++why) {
        std::snprintf(item, sizeof(item), " %s %llu", FastCensusBreakNames[why], static_cast<unsigned long long>(runs.breaks[why]));
        ends += item;
    }
    const auto runCount = runs.Runs();
    std::fprintf(stderr, "[fastpath] census runs queue 0 (10 s): %llu runs of %llu eligible draws (mean %.1f, %.1f%% in runs of 16 or more), by length:%s; ended by:%s\n", static_cast<unsigned long long>(runCount), static_cast<unsigned long long>(runs.packets), runCount == 0 ? 0.0 : static_cast<double>(runs.packets) / static_cast<double>(runCount), share(runs.longPackets, runs.packets), lengths.c_str(), ends.c_str());
    census.tally.Reset();
    census.dropped = census.unseen = 0;
    census.descriptors = {};
    census.maxDescriptors = 0;
    census.keyed = 0;
    census.full.clear();
    census.state.clear();
    census.stateNoTargets.clear();
    census.fullChanges = census.stateChanges = census.stateNoTargetChanges = 0;
    census.reads = census.blockReads = census.exactReads = census.blockDraws = 0;
    census.programVariants.clear();
    census.variantsPerProgram.clear();
    census.bindlessUnknown = 0;
    census.cpuPaths = {};
    census.runs.Reset();
}

void closeRun(std::uint32_t queue, FastCensusBreak why) {
    auto& run = threadRun();
    if (queue != 0) {
        run.length = 0;
        return;
    }
    auto& census = totals();
    std::lock_guard lock(census.mutex);
    census.runs.Close(run.length, why);
    run.length = 0;
    reportLocked(census);
}

}

bool FastCensusActive() {
    return Graphics::FastCensus();
}

void NoteFastCensusDraw(const QueueState& queue, const Graphics::State& graphics, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<DrawProgram>& programs, const std::vector<const ShaderRecompiler::RecompileResult*>& results, const std::vector<bool>& recompiled, const std::vector<StageCapture>& captures, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool indirect, const std::optional<Graphics::IndirectDrawPath>& cpuIndirect, bool debug) {
    using Role = ShaderRecompiler::ProgramRole;
    using DescriptorRole = ShaderRecompiler::DescriptorRole;
    auto& pending = pendingDraw();
    pending = {};
    pending.evaluated = true;
    pending.keyed = true;
    pending.keys = ComputeFastCensusKeys(queue, CensusRanges);
    std::uint32_t reasons = 0;
    const auto set = [&](FastCensusReason reason) { reasons |= FastCensusBit(reason); };
    if (graphics.stages.path != Graphics::ShaderPath::Vertex || graphics.stages.mesh || graphics.stages.tessellation) set(FastCensusReason::Stages);
    if (std::any_of(roles.begin(), roles.end(), [](Role role) { return role != Role::Main && role != Role::Fragment; })) set(FastCensusReason::Stages);
    if (graphics.rectList) set(FastCensusReason::RectList);
    if (indirect && cpuIndirect) {
        set(FastCensusReason::CpuIndirect);
        pending.cpuPath = static_cast<std::uint8_t>(*cpuIndirect);
    }
    // The verify, dump and probe modes the fast path leaves to the old path (design 2.11).
    static const bool debugModes = std::getenv("APS5_CAPTURE_INPUTS") != nullptr || std::getenv("APS5_DUMP_TARGETS") != nullptr || std::getenv("APS5_DUMP_DRAW_PROGRAM") != nullptr;
    if (debug || debugModes) set(FastCensusReason::Debug);
    // A walk read in a 64 KiB block with a pending GPU write declines (F2's PendingBlocks); the
    // exact overlap is what the slow query would still allow.
    const auto snapshot = Graphics::Recorder::PendingWriteSnapshot();
    const auto countRead = [&](std::uint64_t address, std::size_t bytes) {
        if (bytes == 0) return;
        ++pending.reads;
        if (snapshot == nullptr) return;
        constexpr std::uint64_t Block = 0x10000;
        const auto first = address & ~(Block - 1);
        const auto end = (address + bytes + Block - 1) & ~(Block - 1);
        if (!Graphics::Recorder::SnapshotOverlaps(snapshot.get(), first, static_cast<std::size_t>(end - first))) return;
        ++pending.blockReads;
        if (Graphics::Recorder::SnapshotOverlaps(snapshot.get(), address, bytes)) ++pending.exactReads;
    };
    for (std::size_t i = 0; i < results.size() && i < programs.size(); ++i) {
        const auto* result = results[i];
        if (result == nullptr) continue;
        for (const auto& binding : result->bindings) {
            pending.descriptors += binding.count;
            switch (binding.role) {
            case DescriptorRole::GuestBuffers:
                // An element beyond bufferWritten counts as written (Recompiler.hpp).
                for (std::uint32_t element = 0; element < binding.count; ++element) {
                    if (element >= binding.bufferWritten.size() || binding.bufferWritten[element] || (element < binding.bufferAtomic.size() && binding.bufferAtomic[element])) {
                        set(FastCensusReason::Writes);
                        break;
                    }
                }
                break;
            case DescriptorRole::GuestImages:
                if (std::find(binding.imageWritten.begin(), binding.imageWritten.end(), true) != binding.imageWritten.end()) set(FastCensusReason::Writes);
                break;
            case DescriptorRole::Gds: set(FastCensusReason::Writes); break;
            case DescriptorRole::BdaPagetable: set(FastCensusReason::Bda); break;
            case DescriptorRole::FlattenedSrt:
                if (!binding.deferredWords.empty()) set(FastCensusReason::DeferredFlat);
                break;
            default: break;
            }
        }
        // The walk's reads: this draw's capture, or the runs a hit validated, and the vertex V# reads.
        const bool captured = i < recompiled.size() && recompiled[i] && i < captures.size();
        if (captured) {
            for (const auto& region : captures[i].regions) countRead(region.guestAddress, region.bytes.size());
        } else if (i < matchedRegions.size()) {
            for (const auto& region : matchedRegions[i]) countRead(region.guestAddress, region.bytes.size());
        }
        if (i < decodeReads.size()) {
            for (const auto& read : decodeReads[i]) countRead(read.address, read.bytes.size());
        }
        if (pending.stages == MaxStages) continue;
        const auto stage = pending.stages++;
        pending.programs[stage] = programs[i].binary.codeAddress;
        pending.variants[stage] = result->variantId;
        const auto* capture = i < captures.size() ? captures[i].capture.get() : nullptr;
        if (capture == nullptr) {
            pending.bindless[stage] = Bindless::Unknown;
        } else {
            const auto& images = capture->specialization.images;
            pending.bindless[stage] = std::any_of(images.begin(), images.end(), [](const ShaderRecompiler::ResourceSpecialization::Image& image) { return image.indirectRoot != ShaderRecompiler::ImageResource::NoIndirectImage; }) ? Bindless::Yes : Bindless::No;
        }
    }
    if (pending.descriptors > FastCensusPushLimit) set(FastCensusReason::Descriptors);
    if (pending.blockReads != 0) set(FastCensusReason::PendingBlock);
    pending.reasons = reasons;
}

void NoteFastCensusMetadataPass() {
    if (!FastCensusActive()) return;
    auto& pending = pendingDraw();
    pending = {};
    pending.evaluated = true;
    pending.reasons = FastCensusBit(FastCensusReason::MetadataPass);
}

void NoteFastCensusDropped() {
    pendingDraw() = {};
}

void NoteFastCensusPacket(std::uint32_t queue, std::uint32_t header, bool drawPacket, bool flip, bool renderingWait, bool wroteOnGpu) {
    if (!FastCensusActive()) return;
    if (!drawPacket) {
        if (const auto why = ClassifyFastCensusPacket(header, flip, renderingWait, wroteOnGpu)) closeRun(queue, *why);
        return;
    }
    auto& pending = pendingDraw();
    const auto draw = pending;
    pending = {};
    Graphics::DrawCensusNote note;
    if (auto* threadNote = Graphics::ThreadDrawCensusNote()) {
        note = *threadNote;
        *threadNote = {};
    }
    auto& run = threadRun();
    auto& census = totals();
    std::lock_guard lock(census.mutex);
    // Nothing to draw, a rejection or a throw (NoteFastCensusDropped clears a draw that failed
    // after the census point): it neither counts nor ends a run.
    if (!draw.evaluated) {
        ++census.dropped;
        reportLocked(census);
        return;
    }
    auto reasons = draw.reasons;
    const auto set = [&](FastCensusReason reason) { reasons |= FastCensusBit(reason); };
    if (note.seen) {
        if (note.notResident) set(FastCensusReason::NotResident);
        if (note.readsTarget) set(FastCensusReason::ReadsTarget);
        if (note.copiedWrites) set(FastCensusReason::CopiedWrites);
        if (note.lease) set(FastCensusReason::Lease);
        if (note.indirect && note.path != Graphics::IndirectDrawPath::Gpu) {
            set(FastCensusReason::CpuIndirect);
            ++census.cpuPaths[static_cast<std::size_t>(note.path)];
        }
        if (note.rewritesRecords) set(FastCensusReason::RewritesRecords);
    } else if (draw.keyed) {
        ++census.unseen;
    }
    if (draw.cpuPath != NoPath && draw.cpuPath < IndirectPaths) ++census.cpuPaths[draw.cpuPath];
    for (std::uint32_t stage = 0; stage < draw.stages; ++stage) {
        const auto program = draw.programs[stage];
        const auto variant = draw.variants[stage];
        if (variant != 0 && census.programVariants.insert(FastCensusMix(program, variant)).second) ++census.variantsPerProgram[program];
        if (draw.bindless[stage] != Bindless::Unknown) {
            if (variant != 0) {
                if (census.bindlessByVariant.size() > (1u << 16u)) census.bindlessByVariant.clear();
                census.bindlessByVariant[variant] = draw.bindless[stage] == Bindless::Yes;
            }
            if (draw.bindless[stage] == Bindless::Yes) set(FastCensusReason::Bindless);
            continue;
        }
        const auto found = variant == 0 ? census.bindlessByVariant.end() : census.bindlessByVariant.find(variant);
        if (found == census.bindlessByVariant.end()) ++census.bindlessUnknown;
        else if (found->second) set(FastCensusReason::Bindless);
    }
    census.tally.Note(reasons);
    if (draw.keyed) {
        ++census.keyed;
        ++census.descriptors[FastCensusDescriptorBucket(draw.descriptors)];
        census.maxDescriptors = std::max(census.maxDescriptors, draw.descriptors);
        if (census.full.size() > (1u << 20u)) census.full.clear();
        if (census.state.size() > (1u << 20u)) census.state.clear();
        if (census.stateNoTargets.size() > (1u << 20u)) census.stateNoTargets.clear();
        census.full.insert(draw.keys.full);
        census.state.insert(draw.keys.state);
        census.stateNoTargets.insert(draw.keys.stateNoTargets);
        if (run.keyed) {
            if (draw.keys.full != run.last.full) ++census.fullChanges;
            if (draw.keys.state != run.last.state) ++census.stateChanges;
            if (draw.keys.stateNoTargets != run.last.stateNoTargets) ++census.stateNoTargetChanges;
        }
        run.keyed = true;
        run.last = draw.keys;
        census.reads += draw.reads;
        census.blockReads += draw.blockReads;
        census.exactReads += draw.exactReads;
        if (draw.blockReads != 0) ++census.blockDraws;
    }
    if (reasons == 0) {
        ++run.length;
    } else {
        if (queue == 0) census.runs.Close(run.length, FastCensusBreak::DeclinedDraw);
        run.length = 0;
    }
    reportLocked(census);
}

void NoteFastCensusSubmissionEnd(std::uint32_t queue) {
    if (!FastCensusActive()) return;
    closeRun(queue, FastCensusBreak::SubmissionEnd);
}

}
