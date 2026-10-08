#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

using Decline = Graphics::FastDispatchDecline;
constexpr auto DeclineCount = static_cast<std::size_t>(Decline::Count);
constexpr auto WalkDeclineCount = static_cast<std::size_t>(WalkDecline::Count);
constexpr auto MismatchCount = static_cast<std::size_t>(WalkMismatch::Count);

// The [fastpath] dispatches line's counters (10 s windows under APS5_PROFILE_DRAW): the phases of
// taken dispatches in nanoseconds (walk, variant, verify, prepare, lock wait, resolve, record, whole
// call), the elements they bound in place with an offset adjustment (no verify covers those), the
// declines by reason and the walk's own by reason, and the verify compare's results.
struct Counters {
    std::uint64_t dispatches = 0, taken = 0, takenIndirect = 0, leadSkipped = 0, ringFull = 0, adjusted = 0;
    std::uint64_t walkNs = 0, variantNs = 0, verifyNs = 0, prepareNs = 0, lockNs = 0, resolveNs = 0, recordNs = 0, totalNs = 0;
    std::array<std::uint64_t, DeclineCount> declines{};
    std::array<std::uint64_t, WalkDeclineCount> walkDeclines{};
    std::uint64_t verified = 0, verifyMismatched = 0, feedbackOnly = 0, flatFeedback = 0, deferredSkipped = 0;
    std::array<std::uint64_t, MismatchCount> mismatches{};
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

HostMutex countersMutex;
Counters counters;

struct Scratch {
    ShaderRecompiler::ResourceSnapshot snapshot;
    ShaderRecompiler::ResourceSpecialization specialization;
    ShaderRecompiler::RecompileResult walked;
};
struct ScratchTag {};

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

// The modes whose diagnostics need the old path's capture or build (design 2.11): probes, dumps,
// traces, synchronous dispatches, the barrier validator, a locked device; and the modes the fast
// dispatch has no counterpart of: GPU results kept out of guest memory (APS5_GPU_NO_WRITEBACK,
// MarkGpuWrites' SkipWriteBack), queued labels without the table the reader asks
// (APS5_NO_QUEUED_LABELS, APS5_NO_LABEL_BATCHING: the old path's recordQueuedLabelsAfterCapture
// re-capture covers them).
bool debugMode() {
    static const bool active = [] {
        for (const char* name : {"APS5_PROBE_DISPATCH", "APS5_DUMP_SHADERS", "APS5_TRACE_DISPATCH_IO", "APS5_TRACE_DISPATCH_CACHE", "APS5_SYNC_DISPATCH", "APS5_TRACE_CAPSYNC", "APS5_CAPTURE_INPUTS", "APS5_NO_UNLOCKED_DEVICE", "APS5_GPU_NO_WRITEBACK"}) {
            if (std::getenv(name) != nullptr) return true;
        }
        return !QueuedLabelTable() || Graphics::Recorder::BarrierValidate();
    }();
    return active;
}

void report(const Counters& total) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto taken = static_cast<double>(total.taken != 0 ? total.taken : 1);
    const auto perTaken = [&](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0 / taken; };
    std::string declines;
    std::string walks;
    std::string kinds;
    char item[96];
    for (std::size_t reason = 0; reason < DeclineCount; ++reason) {
        std::snprintf(item, sizeof(item), "%s%s %llu", reason == 0 ? "" : ", ", Graphics::FastDispatchDeclineNames[reason], count(total.declines[reason]));
        declines += item;
    }
    for (std::size_t reason = 0; reason < WalkDeclineCount; ++reason) {
        std::snprintf(item, sizeof(item), "%s%s %llu", reason == 0 ? "" : ", ", WalkDeclineNames[reason], count(total.walkDeclines[reason]));
        walks += item;
    }
    for (std::size_t kind = 0; kind < MismatchCount; ++kind) {
        std::snprintf(item, sizeof(item), "%s%s %llu", kind == 0 ? "" : ", ", WalkMismatchNames[kind], count(total.mismatches[kind]));
        kinds += item;
    }
    std::fprintf(stderr, "[fastpath] dispatches (10 s): %llu seen, %llu taken (%.1f%%, %llu indirect); us per taken: walk %.2f, variant %.2f, verify %.2f, prepare %.2f, lock wait %.2f, resolve %.2f, record %.2f, total %.2f; lead barriers skipped %llu, ring full %llu, adjusted in place %llu; declines: %s; walk declines: %s; verify: %llu compared, %llu mismatched (%s), T# feedback-only %llu, flat T# copies feedback-only %llu, deferred words skipped %llu\n", count(total.dispatches), count(total.taken), 100.0 * static_cast<double>(total.taken) / static_cast<double>(total.dispatches != 0 ? total.dispatches : 1), count(total.takenIndirect), perTaken(total.walkNs), perTaken(total.variantNs), perTaken(total.verifyNs), perTaken(total.prepareNs), perTaken(total.lockNs), perTaken(total.resolveNs), perTaken(total.recordNs), perTaken(total.totalNs), count(total.leadSkipped), count(total.ringFull), count(total.adjusted), declines.c_str(), walks.c_str(), count(total.verified), count(total.verifyMismatched), kinds.c_str(), count(total.feedbackOnly), count(total.flatFeedback), count(total.deferredSkipped));
}

}

