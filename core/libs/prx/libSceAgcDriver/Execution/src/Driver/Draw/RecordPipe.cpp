#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordPipe.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/PassDump.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/HostThread.hpp"
#include "Recompiler.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

// Slot k = serial % Depth: a draw's whole DrawScratch (stages, results, snapshots and every byte they
// point into), the rest its locked half reads, and its queue state for a redo; or a dispatch (stage 3;
// the scratch idles). APS5_RECORD_PIPE_VERIFY: the item's overlay estimate.
struct RecordPipeSlot {
    RecordPipeKind kind = RecordPipeKind::Draw;
    DrawScratch scratch;
    RecordPipeDraw draw;
    QueueStateMark mark;
    RecordPipeDispatch dispatch;
    // APS5_LAZY_LABELS: a label item's bytes and packet opcode.
    DeferredLabel label{};
    std::uint32_t labelOpcode = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> estimate;
    // APS5_PROFILE_DRAW: when A published the item (B's wake latency).
    std::chrono::steady_clock::time_point published;
    // APS5_RECORD_PIPE_EPOCH: A's collect epoch at hand-over (RecordPipeEpochFor).
    std::uint64_t epoch = 0;
};

// The [pipe] line's dispatch counts (NoteRecordPipeDispatch): handed over (eligible; those writing a
// storage image apart), then the drains in RecordPipeDispatchVerdict's order, BDA by stores.
enum PipeDispatchCount : std::size_t { DispatchEligible, DispatchImageWrites, DispatchLocal, DispatchNoWindow, DispatchLabelsPending, DispatchBdaLoads, DispatchBdaStores, PipeDispatchCounts };

// The [pipe] B line (S4.0, APS5_PROFILE_DRAW): B's time inside its holds by phase, and its counts
// (draws by path, then dispatches, hold breaks, idle waits and wakes after one, and B's collect epoch
// bumps under APS5_RECORD_PIPE_EPOCH).
enum PipeBPhase : std::size_t { BVerify, BNoteWriters, BRecipe, BRestart, BDraw, BAttach, BDispatch, BLabel, BSubmitDue, BCleanup, PipeBPhases };
enum PipeBCount : std::size_t { BItems, BRecipeItems, BRestartItems, BFullItems, BBuiltItems, BStaleItems, BDispatchItems, BLabelItems, BHoldBreaks, BIdleWaits, BIdleSlept, BWakes, BEpochs, BImportMemoHits, BImportMemoMisses, PipeBCounts };

std::uint64_t pipeNs(std::chrono::steady_clock::duration duration) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

// B's phase clock (APS5_PROFILE_DRAW; idle while `on` is false): Lap adds the time since the last
// mark to a phase.
struct PipeLap {
    bool on = false;
    std::chrono::steady_clock::time_point mark;
    void Mark() {
        if (on) mark = std::chrono::steady_clock::now();
    }
    void Lap(std::array<std::atomic<std::uint64_t>, PipeBPhases>& phases, std::size_t phase) {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        phases[phase].fetch_add(pipeNs(now - mark), std::memory_order_relaxed);
        mark = now;
    }
};

// The calling thread's user and kernel time (ns; 0 where unknown).
std::uint64_t threadCpuNs() {
#ifdef _WIN32
    FILETIME creation, exitTime, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exitTime, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    return 0;
#endif
}

// The queue-0 pipe. A (the queue-0 worker) owns the overlay, the redo queue state and the plain
// counters; B (the recording thread) publishes through `ring.consumed`, `stale` and its atomics.
struct RecordPipeState {
    RecordPipeRing ring;
    RecordPipeStale stale;
    std::array<RecordPipeSlot, RecordPipeRing::Depth> slots;
    HostThread thread;
    std::atomic<bool> stopping{false}, aSleeping{false}, bSleeping{false}, failed{false};
    std::atomic<void*> aWake{nullptr}, bWake{nullptr};
    std::exception_ptr failure;
    RecordPipeOverlay overlay;
    // APS5_RECORD_PIPE_VERIFY: the write log (B's; A's while it redoes a stale item) and the counts
    // (atomics: A checks the draws it drained for, B the items it records).
    RecordPipeWriteLog writeLog;
    std::atomic<std::uint64_t> verifyMissTotal{0}, verifyChecks{0}, verifyMisses{0}, verifyEstimateMissTotal{0}, verifyEstimateMisses{0};
    std::unique_ptr<QueueState> redoQueue;
    std::uint64_t run = 0, items = 0, runs = 0, staleRedos = 0, overlayDraws = 0, overlayLoads = 0, overlayDispatches = 0;
    std::array<std::uint64_t, PipeCensusDrains> drains{};
    double ringWaitMs = 0, drainWaitMs = 0;
    // The drains that found unrecorded items, and A's wait in them, by reason.
    std::array<std::uint64_t, PipeCensusDrains> drainsWaited{};
    std::array<double, PipeCensusDrains> drainWaitByReason{};
    // Due batch submits A left to B, and B's.
    std::uint64_t submitsLeft = 0;
    // S3.0 (APS5_PROFILE_DRAW): queue-0 dispatches at the would-be hand-over, by verdict; and those
    // handed over (S3.1).
    std::array<std::uint64_t, PipeDispatchCounts> dispatches{};
    std::uint64_t dispatchItems = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> holds{0}, busyNs{0}, lockWaitNs{0}, bSubmits{0};
    // The [pipe] B line: B's phase clock, phases and counts; its hold breaks (release, yield and
    // relock) and their relock wait, idle waits, wake latency, heap operations and CPU time; A's ring
    // occupancy at hand-over (RecordPipeOccupancyBucket).
    PipeLap bLap;
    std::array<std::atomic<std::uint64_t>, PipeBPhases> bPhaseNs{};
    std::array<std::atomic<std::uint64_t>, PipeBCounts> bCounts{};
    std::atomic<std::uint64_t> holdBreakNs{0}, holdBreakLockNs{0}, idleNs{0}, wakeNs{0}, wakeMaxNs{0}, bAllocations{0}, bFrees{0}, bCpuNs{0};
    std::array<std::uint64_t, RecordPipeOccupancyBuckets> occupancy{};
    // P-LABEL (APS5_PROFILE_DRAW, the [pipe] labels line): the last drain's wait (0: it found the ring
    // empty), the label drains that waited by kind and by their readers (APS5_LABEL_CENSUS), what A did
    // after them, and the labels handed over (APS5_LAZY_LABELS).
    double lastDrainMs = 0;
    std::array<std::uint64_t, RecordPipeLabelKinds> labelDrains{};
    std::array<double, RecordPipeLabelKinds> labelDrainMs{};
    std::array<std::uint64_t, RecordPipeLabelReaderCount> readerDrains{};
    std::array<double, RecordPipeLabelReaderCount> readerDrainMs{};
    RecordPipeLabelNext labelNext;
    std::uint64_t labelItems = 0;
    // APS5_LAZY_LABELS=2: the labels riding the open run (cleared at each drain), and the waits they
    // satisfied in order.
    std::vector<RecordPipeRidingLabel> riding;
    std::uint64_t ridingWaits = 0;
    // The drains that end a run holding a satisfied wait, by reason (where its label's and wait's drain went).
    bool runRode = false;
    std::array<std::uint64_t, PipeCensusDrains> rodeDrains{};
    std::array<double, PipeCensusDrains> rodeDrainMs{};
};

// APS5_LABEL_CENSUS=1 (P-LABEL; default off): queue 0's label addresses and their readers
// (RecordPipeLabelReaders), and every queue's waits by queue / 8: on a queue-0 label, on anything
// else (count and ms). Under the mutex: A notes labels, each queue's worker its waits.
bool labelCensus() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_LABEL_CENSUS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

struct LabelCensusState {
    HostMutex mutex;
    RecordPipeLabelReaders readers;
    std::uint64_t labels = 0;
    std::array<std::uint64_t, 16> labelWaits{}, otherWaits{};
    std::array<double, 16> labelWaitMs{}, otherWaitMs{};
};

LabelCensusState& labelCensusState() {
    static LabelCensusState* const state = new LabelCensusState;
    return *state;
}

// APS5_PROFILE_DRAW: the queue-0 worker's wait for a submission (NoteRecordPipeIdle).
std::atomic<std::uint64_t> queueZeroIdleNs{0};

