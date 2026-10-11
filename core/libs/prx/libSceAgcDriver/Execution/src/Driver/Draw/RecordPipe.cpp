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
    std::unique_ptr<QueueState> redoQueue;
    std::uint64_t run = 0, items = 0, runs = 0, staleRedos = 0, overlayDraws = 0, overlayLoads = 0;
    std::array<std::uint64_t, PipeCensusDrains> drains{};
    double ringWaitMs = 0, drainWaitMs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> holds{0}, busyNs{0}, lockWaitNs{0};
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

bool recordPipeProfile() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
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
    ++pipe.items;
    ++pipe.run;
    wakeSide(pipe.bSleeping, pipe.bWake);
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
            ~Replay() { replaying = false; }
        } replay{recordPipeReplaying()};
        replay.replaying = true;
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
    const auto start = recordPipeProfile() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    pipeWait(pipe.aSleeping, pipe.aWake, [&] { return RecordPipeRing::Empty(pipe.ring.handed.load(std::memory_order_relaxed), pipe.ring.consumed.load(std::memory_order_acquire)); }, [&] { recordPipeWaitRound(); });
    if (recordPipeProfile()) pipe.drainWaitMs += sinceMs(start);
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
    std::string drains;
    char item[64];
    for (std::size_t why = 0; why < PipeCensusDrains; ++why) {
        std::snprintf(item, sizeof(item), " %s %llu", PipeCensusDrainNames[why], static_cast<unsigned long long>(pipe.drains[why]));
        drains += item;
    }
    const auto ms = [](std::atomic<std::uint64_t>& ns) { return static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 1e6; };
    const auto busyMs = ms(pipe.busyNs), lockWaitMs = ms(pipe.lockWaitNs);
    std::fprintf(stderr, "[pipe] queue 0 (10 s): items %llu, runs %llu (mean %.1f), holds %llu, drains:%s, A waited ms (ring full) %.1f, A drain wait ms %.1f, B busy ms %.1f, B lock wait ms %.1f, stale redos %llu, overlay hits %llu (draw %llu, load %llu)\n", static_cast<unsigned long long>(pipe.items), static_cast<unsigned long long>(pipe.runs), pipe.runs == 0 ? 0.0 : static_cast<double>(pipe.items) / static_cast<double>(pipe.runs), static_cast<unsigned long long>(pipe.holds.exchange(0, std::memory_order_relaxed)), drains.c_str(), pipe.ringWaitMs, pipe.drainWaitMs, busyMs, lockWaitMs, static_cast<unsigned long long>(pipe.staleRedos), static_cast<unsigned long long>(pipe.overlayDraws + pipe.overlayLoads), static_cast<unsigned long long>(pipe.overlayDraws), static_cast<unsigned long long>(pipe.overlayLoads));
    pipe.items = pipe.runs = pipe.staleRedos = pipe.overlayDraws = pipe.overlayLoads = 0;
    pipe.drains = {};
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
    auto& pipe = recordPipe();
    static const auto hold = pipeMicros("APS5_RECORD_PIPE_HOLD_US", 200);
    const bool profile = recordPipeProfile();
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    auto held = std::chrono::steady_clock::time_point{};
    const auto release = [&] {
        if (!gpuLock.owns_lock()) return;
        gpuLock.unlock();
        if (profile) pipe.busyNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - held).count()), std::memory_order_relaxed);
    };
    try {
        for (;;) {
            const auto serial = pipe.ring.NextItem();
            if (serial == 0) {
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
            if (recordPipeItem(slot.scratch, slot.draw)) {
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
