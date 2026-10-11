#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPIPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPIPE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

// The queue-0 recording pipe (APS5_RECORD_PIPE=1; docs/design/parallel-recording.md stage 1 and
// parallel-recording-s1-plan.md): the queue thread (A) hands direct and GPU-indirect draws to one
// recording thread (B) through a ring of slots that each own a whole DrawScratch. The pure parts
// below are header-only for tests/Submit.cpp; the threads, drains and stale redo are in RecordPipe.cpp.

// APS5_RECORD_PIPE=1 (read once; default off), declined under the switches that need A-side state
// (plan 7). The stage-0 census is off while it is on.
bool RecordPipeActive();

// Item serials count from 1 (0: none). `handed` is the last serial A published, `consumed` the last
// B recorded; 64-bit, so they never wrap. Serial s lives in slot s % Depth: A may fill the next slot
// only while fewer than Depth items are unrecorded (else B still owns it).
struct RecordPipeRing {
    static constexpr std::uint64_t Depth = 32;
    static constexpr std::size_t Slot(std::uint64_t serial) { return static_cast<std::size_t>(serial % Depth); }
    static constexpr std::uint64_t Pending(std::uint64_t handed, std::uint64_t consumed) { return handed - consumed; }
    static constexpr bool Empty(std::uint64_t handed, std::uint64_t consumed) { return handed == consumed; }
    static constexpr bool Full(std::uint64_t handed, std::uint64_t consumed) { return handed - consumed >= Depth; }

    std::atomic<std::uint64_t> handed{0}, consumed{0};
    // A: the serial whose slot (and DrawScratch) it may fill next, or 0 while the ring is full.
    std::uint64_t NextFree() const {
        const auto last = handed.load(std::memory_order_relaxed);
        return Full(last, consumed.load(std::memory_order_acquire)) ? 0 : last + 1;
    }
    // A, once the slot is filled: the release pairs with NextItem's acquire.
    void Publish(std::uint64_t serial) { handed.store(serial, std::memory_order_release); }
    // B: the serial of the next item to record, or 0 while the ring is empty.
    std::uint64_t NextItem() const {
        const auto last = consumed.load(std::memory_order_relaxed);
        return Empty(handed.load(std::memory_order_acquire), last) ? 0 : last + 1;
    }
    // B, after the device call published the item's pending writes: A may reuse the slot and prune.
    void Consume(std::uint64_t serial) { consumed.store(serial, std::memory_order_release); }
};

// The writes of handed items B has not recorded (shader-written buffers; padded color, depth, HTILE
// and DCC targets), each range tagged with its newest item's serial: A adds an item's ranges at
// hand-over and prunes by `consumed` before a capture, validation or indirect read checks it.
using RecordPipeOverlay = PipeCensusOverlay;

// An item's queue state at hand-over, for A's redo of a stale item: QueueState without constantRam
// (draws never read it; DUMP_CONST_RAM, its only reader, drains) and without the fastState memo
// (Restore resets it). ~9 KB with the register files instead of 57; saving into the slot's
// long-lived mark keeps the vectors' capacity.
struct QueueStateMark {
    Registers shader, context, userConfig;
    std::optional<Registers> savedContext;
    std::uint64_t indexBase = 0, drawIndirectBase = 0, dispatchIndirectBase = 0;
    std::uint32_t indexBufferSize = 0, indexType = 0, instanceCount = 1;
    std::vector<std::string> markers;
    std::uint64_t stateSerial = 0;

    template<typename From, typename To>
    static void Copy(const From& from, To& to) {
        to.shader = from.shader; to.context = from.context; to.userConfig = from.userConfig; to.savedContext = from.savedContext;
        to.indexBase = from.indexBase; to.drawIndirectBase = from.drawIndirectBase; to.dispatchIndirectBase = from.dispatchIndirectBase;
        to.indexBufferSize = from.indexBufferSize; to.indexType = from.indexType; to.instanceCount = from.instanceCount;
        to.markers = from.markers; to.stateSerial = from.stateSerial;
    }
    void Save(const QueueState& queue) { Copy(queue, *this); }
    void Restore(QueueState& queue) const {
        Copy(*this, queue);
        queue.fastState = {};
    }
};
// The mark has every other QueueState field in the same order (so the same padding): a new field
// fails here unless it fits in padding.
static_assert(sizeof(QueueState) == sizeof(QueueStateMark) + sizeof(QueueState::constantRam) + sizeof(QueueStateMemo));

// Stage 1's hand-over: a draw about to take the GPU lock directly (queue 0, retry level 0, no locked
// prepare, not replaying, a windowed device, a slot scratch) with no deferred labels is handed over;
// every other recording draw drains before lockForDraw, with the census's reason.
inline PipeCensusVerdict RecordPipeDrawVerdict(PipeCensusLock lock, bool labelsPending) {
    return ClassifyPipeCensus({0, true, false, false, false, lock, PipeCensusDispatch::None, labelsPending}, false);
}

// Whether A drains before a packet runs (PacketExecution.cpp, before flushBetweenPackets): draws
// decide in the packet; both dispatches drain (the game always puts a label or wait between draws
// and compute, so it costs nothing, and the fill/copy HLE is covered); a sample dump (0x46 event
// 0x39) and an indirect register load (0x63/0x64/0x9f) whose pairs lie in the pruned overlay drain;
// the rest as the census. wroteOnGpu is not known yet and needs no test: it is set only for labels
// and copies (0x37/0x49/0x40/0x50/0x83, PacketMemory.cpp), which drain by opcode.
inline std::optional<PipeCensusDrain> RecordPipeTopDrain(std::span<const std::uint32_t> packet, bool drawPacket, bool flip, bool renderingWait, const RecordPipeOverlay& overlay) {
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if (drawPacket) return std::nullopt;
    if (opcode == 0x15 && !flip && !renderingWait) return PipeCensusDrain::Dispatch;
    if (opcode == 0x46 && packet.size() > 1 && (packet[1] & 0x3fu) == 0x39u) return PipeCensusDrain::Other;
    if ((opcode == 0x63 || opcode == 0x64 || opcode == 0x9f) && packet.size() > 4) {
        const auto begin = packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u);
        const auto end = begin + static_cast<std::uint64_t>(packet[4]) * 8;
        if (end > begin && overlay.Overlaps(begin, end) != 0) return PipeCensusDrain::Overlap;
        return std::nullopt;
    }
    return ClassifyPipeCensus({packet[0], false, flip, renderingWait}, false).drain;
}

// The stale handshake: B, whose device call threw SnapshotStale for item k, records nothing for it,
// releases the GPU mutex, raises k and waits; A, at its next service point (a packet boundary or any
// of its pipe waits), redoes k through the retry ladder and finishes it; B then consumes k and goes on
// with k + 1, which was prepared while k was unconsumed (its writes in the overlay).
struct RecordPipeStale {
    std::atomic<std::uint64_t> raised{0}, redone{0};
    // B.
    void Raise(std::uint64_t serial) { raised.store(serial, std::memory_order_release); }
    bool Redone(std::uint64_t serial) const { return redone.load(std::memory_order_acquire) == serial; }
    // A: the item to redo, or 0; Finish publishes the redo (the release pairs with Redone).
    std::uint64_t Pending() const { return raised.load(std::memory_order_acquire); }
    void Finish(std::uint64_t serial) {
        raised.store(0, std::memory_order_relaxed);
        redone.store(serial, std::memory_order_release);
    }
};

}

#endif