// A label packet's facts for RecordPipeLabelRides and the census (0x37 and 0x49 decode; others none).
RecordPipeLabelFacts recordPipeLabelFacts(std::span<const std::uint32_t> packet) {
    RecordPipeLabelFacts facts;
    facts.opcode = (packet[0] >> 8u) & 0xffu;
    facts.interrupt = facts.opcode == 0x49 && packet.size() > 2 && ((packet[2] >> 24u) & 7u) != 0;
    if (facts.opcode != 0x37 && facts.opcode != 0x49) return facts;
    if (const auto label = Pm4::DecodeLabelWrite(packet)) {
        facts.decoded = true;
        facts.address = label->address;
        facts.bytes = label->Bytes().size();
    }
    return facts;
}

// Never destroyed: Driver::stop joins B, and a static's destructor could run before the driver's.
RecordPipeState& recordPipe() {
    static RecordPipeState* const pipe = new RecordPipeState;
    return *pipe;
}

// Set while A redoes a stale item: that draw neither hands over nor drains.
bool& recordPipeReplaying() {
    static thread_local bool replaying = false;
    return replaying;
}

// A: set by the hand-over, read and cleared at the packet's end (recordPipeTookDraw).
bool& recordPipeHandedDraw() {
    static thread_local bool handed = false;
    return handed;
}

// A: set while a frame trace runs (APS5_CAPTURE_DIR, FrameTrace.hpp), taken at a packet boundary
// after a drain (recordPipeBoundary): the traced packets record on A as without the pipe.
bool& recordPipeTracing() {
    static thread_local bool tracing = false;
    return tracing;
}

bool recordPipeProfile() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

// APS5_RECORD_PIPE_VERIFY=1, only with the pipe on (default off): RecordPipeWriteLog.
bool recordPipeVerify() {
    static const bool verify = [] {
        const char* value = std::getenv("APS5_RECORD_PIPE_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0 && RecordPipeActive();
    }();
    return verify;
}

// A draw's capture and validation reads against the actual writes of the items in (consumed as it
// began, serial), all recorded by now: B before recording item `serial`, A after draining for a draw
// it records itself ("drained item", serial handed + 1). The draw passed recordPipeOverlaps, so a hit
// is a write the overlay estimate missed.
void verifyPipeReads(RecordPipeState& pipe, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t consumed, std::uint64_t serial, const char* who, std::uint32_t opcode, std::uint64_t key) {
    if (serial <= consumed + 1) return;
    pipe.verifyChecks.fetch_add(1, std::memory_order_relaxed);
    for (const auto& region : regions) {
        const auto begin = region.guestAddress, end = begin + region.bytes.size();
        const auto writer = pipe.writeLog.Writer(consumed, serial, begin, end);
        if (writer == 0) continue;
        pipe.verifyMisses.fetch_add(1, std::memory_order_relaxed);
        const auto total = pipe.verifyMissTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 20 || total % 1000 == 0) std::fprintf(stderr, "[pipe] verify miss: %s %llu (opcode 0x%x, key 0x%llx) read [0x%llx,0x%llx) written by item %llu (unrecorded at its capture); overlay estimate had it: no\n", who, static_cast<unsigned long long>(serial), opcode, static_cast<unsigned long long>(key), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end), static_cast<unsigned long long>(writer));
        return;
    }
}

// APS5_RECORD_PIPE_VERIFY, A at hand-over: the overlay ranges left under the item's serial (its own,
// and the known ranges that took it), the estimate verifyPipeEstimate checks its writes against.
void notePipeEstimate(const RecordPipeOverlay& overlay, std::uint64_t serial, std::vector<std::pair<std::uint64_t, std::uint64_t>>& estimate) {
    estimate.clear();
    for (const auto& range : overlay.ranges) {
        if (range.serial == serial) estimate.emplace_back(range.begin, range.end);
    }
}

// B, after recording item `serial` (and A's redo, if it turned stale): a logged write outside the
// item's estimate (RecordPipeEstimateMiss) is a gap in the overlay, unless it is a write-back of an
// older image.
void verifyPipeEstimate(RecordPipeState& pipe, std::uint64_t serial, std::uint32_t opcode, std::uint64_t key, std::span<const std::pair<std::uint64_t, std::uint64_t>> estimate) {
    const auto& item = pipe.writeLog.items[RecordPipeRing::Slot(serial)];
    if (item.serial != serial) return;
    const auto miss = RecordPipeEstimateMiss(item.ranges, estimate);
    if (!miss) return;
    pipe.verifyEstimateMisses.fetch_add(1, std::memory_order_relaxed);
    const auto total = pipe.verifyEstimateMissTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (total <= 20 || total % 1000 == 0) std::fprintf(stderr, "[pipe] verify estimate miss: item %llu (opcode 0x%x, key 0x%llx) wrote [0x%llx,0x%llx)\n", static_cast<unsigned long long>(serial), opcode, static_cast<unsigned long long>(key), static_cast<unsigned long long>(miss->first), static_cast<unsigned long long>(miss->second));
}

// B submits a batch whose oldest recorded label is past the deadline (APS5_LABEL_FLUSH_US, 1 ms) after
// each item it records and before it sleeps; A leaves the deadline submit to B while B has items
// (its try fails under B's holds, PacketFlush.cpp, so the label waited for A's outright lock at 4x the
// deadline). APS5_RECORD_PIPE_NO_B_SUBMIT=1: A submits as before.
bool recordPipeBSubmit() {
    static const bool enabled = std::getenv("APS5_RECORD_PIPE_NO_B_SUBMIT") == nullptr;
    return enabled;
}

std::chrono::microseconds pipeMicros(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    return std::chrono::microseconds(text ? std::strtoull(text, nullptr, 10) : fallback);
}

double sinceMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Until done(): `between` (A: the stale service and the failure checks), then a spin of
// APS5_RECORD_PIPE_SPIN_US (50), then sleeps of a poll interval on the thread's wake event; done() is
// checked again after `sleeping` is set, so the other side's wake (wakeSide) is never lost. `slept`
// (APS5_PROFILE_DRAW), if given, is set once the wait sleeps.
template <typename TDone, typename TBetween>
void pipeWait(std::atomic<bool>& sleeping, std::atomic<void*>& wake, TDone&& done, TBetween&& between, bool* slept = nullptr) {
    static const auto spin = pipeMicros("APS5_RECORD_PIPE_SPIN_US", 50);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        between();
        if (done()) return;
        if (std::chrono::steady_clock::now() - start < spin) {
            std::this_thread::yield();
            continue;
        }
        void* event = PollWakeEvent();
        wake.store(event, std::memory_order_release);
        sleeping.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!done()) {
            if (slept != nullptr) *slept = true;
            if (!PollSleepOrWake(event, PollTryInterval)) PollSleep();
        }
        sleeping.store(false, std::memory_order_relaxed);
    }
}

void wakeSide(const std::atomic<bool>& sleeping, const std::atomic<void*>& wake) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping.load(std::memory_order_seq_cst)) PollWake(wake.load(std::memory_order_acquire));
}

// An item's render targets, padded (plan 4): tiling and the depth/stencil estimate (+64 KiB), DCC
// (a byte per 256 +4 KiB) and HTILE (a dword per 64 texels +4 KiB).
void notePipeTargets(const Graphics::State& graphics, std::uint64_t serial, RecordPipeOverlay& overlay) {
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin != 0 && bytes != 0) overlay.Add({begin, begin + bytes, RecordPipeOverlay::Target, serial});
    };
    for (const auto& color : graphics.colors) {
        add(color.address, color.bytes);
        if (color.dccAddress != 0) add(color.dccAddress, color.bytes / 256 + 4096);
    }
    if (!graphics.depth) return;
    const auto& depth = *graphics.depth;
    const auto texels = static_cast<std::uint64_t>(depth.extent.width) * depth.extent.height;
    if (graphics.depthWrite) add(depth.address, texels * 4 + 65536);
    if (graphics.stencilTest && depth.stencilAddress != 0) add(depth.stencilAddress, texels + 65536);
    if (graphics.depthWrite || graphics.stencilTest) add(depth.htileAddress, texels / 16 + 4096);
}

}

