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
// call), the whole calls of declined ones (what they spent before the old path took over), the
// elements they bound in place with an offset adjustment (no verify covers those), the declines by
// reason and the walk's own by reason, and the verify compare's results (a binding the old capture
// left words of to the GPU is the "deferred" mismatch kind). Taken dispatches also split their
// resolve (FastDispatchTiming) and count their in-place elements and the dispatches whose one
// registry scan let every element skip FlushPending; the dispatch walks' reads that met a pending
// block and those of them the exact ranges let through ("pending").
struct Counters {
    std::uint64_t dispatches = 0, taken = 0, takenIndirect = 0, leadSkipped = 0, ringFull = 0, adjusted = 0;
    std::uint64_t walkNs = 0, variantNs = 0, verifyNs = 0, prepareNs = 0, lockNs = 0, resolveNs = 0, recordNs = 0, totalNs = 0, declinedNs = 0;
    std::uint64_t buffersNs = 0, imagesNs = 0, samplersNs = 0, flushNs = 0, ringNs = 0, importsNs = 0, elements = 0, flushSkipped = 0;
    FastPendingReads pending;
    std::array<std::uint64_t, DeclineCount> declines{};
    std::array<std::uint64_t, WalkDeclineCount> walkDeclines{};
    std::uint64_t verified = 0, verifyMismatched = 0, feedbackOnly = 0, flatFeedback = 0;
    std::array<std::uint64_t, MismatchCount> mismatches{};
    // The verify's served words: compared with the old capture's reads (differing, not read by
    // it), and the sampled check against memory's final bytes.
    std::uint64_t servedCompared = 0, servedDiffering = 0, servedUnread = 0;
    FastServedCheck servedFinal;
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
    std::fprintf(stderr, "[fastpath] dispatches (10 s): %llu seen, %llu taken (%.1f%%, %llu indirect); us per taken: walk %.2f, variant %.2f, verify %.2f, prepare %.2f, lock wait %.2f, resolve %.2f, record %.2f, total %.2f; declined dispatches spent %.1f ms before declining; lead barriers skipped %llu, ring full %llu, adjusted in place %llu; declines: %s; walk declines: %s; verify: %llu compared, %llu mismatched (%s), T# feedback-only %llu, flat T# copies feedback-only %llu; resolve us per taken: buffers %.2f, images %.2f, samplers %.2f, flush %.2f, ring %.2f, imports %.2f; %.1f in-place elements per taken, flush skipped by one scan %llu (%s); dispatch walk reads in pending blocks %llu, read past by the exact ranges %llu%s%s; verify's served words: %llu compared with the old capture's (%llu differ, %llu it did not read), %llu with memory once their writes landed (%llu unreadable; differing: known value %llu, write evidence %llu, queued label %llu)\n", count(total.dispatches), count(total.taken), 100.0 * static_cast<double>(total.taken) / static_cast<double>(total.dispatches != 0 ? total.dispatches : 1), count(total.takenIndirect), perTaken(total.walkNs), perTaken(total.variantNs), perTaken(total.verifyNs), perTaken(total.prepareNs), perTaken(total.lockNs), perTaken(total.resolveNs), perTaken(total.recordNs), perTaken(total.totalNs), static_cast<double>(total.declinedNs) / 1e6, count(total.leadSkipped), count(total.ringFull), count(total.adjusted), declines.c_str(), walks.c_str(), count(total.verified), count(total.verifyMismatched), kinds.c_str(), count(total.feedbackOnly), count(total.flatFeedback), perTaken(total.buffersNs), perTaken(total.imagesNs), perTaken(total.samplersNs), perTaken(total.flushNs), perTaken(total.ringNs), perTaken(total.importsNs), static_cast<double>(total.elements) / taken, count(total.flushSkipped), Graphics::FastDispatchBatchedElements() ? "per dispatch" : "APS5_FAST_DISPATCH_PER_ELEMENT", count(total.pending.inBlocks), count(total.pending.readPast), FastPendingExact() ? "" : " (APS5_FAST_PENDING_BLOCKS: blocks only)", FastPendingServedText(total.pending).c_str(), count(total.servedCompared), count(total.servedDiffering), count(total.servedUnread), count(total.servedFinal.compared), count(total.servedFinal.unreadable), count(total.servedFinal.mismatched[0]), count(total.servedFinal.mismatched[1]), count(total.servedFinal.mismatched[2]));
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
        // APS5_FAST_DISPATCH_VERIFY: the words the walk serves without their final value, for the
        // verify below.
        const FastServedLogScope servedLog(fastDispatchVerify());
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
            local.pending.Add(reader);
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
        // The old result is a fresh capture (no cache hit, no stage compare), so the flat T# copy
        // excuse fires only when a T#'s feedback bits changed in memory between the walk and the
        // capture: "flat T# copies feedback-only" then comes with "T# feedback-only" bindings.
        lap = std::chrono::steady_clock::now();
        if (!declined && fastDispatchVerify()) {
            ShaderMemory shaderMemory(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
            const auto capture = shaderMemory.Capture(request, handle.get());
            auto captured = request;
            const auto regions = shaderMemory.Regions();
            captured.context.memory = regions;
            const auto old = ShaderRecompiler::Recompile(captured, *capture);
            // Counted once the old result exists: a capture or recompile that throws is an
            // exception decline, not a comparison.
            ++local.verified;
            WalkDifference first;
            // A binding the old capture left words of to the GPU is a mismatch here (kind
            // "deferred"): the walk bound words whose producer the reader did not see. Nothing is
            // skipped in this mode, so the skip count stays local.
            std::uint64_t deferredSkipped = 0;
            auto kinds = CompareWalkedResults(*old, scratch.walked, first, local.feedbackOnly, local.flatFeedback, deferredSkipped, true);
            // The words the walk served without their final value (known values, write evidence,
            // queued labels) against the capture's own reads of them: a difference is the "served
            // word" kind. On every 16th dispatch with any (each waits for the writes pending over
            // its words), also against the bytes memory holds once those writes landed: counted
            // apart by source, not a kind, since the capture at the same point serves an evidence
            // word's stale value the same way.
            if (const auto& served = FastServedWords().words; !served.empty()) {
                local.servedCompared += served.size();
                const auto differing = CompareServedWords(served, regions, local.servedUnread);
                local.servedDiffering += differing;
                static std::atomic<std::uint64_t> finalChecks{0};
                FastServedCheck check;
                if (finalChecks.fetch_add(1, std::memory_order_relaxed) % 16 == 0) CheckServedWords(served, "dispatch verify", check);
                local.servedFinal.compared += check.compared;
                local.servedFinal.unreadable += check.unreadable;
                for (std::size_t source = 0; source < check.mismatched.size(); ++source) local.servedFinal.mismatched[source] += check.mismatched[source];
                if (differing != 0) {
                    if (kinds == 0) first = {WalkMismatch::Served, 0, 0, 0, 0};
                    kinds |= 1u << static_cast<unsigned>(WalkMismatch::Served);
                }
            }
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
        local.buffersNs = timing.buffersNs;
        local.imagesNs = timing.imagesNs;
        local.samplersNs = timing.samplersNs;
        local.flushNs = timing.flushNs;
        local.ringNs = timing.ringNs;
        local.importsNs = timing.importsNs;
        local.elements = timing.elements;
        local.flushSkipped = timing.flushSkipped ? 1 : 0;
    } else {
        local.walkNs = local.variantNs = local.verifyNs = local.prepareNs = local.lockNs = 0;
        local.declinedNs = nanosecondsSince(started);
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
    total.declinedNs += local.declinedNs;
    total.buffersNs += local.buffersNs;
    total.imagesNs += local.imagesNs;
    total.samplersNs += local.samplersNs;
    total.flushNs += local.flushNs;
    total.ringNs += local.ringNs;
    total.importsNs += local.importsNs;
    total.elements += local.elements;
    total.flushSkipped += local.flushSkipped;
    total.pending.Add(local.pending);
    for (std::size_t reason = 0; reason < DeclineCount; ++reason) total.declines[reason] += local.declines[reason];
    for (std::size_t reason = 0; reason < WalkDeclineCount; ++reason) total.walkDeclines[reason] += local.walkDeclines[reason];
    total.verified += local.verified;
    total.verifyMismatched += local.verifyMismatched;
    total.feedbackOnly += local.feedbackOnly;
    total.flatFeedback += local.flatFeedback;
    total.servedCompared += local.servedCompared;
    total.servedDiffering += local.servedDiffering;
    total.servedUnread += local.servedUnread;
    total.servedFinal.compared += local.servedFinal.compared;
    total.servedFinal.unreadable += local.servedFinal.unreadable;
    for (std::size_t source = 0; source < total.servedFinal.mismatched.size(); ++source) total.servedFinal.mismatched[source] += local.servedFinal.mismatched[source];
    for (std::size_t kind = 0; kind < MismatchCount; ++kind) total.mismatches[kind] += local.mismatches[kind];
    if (std::chrono::steady_clock::now() - total.lastReport >= std::chrono::seconds(10)) {
        report(total);
        total = Counters{};
    }
    return taken;
}

}
