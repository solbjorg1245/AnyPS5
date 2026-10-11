#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawScratch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/RecordPipe.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/PassDump.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libc/include/HostThread.hpp"
#include "Recompiler.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace AgcDriver::DriverDetail {

namespace {

// Slot k = serial % Depth: the item's whole DrawScratch (stages, results, snapshots and every byte
// they point into), the rest its locked half reads, and its queue state for a redo.
struct RecordPipeSlot {
    DrawScratch scratch;
    RecordPipeDraw draw;
    QueueStateMark mark;
};

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
    std::atomic<std::uint64_t> verifyMissTotal{0}, verifyChecks{0}, verifyMisses{0};
    std::unique_ptr<QueueState> redoQueue;
    std::uint64_t run = 0, items = 0, runs = 0, staleRedos = 0, overlayDraws = 0, overlayLoads = 0;
    std::array<std::uint64_t, PipeCensusDrains> drains{};
    double ringWaitMs = 0, drainWaitMs = 0;
    // The drains that found unrecorded items, and A's wait in them, by reason.
    std::array<std::uint64_t, PipeCensusDrains> drainsWaited{};
    std::array<double, PipeCensusDrains> drainWaitByReason{};
    // Due batch submits A left to B, and B's.
    std::uint64_t submitsLeft = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> holds{0}, busyNs{0}, lockWaitNs{0}, bSubmits{0};
};

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
// checked again after `sleeping` is set, so the other side's wake (wakeSide) is never lost.
template <typename TDone, typename TBetween>
void pipeWait(std::atomic<bool>& sleeping, std::atomic<void*>& wake, TDone&& done, TBetween&& between) {
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
        if (!done() && !PollSleepOrWake(event, PollTryInterval)) PollSleep();
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
        const char* value = std::getenv("APS5_RECORD_PIPE");
        if (value == nullptr || std::strcmp(value, "1") != 0) return false;
        const auto set = [](const char* name) { return std::getenv(name) != nullptr; };
        const char* fastDispatch = std::getenv("APS5_FAST_DISPATCH");
        const bool declined = FastDrawEnabled() || FastDrawVerifyEvery() != 0 || set("APS5_LOCKED_DRAW_PREPARE") || Graphics::Recorder::DispatchOverlap() || FastCensusActive() || (fastDispatch != nullptr && std::strcmp(fastDispatch, "0") != 0) || PassDump::Enabled() || set("APS5_CAPTURE_DIR") || Graphics::CheckpointsRequested() || set("APS5_DUMP_DRAW_SHADERS") || set("APS5_DUMP_DRAW_SLOT1") || ShaderRecompiler::DebugProbeActive() || !LabelBatchSubmit() || set("APS5_DRAIN_ALL") || set("APS5_DRAW_DRAIN");
        std::fprintf(stderr, "[pipe] queue 0 recording pipe %s\n", declined ? "declined: a fast-path, verify, dump or drain switch is set" : "on");
        return !declined;
    }();
    return active;
}

bool Driver::recordPipeDrawing(std::uint32_t queue) {
    return queue == 0 && RecordPipeActive() && !recordPipeReplaying();
}

DrawScratch* Driver::recordPipeScratch() {
    auto& pipe = recordPipe();
    const auto serial = pipe.ring.NextFree();
    return serial == 0 ? nullptr : &pipe.slots[RecordPipeRing::Slot(serial)].scratch;
}

std::uint64_t Driver::recordPipeConsumed() {
    return recordPipe().ring.consumed.load(std::memory_order_acquire);
}

bool Driver::recordPipeOverlaps(std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t consumed) {
    auto& pipe = recordPipe();
    // `consumed` as the draw began: the items recorded by then were in the pending writes its
    // captures and validation consulted.
    pipe.overlay.Prune(consumed);
    if (pipe.overlay.ranges.empty()) return false;
    for (const auto& region : regions) {
        if (pipe.overlay.Overlaps(region.guestAddress, region.guestAddress + region.bytes.size()) == 0) continue;
        ++pipe.overlayDraws;
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
    slot.draw = std::move(draw);
    slot.mark.Save(queue);
    for (const auto& stage : scratch.stages) notePipeItemWrites(*stage.program, serial, pipe.overlay);
    notePipeTargets(slot.draw.decode->state, serial, pipe.overlay);
    if (!pipe.thread.joinable()) pipe.thread = HostThread([this] { runRecordPipe(); });
    pipe.ring.Publish(serial);
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
    }
    pipe.overlay.ranges.clear();
}