bool RecordPipeActive() {
    static const bool active = [] {
        // On by default since s60 (t572/t573 and t580/t581 same-binary A/Bs, t575 verify 0 misses);
        // APS5_RECORD_PIPE=0 is the old path (A records every draw itself).
        const char* value = std::getenv("APS5_RECORD_PIPE");
        if (value != nullptr && std::strcmp(value, "0") == 0) return false;
        const auto set = [](const char* name) { return std::getenv(name) != nullptr; };
        const char* fastDispatch = std::getenv("APS5_FAST_DISPATCH");
        const bool declined = FastDrawEnabled() || FastDrawVerifyEvery() != 0 || set("APS5_LOCKED_DRAW_PREPARE") || Graphics::Recorder::DispatchOverlap() || FastCensusActive() || (fastDispatch != nullptr && std::strcmp(fastDispatch, "0") != 0) || PassDump::Enabled() || Graphics::CheckpointsRequested() || set("APS5_DUMP_DRAW_SHADERS") || set("APS5_DUMP_DRAW_SLOT1") || ShaderRecompiler::DebugProbeActive() || !LabelBatchSubmit() || set("APS5_DRAIN_ALL") || set("APS5_DRAW_DRAIN");
        std::fprintf(stderr, "[pipe] queue 0 recording pipe %s\n", declined ? "declined: a fast-path, verify, dump or drain switch is set" : "on");
        return !declined;
    }();
    return active;
}

bool RecordPipeLabelsActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_LAZY_LABELS");
        if (value == nullptr || std::strcmp(value, "0") == 0 || !RecordPipeActive()) return false;
        const bool declined = !DeferLabels() || !QueuedLabelTable() || std::getenv("APS5_DRAIN_ALL") != nullptr;
        std::fprintf(stderr, "[pipe] queue 0 labels ride the recording pipe (APS5_LAZY_LABELS=%s): %s\n", value, declined ? "declined: APS5_LABEL_LOCK_EACH, APS5_DRAIN_COMPLETION_LABELS, APS5_DRAIN_ALL or no queued-label table" : std::strcmp(value, "2") == 0 ? "on, with queue 0's waits on them" : "on");
        return !declined;
    }();
    return active;
}

bool RecordPipeWaitsActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_LAZY_LABELS");
        return value != nullptr && std::strcmp(value, "2") == 0 && RecordPipeLabelsActive();
    }();
    return active;
}

// A: set by the boundary for a wait satisfied in order, read and cleared by its packet (recordPipeTookWait).
bool& recordPipeWaitRode() {
    static thread_local bool rode = false;
    return rode;
}

bool RecordPipeLabelCensus() {
    return labelCensus();
}

void NoteRecordPipeLabelWait(std::uint64_t address, std::uint32_t queue, double ms) {
    if (!labelCensus()) return;
    auto& census = labelCensusState();
    const auto index = static_cast<std::size_t>((queue / 8u) & 15u);
    std::lock_guard lock(census.mutex);
    if (census.readers.NoteWait(address, queue)) {
        ++census.labelWaits[index];
        census.labelWaitMs[index] += ms;
    } else {
        ++census.otherWaits[index];
        census.otherWaitMs[index] += ms;
    }
}

void NoteRecordPipeIdle(std::uint64_t ns) {
    queueZeroIdleNs.fetch_add(ns, std::memory_order_relaxed);
}

bool RecordPipeDispatchActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECORD_PIPE_DISPATCH");
        if (value == nullptr || std::strcmp(value, "1") != 0 || !RecordPipeActive()) return false;
        const bool declined = std::getenv("APS5_PROBE_DISPATCH") != nullptr || std::getenv("APS5_ASPECT_PROBE") != nullptr;
        std::fprintf(stderr, "[pipe] queue 0 recording pipe: direct dispatches %s\n", declined ? "declined: a dispatch or aspect probe is set" : "on");
        return !declined;
    }();
    return active;
}

bool RecordPipeEpochActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECORD_PIPE_EPOCH");
        if (value == nullptr || std::strcmp(value, "1") != 0 || !RecordPipeActive()) return false;
        std::fprintf(stderr, "[pipe] queue 0 recording pipe: B collect epochs follow A's\n");
        return true;
    }();
    return active;
}

void RecordPipeImageRanges(std::span<const std::uint32_t> words, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    if (words.size() >= 8 && words[0] == 0 && (words[1] & 0xffu) == 0) return;
    std::uint64_t base = 0, bytes = 0, dcc = 0;
    if (words.size() >= 8) {
        try {
            const auto image = Graphics::DecodeTextureResource(words.first(8));
            base = image.baseAddress;
            dcc = image.dccAddress;
            bytes = Graphics::DescribeSurface(image).guestBytes;
        } catch (const std::exception&) {
            bytes = 0;
        }
    }
    if (bytes == 0) {
        ranges.emplace_back(0, ~0ull);
        return;
    }
    ranges.emplace_back(base, base + bytes + 65536);
    if (dcc != 0) ranges.emplace_back(dcc, dcc + bytes / 256 + 4096);
}

void NoteRecordPipeDispatch(const RecordPipeDispatchFacts& facts) {
    auto& counts = recordPipe().dispatches;
    if (RecordPipeDispatchVerdict(facts).eligible) {
        ++counts[DispatchEligible];
        if (facts.imageWrites) ++counts[DispatchImageWrites];
    } else if (!facts.replaying) {
        ++counts[facts.local ? DispatchLocal : !facts.window ? DispatchNoWindow : facts.labelsPending ? DispatchLabelsPending : facts.bdaStores ? DispatchBdaStores : DispatchBdaLoads];
    }
}

bool Driver::recordPipeDrawing(std::uint32_t queue) {
    return queue == 0 && RecordPipeActive() && !recordPipeReplaying() && !recordPipeTracing();
}

bool Driver::recordPipeDispatching(std::uint32_t queue) {
    return queue == 0 && RecordPipeDispatchActive() && !recordPipeReplaying() && !recordPipeTracing();
}

bool Driver::recordPipeLabeling(std::uint32_t queue) {
    return queue == 0 && RecordPipeLabelsActive() && !recordPipeReplaying() && !recordPipeTracing();
}

bool Driver::recordPipeWaiting(std::uint32_t queue) {
    return queue == 0 && RecordPipeWaitsActive() && !recordPipeReplaying() && !recordPipeTracing();
}

bool Driver::recordPipeTookWait() {
    const bool rode = recordPipeWaitRode();
    recordPipeWaitRode() = false;
    return rode;
}

DrawScratch* Driver::recordPipeScratch() {
    auto& pipe = recordPipe();
    const auto serial = pipe.ring.NextFree();
    return serial == 0 ? nullptr : &pipe.slots[RecordPipeRing::Slot(serial)].scratch;
}

std::uint64_t Driver::recordPipeConsumed() {
    return recordPipe().ring.consumed.load(std::memory_order_acquire);
}

bool Driver::recordPipeOverlaps(std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t consumed, bool forDispatch) {
    auto& pipe = recordPipe();
    // `consumed` as the draw or dispatch began: the items recorded by then were in the pending writes its
    // captures and validation consulted.
    pipe.overlay.Prune(consumed);
    if (pipe.overlay.ranges.empty()) return false;
    for (const auto& region : regions) {
        if (pipe.overlay.Overlaps(region.guestAddress, region.guestAddress + region.bytes.size()) == 0) continue;
        ++(forDispatch ? pipe.overlayDispatches : pipe.overlayDraws);
        return true;
    }
    return false;
}

void Driver::verifyRecordPipeDrained(std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t consumed, std::uint32_t opcode, std::uint64_t key) {
    if (!recordPipeVerify()) return;
    auto& pipe = recordPipe();
    verifyPipeReads(pipe, regions, consumed, pipe.ring.handed.load(std::memory_order_relaxed) + 1, "drained item", opcode, key);
}

void Driver::handOverDraw(DrawScratch& scratch, RecordPipeDraw&& draw, const QueueState& queue) {
    auto& pipe = recordPipe();
    const auto serial = pipe.ring.handed.load(std::memory_order_relaxed) + 1;
    auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
    require(&slot.scratch == &scratch && pipe.ring.NextFree() == serial, "recording pipe: the draw's scratch is not the next free slot");
    slot.kind = RecordPipeKind::Draw;
    slot.draw = std::move(draw);
    slot.mark.Save(queue);
    for (const auto& stage : scratch.stages) notePipeItemWrites(*stage.program, serial, pipe.overlay);
    notePipeTargets(slot.draw.decode->state, serial, pipe.overlay);
    // Stage 3: the storage images the stages write too (stage 1 left them out).
    if (RecordPipeDispatchActive()) {
        for (const auto& stage : scratch.stages) notePipeItemImages(*stage.program, serial, pipe.overlay);
    }
    if (recordPipeVerify()) notePipeEstimate(pipe.overlay, serial, slot.estimate);
    publishRecordPipeItem(serial);
}

