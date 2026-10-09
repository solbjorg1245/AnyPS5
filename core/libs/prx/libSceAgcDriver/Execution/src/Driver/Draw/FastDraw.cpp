#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastWalk.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

using Graphics::FastDecline;
constexpr std::size_t DeclineCount = static_cast<std::size_t>(FastDecline::Count);

// The front end's per-thread state: the draw's decode (Driver::fastDrawDecodeInto), per stage the
// walk's snapshot and specialization and the populated result, the stages handed to DrawFast, the
// linked programs of the compile requests.
struct FrontScratch {
    DrawDecode decode;
    std::array<ShaderRecompiler::ResourceSnapshot, 2> snapshots;
    std::array<ShaderRecompiler::ResourceSpecialization, 2> specializations;
    std::array<ShaderRecompiler::RecompileResult, 2> results;
    std::array<Graphics::CompiledShader, 2> stages{};
    std::vector<ShaderRecompiler::LinkedProgram> linked;
};
struct FrontScratchTag {};

// APS5_FAST_DRAW_VERIFY: the fast results of the draw the old path draws instead.
struct VerifyStash {
    bool armed = false;
    bool compared = false;
    std::size_t stages = 0;
    std::array<ShaderRecompiler::RecompileResult, 2> results;
};
struct VerifyStashTag {};

// The [fastpath] draws columns: the front end's parts, then DrawFast's (FastDrawOutcome).
enum FastPart : std::size_t { PartState, PartWalk, PartVariant, PartLock, PartLabels, PartInputs, PartTargets, PartBindings, PartPipeline, PartRecord, PartCount };
constexpr std::array<const char*, PartCount> FastPartNames{"state", "walk", "variant", "lock", "labels", "inputs", "targets", "bindings", "pipeline", "record"};
constexpr std::size_t VerifyKinds = 16;

struct FastDrawCounters {
    std::uint64_t offered = 0, taken = 0, indirect = 0, continued = 0, allocations = 0;
    std::array<double, PartCount> us{};
    double takenUs = 0;
    double declinedUs = 0;
    std::array<std::uint64_t, DeclineCount> declines{};
    // declinedUs by reason: the time a draw spent before declining for it.
    std::array<double, DeclineCount> declineUs{};
    std::uint64_t verifyDraws = 0, verifyStages = 0, verifyNotCompared = 0, verifyVertex = 0;
    // [0]: plain old results, [1]: results the old path bound by a heuristic.
    std::array<std::uint64_t, 2> verifyMismatched{};
    std::array<std::uint64_t, VerifyKinds> verifyKinds{};
    // The verified draws' served words (CheckServedWords): compared, unreadable, differing by source.
    FastServedCheck verifyServed;
    // The F3b walks' reads (vertex fetch and stage walks, taken and declined draws) that met a
    // pending block, and those the exact ranges let through.
    FastPendingReads pending;
};