void Driver::recordPipeBoundary(std::span<const std::uint32_t> packet, std::uint32_t header) {
    auto& pipe = recordPipe();
    serviceRecordPipe();
    const bool special = header == FlipPacketHeader || header == RenderingWaitPacketHeader;
    if (!special && Pm4::DrawOpcode((header >> 8u) & 0xffu)) {
        // The draw's lease takes slot handed + 1: wait while B still owns it.
        const auto handed = pipe.ring.handed.load(std::memory_order_relaxed);
        if (!RecordPipeRing::Full(handed, pipe.ring.consumed.load(std::memory_order_acquire))) return;
        const auto start = recordPipeProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        pipeWait(pipe.aSleeping, pipe.aWake, [&] { return !RecordPipeRing::Full(handed, pipe.ring.consumed.load(std::memory_order_acquire)); }, [&] { recordPipeWaitRound(); });
        if (recordPipeProfile()) pipe.ringWaitMs += sinceMs(start);
        return;
    }
    // No run open: the ring is empty.
    if (pipe.run == 0) return;
    pipe.overlay.Prune(pipe.ring.consumed.load(std::memory_order_acquire));
    const auto why = RecordPipeTopDrain(packet, false, header == FlipPacketHeader, header == RenderingWaitPacketHeader, pipe.overlay);
    if (!why) return;
    if (*why == PipeCensusDrain::Overlap) ++pipe.overlayLoads;
    drainRecordPipe(*why);
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
    char verify[80] = "";
    if (recordPipeVerify()) std::snprintf(verify, sizeof(verify), ", verify: checks %llu, misses %llu", static_cast<unsigned long long>(pipe.verifyChecks.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(pipe.verifyMisses.exchange(0, std::memory_order_relaxed)));
    std::fprintf(stderr, "[pipe] queue 0 (10 s): items %llu, runs %llu (mean %.1f), holds %llu, drains:%s, A waited ms (ring full) %.1f, A drain wait ms %.1f (by reason, ms/drains that waited:%s), B busy ms %.1f, B lock wait ms %.1f, B deadline submits %llu (A left %llu to B), stale redos %llu, overlay hits %llu (draw %llu, load %llu)%s\n", static_cast<unsigned long long>(pipe.items), static_cast<unsigned long long>(pipe.runs), pipe.runs == 0 ? 0.0 : static_cast<double>(pipe.items) / static_cast<double>(pipe.runs), static_cast<unsigned long long>(pipe.holds.exchange(0, std::memory_order_relaxed)), drains.c_str(), pipe.ringWaitMs, pipe.drainWaitMs, waits.c_str(), busyMs, lockWaitMs, static_cast<unsigned long long>(pipe.bSubmits.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(pipe.submitsLeft), static_cast<unsigned long long>(pipe.staleRedos), static_cast<unsigned long long>(pipe.overlayDraws + pipe.overlayLoads), static_cast<unsigned long long>(pipe.overlayDraws), static_cast<unsigned long long>(pipe.overlayLoads), verify);
    pipe.items = pipe.runs = pipe.staleRedos = pipe.overlayDraws = pipe.overlayLoads = pipe.submitsLeft = 0;
    pipe.drains = pipe.drainsWaited = {};
    pipe.drainWaitByReason = {};
    pipe.ringWaitMs = pipe.drainWaitMs = 0;
    pipe.lastReport = std::chrono::steady_clock::now();
}

bool Driver::recordPipeItem(DrawScratch& scratch, RecordPipeDraw& item) {
    // As lockForDraw: a device replaced since A prepared the draw (never a windowed one, so rare).
    if (auto current = device.Load(); current != nullptr && current != item.device) {
        static std::atomic<std::uint64_t> replaced{0};
        std::fprintf(stderr, "[pipe] device replaced before recording (%llu)\n", static_cast<unsigned long long>(++replaced));
        item.device = std::move(current);
    }
    noteDrawWriters(scratch.stages, 0);
    const auto& graphics = item.decode->state;
    try {
        if (item.recipe != nullptr) {
            if (item.device->DrawFromRecipe(graphics, item.parameters, scratch.stages, scratch.snapshots, item.recipe) == RecipeOutcome::Recorded) return false;
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
        }
        std::shared_ptr<const DrawRecipe> built;
        item.device->Draw(graphics, item.parameters, scratch.stages, scratch.snapshots, scratch.recipeStages.empty() ? nullptr : &built);
        if (built != nullptr) attachDrawRecipe(item.key, scratch.recipeStages, std::move(built));
    } catch (const Graphics::SnapshotStale& error) {
        if (item.retryable) return true;
        reportSkip("draw", error.what());
    } catch (const std::exception& error) {
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
    const bool profile = recordPipeProfile(), verify = recordPipeVerify(), bSubmit = recordPipeBSubmit();
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    auto held = std::chrono::steady_clock::time_point{};
    const auto release = [&] {
        if (!gpuLock.owns_lock()) return;
        gpuLock.unlock();
        if (profile) pipe.busyNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - held).count()), std::memory_order_relaxed);
    };
    // Under the mutex: the batch goes to the GPU once its oldest label is past the deadline.
    const auto submitDue = [&] {
        if (!bSubmit || !gpuLock.owns_lock()) return;
        const auto since = Graphics::Recorder::PendingLabelSince();
        if (!since.has_value() || std::chrono::steady_clock::now() - *since < labelFlushDeadline()) return;
        const auto localDevice = device.Load();
        if (localDevice == nullptr) return;
        localDevice->SubmitRecorded(true);
        pipe.bSubmits.fetch_add(1, std::memory_order_relaxed);
    };
    try {
        for (;;) {
            const auto serial = pipe.ring.NextItem();
            if (serial == 0) {
                submitDue();
                release();
                pipeWait(pipe.bSleeping, pipe.bWake, [&] { return pipe.stopping.load(std::memory_order_acquire) || pipe.ring.NextItem() != 0; }, [] {});
                if (pipe.stopping.load(std::memory_order_acquire)) break;
                continue;
            }
            if (gpuLock.owns_lock() && std::chrono::steady_clock::now() - held >= hold) {
                release();
                std::this_thread::yield();
            }
            if (!gpuLock.owns_lock()) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
                const auto start = std::chrono::steady_clock::now();
                gpuLock.lock();
                held = std::chrono::steady_clock::now();
                pipe.holds.fetch_add(1, std::memory_order_relaxed);
                if (profile) pipe.lockWaitNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(held - start).count()), std::memory_order_relaxed);
            }
            auto& slot = pipe.slots[RecordPipeRing::Slot(serial)];
            GuestMemory::SetCurrentPacket(slot.draw.opcode, 0);
            if (verify) verifyPipeReads(pipe, slot.scratch.memory, slot.draw.consumed, serial, "item", slot.draw.opcode, slot.draw.key);
            GuestMemory::recordedWrites = verify ? &pipe.writeLog.Begin(serial) : nullptr;
            const bool stale = recordPipeItem(slot.scratch, slot.draw);
            GuestMemory::recordedWrites = nullptr;
            submitDue();
            if (stale) {
                // Stale: A redoes the item at its next service point, under its own lock.
                release();
                pipe.stale.Raise(serial);
                wakeSide(pipe.aSleeping, pipe.aWake);
                pipeWait(pipe.bSleeping, pipe.bWake, [&] { return pipe.stopping.load(std::memory_order_acquire) || pipe.stale.Redone(serial); }, [] {});
                if (!pipe.stale.Redone(serial)) break;
            }
            // The device call published the item's pending writes: release what it held, then the slot.
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