void Driver::handOverDispatch(RecordPipeDispatch&& item) {
    auto& pipe = recordPipe();
    const auto serial = pipe.ring.handed.load(std::memory_order_relaxed) + 1;
    // The packet's boundary waited for the slot (recordPipeBoundary).
    require(pipe.ring.NextFree() == serial, "recording pipe: the dispatch's slot is not free");
    auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
    slot.kind = RecordPipeKind::Dispatch;
    slot.dispatch = std::move(item);
    notePipeItemWrites(*slot.dispatch.compiled, serial, pipe.overlay);
    notePipeItemImages(*slot.dispatch.compiled, serial, pipe.overlay);
    if (recordPipeVerify()) notePipeEstimate(pipe.overlay, serial, slot.estimate);
    ++pipe.dispatchItems;
    publishRecordPipeItem(serial);
}

bool Driver::handOverLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint32_t opcode) {
    auto& pipe = recordPipe();
    // The boundary's verdict (it skipped the drain for exactly these labels).
    if (!RecordPipeLabelRides({opcode, false, true, address, bytes.size()})) return false;
    // Nothing unrecorded: the label joins the deferred list as before (the stage-1 rule holds: a
    // non-empty list means an empty ring).
    const auto handed = pipe.ring.handed.load(std::memory_order_relaxed);
    if (RecordPipeRing::Empty(handed, pipe.ring.consumed.load(std::memory_order_acquire)) || !deferredLabels().labels.empty()) return false;
    const auto serial = handed + 1;
    // The packet's boundary waited for the slot (recordPipeBoundary).
    require(pipe.ring.NextFree() == serial, "recording pipe: the label's slot is not free");
    auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
    slot.kind = RecordPipeKind::Label;
    slot.labelOpcode = opcode;
    slot.label.address = address;
    slot.label.size = bytes.size();
    std::memcpy(slot.label.bytes.data(), bytes.data(), bytes.size());
    // A capture, validation or indirect read over the label's bytes drains until B records it.
    pipe.overlay.Add({address, address + bytes.size(), RecordPipeOverlay::Write, serial});
    if (RecordPipeWaitsActive()) {
        auto& riding = pipe.riding.emplace_back();
        riding.address = address;
        riding.size = bytes.size();
        std::memcpy(riding.bytes.data(), bytes.data(), bytes.size());
        riding.serial = serial;
    }
    ++pipe.labelItems;
    publishRecordPipeItem(serial);
    // Not a draw or dispatch: the packet's recorded-work count is not B's (recordPipeTookDraw).
    recordPipeHandedDraw() = false;
    return true;
}

// B, under the GPU mutex: a label item, as recordDeferredLabels records one (B's label group closes
// before its next other item or release). No reap first: only the graphics worker runs completions
// under the mutex (WriteLabelOnGpu); the label goes behind a completion instead where one overlaps.
void Driver::recordPipeLabel(std::uint64_t address, std::span<const std::byte> bytes) {
    const auto localDevice = device.Load();
    const auto stamp = ++eventSerial;
    const int reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(address, bytes, stamp, 0, false) : 4;
    countLabelOutcome(reason);
    if (reason != 0 && reason != 5 && reason != 6) {
        if (reason != 1 && localDevice != nullptr) localDevice->WaitIdle();
        GuestMemory::Write(address, bytes, 4);
    }
    noteLabelStore(address, bytes, stamp);
}

// A, the slot filled: B starts with the first item; the release publishes the slot.
void Driver::publishRecordPipeItem(std::uint64_t serial) {
    auto& pipe = recordPipe();
    if (!pipe.thread.joinable()) pipe.thread = HostThread([this] { runRecordPipe(); });
    // APS5_PROFILE_DRAW: the publish time (B's wake latency), then the items unrecorded with this one.
    const bool profile = recordPipeProfile();
    if (profile) pipe.slots[RecordPipeRing::Slot(serial)].published = std::chrono::steady_clock::now();
    // APS5_RECORD_PIPE_EPOCH: the epoch A would have recorded the item in.
    if (RecordPipeEpochActive()) pipe.slots[RecordPipeRing::Slot(serial)].epoch = GuestMemory::ThreadCollectEpoch();
    pipe.ring.Publish(serial);
    if (profile) {
        ++pipe.occupancy[RecordPipeOccupancyBucket(RecordPipeRing::Pending(serial, pipe.ring.consumed.load(std::memory_order_acquire)))];
        pipe.labelNext.Note(RecordPipeLabelNext::Handed);
    }
    recordPipeHandedDraw() = true;
    ++pipe.items;
    ++pipe.run;
    wakeSide(pipe.bSleeping, pipe.bWake);
}

bool Driver::recordPipeTookDraw() {
    const bool handed = recordPipeHandedDraw();
    recordPipeHandedDraw() = false;
    return handed;
}

bool Driver::recordPipeTakesSubmit() {
    if (!RecordPipeActive() || !recordPipeBSubmit()) return false;
    auto& pipe = recordPipe();
    // B checks the deadline after its next item, unless it has none or waits for a stale redo.
    if (RecordPipeRing::Empty(pipe.ring.handed.load(std::memory_order_relaxed), pipe.ring.consumed.load(std::memory_order_acquire)) || pipe.stale.Pending() != 0) return false;
    ++pipe.submitsLeft;
    return true;
}

void Driver::serviceRecordPipe() {
    auto& pipe = recordPipe();
    const auto serial = pipe.stale.Pending();
    if (serial == 0) return;
    // B recorded nothing for the item and waits: the packet runs here through the retry ladder,
    // with the queue state it had at hand-over, before B records the items after it.
    auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
    if (pipe.redoQueue == nullptr) pipe.redoQueue = std::make_unique<QueueState>();
    slot.mark.Restore(*pipe.redoQueue);
    {
        struct Replay {
            bool& replaying;
            ~Replay() {
                replaying = false;
                GuestMemory::recordedWrites = nullptr;
            }
        } replay{recordPipeReplaying()};
        replay.replaying = true;
        // APS5_RECORD_PIPE_VERIFY: the redo's writes are the item's (B waits, so the log is A's).
        if (recordPipeVerify()) GuestMemory::recordedWrites = &pipe.writeLog.items[RecordPipeRing::Slot(serial)].ranges;
        try {
            std::string rejected;
            if (redrawStale(*pipe.redoQueue, slot.draw.packet, *slot.draw.submission, rejected, slot.draw.hit) == DrawVerdict::Rejected) reportSkip("draw", rejected);
        } catch (const std::exception& error) {
            reportSkip("draw", error.what());
        }
    }
    ++pipe.staleRedos;
    pipe.stale.Finish(serial);
    wakeSide(pipe.bSleeping, pipe.bWake);
}

// Each round of A's pipe waits: a stale item is redone (B waits for it), and B's failure becomes the
// driver's, which the packet loop rethrows (CheckFailure is outside the packets' tolerate).
void Driver::recordPipeWaitRound() {
    auto& pipe = recordPipe();
    serviceRecordPipe();
    if (pipe.failed.load(std::memory_order_acquire)) ReportFailure(pipe.failure);
    CheckFailure();
}

void Driver::drainRecordPipe(PipeCensusDrain why) {
    auto& pipe = recordPipe();
    // P-LABEL census: a drain after a label drain (labels in a row stay one event).
    if (why != PipeCensusDrain::Label) pipe.labelNext.Note(static_cast<std::size_t>(why));
    pipe.lastDrainMs = 0;
    if (pipe.run != 0) {
        ++pipe.runs;
        ++pipe.drains[static_cast<std::size_t>(why)];
        pipe.run = 0;
    }
    const auto empty = [&] { return RecordPipeRing::Empty(pipe.ring.handed.load(std::memory_order_relaxed), pipe.ring.consumed.load(std::memory_order_acquire)); };
    // Only a drain that finds unrecorded items waits; it is timed by reason.
    const bool timed = recordPipeProfile() && !empty();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    pipeWait(pipe.aSleeping, pipe.aWake, empty, [&] { recordPipeWaitRound(); });
    if (timed) {
        const auto ms = sinceMs(start);
        pipe.drainWaitMs += ms;
        pipe.drainWaitByReason[static_cast<std::size_t>(why)] += ms;
        ++pipe.drainsWaited[static_cast<std::size_t>(why)];
        pipe.lastDrainMs = ms;
    }
    if (pipe.runRode) {
        pipe.runRode = false;
        ++pipe.rodeDrains[static_cast<std::size_t>(why)];
        pipe.rodeDrainMs[static_cast<std::size_t>(why)] += pipe.lastDrainMs;
    }
    pipe.overlay.ranges.clear();
    pipe.riding.clear();
}