struct FastDrawTotals {
    HostMutex mutex;
    FastDrawCounters counters;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

FastDrawTotals& totals() {
    static FastDrawTotals instance;
    return instance;
}

bool profileDraw() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

FastDecline walkDecline(FastWalkDecline decline) {
    switch (decline) {
        case FastWalkDecline::NoSource: return FastDecline::NoSource;
        case FastWalkDecline::Pending: return FastDecline::WalkPending;
        // The reader's storage-image and unit-shadow pages (FastSrtRead), counted with the pending blocks.
        case FastWalkDecline::PendingStorage: return FastDecline::WalkPending;
        case FastWalkDecline::Unmapped: return FastDecline::WalkUnmapped;
        case FastWalkDecline::QueuedLabel: return FastDecline::WalkQueuedLabel;
        case FastWalkDecline::NoProgram: return FastDecline::WalkNoProgram;
        case FastWalkDecline::IncompletePlan: return FastDecline::WalkIncomplete;
        case FastWalkDecline::Bindless: return FastDecline::WalkBindless;
        default: return FastDecline::WalkOther;
    }
}

void report(const FastDrawCounters& total) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto taken = static_cast<double>(total.taken != 0 ? total.taken : 1);
    std::string parts;
    std::string declines;
    char item[96];
    for (std::size_t part = 0; part < PartCount; ++part) {
        std::snprintf(item, sizeof(item), "%s%s %.2f", part == 0 ? "" : ", ", FastPartNames[part], total.us[part] / taken);
        parts += item;
    }
    for (std::size_t reason = 0; reason < DeclineCount; ++reason) {
        if (total.declines[reason] == 0) continue;
        std::snprintf(item, sizeof(item), "%s%s %llu (%.1f us each)", declines.empty() ? "" : ", ", Graphics::FastDeclineName(static_cast<FastDecline>(reason)), count(total.declines[reason]), total.declineUs[reason] / static_cast<double>(total.declines[reason]));
        declines += item;
    }
    std::fprintf(stderr, "[fastpath] draws (10 s): %llu offered, %llu taken (%.1f%%; %llu indirect, %llu continued a pass); us per taken draw: %s = %.2f; %.1f allocations per taken draw; declined draws spent %.1f ms before declining; declines: %s; draw walk reads in pending blocks %llu, read past by the exact ranges %llu%s%s\n", count(total.offered), count(total.taken), total.offered != 0 ? 100.0 * static_cast<double>(total.taken) / static_cast<double>(total.offered) : 0.0, count(total.indirect), count(total.continued), parts.c_str(), total.takenUs / taken, static_cast<double>(total.allocations) / taken, total.declinedUs / 1000.0, declines.empty() ? "none" : declines.c_str(), count(total.pending.inBlocks), count(total.pending.readPast), FastPendingExact() ? "" : " (APS5_FAST_PENDING_BLOCKS: blocks only)", FastPendingServedText(total.pending).c_str());
    if (FastDrawVerifyEvery() == 0) return;
    const auto bindings = Graphics::TakeFastVerifyCounts();
    std::string kinds;
    const auto names = FastWalkMismatchNames();
    for (std::size_t kind = 0; kind < names.size() && kind < VerifyKinds; ++kind) {
        std::snprintf(item, sizeof(item), "%s%s %llu", kind == 0 ? "" : ", ", names[kind], count(total.verifyKinds[kind]));
        kinds += item;
    }
    const auto& served = total.verifyServed;
    std::fprintf(stderr, "[fastpath] draw verify (10 s, every %u): %llu draws compared (%llu stages; %llu armed draws the old path left uncompared), mismatched stages %llu, after heuristic hits %llu; by kind: %s, vertex attributes %llu; bindings: %llu compared, %llu matched, %llu declined by the fast path, mismatches: layout %llu, buffer %llu, data %llu, image %llu, sampler %llu, push %llu; buffers the old path copied %llu; served words against memory once their pending writes landed: %llu compared (%llu unreadable), differing: known value %llu, write evidence %llu, queued label %llu\n", FastDrawVerifyEvery(), count(total.verifyDraws), count(total.verifyStages), count(total.verifyNotCompared), count(total.verifyMismatched[0]), count(total.verifyMismatched[1]), kinds.c_str(), count(total.verifyVertex), count(bindings.compared), count(bindings.matched), count(bindings.declined), count(bindings.layout), count(bindings.buffer), count(bindings.data), count(bindings.image), count(bindings.sampler), count(bindings.push), count(bindings.copied), count(served.compared), count(served.unreadable), count(served.mismatched[0]), count(served.mismatched[1]), count(served.mismatched[2]));
}