bool Driver::fastDispatchEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FAST_DISPATCH");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool Driver::fastDispatchVerify() {
    static const bool verify = fastDispatchEnabled() && std::getenv("APS5_FAST_DISPATCH_VERIFY") != nullptr;
    return verify;
}

bool Driver::fastDispatch(const Submission& submission, const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<VulkanDevice>& localDevice, std::uint64_t address, const std::array<std::uint32_t, 3>& groups, std::uint64_t indirectArguments) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = std::chrono::steady_clock::now();
    Counters local;
    local.dispatches = 1;
    std::optional<Decline> declined;
    WalkDecline walkDecline = WalkDecline::Count;
    bool taken = false;
    // A throw from the packet's labels or the writer evidence is the packet's, as on the old path
    // (the labels' queue is cleared by then: falling back would lose them).
    bool packetStep = false;
    Graphics::FastDispatchTiming timing;
    try {
        auto& scratch = HostThreadLocal<Scratch, ScratchTag>();
        if (debugMode()) declined = Decline::Debug;
        else if (localDevice == nullptr) declined = Decline::NoDevice;
        // Without the pending-block table the reader could not see recorded GPU writes.
        else if (!Graphics::Recorder::PendingBlocksTracked()) declined = Decline::Untracked;
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
        if (!declined) {
            // A poisoned source (FailureMemo) is the old path's to skip.
            const std::string* poisoned = nullptr;
            handle = SourceHandleFor(snapshot, codeOffset, localDevice->Serial(), request, false, &poisoned);
            if (handle == nullptr) {
                declined = Decline::Walk;
                walkDecline = WalkDecline::NoSource;
            }
        }
        // The walk over the live user words with the direct reader (the registered code and header
        // first, as ShaderMemory serves them).
        auto lap = std::chrono::steady_clock::now();
        if (!declined) {
            FastReader reader{{}, memory};
            ShaderRecompiler::SrtRuntime runtime;
            runtime.userContext = &reader;
            runtime.readMemory = &FastSrtRead;
            runtime.readSpecializationMemory = &FastSrtRead;
            runtime.expressRead = &FastSrtRead;
            const auto status = ShaderRecompiler::WalkResources(*handle, request.context.userData, address, runtime, scratch.snapshot, scratch.specialization);
            local.walkNs = nanosecondsSince(lap);
            if (status != ShaderRecompiler::WalkStatus::Walked) {
                declined = Decline::Walk;
                walkDecline = WalkDeclineOf(status, reader);
            }
        }
        // The variant the source compiled for this specialization, populated over the walk: a
        // specialization not compiled yet is the old path's to compile (its capture registers it).
        lap = std::chrono::steady_clock::now();
        if (!declined) {
            if (!ShaderRecompiler::PopulateVariant(*handle, request, scratch.snapshot, scratch.specialization, scratch.walked)) declined = Decline::VariantMiss;
            else if (scratch.walked.bdaAbiVersion != 0) declined = Decline::Bda;
            local.variantNs = nanosecondsSince(lap);
        }
        // APS5_FAST_DISPATCH_VERIFY: the old path's capture of the same request (dry: nothing is
        // recorded) and its result, compared binding word by binding word; any difference declines.
        lap = std::chrono::steady_clock::now();
        if (!declined && fastDispatchVerify()) {
            ++local.verified;
            ShaderMemory shaderMemory(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
            const auto capture = shaderMemory.Capture(request, handle.get());
            auto captured = request;
            const auto regions = shaderMemory.Regions();
            captured.context.memory = regions;
            const auto old = ShaderRecompiler::Recompile(captured, *capture);
            WalkDifference first;
            // A word the old capture left to the GPU is a mismatch here: the walk bound a word whose
            // producer the reader did not see.
            const auto kinds = CompareWalkedResults(*old, scratch.walked, first, local.feedbackOnly, local.flatFeedback, local.deferredSkipped, true);
            if (kinds != 0) {
                ++local.verifyMismatched;
                for (std::size_t kind = 0; kind < MismatchCount; ++kind) {
                    if ((kinds & (1u << kind)) != 0) ++local.mismatches[kind];
                }
                declined = Decline::Verify;
                static std::atomic<std::uint32_t> printed{0};
                if (printed.fetch_add(1, std::memory_order_relaxed) < 20) {
                    std::fprintf(stderr, "[fastpath] dispatch verify mismatch: program 0x%llx: %s (kinds 0x%x), binding %zu word %zu: old %08x, walk %08x; variant old %llu, walk %llu\n", static_cast<unsigned long long>(address), first.kind < WalkMismatch::Count ? WalkMismatchNames[static_cast<std::size_t>(first.kind)] : "?", kinds, first.binding, first.word, first.old, first.walked, static_cast<unsigned long long>(old->variantId), static_cast<unsigned long long>(scratch.walked.variantId));
                }
            }
            local.verifyNs = nanosecondsSince(lap);
        }
        // The declines that need no lock and the variant's pipeline, outside the mutex every queue takes.
        Graphics::FastDispatchCall call;
        if (!declined) {
            lap = std::chrono::steady_clock::now();
            declined = localDevice->PrepareFastDispatch(scratch.walked, groups[0], groups[1], groups[2], indirectArguments, address, call);
            local.prepareNs = nanosecondsSince(lap);
        }
        if (!declined) {
            lap = std::chrono::steady_clock::now();
            GuestMemory::TagGpuLockSite(indirectArguments != 0 ? GuestMemory::GpuLockSite::Indirect : GuestMemory::GpuLockSite::Dispatch);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            local.lockNs = nanosecondsSince(lap);
            // As the old path under the lock: the packet's labels, then the writer evidence
            // (before the record when keyed by writer, after it otherwise).
            packetStep = true;
            recordLabelsForPacket(localDevice.get(), submission.queue);
            const bool noteWrites = writeEvidenceEnabled() || traceCapSync();
            if (noteWrites && writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, scratch.walked);
            packetStep = false;
            declined = localDevice->FastDispatch(call, timing);
            if (!declined && noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, scratch.walked);
            taken = !declined;
        }
    } catch (const std::exception&) {
        // Recorded commands cannot be taken back: the error is the packet's, as on the old path.
        if (timing.recorded || packetStep) throw;
        declined = Decline::Exception;
    }
    if (taken) {
        local.taken = 1;
        local.takenIndirect = indirectArguments != 0 ? 1 : 0;
        local.resolveNs = timing.resolveNs;
        local.recordNs = timing.recordNs;
        local.totalNs = nanosecondsSince(started);
        local.leadSkipped = timing.leadSkipped ? 1 : 0;
        local.adjusted = timing.adjusted;
    } else {
        local.walkNs = local.variantNs = local.verifyNs = local.prepareNs = local.lockNs = 0;
        if (declined) ++local.declines[static_cast<std::size_t>(*declined)];
        if (walkDecline != WalkDecline::Count) ++local.walkDeclines[static_cast<std::size_t>(walkDecline)];
    }
    // The counters feed only the digest line: no cross-queue lock without it.
    if (!profile) return taken;
    local.ringFull = timing.ringFull ? 1 : 0;
    std::lock_guard lock(countersMutex);
    auto& total = counters;
    total.dispatches += local.dispatches;
    total.taken += local.taken;
    total.takenIndirect += local.takenIndirect;
    total.leadSkipped += local.leadSkipped;
    total.ringFull += local.ringFull;
    total.adjusted += local.adjusted;
    total.walkNs += local.walkNs;
    total.variantNs += local.variantNs;
    total.verifyNs += local.verifyNs;
    total.prepareNs += local.prepareNs;
    total.lockNs += local.lockNs;
    total.resolveNs += local.resolveNs;
    total.recordNs += local.recordNs;
    total.totalNs += local.totalNs;
    for (std::size_t reason = 0; reason < DeclineCount; ++reason) total.declines[reason] += local.declines[reason];
    for (std::size_t reason = 0; reason < WalkDeclineCount; ++reason) total.walkDeclines[reason] += local.walkDeclines[reason];
    total.verified += local.verified;
    total.verifyMismatched += local.verifyMismatched;
    total.feedbackOnly += local.feedbackOnly;
    total.flatFeedback += local.flatFeedback;
    total.deferredSkipped += local.deferredSkipped;
    for (std::size_t kind = 0; kind < MismatchCount; ++kind) total.mismatches[kind] += local.mismatches[kind];
    if (std::chrono::steady_clock::now() - total.lastReport >= std::chrono::seconds(10)) {
        report(total);
        total = Counters{};
    }
    return taken;
}

}