void Driver::recordPipeBoundary(std::span<const std::uint32_t> packet, std::uint32_t header) {
    auto& pipe = recordPipe();
    serviceRecordPipe();
    // APS5_CAPTURE_DIR: a frame trace starts and ends at a present (the presenting thread); A follows
    // it here, before the packet and after a drain, so a traced command's line, write notes and
    // dumps see it recorded. The directory alone no longer declines the pipe.
    if (const bool tracing = FrameTrace::Noting(); tracing != recordPipeTracing()) {
        drainRecordPipe(PipeCensusDrain::Other);
        recordPipeTracing() = tracing;
    }
    const bool special = header == FlipPacketHeader || header == RenderingWaitPacketHeader;
    const bool dispatches = RecordPipeDispatchActive();
    const auto opcode = (header >> 8u) & 0xffu;
    // A label's facts: APS5_LAZY_LABELS (a label that rides skips the drain and takes a slot), the
    // census (its kind, its readers).
    const bool labelPacket = !special && (opcode == 0x37 || opcode == 0x49);
    const bool labeling = labelPacket && recordPipeLabeling(0);
    const auto label = labelPacket && (labeling || recordPipeProfile()) ? recordPipeLabelFacts(packet) : RecordPipeLabelFacts{};
    if (label.decoded && labelCensus()) {
        auto& census = labelCensusState();
        std::lock_guard lock(census.mutex);
        census.readers.NoteLabel(label.address);
        ++census.labels;
    }
    const bool rides = labeling && RecordPipeLabelRides(label);
    // APS5_LAZY_LABELS=2: a queue-0 wait satisfied in order behind a riding label neither drains nor waits.
    recordPipeWaitRode() = false;
    if (!special && (opcode == 0x3c || opcode == 0x93) && packet.size() >= 6 && pipe.run != 0 && !pipe.riding.empty() && recordPipeWaiting(0)) {
        const auto consumed = pipe.ring.consumed.load(std::memory_order_acquire);
        pipe.overlay.Prune(consumed);
        const auto value = RecordPipeRidingValue(pipe.riding, pipe.overlay, consumed, packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), Pm4::WaitAwaitedBytes(packet));
        if (value && Pm4::WaitComparesValue(packet, *value)) {
            recordPipeWaitRode() = true;
            ++pipe.ridingWaits;
            pipe.runRode = true;
            return;
        }
    }
    if (!special && (Pm4::DrawOpcode(opcode) || (dispatches && opcode == 0x15) || rides)) {
        // The draw's lease, the dispatch's or the label's hand-over takes slot handed + 1: wait while B still owns it.
        const auto handed = pipe.ring.handed.load(std::memory_order_relaxed);
        if (!RecordPipeRing::Full(handed, pipe.ring.consumed.load(std::memory_order_acquire))) return;
        const auto start = recordPipeProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        pipeWait(pipe.aSleeping, pipe.aWake, [&] { return !RecordPipeRing::Full(handed, pipe.ring.consumed.load(std::memory_order_acquire)); }, [&] { recordPipeWaitRound(); });
        if (recordPipeProfile()) pipe.ringWaitMs += sinceMs(start);
        return;
    }
    // No run open: the ring is empty. The census notes what follows a label drain (a draw decides in
    // its packet: handed, or a drain).
    if (pipe.run == 0) {
        if (pipe.labelNext.armed) {
            const auto next = RecordPipeTopDrain(packet, false, header == FlipPacketHeader, header == RenderingWaitPacketHeader, pipe.overlay, dispatches);
            if (next && *next != PipeCensusDrain::Label) pipe.labelNext.Note(static_cast<std::size_t>(*next));
        }
        return;
    }
    pipe.overlay.Prune(pipe.ring.consumed.load(std::memory_order_acquire));
    const auto why = RecordPipeTopDrain(packet, false, header == FlipPacketHeader, header == RenderingWaitPacketHeader, pipe.overlay, dispatches);
    if (!why) return;
    if (*why == PipeCensusDrain::Overlap) ++pipe.overlayLoads;
    drainRecordPipe(*why);
    // P-LABEL census: a label drain that waited, by kind and readers; what follows it is noted next.
    if (*why != PipeCensusDrain::Label || pipe.lastDrainMs <= 0 || !recordPipeProfile()) return;
    const auto kind = static_cast<std::size_t>(RecordPipeLabelKindOf(opcode, label.interrupt));
    ++pipe.labelDrains[kind];
    pipe.labelDrainMs[kind] += pipe.lastDrainMs;
    pipe.labelNext.Arm(pipe.lastDrainMs);
    if (!labelCensus()) return;
    auto& census = labelCensusState();
    std::size_t reader = static_cast<std::size_t>(RecordPipeLabelReader::None);
    if (label.decoded) {
        std::lock_guard lock(census.mutex);
        reader = static_cast<std::size_t>(census.readers.ReaderOf(label.address));
    }
    ++pipe.readerDrains[reader];
    pipe.readerDrainMs[reader] += pipe.lastDrainMs;
}

