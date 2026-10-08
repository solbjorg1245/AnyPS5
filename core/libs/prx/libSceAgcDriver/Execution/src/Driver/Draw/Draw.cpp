#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

struct DrawScratchTag {};

bool drawScratchEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_SCRATCH") == nullptr;
    return enabled;
}

// A draw whose captured regions no longer agree with the memory serving them (AddSnapshot's
// Graphics::SnapshotStale, ~95 G-buffer draws per 10 s in t360) was dropped: the packet runs again
// instead, each retry stricter. APS5_NO_SNAPSHOT_RETRY=1: dropped as before.
bool snapshotRetryEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SNAPSHOT_RETRY") == nullptr;
    return enabled;
}

// The retry of the packet this thread draws: 1 captures every stage fresh (no draw-cache entry,
// so no stored stage bytes) with every word a recorded write covers read through the hook
// (Driver::forcedSyncReads), 2 also captures under the GPU mutex (nothing records between the
// capture and the draw), 3 also binds a range that still differs as it is (the memory the GPU
// reads, as on hardware). The CPU-indirect records before `resumeRecord` were drawn already.
struct SnapshotRetry {
    std::uint32_t level = 0;
    std::uint32_t resumeRecord = 0;
};

constexpr std::uint32_t SnapshotRetryLevels = 3;

// APS5_NO_SNAPSHOT_SERVE=1: no last retry; a draw still differing after the locked capture is
// dropped as before, so serving the mismatch (words the recompiler baked from the capture beside
// live memory) can be A/B'd apart from the recaptures.
std::uint32_t snapshotRetryLimit() {
    static const std::uint32_t limit = std::getenv("APS5_NO_SNAPSHOT_SERVE") == nullptr ? SnapshotRetryLevels : SnapshotRetryLevels - 1;
    return limit;
}

SnapshotRetry& snapshotRetry() {
    static thread_local SnapshotRetry retry;
    return retry;
}

}

DrawScratchLease::DrawScratchLease() {
    if (drawScratchEnabled()) {
        auto& shared = HostThreadLocal<DrawScratch, DrawScratchTag>();
        if (shared.depth == 0) scratch = &shared;
    }
    if (scratch == nullptr) {
        owned = std::make_unique<DrawScratch>();
        scratch = owned.get();
    }
    ++scratch->depth;
}