void commit(const FastDrawCounters& local) {
    // Nothing reports the counters without APS5_PROFILE_DRAW: no shared lock per offered draw then.
    if (!profileDraw()) return;
    auto& shared = totals();
    std::lock_guard lock(shared.mutex);
    auto& total = shared.counters;
    total.offered += local.offered;
    total.taken += local.taken;
    total.indirect += local.indirect;
    total.continued += local.continued;
    total.allocations += local.allocations;
    total.pending.Add(local.pending);
    if (local.taken != 0) {
        for (std::size_t part = 0; part < PartCount; ++part) total.us[part] += local.us[part];
    }
    total.takenUs += local.takenUs;
    total.declinedUs += local.declinedUs;
    for (std::size_t reason = 0; reason < DeclineCount; ++reason) {
        total.declines[reason] += local.declines[reason];
        total.declineUs[reason] += local.declineUs[reason];
    }
    total.verifyDraws += local.verifyDraws;
    total.verifyStages += local.verifyStages;
    total.verifyNotCompared += local.verifyNotCompared;
    total.verifyVertex += local.verifyVertex;
    for (std::size_t column = 0; column < 2; ++column) total.verifyMismatched[column] += local.verifyMismatched[column];
    for (std::size_t kind = 0; kind < VerifyKinds; ++kind) total.verifyKinds[kind] += local.verifyKinds[kind];
    total.verifyServed.compared += local.verifyServed.compared;
    total.verifyServed.unreadable += local.verifyServed.unreadable;
    for (std::size_t source = 0; source < total.verifyServed.mismatched.size(); ++source) total.verifyServed.mismatched[source] += local.verifyServed.mismatched[source];
    if (!profileDraw() || std::chrono::steady_clock::now() - shared.lastReport < std::chrono::seconds(10)) return;
    shared.lastReport = std::chrono::steady_clock::now();
    report(total);
    total = FastDrawCounters{};
}

bool sameAttributes(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& fast) {
    if (old.vertexAttributes.size() != fast.vertexAttributes.size()) return false;
    for (std::size_t i = 0; i < old.vertexAttributes.size(); ++i) {
        const auto& a = old.vertexAttributes[i];
        const auto& b = fast.vertexAttributes[i];
        if (a.location != b.location || a.components != b.components || a.fetchIndex != b.fetchIndex || a.resource.fields != b.resource.fields) return false;
    }
    return true;
}

}

bool FastDrawEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_FAST_DRAW");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return enabled;
}

bool FastDrawIndirect() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_FAST_DRAW_INDIRECT");
        return text == nullptr || std::strcmp(text, "0") != 0;
    }();
    return enabled;
}

std::uint32_t FastDrawVerifyEvery() {
    static const std::uint32_t every = [] {
        const char* text = std::getenv("APS5_FAST_DRAW_VERIFY");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 0u;
    }();
    return every;
}

bool FastDrawVerifyPending() {
    if (FastDrawVerifyEvery() == 0) return false;
    const auto& stash = HostThreadLocal<VerifyStash, VerifyStashTag>();
    return stash.armed && !stash.compared;
}