void Driver::recordPipeSubmissionEnd() {
    drainRecordPipe(PipeCensusDrain::SubmissionEnd);
    auto& pipe = recordPipe();
    if (!recordPipeProfile() || std::chrono::steady_clock::now() - pipe.lastReport < std::chrono::seconds(10)) return;
    std::string drains, waits;
    char item[64];
    for (std::size_t why = 0; why < PipeCensusDrains; ++why) {
        std::snprintf(item, sizeof(item), " %s %llu", PipeCensusDrainNames[why], static_cast<unsigned long long>(pipe.drains[why]));
        drains += item;
        if (pipe.drainsWaited[why] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.1f/%llu", PipeCensusDrainNames[why], pipe.drainWaitByReason[why], static_cast<unsigned long long>(pipe.drainsWaited[why]));
        waits += item;
    }
    if (waits.empty()) waits = " none";
    const auto ms = [](std::atomic<std::uint64_t>& ns) { return static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 1e6; };
    const auto busyMs = ms(pipe.busyNs), lockWaitMs = ms(pipe.lockWaitNs);
    const auto& counts = pipe.dispatches;
    char dispatches[320];
    std::snprintf(dispatches, sizeof(dispatches), ", dispatches: handed %llu, eligible %llu (image writes %llu), bda loads %llu, bda stores %llu, labels pending %llu, local packet %llu, no window %llu", static_cast<unsigned long long>(pipe.dispatchItems), static_cast<unsigned long long>(counts[DispatchEligible]), static_cast<unsigned long long>(counts[DispatchImageWrites]), static_cast<unsigned long long>(counts[DispatchBdaLoads]), static_cast<unsigned long long>(counts[DispatchBdaStores]), static_cast<unsigned long long>(counts[DispatchLabelsPending]), static_cast<unsigned long long>(counts[DispatchLocal]), static_cast<unsigned long long>(counts[DispatchNoWindow]));
    char verify[128] = "";
    if (recordPipeVerify()) std::snprintf(verify, sizeof(verify), ", verify: checks %llu, misses %llu, estimate misses %llu", static_cast<unsigned long long>(pipe.verifyChecks.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(pipe.verifyMisses.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(pipe.verifyEstimateMisses.exchange(0, std::memory_order_relaxed)));
    std::fprintf(stderr, "[pipe] queue 0 (10 s): items %llu, runs %llu (mean %.1f), holds %llu, drains:%s, A waited ms (ring full) %.1f, A drain wait ms %.1f (by reason, ms/drains that waited:%s), B busy ms %.1f, B lock wait ms %.1f, B deadline submits %llu (A left %llu to B), stale redos %llu, overlay hits %llu (draw %llu, load %llu, dispatch %llu)%s%s\n", static_cast<unsigned long long>(pipe.items), static_cast<unsigned long long>(pipe.runs), pipe.runs == 0 ? 0.0 : static_cast<double>(pipe.items) / static_cast<double>(pipe.runs), static_cast<unsigned long long>(pipe.holds.exchange(0, std::memory_order_relaxed)), drains.c_str(), pipe.ringWaitMs, pipe.drainWaitMs, waits.c_str(), busyMs, lockWaitMs, static_cast<unsigned long long>(pipe.bSubmits.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(pipe.submitsLeft), static_cast<unsigned long long>(pipe.staleRedos), static_cast<unsigned long long>(pipe.overlayDraws + pipe.overlayLoads + pipe.overlayDispatches), static_cast<unsigned long long>(pipe.overlayDraws), static_cast<unsigned long long>(pipe.overlayLoads), static_cast<unsigned long long>(pipe.overlayDispatches), dispatches, verify);
    // B's line (S4.0): us per item by phase inside its holds (other: B busy minus the rows), hold
    // breaks, idle waits, the wake latency after one, heap operations and CPU time; A's ring at hand-over.
    std::array<double, PipeBPhases> phaseNs{};
    double rowsNs = 0;
    for (std::size_t phase = 0; phase < PipeBPhases; ++phase) {
        phaseNs[phase] = static_cast<double>(pipe.bPhaseNs[phase].exchange(0, std::memory_order_relaxed));
        rowsNs += phaseNs[phase];
    }
    std::array<unsigned long long, PipeBCounts> itemCounts{};
    for (std::size_t which = 0; which < PipeBCounts; ++which) itemCounts[which] = pipe.bCounts[which].exchange(0, std::memory_order_relaxed);
    const auto bItems = itemCounts[BItems] == 0 ? 1.0 : static_cast<double>(itemCounts[BItems]);
    const auto holdBreakMs = ms(pipe.holdBreakNs), holdBreakLockMs = ms(pipe.holdBreakLockNs), idleMs = ms(pipe.idleNs), wakeMs = ms(pipe.wakeNs), cpuMs = ms(pipe.bCpuNs);
    const auto wakeMaxNs = static_cast<double>(pipe.wakeMaxNs.exchange(0, std::memory_order_relaxed));
    const auto allocations = static_cast<double>(pipe.bAllocations.exchange(0, std::memory_order_relaxed)), frees = static_cast<double>(pipe.bFrees.exchange(0, std::memory_order_relaxed));
    char rows[352];
    std::snprintf(rows, sizeof(rows), "verify %.2f, note writers %.2f, recipe %.2f, restart+draw %.2f, draw %.2f, attach %.2f, dispatch %.2f, label %.2f, submit due %.2f, cleanup %.2f, other %.2f", phaseNs[BVerify] / 1e3 / bItems, phaseNs[BNoteWriters] / 1e3 / bItems, phaseNs[BRecipe] / 1e3 / bItems, phaseNs[BRestart] / 1e3 / bItems, phaseNs[BDraw] / 1e3 / bItems, phaseNs[BAttach] / 1e3 / bItems, phaseNs[BDispatch] / 1e3 / bItems, phaseNs[BLabel] / 1e3 / bItems, phaseNs[BSubmitDue] / 1e3 / bItems, phaseNs[BCleanup] / 1e3 / bItems, (busyMs * 1e6 - rowsNs) / 1e3 / bItems);
    const auto& occupancy = pipe.occupancy;
    char importMemo[96] = "";
    if (Graphics::DrawMemos()) std::snprintf(importMemo, sizeof(importMemo), "; import memo (APS5_DRAW_MEMOS): hits %llu, misses %llu", itemCounts[BImportMemoHits], itemCounts[BImportMemoMisses]);
    std::fprintf(stderr, "[pipe] B (10 s): items %llu (draws %llu: recipe %llu, restart %llu, full %llu, built %llu, stale %llu; dispatches %llu; labels %llu), us per item: %s (busy minus the rows); hold breaks %llu (%.1f ms, relock wait %.1f ms); idle ms %.1f in %llu waits (%llu slept); wake latency us mean %.1f max %.1f (%llu); heap per item: %.2f allocations, %.2f frees; cpu ms %.1f (busy ms %.1f); ring at hand-over: 1 %llu, 2-4 %llu, 5-16 %llu, 17-31 %llu, full %llu; collect epochs %llu%s\n", itemCounts[BItems], itemCounts[BItems] > itemCounts[BDispatchItems] + itemCounts[BLabelItems] ? itemCounts[BItems] - itemCounts[BDispatchItems] - itemCounts[BLabelItems] : 0ull, itemCounts[BRecipeItems], itemCounts[BRestartItems], itemCounts[BFullItems], itemCounts[BBuiltItems], itemCounts[BStaleItems], itemCounts[BDispatchItems], itemCounts[BLabelItems], rows, itemCounts[BHoldBreaks], holdBreakMs, holdBreakLockMs, idleMs, itemCounts[BIdleWaits], itemCounts[BIdleSlept], itemCounts[BWakes] == 0 ? 0.0 : wakeMs * 1e3 / static_cast<double>(itemCounts[BWakes]), wakeMaxNs / 1e3, itemCounts[BWakes], allocations / bItems, frees / bItems, cpuMs, busyMs, static_cast<unsigned long long>(occupancy[0]), static_cast<unsigned long long>(occupancy[1]), static_cast<unsigned long long>(occupancy[2]), static_cast<unsigned long long>(occupancy[3]), static_cast<unsigned long long>(occupancy[4]), itemCounts[BEpochs], importMemo);
    // P-LABEL: the label drains that waited (ms/drains) by kind, by what A did next (handed: riding
    // the ring the label would have joined two runs) and by their readers (APS5_LABEL_CENSUS); the
    // labels handed over (APS5_LAZY_LABELS); the queue-0 worker's wait for a submission.
    std::string labelKinds, labelNext, labelReaders;
    for (std::size_t kind = 0; kind < RecordPipeLabelKinds; ++kind) {
        if (pipe.labelDrains[kind] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.1f/%llu", RecordPipeLabelKindNames[kind], pipe.labelDrainMs[kind], static_cast<unsigned long long>(pipe.labelDrains[kind]));
        labelKinds += item;
    }
    for (std::size_t bucket = 0; bucket < RecordPipeLabelNext::Buckets; ++bucket) {
        if (pipe.labelNext.counts[bucket] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.1f/%llu", bucket == RecordPipeLabelNext::Handed ? "handed" : PipeCensusDrainNames[bucket], pipe.labelNext.ms[bucket], static_cast<unsigned long long>(pipe.labelNext.counts[bucket]));
        labelNext += item;
    }
    for (std::size_t reader = 0; reader < RecordPipeLabelReaderCount; ++reader) {
        if (pipe.readerDrains[reader] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.1f/%llu", RecordPipeLabelReaderNames[reader], pipe.readerDrainMs[reader], static_cast<unsigned long long>(pipe.readerDrains[reader]));
        labelReaders += item;
    }
    std::string rodeEnds;
    for (std::size_t why = 0; why < PipeCensusDrains; ++why) {
        if (pipe.rodeDrains[why] == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.1f/%llu", PipeCensusDrainNames[why], pipe.rodeDrainMs[why], static_cast<unsigned long long>(pipe.rodeDrains[why]));
        rodeEnds += item;
    }
    pipe.rodeDrains = {};
    pipe.rodeDrainMs = {};
    std::fprintf(stderr, "[pipe] labels (10 s): label drains that waited (ms/drains):%s; next after them:%s; by readers (APS5_LABEL_CENSUS):%s; labels handed over %llu, waits satisfied behind them %llu (their runs ended by, ms/drains:%s); A idle ms %.1f (no submission)\n", labelKinds.empty() ? " none" : labelKinds.c_str(), labelNext.empty() ? " none" : labelNext.c_str(), labelReaders.empty() ? " none" : labelReaders.c_str(), static_cast<unsigned long long>(pipe.labelItems), static_cast<unsigned long long>(pipe.ridingWaits), rodeEnds.empty() ? " none" : rodeEnds.c_str(), static_cast<double>(queueZeroIdleNs.exchange(0, std::memory_order_relaxed)) / 1e6);
    pipe.labelDrains = {};
    pipe.labelDrainMs = {};
    pipe.readerDrains = {};
    pipe.readerDrainMs = {};
    pipe.labelNext.counts = {};
    pipe.labelNext.ms = {};
    pipe.labelItems = pipe.ridingWaits = 0;
    if (labelCensus()) {
        // APS5_LABEL_CENSUS: every queue's waits (n/ms) on a queue-0 label and on anything else.
        auto& census = labelCensusState();
        std::string onLabels, elsewhere;
        std::uint64_t labels = 0;
        std::size_t known = 0;
        {
            std::lock_guard lock(census.mutex);
            for (std::size_t index = 0; index < census.labelWaits.size(); ++index) {
                if (census.labelWaits[index] != 0) {
                    std::snprintf(item, sizeof(item), " 0x%zx %llu/%.1f", index * 8, static_cast<unsigned long long>(census.labelWaits[index]), census.labelWaitMs[index]);
                    onLabels += item;
                }
                if (census.otherWaits[index] != 0) {
                    std::snprintf(item, sizeof(item), " 0x%zx %llu/%.1f", index * 8, static_cast<unsigned long long>(census.otherWaits[index]), census.otherWaitMs[index]);
                    elsewhere += item;
                }
            }
            labels = census.labels;
            known = census.readers.waiters.size();
            census.labels = 0;
            census.labelWaits = census.otherWaits = {};
            census.labelWaitMs = census.otherWaitMs = {};
        }
        std::fprintf(stderr, "[label-census] (10 s): queue-0 label packets %llu (%zu addresses known); waits on them by queue (n/ms):%s; waits on other addresses by queue:%s\n", static_cast<unsigned long long>(labels), known, onLabels.empty() ? " none" : onLabels.c_str(), elsewhere.empty() ? " none" : elsewhere.c_str());
    }
    pipe.occupancy = {};
    pipe.items = pipe.runs = pipe.staleRedos = pipe.overlayDraws = pipe.overlayLoads = pipe.overlayDispatches = pipe.submitsLeft = pipe.dispatchItems = 0;
    pipe.drains = pipe.drainsWaited = {};
    pipe.dispatches = {};
    pipe.drainWaitByReason = {};
    pipe.ringWaitMs = pipe.drainWaitMs = 0;
    pipe.lastReport = std::chrono::steady_clock::now();
}

bool Driver::recordPipeItem(DrawScratch& scratch, RecordPipeDraw& item) {
    // APS5_PROFILE_DRAW: B's phases (a stale or failed call's time joins the path it took) and paths.
    auto& pipe = recordPipe();
    auto& lap = pipe.bLap;
    const auto count = [&](PipeBCount which) {
        if (lap.on) pipe.bCounts[which].fetch_add(1, std::memory_order_relaxed);
    };
    lap.Mark();
    noteDrawWriters(scratch.stages, 0);
    lap.Lap(pipe.bPhaseNs, BNoteWriters);
    const auto& graphics = item.decode->state;
    auto phase = BDraw;
    try {
        if (item.recipe != nullptr) {
            phase = BRecipe;
            if (item.device->DrawFromRecipe(graphics, item.parameters, scratch.stages, scratch.snapshots, item.recipe) == RecipeOutcome::Recorded) {
                lap.Lap(pipe.bPhaseNs, BRecipe);
                count(BRecipeItems);
                return false;
            }
            phase = BRestart;
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
        }
        std::shared_ptr<const DrawRecipe> built;
        item.device->Draw(graphics, item.parameters, scratch.stages, scratch.snapshots, scratch.recipeStages.empty() ? nullptr : &built);
        lap.Lap(pipe.bPhaseNs, phase);
        count(phase == BRestart ? BRestartItems : BFullItems);
        if (built != nullptr) {
            attachDrawRecipe(item.key, scratch.recipeStages, std::move(built));
            lap.Lap(pipe.bPhaseNs, BAttach);
            count(BBuiltItems);
        }
    } catch (const Graphics::SnapshotStale& error) {
        lap.Lap(pipe.bPhaseNs, phase);
        if (item.retryable) return true;
        reportSkip("draw", error.what());
    } catch (const std::exception& error) {
        lap.Lap(pipe.bPhaseNs, phase);
        reportSkip("draw", error.what());
    }
    return false;
}

void Driver::runRecordPipe() noexcept {
    onWorkerThread() = true;
    PinWorkerThread("recording pipe");
    // Batches name queue 0 (G/Recorder.cpp): B's items are queue 0's.
    GuestMemory::TagGpuLockThread(0);
    GuestMemory::MarkRecordPipeThread();
    auto& pipe = recordPipe();
    static const auto hold = pipeMicros("APS5_RECORD_PIPE_HOLD_US", 200);
    const bool profile = recordPipeProfile(), verify = recordPipeVerify(), bSubmit = recordPipeBSubmit(), epochs = RecordPipeEpochActive();
    // APS5_RECORD_PIPE_EPOCH: the hand-over epoch of the last item B recorded (0: none yet).
    std::uint64_t epochStamp = 0;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    auto held = std::chrono::steady_clock::time_point{};
    // APS5_PROFILE_DRAW (the [pipe] B line): the phase clock, the heap and CPU marks, the start of a
    // hold break (the epoch: none) and whether the item ended an idle wait.
    auto& lap = pipe.bLap;
    lap.on = profile;
    auto heapMark = profile ? HostHeap::ThreadCounters() : HostHeap::Counters{};
    std::uint64_t cpuMark = profile ? threadCpuNs() : 0;
    // APS5_DRAW_MEMOS=1 with APS5_PROFILE_DRAW: B's import memo answers so far (its memo starts empty).
    const bool drawMemos = profile && Graphics::DrawMemos();
    std::uint64_t memoHitsMark = 0, memoMissesMark = 0;
    auto breakStart = std::chrono::steady_clock::time_point{};
    bool afterIdle = false;
    const auto count = [&](PipeBCount which) {
        if (profile) pipe.bCounts[which].fetch_add(1, std::memory_order_relaxed);
    };
    // APS5_LAZY_LABELS: B's label group (its labels in a row) closes before its next other item and
    // before it releases the mutex, as recordDeferredLabels closes A's.
    bool labelGroupOpen = false;
    const auto closeLabels = [&] {
        if (!labelGroupOpen) return;
        labelGroupOpen = false;
        Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
    };
    const auto release = [&] {
        if (!gpuLock.owns_lock()) return;
        closeLabels();
        gpuLock.unlock();
        if (!profile) return;
        pipe.busyNs.fetch_add(pipeNs(std::chrono::steady_clock::now() - held), std::memory_order_relaxed);
        // B's heap operations and CPU time since the last release.
        const auto heap = HostHeap::ThreadCounters();
        const auto cpu = threadCpuNs();
        pipe.bAllocations.fetch_add(heap.allocations - heapMark.allocations, std::memory_order_relaxed);
        pipe.bFrees.fetch_add(heap.releases - heapMark.releases, std::memory_order_relaxed);
        if (cpu >= cpuMark) pipe.bCpuNs.fetch_add(cpu - cpuMark, std::memory_order_relaxed);
        heapMark = heap;
        cpuMark = cpu;
        if (!drawMemos) return;
        const auto& memo = Graphics::ThreadHostImportMemo();
        pipe.bCounts[BImportMemoHits].fetch_add(memo.hits - memoHitsMark, std::memory_order_relaxed);
        pipe.bCounts[BImportMemoMisses].fetch_add(memo.misses - memoMissesMark, std::memory_order_relaxed);
        memoHitsMark = memo.hits;
        memoMissesMark = memo.misses;
    };
    // Under the mutex: the batch goes to the GPU once its oldest label is past the deadline.
    const auto submitDueNow = [&] {
        if (!bSubmit || !gpuLock.owns_lock()) return;
        const auto since = Graphics::Recorder::PendingLabelSince();
        if (!since.has_value() || std::chrono::steady_clock::now() - *since < labelFlushDeadline()) return;
        const auto localDevice = device.Load();
        if (localDevice == nullptr) return;
        { const Graphics::SubmitCauseScope submitCause(Graphics::SubmitCause::PipeDeadline); localDevice->SubmitRecorded(true); }
        pipe.bSubmits.fetch_add(1, std::memory_order_relaxed);
    };
    // APS5_PROFILE_DRAW: timed while B holds the mutex (without it the check returns at once).
    const auto submitDue = [&] {
        if (!profile || !gpuLock.owns_lock()) return submitDueNow();
        lap.Mark();
        submitDueNow();
        lap.Lap(pipe.bPhaseNs, BSubmitDue);
    };
    try {
        for (;;) {
            const auto serial = pipe.ring.NextItem();
            if (serial == 0) {
                submitDue();
                release();
                // APS5_PROFILE_DRAW: B's idle wait, whether it slept, and the next item's wake latency.
                const auto idleStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                bool slept = false;
                pipeWait(pipe.bSleeping, pipe.bWake, [&] { return pipe.stopping.load(std::memory_order_acquire) || pipe.ring.NextItem() != 0; }, [] {}, profile ? &slept : nullptr);
                if (profile) {
                    pipe.idleNs.fetch_add(pipeNs(std::chrono::steady_clock::now() - idleStart), std::memory_order_relaxed);
                    count(BIdleWaits);
                    if (slept) count(BIdleSlept);
                    afterIdle = true;
                }
                if (pipe.stopping.load(std::memory_order_acquire)) break;
                continue;
            }
            if (gpuLock.owns_lock()) {
                if (const auto now = std::chrono::steady_clock::now(); now - held >= hold) {
                    breakStart = now;
                    release();
                    std::this_thread::yield();
                }
            }
            auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
            if (afterIdle) {
                // The item that ended an idle wait: since A published it.
                afterIdle = false;
                const auto now = std::chrono::steady_clock::now();
                const auto latency = now > slot.published ? pipeNs(now - slot.published) : 0;
                pipe.wakeNs.fetch_add(latency, std::memory_order_relaxed);
                if (latency > pipe.wakeMaxNs.load(std::memory_order_relaxed)) pipe.wakeMaxNs.store(latency, std::memory_order_relaxed);
                count(BWakes);
            }
            const bool dispatchItem = slot.kind == RecordPipeKind::Dispatch, labelItem = slot.kind == RecordPipeKind::Label;
            const auto opcode = dispatchItem ? 0x15u : labelItem ? slot.labelOpcode : slot.draw.opcode;
            const auto key = dispatchItem ? slot.dispatch.key : labelItem ? 0 : slot.draw.key;
            if (!labelItem) closeLabels();
            if (!gpuLock.owns_lock()) {
                // A hold's lock site is its first item's kind.
                GuestMemory::TagGpuLockSite(dispatchItem ? GuestMemory::GpuLockSite::Dispatch : labelItem ? GuestMemory::GpuLockSite::Label : GuestMemory::GpuLockSite::Draw);
                const auto start = std::chrono::steady_clock::now();
                gpuLock.lock();
                held = std::chrono::steady_clock::now();
                pipe.holds.fetch_add(1, std::memory_order_relaxed);
                if (profile) {
                    pipe.lockWaitNs.fetch_add(pipeNs(held - start), std::memory_order_relaxed);
                    // A hold break: its release, yield and relock, and the relock's wait (in the lock wait too).
                    if (breakStart != std::chrono::steady_clock::time_point{}) {
                        count(BHoldBreaks);
                        pipe.holdBreakNs.fetch_add(pipeNs(held - breakStart), std::memory_order_relaxed);
                        pipe.holdBreakLockNs.fetch_add(pipeNs(held - start), std::memory_order_relaxed);
                    }
                }
                breakStart = {};
            }
            count(BItems);
            // APS5_RECORD_PIPE_EPOCH: a new epoch where A passed an ordering point since the last item
            // (every bump is A's: the queue's ordering points), so walks are reused only within A's epoch.
            if (epochs) {
                switch (RecordPipeEpochFor(slot.epoch, epochStamp)) {
                case RecordPipeEpochStep::Keep:
                    break;
                case RecordPipeEpochStep::Bump:
                    GuestMemory::BumpCollectEpoch();
                    count(BEpochs);
                    break;
                case RecordPipeEpochStep::Unordered:
                    GuestMemory::ResetCollectEpoch();
                    break;
                }
                epochStamp = slot.epoch;
            }
            GuestMemory::SetCurrentPacket(opcode, 0);
            if (labelItem) {
                // A label: its bytes in order after the items before it; no capture or device of its own.
                // APS5_RECORD_PIPE_VERIFY: its write is the item's (verifyPipeReads), with no estimate check.
                lap.Mark();
                GuestMemory::recordedWrites = nullptr;
                if (verify) pipe.writeLog.Begin(serial).emplace_back(slot.label.address, slot.label.address + slot.label.size);
                recordPipeLabel(slot.label.address, std::span<const std::byte>(slot.label.bytes).first(slot.label.size));
                labelGroupOpen = true;
                lap.Lap(pipe.bPhaseNs, BLabel);
                count(BLabelItems);
                submitDue();
                lap.Mark();
                pipe.ring.Consume(serial);
                wakeSide(pipe.aSleeping, pipe.aWake);
                lap.Lap(pipe.bPhaseNs, BCleanup);
                continue;
            }
            if (verify) {
                lap.Mark();
                verifyPipeReads(pipe, dispatchItem ? slot.dispatch.captured : slot.scratch.memory, dispatchItem ? slot.dispatch.consumed : slot.draw.consumed, serial, "item", opcode, key);
                lap.Lap(pipe.bPhaseNs, BVerify);
            }
            GuestMemory::recordedWrites = verify ? &pipe.writeLog.Begin(serial) : nullptr;
            // As lockForDraw: a device replaced since A prepared the item (never a windowed one, so rare).
            auto& itemDevice = dispatchItem ? slot.dispatch.device : slot.draw.device;
            if (auto current = device.Load(); current != nullptr && current != itemDevice) {
                static std::atomic<std::uint64_t> replaced{0};
                std::fprintf(stderr, "[pipe] device replaced before recording (%llu)\n", static_cast<unsigned long long>(++replaced));
                itemDevice = std::move(current);
            }
            bool stale = false;
            if (dispatchItem) {
                lap.Mark();
                recordPipeDispatch(slot.dispatch);
                lap.Lap(pipe.bPhaseNs, BDispatch);
                count(BDispatchItems);
            } else {
                stale = recordPipeItem(slot.scratch, slot.draw);
            }
            GuestMemory::recordedWrites = nullptr;
            submitDue();
            if (stale) {
                count(BStaleItems);
                // Stale: A redoes the item at its next service point, under its own lock.
                release();
                pipe.stale.Raise(serial);
                wakeSide(pipe.aSleeping, pipe.aWake);
                pipeWait(pipe.bSleeping, pipe.bWake, [&] { return pipe.stopping.load(std::memory_order_acquire) || pipe.stale.Redone(serial); }, [] {});
                if (!pipe.stale.Redone(serial)) break;
            }
            if (verify) {
                lap.Mark();
                verifyPipeEstimate(pipe, serial, opcode, key, slot.estimate);
                lap.Lap(pipe.bPhaseNs, BVerify);
            }
            // The device call published the item's pending writes: release what it held, then the slot.
            lap.Mark();
            if (dispatchItem) slot.dispatch = {};
            auto& scratch = slot.scratch;
            scratch.results.clear();
            scratch.hits.results.clear();
            scratch.matched.clear();
            scratch.fresh.clear();
            scratch.recipeStages.clear();
            scratch.stageCaptures.clear();
            scratch.shaderMemory.reset();
            slot.draw.decode.reset();
            slot.draw.entry.reset();
            slot.draw.device.reset();
            slot.draw.recipe.reset();
            // B counts the item once it is in the open batch (A skips handed draws at the packet's
            // end): a submit between A's count and B's recording cleared it, and the EOP's and the
            // submission end's submit-if-open then left the recorded draw in an unsubmitted batch.
            Graphics::Recorder::CountRecordedWork();
            pipe.ring.Consume(serial);
            wakeSide(pipe.aSleeping, pipe.aWake);
            lap.Lap(pipe.bPhaseNs, BCleanup);
        }
    } catch (...) {
        pipe.failure = std::current_exception();
        pipe.failed.store(true, std::memory_order_release);
        wakeSide(pipe.aSleeping, pipe.aWake);
    }
    release();
}

void Driver::stopRecordPipe() {
    if (!RecordPipeActive()) return;
    auto& pipe = recordPipe();
    if (!pipe.thread.joinable()) return;
    pipe.stopping.store(true, std::memory_order_release);
    PollWake(pipe.bWake.load(std::memory_order_acquire));
    pipe.thread.join();
}

}