DrawScratchLease::~DrawScratchLease() {
    --scratch->depth;
}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected) {
    PerformanceTimer timing("Driver.Draw");
    DrawScratchLease scratch;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    std::uint64_t captures = 0;
    auto phaseLap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    DrawPhaseTiming phaseTiming{profile, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[DrawRowPrologue] = std::chrono::duration<double, std::milli>(phaseLap - packetStartedAt()).count();

    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    auto drawParameters = Pm4::ResolveDraw(packet, queue);
    bool traceIndirect = false;
    if (const auto verdict = precheckDraw(queue, submission, packet, drawParameters, rejected, traceIndirect)) return *verdict;
    phaseTiming.Phase(DrawRowPrecheck);
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;

    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const bool lockedPrepareAlways = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr;
    auto& retry = snapshotRetry();
    const auto resumeRecord = retry.resumeRecord;
    const bool lockedPrepare = lockedPrepareAlways || retry.level >= 2;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    std::shared_ptr<VulkanDevice> localDevice;
    if (lockedPrepare) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    } else if ((localDevice = device.Load()) == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard createLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_setup");
    phaseTiming.Phase(DrawRowVectors);

    const bool useDrawEntries = drawEntries() && !ShaderRecompiler::DebugProbeActive() && dumpTarget == 0 && dumpSlot1 == 0 && retry.level == 0;
    const bool registerKey = useDrawEntries && registerKeyEnabled();
    DrawKey drawKey;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    auto& relocation = scratch->relocation;
    relocation.Reset();
    if (registerKey) {
        const auto keyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        drawKey = drawRegisterKey(queue, *submission.shaders, localDevice->Serial());
        std::lock_guard cacheLock(drawCacheMutex);
        ++drawEntryCounters.lookups;
        ++drawEntryCounters.registerKeyLookups;
        if (profile) drawEntryCounters.keyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count();
        const auto found = drawCache.find(drawKey.key);
        if (found != drawCache.end()) {
            entry = found->second;
            decode = entry->decode;
        } else {
            ++drawEntryCounters.absent;
            noteAbsentDrawKeyLocked(drawKey.key, drawKey.base, &drawKey);
            // A new key under a known base key: its entry, relocated by the pointer pairs' delta
            // (DrawRelocation.cpp), stands in and moves to the new key on a hit or a miss.
            if (drawRelocation()) {
                const auto findStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                findRelocationCandidates(drawKey, relocation.candidates);
                if (!relocation.candidates.empty()) ++drawEntryCounters.relocationCandidates;
                if (profile) drawEntryCounters.relocationFindUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - findStart).count();
            }
        }
    }
    phaseTiming.Phase(DrawRowKeyLookupValidate);
    DrawRelocation* relocating = nullptr;
    if (!relocation.candidates.empty()) {
        relocating = &relocation;
        relocation.target = drawKey;
        const auto chooseStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool chosen = chooseRelocationCandidate(relocation);
        const auto chooseEnd = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (chosen) {
            entry = relocation.entry;
            // The candidate's decode with the live pointer words: the same registers otherwise.
            relocation.decode = relocatedDecode(*entry->decode, drawKey);
            decode = relocation.decode;
        }
        if (profile) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.relocationChooseUs += std::chrono::duration<double, std::micro>(chooseEnd - chooseStart).count();
            if (chosen) drawEntryCounters.relocationDecodeUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - chooseEnd).count();
        }
    }

    resolveDrawDecode(queue, submission, decode, registerKey, drawKey.key, profile);
    const auto& graphics = decode->state;
    const auto& pixel = decode->pixel;
    auto& programs = scratch->programs;
    programs = decode->programs;
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = programs.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters, programs.front().binary.codeAddress);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    if (!drawParameters.indirect) setMeshIndexBuffer(drawParameters);
    else if (graphics.stages.mesh) setMeshIndexBuffer(Pm4::DrawParameters{drawParameters.indexAddress, std::max(drawParameters.indexCount, 1u), drawParameters.indexSize, 1, 0, drawParameters.indexed});
    const std::vector<Role>& roles = decode->roles;
    phaseTiming.Phase(DrawRowDecode);

    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        if (location == 0x280u) return std::nullopt;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::Fragment || roles[i] == Role::GeometryBack || location < programs[i].userDataBase) continue;
            const auto word = location - programs[i].userDataBase + (8u - programs[i].firstUserSgpr);
            if (word < programs[i].userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
        }
        return std::nullopt;
    };
    if (drawParameters.indirect) {
        auto& indirect = *drawParameters.indirect;
        const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
            const auto word = locate(location);
            if (!word || word->first != 0) return -1;
            return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
        };
        indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
        indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
        indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
    }
    auto& memory = scratch->memory;
    memory.clear();
    memory.reserve(2 * programs.size() + 8);
    auto& linked = scratch->linked;
    linked.clear();
    linked.reserve(programs.size());
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        memory.insert(memory.end(), program.memory.begin(), program.memory.end());
        linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    timing.Mark("prepare");
    phaseTiming.Phase(DrawRowProgramPrepare);

    auto& vertexInfos = scratch->vertexInfos;
    vertexInfos.assign(programs.size(), std::nullopt);
    auto& decodeReads = scratch->decodeReads;
    decodeReads.resize(programs.size());
    for (auto& reads : decodeReads) reads.clear();
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        decodeReads[i].clear();
        vertexInfos[i] = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decodeReads[i]);
    };
    if (!registerKey) {
        for (std::size_t i = 0; i < programs.size(); ++i) decodeVertexInfo(i);
        phaseTiming.Phase(DrawRowDecode);
    }
    ShaderMemory shaderMemory(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    // Shared with the stage captures and the draw cache: a result is immutable once compiled, and
    // the deep copy each stage of each draw made (bindings with their descriptor words) was a fifth
    // of the draw thread. A slot replaced later (rect list, CPU-indirect patching) re-points the
    // stages that referenced the old object.
    auto& results = scratch->results;
    auto& stages = scratch->stages;
    results.clear();
    stages.clear();
    results.reserve(programs.size() + (graphics.rectList ? 2u : 0u));
    stages.reserve(programs.size());
    std::uint32_t pushCursorBytes = 0;

    auto& programResults = scratch->programResults;
    programResults.assign(programs.size(), nullptr);

    auto& stageCaptures = scratch->stageCaptures;
    stageCaptures.clear();
    stageCaptures.resize(programs.size());
    auto& matched = scratch->matched;
    matched.assign(programs.size(), nullptr);
    auto& matchedRegions = scratch->matchedRegions;
    matchedRegions.resize(programs.size());
    for (auto& regions : matchedRegions) regions.clear();
    auto& hits = scratch->hits;
    hits.liveWords.clear();
    hits.results.clear();
    hits.data = false;
    hits.partial = false;

    auto& fresh = scratch->fresh;
    fresh.assign(programs.size(), nullptr);

    auto& recompiled = scratch->recompiled;
    recompiled.assign(programs.size(), false);
    bool drawHit = false;
    bool verifyHit = false;
    if (Graphics::CheckpointsRequested() && !programs.empty()) Graphics::SetCheckpointWork(programs.front().binary.codeAddress, programs.back().binary.codeAddress);
    lookupDraw(submission, localDevice, graphics, pixel, programs, roles, vertexInfos, useDrawEntries, registerKey, profile, drawKey.key, entry, matched, matchedRegions, hits, drawHit, verifyHit, phaseTiming, phaseMs, relocating);

    if (registerKey) {

        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (matched[i] != nullptr && (drawHit || hits.partial) && !verifyDrawRecipe()) {
                if (matched[i]->vertexInfo != nullptr) vertexInfos[i] = *matched[i]->vertexInfo;
                continue;
            }
            decodeVertexInfo(i);
            if (matched[i] != nullptr && verifyDrawRecipe() && (vertexInfos[i].has_value() != (matched[i]->vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *matched[i]->vertexInfo)))) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit has a vertex stage info unlike its variant's\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress));
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.verifyDecodeMismatches;
            }
        }
        phaseTiming.Phase(DrawRowDecode);
    }

    static const bool indxOffsetSkipFold = std::getenv("APS5_INDX_OFFSET_SKIP_FOLD") != nullptr;
    static const bool indexedOffsetFold = std::getenv("APS5_NO_INDEXED_OFFSET_FOLD") == nullptr;
    const auto fold = [&](const ShaderRecompiler::RecompileResult& main, Pm4::DrawParameters& parameters) {
        if (parameters.indexed && !indexedOffsetFold) return;
        if (main.vertexOffsetSgpr >= 0 && (parameters.firstVertex == 0 || !indxOffsetSkipFold)) {
            const auto offset = drawUserWord(programs.front(), main.vertexOffsetSgpr);
            require(offset <= std::numeric_limits<std::uint32_t>::max() - parameters.firstVertex, "draw vertex offset overflow");
            parameters.firstVertex += offset;
        }
        if (main.instanceOffsetSgpr >= 0) parameters.firstInstance = drawUserWord(programs.front(), main.instanceOffsetSgpr);
    };

    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    auto& pushOffsets = scratch->pushOffsets;
    pushOffsets.assign(programs.size(), 0);
    auto& resultIndex = scratch->resultIndex;
    resultIndex.assign(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (roles[i] == Role::GeometryBack) continue;
        const auto& program = programs[i];
        pushOffsets[i] = pushCursorBytes;
        if (matched[i] != nullptr && (drawHit || hits.partial)) {

            programResults[i] = hits.results[i].get();
            memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
        } else {
            resultIndex[i] = results.size();
            results.push_back(compileDrawStage(i, pushCursorBytes, queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit || hits.partial, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs));
            programResults[i] = results.back().get();
        }
        const auto& result = *programResults[i];
        if (i == 0 && drawParameters.indirect) {
            indirectCpu = classifyIndirectDraw(result, graphics, programs.front(), localDevice, drawParameters, traceIndirect);
        } else if (i == 0) {
            fold(result, drawParameters);
        }
        require(result.pushConstants.size() <= Graphics::PipelinePushConstantBytes - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
        stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes});
        pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
    }

    cacheDrawStages(useDrawEntries, drawHit, drawParameters, indirectCpu, programs, stageCaptures, vertexInfos, decodeReads, verifyHit, matched, hits, fresh, drawKey, registerKey, decode, phaseTiming, relocating);
    // APS5_FAST_CENSUS: what the fast path would decline on, committed when the packet ends (F0).
    if (FastCensusActive()) NoteFastCensusDraw(queue, graphics, roles, programs, programResults, recompiled, stageCaptures, matchedRegions, decodeReads, drawParameters.indirect.has_value(), indirectCpu, ShaderRecompiler::DebugProbeActive() || dumpTarget != 0 || dumpSlot1 != 0);
    timing.Mark("shader_compile_and_link");

    for (const auto& reads : decodeReads) {
        for (const auto& read : reads) memory.push_back({read.address, std::as_bytes(std::span(read.bytes))});
    }

    if (recordQueuedLabelsAfterCapture(submission.queue, memory)) return draw(queue, packet, submission, rejected);

    // The device's draw, true when it threw SnapshotStale and the packet is to run again
    // (retryStale); the last retry serves the mismatch, so it never gets here.
    const bool retryable = snapshotRetryEnabled() && retry.level < snapshotRetryLimit();
    const auto stale = [&](auto&& work) {
        try {
            work();
        } catch (const Graphics::SnapshotStale&) {
            if (!retryable) throw;
            return true;
        }
        return false;
    };
    const auto retryStale = [&](std::uint32_t resume) {
        struct Restore {
            SnapshotRetry& retry;
            SnapshotRetry state;
            bool& sync;
            bool syncBefore;
            bool& serves;
            bool servesBefore;
            ~Restore() {
                retry = state;
                sync = syncBefore;
                serves = servesBefore;
            }
        };
        auto& sync = forcedSyncReads();
        auto& serves = Graphics::ThreadServesStaleSnapshots();
        const Restore restore{retry, retry, sync, sync, serves, serves};
        retry.level = restore.state.level + 1;
        retry.resumeRecord = resume;
        sync = true;
        serves = retry.level >= SnapshotRetryLevels;
        Graphics::CountSnapshotRetry(retry.level, drawHit || hits.partial);
        if (gpuLock.owns_lock()) gpuLock.unlock();
        return draw(queue, packet, submission, rejected);
    };

    bool rectListBuilt = false;

    std::size_t rectIndex = 0;
    const auto buildRectList = [&] {
        phaseTiming.Phase(DrawRowVectors);
        require(programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr, "rect-list requires vertex and fragment programs");
        auto rectangle = ShaderRecompiler::BuildRectListShaders(*programResults[0], *programResults[1], localDevice->Target());
        if (rectListBuilt) {
            results[rectIndex] = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.control));
            results[rectIndex + 1] = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.evaluation));
            stages[1].program = results[rectIndex].get();
            stages[2].program = results[rectIndex + 1].get();
            phaseTiming.Phase(DrawRowRectList);
            return;
        }
        require(stages.size() == 2, "rect-list requires vertex and fragment programs");
        rectIndex = results.size();
        results.push_back(std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.control)));
        results.push_back(std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(rectangle.evaluation)));
        stages.insert(stages.begin() + 1, {{Stage::TessellationControl, results[rectIndex].get(), 0}, {Stage::TessellationEvaluation, results[rectIndex + 1].get(), 0}});
        rectListBuilt = true;
        phaseTiming.Phase(DrawRowRectList);
    };
    if (graphics.rectList) buildRectList();
    auto& snapshots = scratch->snapshots;
    const auto snapshot = [&] {
        snapshots.clear();
        snapshots.reserve(memory.size());
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
    };
    snapshot();
    timing.Mark("post_compile_prepare");
    const auto lockForDraw = [&] {
        if (gpuLock.owns_lock()) return;
        phaseTiming.Phase(DrawRowVectors);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);

        if (auto current = device.Load(); current != nullptr && current != localDevice) {
            static std::atomic<std::uint64_t> replaced{0};
            std::fprintf(stderr, "[draw] device replaced during unlocked preparation (%llu)\n", static_cast<unsigned long long>(++replaced));
            localDevice = std::move(current);
        }

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    };
    if (drawParameters.indirect && indirectCpu) {

        const auto indirect = *drawParameters.indirect;
        if (drawHit || hits.partial) {

            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (programResults[i] == nullptr || (hits.partial && matched[i] == nullptr)) continue;
                require(matched[i] != nullptr && hits.results[i].get() == programResults[i], "a draw hit's stage result is not its variant's");
                resultIndex[i] = results.size();
                results.push_back(hits.results[i]);
            }
        }
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
        auto& records = scratch->records;
        records.clear();
        records.reserve(count);
        for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
        Graphics::CountIndirectDraw(*indirectCpu, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        const auto baseVertexWord = locate(indirect.baseVertexLocation);
        const auto startInstanceWord = locate(indirect.startInstanceLocation);
        const auto drawIndexWord = indirect.drawIndexEnabled ? locate(indirect.drawIndexLocation) : std::nullopt;
        for (std::uint32_t record = resumeRecord; record < records.size(); ++record) {
            const auto& arguments = records[record];
            if (traceIndirect) std::fprintf(stderr, "[draw]   record %u: count %u instances %u first %u vertexOffset %u startInstance %u\n", record, arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance);
            if (arguments.count == 0 || arguments.instances == 0) continue;
            std::set<std::size_t> patched;
            const auto patch = [&](const std::optional<std::pair<std::size_t, std::size_t>>& word, std::uint32_t value) {
                if (!word) return;
                programs[word->first].userData[word->second] = value;
                patched.insert(word->first);
            };
            patch(baseVertexWord, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
            patch(startInstanceWord, arguments.firstInstance);
            patch(drawIndexWord, record);
            Pm4::DrawParameters direct{0, arguments.count, 0, arguments.instances, drawParameters.flags, drawParameters.indexed, 0, 0};
            if (drawParameters.indexed) {

                if (arguments.firstVertexOrIndex >= drawParameters.indexCount) continue;
                direct.indexAddress = drawParameters.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * drawParameters.indexSize;
                direct.indexCount = std::min(arguments.count, drawParameters.indexCount - arguments.firstVertexOrIndex);
                direct.indexSize = drawParameters.indexSize;
            } else {
                direct.firstVertex = indirect.indxOffset;
            }
            if (graphics.stages.mesh) {
                setMeshIndexBuffer(direct);
                patched.insert(0);
            }
            for (const auto programIndex : patched) {
                auto& result = results[resultIndex[programIndex]];
                const auto pushBytes = result->pushConstants.size();
                const auto* previous = result.get();
                decodeVertexInfo(programIndex);
                result = compileDrawStage(programIndex, pushOffsets[programIndex], queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit || hits.partial, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs);
                require(result->pushConstants.size() == pushBytes, "patched program changed its push constant layout");
                for (auto& stage : stages) {
                    if (stage.program == previous) stage.program = result.get();
                }
                programResults[programIndex] = result.get();
            }
            fold(*programResults[0], direct);
            if (graphics.rectList && patched.contains(0)) buildRectList();
            snapshot();
            if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
                rejected = std::move(*known);
                return DrawVerdict::Rejected;
            }
            lockForDraw();
            noteDrawWriters(stages, submission.queue);
            phaseTiming.Phase(DrawRowVectors);
            if (stale([&] { localDevice->Draw(graphics, direct, stages, snapshots); })) return retryStale(record);
            phaseTiming.Phase(DrawRowGraphics);
        }
        timing.Mark("draw_and_resource_release");
        return drawn();
    }

    auto& recipeStages = scratch->recipeStages;
    recipeStages.clear();
    // A recipe replays the resources recorded with its variants' words: not for refreshed data.
    if (registerKey && !drawParameters.indirect && Graphics::DrawRecipes() && !hits.data) {
        recipeStages.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) recipeStages.push_back(matched[i] != nullptr && (drawHit || hits.partial) ? matched[i] : fresh[i]);
        if (std::all_of(recipeStages.begin(), recipeStages.end(), [](const std::shared_ptr<DispatchVariant>& variant) { return variant == nullptr; })) recipeStages.clear();
    }
    std::shared_ptr<const DrawRecipe> recipe;
    if (drawHit && !recipeStages.empty()) {
        recipe = findDrawRecipe(drawKey.key, recipeStages);
        if (recipe == nullptr) VulkanDevice::NoteDrawRecipeMiss(VulkanDevice::DrawRecipePrecheck::NoRecipe);
    }
    if (recipe == nullptr) {
        if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
            rejected = std::move(*known);
            return DrawVerdict::Rejected;
        }
    }
    lockForDraw();
    noteDrawWriters(stages, submission.queue);
    phaseTiming.Phase(DrawRowVectors);
    if (recipe != nullptr) {
        bool recorded = false;
        if (stale([&] { recorded = localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe) == RecipeOutcome::Recorded; })) return retryStale(0);
        if (recorded) {
            phaseTiming.Phase(DrawRowGraphics);
            timing.Mark("draw_and_resource_release");
            return drawn();
        }

        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    if (stale([&] { localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built); })) return retryStale(0);
    phaseTiming.Phase(DrawRowGraphics);
    if (built != nullptr) attachDrawRecipe(drawKey.key, recipeStages, std::move(built));
    timing.Mark("draw_and_resource_release");
    return drawn();
}

}