void VerifyFastDrawStages(std::span<const ShaderRecompiler::RecompileResult* const> old, bool heuristic) {
    auto& stash = HostThreadLocal<VerifyStash, VerifyStashTag>();
    if (!stash.armed || stash.compared) return;
    stash.compared = true;
    FastDrawCounters local;
    local.verifyDraws = 1;
    const auto column = heuristic ? 1u : 0u;
    for (std::size_t i = 0; i < stash.stages; ++i) {
        ++local.verifyStages;
        std::uint32_t kinds = 0;
        bool attributes = true;
        if (i >= old.size() || old[i] == nullptr) {
            // The old path bound no result for the stage the fast path populated: a layout difference.
            kinds = 1u << 2u;
        } else {
            kinds = CompareWalkedResult(*old[i], stash.results[i], true);
            attributes = sameAttributes(*old[i], stash.results[i]);
        }
        if (kinds == 0 && attributes) continue;
        ++local.verifyMismatched[column];
        if (!attributes) ++local.verifyVertex;
        for (std::size_t kind = 0; kind < VerifyKinds; ++kind) {
            if ((kinds & (1u << kind)) != 0) ++local.verifyKinds[kind];
        }
        static std::atomic<std::uint32_t> printed{0};
        if (printed.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[fastpath] draw verify: stage %zu%s differs from the old path's result: kinds 0x%x%s; variant old %llu, fast %llu\n", i, heuristic ? " after a heuristic hit" : "", kinds, attributes ? "" : ", vertex attributes", static_cast<unsigned long long>(i < old.size() && old[i] != nullptr ? old[i]->variantId : 0), static_cast<unsigned long long>(stash.results[i].variantId));
    }
    commit(local);
}

FastDrawVerifyScope::~FastDrawVerifyScope() {
    if (FastDrawVerifyEvery() == 0) return;
    auto& stash = HostThreadLocal<VerifyStash, VerifyStashTag>();
    if (!stash.armed) return;
    const bool compared = stash.compared;
    stash.armed = false;
    stash.compared = false;
    // An armed comparison Graphics::Draw did not reach (a recipe, a rejection, a CPU-side path).
    Graphics::ThreadFastVerifyArmed() = false;
    if (compared) return;
    FastDrawCounters local;
    local.verifyNotCompared = 1;
    commit(local);
}

std::optional<DrawVerdict> Driver::fastDraw(QueueState& queue, const Submission& submission, const Pm4::DrawParameters& drawParameters, std::string& rejected, bool traceIndirect, bool retrying, bool debugMode, DrawPhaseTiming& phaseTiming) {
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    const bool profile = profileDraw();
    FastDrawCounters local;
    local.offered = 1;
    // APS5_FAST_DRAW_VERIFY: the words this draw's walks serve without their final value, for the
    // verified draw's check below.
    const FastServedLogScope servedLog(FastDrawVerifyEvery() != 0);
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto lap = started;
    const auto heap = profile ? HostHeap::ThreadCounters() : HostHeap::Counters{};
    const auto part = [&](FastPart which) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        local.us[which] += std::chrono::duration<double, std::micro>(now - lap).count();
        lap = now;
    };
    const auto elapsedUs = [&] { return profile ? std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() : 0.0; };
    // The driver's rows as the fast path found them: a decline takes back what its phases charged
    // to decode, capture, recompile, vectors, lock wait, labels and Graphics::Draw.
    const auto entryPhaseMs = phaseTiming.profile ? phaseTiming.phaseMs : std::array<double, DrawDriverPhaseCount>{};
    const auto entryPhaseLap = phaseTiming.phaseLap;
    const auto declined = [&](FastDecline reason) -> std::optional<DrawVerdict> {
        ++local.declines[static_cast<std::size_t>(reason)];
        local.declinedUs = elapsedUs();
        local.declineUs[static_cast<std::size_t>(reason)] = local.declinedUs;
        // The whole fast attempt, from this function's entry, goes to its own row: the old path
        // that follows fills the other rows alone.
        if (phaseTiming.profile) {
            phaseTiming.phaseMs = entryPhaseMs;
            phaseTiming.phaseLap = entryPhaseLap;
        }
        phaseTiming.Phase(DrawRowFastDeclined);
        commit(local);
        return std::nullopt;
    };
    if (drawParameters.indirect && !FastDrawIndirect()) return declined(FastDecline::Indirect);
    if (retrying) return declined(FastDecline::Retry);
    if (debugMode) return declined(FastDecline::DebugMode);
    auto localDevice = device.Load();
    if (localDevice == nullptr) return declined(FastDecline::NoDevice);
    auto& scratch = HostThreadLocal<FrontScratch, FrontScratchTag>();
    auto parameters = drawParameters;
    // The decode lives in the thread's scratch, or in `heldDecode` for a state's first decode.
    std::shared_ptr<const DrawDecode> heldDecode;
    const DrawDecode* decode = nullptr;
    std::size_t count = 0;
    // A failure of the front end (a decode or a vertex fetch that throws, a bad offset SGPR) is a
    // decline at the step it happened in: the old path meets it with its own accounting.
    auto failure = FastDecline::Decode;
    try {
        // The draw state's decode with the live user words (F1's memo).
        decode = &fastDrawDecodeInto(queue, submission, scratch.decode, heldDecode);
        const auto& graphics = decode->state;
        const auto& programs = decode->programs;
        const auto& roles = decode->roles;
        count = programs.size();
        const bool vertexOnly = count == 1 && roles[0] == Role::Main;
        const bool vertexPixel = count == 2 && roles[0] == Role::Main && roles[1] == Role::Fragment;
        if (graphics.stages.path != Graphics::ShaderPath::Vertex || graphics.stages.mesh || graphics.stages.tessellation || graphics.rectList || !(vertexOnly || vertexPixel) || programs[0].binary.stage != Stage::Vertex) return declined(FastDecline::Shape);
        phaseTiming.Phase(DrawRowDecode);
        part(PartState);
        // The patched-SGPR locations of the indirect records (Driver::draw's `locate`): only the
        // vertex stage's user words can hold them.
        if (parameters.indirect) {
            auto& indirect = *parameters.indirect;
            const auto& front = programs.front();
            const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
                if (location == 0x280u || location < front.userDataBase) return -1;
                const auto word = location - front.userDataBase + (8u - front.firstUserSgpr);
                if (word >= front.userData.size()) return -1;
                return static_cast<std::int32_t>(front.firstUserSgpr + word);
            };
            indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
            indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
            indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
        }
        auto& linked = scratch.linked;
        linked.clear();
        for (std::size_t i = 0; i < count; ++i) linked.push_back({roles[i], programs[i].binary, programs[i].userDataBase, programs[i].firstUserSgpr, programs[i].userData});
        static const bool indxOffsetSkipFold = std::getenv("APS5_INDX_OFFSET_SKIP_FOLD") != nullptr;
        static const bool indexedOffsetFold = std::getenv("APS5_NO_INDEXED_OFFSET_FOLD") == nullptr;
        std::uint32_t pushCursor = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& program = programs[i];
            if (program.snapshot == nullptr) return declined(FastDecline::NoSource);
            failure = FastDecline::WalkOther;
            // compileDrawStage's request without memory regions: the walk reads live.
            const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
            ShaderRecompiler::RecompileRequest request{
                program.binary,
                {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(decode->pixel) : std::nullopt, std::nullopt, {}},
                localDevice->Target(),
                {0, 0, pushCursor, Graphics::PipelinePushConstantBytes - pushCursor},
                ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {parameters.indexAddress, parameters.indexCount, parameters.indexSize, parameters.instanceCount}}
            };
            // The vertex stage info from the live attribute and V# words (F2's direct reader),
            // resolved in the request's own (no copy of the 1 KiB info per stage).
            if (program.binary.stage != Stage::Fragment) {
                if (const auto why = FastResolveVertex(programs, program, request.context.vertex.emplace(), &local.pending)) return declined(walkDecline(*why));
            }
            failure = FastDecline::NoSource;
            const std::string* poisoned = nullptr;
            const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, localDevice->Serial(), request, false, &poisoned);
            if (handle == nullptr) return declined(FastDecline::NoSource);
            failure = FastDecline::WalkOther;
            if (const auto why = FastWalkStage(programs, *handle, program, scratch.snapshots[i], scratch.specializations[i], &local.pending)) return declined(walkDecline(*why));
            phaseTiming.Phase(DrawRowCapture);
            part(PartWalk);
            // The variant the specialization selects, populated over the live snapshot; a
            // specialization not compiled yet goes to the old path, which compiles it.
            failure = FastDecline::NoVariant;
            auto& result = scratch.results[i];
            if (!ShaderRecompiler::PopulateVariant(*handle, request, scratch.snapshots[i], scratch.specializations[i], result)) return declined(FastDecline::NoVariant);
            phaseTiming.Phase(DrawRowRecompile);
            part(PartVariant);
            if (result.pushConstants.size() > Graphics::PipelinePushConstantBytes - pushCursor) return declined(FastDecline::PushOverflow);
            scratch.stages[i] = {program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursor};
            pushCursor += static_cast<std::uint32_t>(result.pushConstants.size());
            if (i != 0) continue;
            failure = FastDecline::Decode;
            if (parameters.indirect) {
                // Only records the GPU reads as they are: a patched SGPR the fetch does not fold is CPU work.
                if (classifyIndirectDraw(result, graphics, programs.front(), localDevice, parameters, traceIndirect)) return declined(FastDecline::CpuIndirect);
            } else if (!parameters.indexed || indexedOffsetFold) {
                // Driver::draw's fold: the offset SGPRs the shader reads become the draw's first vertex and instance.
                if (result.vertexOffsetSgpr >= 0 && (parameters.firstVertex == 0 || !indxOffsetSkipFold)) {
                    const auto offset = drawUserWord(programs.front(), result.vertexOffsetSgpr);
                    if (offset > std::numeric_limits<std::uint32_t>::max() - parameters.firstVertex) return declined(FastDecline::Decode);
                    parameters.firstVertex += offset;
                }
                if (result.instanceOffsetSgpr >= 0) parameters.firstInstance = drawUserWord(programs.front(), result.instanceOffsetSgpr);
            }
        }
    } catch (const std::exception&) {
        return declined(failure);
    }
    const auto& graphics = decode->state;
    const std::span<const Graphics::CompiledShader> stages(scratch.stages.data(), count);
    // The bindings' structural declines (written elements, storage images, address roles, deferred
    // words) follow from the results alone: decided here, not under the lock after the inputs.
    if (const auto structural = Graphics::FastStructuralDecline(stages)) return declined(*structural);
    // APS5_FAST_DRAW_VERIFY: every Nth draw goes to the old path with the fast results kept for the compare.
    if (const auto every = FastDrawVerifyEvery(); every != 0) {
        static std::atomic<std::uint64_t> calls{0};
        if (calls.fetch_add(1, std::memory_order_relaxed) % every == 0) {
            auto& stash = HostThreadLocal<VerifyStash, VerifyStashTag>();
            stash.armed = true;
            stash.compared = false;
            stash.stages = count;
            for (std::size_t i = 0; i < count; ++i) stash.results[i] = scratch.results[i];
            // The served words (known values, write evidence, queued labels) against the bytes
            // memory holds once the writes pending over them landed (each read through the flush
            // hook, as the old capture's Sync read); the stage results are compared once the old
            // path drew the draw (VerifyFastDrawStages). A difference counts as the draw's
            // "served word" kind.
            if (const auto& served = FastServedWords().words; !served.empty()) {
                FastServedCheck check;
                CheckServedWords(served, "draw verify", check);
                local.verifyServed = check;
                if (check.Mismatched() != 0) ++local.verifyKinds[static_cast<std::size_t>(WalkMismatch::Served)];
            }
            Graphics::ThreadFastVerifyArmed() = true;
            return declined(FastDecline::Verify);
        }
    }
    if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
        rejected = std::move(*known);
        static_cast<void>(declined(FastDecline::Rejected));
        return DrawVerdict::Rejected;
    }
    if (Graphics::CheckpointsRequested()) Graphics::SetCheckpointWork(decode->programs.front().binary.codeAddress, decode->programs.back().binary.codeAddress);
    phaseTiming.Phase(DrawRowVectors);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
    std::unique_lock gpuLock(GuestMemory::GpuMutex());
    phaseTiming.Phase(DrawRowLockWait);
    part(PartLock);
    // The first draw after a device replacement goes to the old path, which adopts the new device.
    if (device.Load() != localDevice) {
        gpuLock.unlock();
        return declined(FastDecline::DeviceReplaced);
    }
    recordLabelsForPacket(localDevice.get(), submission.queue);
    phaseTiming.Phase(DrawRowLabels);
    part(PartLabels);
    const auto outcome = localDevice->FastDraw(graphics, parameters, stages);
    phaseTiming.Phase(DrawRowGraphics);
    gpuLock.unlock();
    if (!outcome.recorded) return declined(outcome.decline);
    local.taken = 1;
    if (parameters.indirect) local.indirect = 1;
    if (outcome.passContinued) local.continued = 1;
    if (profile) {
        local.us[PartInputs] = outcome.inputsUs;
        local.us[PartTargets] = outcome.targetsUs;
        local.us[PartBindings] = outcome.bindingsUs;
        local.us[PartPipeline] = outcome.pipelineUs;
        local.us[PartRecord] = outcome.recordUs;
        local.takenUs = elapsedUs();
        local.allocations = HostHeap::ThreadCounters().allocations - heap.allocations;
    }
    commit(local);
    return DrawVerdict::Drawn;
}

}
