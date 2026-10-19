#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPIPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_RECORDPIPE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

// The queue-0 recording pipe (APS5_RECORD_PIPE=1; docs/design/parallel-recording.md stage 1 and
// parallel-recording-s1-plan.md): the queue thread (A) hands direct and GPU-indirect draws to one
// recording thread (B) through a ring of slots that each own a whole DrawScratch. The pure parts
// below are header-only for tests/Submit.cpp; the threads, drains and stale redo are in RecordPipe.cpp.

// APS5_RECORD_PIPE (read once; on by default since s60, "0" = off), declined under the switches that
// need A-side state (plan 7); APS5_CAPTURE_DIR no longer does (S3.1b: A records on its own while a
// frame trace runs). The stage-0 census is off while it is on.
bool RecordPipeActive();

// APS5_RECORD_PIPE_DISPATCH=1 (read once, only with the pipe on; default off): queue-0 direct
// dispatches ride it too (parallel-recording-s3-plan.md S3.1), declined under APS5_PROBE_DISPATCH
// and APS5_ASPECT_PROBE (their state is A's).
bool RecordPipeDispatchActive();

// APS5_RECORD_PIPE_EPOCH=1 (read once, only with the pipe on; default off; parallel-recording-s4-plan.md
// S4.1): B's collect epoch follows A's. A stamps each item with its thread's epoch at hand-over
// (GuestMemory::ThreadCollectEpoch); B bumps its own before an item whose stamp differs from the
// previous item's, so B reuses a walk (the collect memo) only among the items A handed over in one
// epoch: the reuse A had while it recorded them itself. Without it B never bumps (a fresh epoch per
// collect: no walk is reused).
bool RecordPipeEpochActive();

// B's collect epoch before an item stamped `stamp`, after one stamped `last` (0: none yet): kept (the
// same epoch), bumped (A passed an ordering point between the two), or reset to a fresh epoch per
// collect (A had none, so nothing ordered the CPU's writes for the item).
enum class RecordPipeEpochStep : std::uint8_t { Keep, Bump, Unordered };
constexpr RecordPipeEpochStep RecordPipeEpochFor(std::uint64_t stamp, std::uint64_t last) {
    return stamp == 0 ? RecordPipeEpochStep::Unordered : stamp == last ? RecordPipeEpochStep::Keep : RecordPipeEpochStep::Bump;
}

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

// APS5_HANDOFF_FAST (P-HANDOFF; read once, only with the pipe on; default on, =0 the old path): B's
// hand-off penalty.
// A draw recorded on B took 12.09 us against 10.24 us inline (t911/t912, APS5_RECORD_PROFILE), the
// gap in the spans that first read what A wrote for the item (inputs +0.71, revalidate +0.39), and B
// dropped the item's references after recording it (cleanup 0.64 us per item: shared counts A had
// raised, capture pages A had allocated). Parts: Release, B leaves the item's references in its slot
// and A drops them when it fills the slot again, on the thread that took them; Prefetch, before
// recording item k B prefetches item k+1's slot when A has published it (its draw, stage and snapshot
// lists, its decode and the captured bytes, up to APS5_HANDOFF_PREFETCH_KB, 16), so those lines come
// over while k records. Unset or "1" takes both; another number is a mask of the parts (2 Release,
// 4 Prefetch), and 0 or junk the old path. On: 63.6 presents/10 s against 60.2 (t914-t919, 3+3).
enum RecordPipeHandoffPart : unsigned { HandoffRelease = 2, HandoffPrefetch = 4 };
constexpr unsigned RecordPipeHandoffParts(const char* value) {
    if (value == nullptr || value[0] == '\0') return HandoffRelease | HandoffPrefetch;
    unsigned parsed = 0;
    for (const char* digit = value; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' || parsed > 1000) return 0;
        parsed = parsed * 10 + static_cast<unsigned>(*digit - '0');
    }
    return parsed == 1 ? HandoffRelease | HandoffPrefetch : parsed & (HandoffRelease | HandoffPrefetch);
}
unsigned RecordPipeHandoff();

// The prefetch's byte budget for one item: Lines hands `touch` each 64-byte line of [data, data +
// bytes) in order while the budget holds a whole line, and takes the lines from it.
struct RecordPipePrefetchBudget {
    static constexpr std::size_t Line = 64;
    std::size_t left = 0;
    template<typename TTouch>
    void Lines(const void* data, std::size_t bytes, TTouch&& touch) {
        if (data == nullptr || bytes == 0) return;
        const auto end = reinterpret_cast<std::uintptr_t>(data) + bytes;
        for (auto line = reinterpret_cast<std::uintptr_t>(data) & ~std::uintptr_t{Line - 1}; line < end && left >= Line; line += Line, left -= Line) touch(reinterpret_cast<const void*>(line));
    }
};

// APS5_RECORD_CENSUS (multi-recorder S0): B's draws by run (a drain ends A's run), cut into chunks of
// K draws as a dispatcher would hand them alternately to two recorders: the chunks, the neighbouring
// chunk pairs of one run (recorded at the same time) and those sharing a template (a conflict the
// per-template lock of S2 would serialise; null templates never conflict).
struct RecordPipeChunkCensus {
    std::size_t k = 16;
    std::uint64_t chunks = 0, pairs = 0, sharedPairs = 0;
    std::size_t position = 0;
    std::vector<const void*> current, previous;

    void NewRun() {
        Close();
        previous.clear();
        position = 0;
    }
    void Note(const void* templateId) {
        if (position != 0 && position % k == 0) Close();
        current.push_back(templateId);
        ++position;
    }
    void Close() {
        if (current.empty()) return;
        ++chunks;
        if (!previous.empty()) {
            ++pairs;
            bool shared = false;
            for (const auto* id : current) {
                for (const auto* earlier : previous) shared = shared || (id != nullptr && id == earlier);
            }
            if (shared) ++sharedPairs;
        }
        previous.swap(current);
        current.clear();
    }
};

// The census's run-length buckets (draws per run): 1, 2-7, 8-31, 32-127, 128-511, 512+.
inline constexpr std::size_t RecordPipeRunBuckets = 6;
constexpr std::size_t RecordPipeRunBucket(std::uint64_t draws) {
    return draws <= 1 ? 0 : draws < 8 ? 1 : draws < 32 ? 2 : draws < 128 ? 3 : draws < 512 ? 4 : 5;
}

// APS5_PROFILE_DRAW, the [pipe] B line: the items unrecorded once A published one (handed minus
// consumed), bucketed 1, 2-4, 5-16, 17-31 and a full ring (0 counts as 1).
inline constexpr std::size_t RecordPipeOccupancyBuckets = 5;
constexpr std::size_t RecordPipeOccupancyBucket(std::uint64_t outstanding) {
    return outstanding <= 1 ? 0 : outstanding <= 4 ? 1 : outstanding <= 16 ? 2 : outstanding < RecordPipeRing::Depth ? 3 : 4;
}

// The writes of handed items B has not recorded (shader-written buffers; padded color, depth, HTILE
// and DCC targets), each range tagged with its newest item's serial: A adds an item's ranges at
// hand-over and prunes by `consumed` before a capture, validation or indirect read checks it.
using RecordPipeOverlay = PipeCensusOverlay;

// What a slot holds: a draw (its DrawScratch and RecordPipeDraw), a dispatch (RecordPipeDispatch) or
// a label (APS5_LAZY_LABELS: its bytes, a DeferredLabel).
enum class RecordPipeKind : std::uint8_t { Draw, Dispatch, Label };

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

// Stage 3's hand-over (parallel-recording-s3-plan.md section 2): a queue-0 dispatch as A knows it
// before PrepareRecipe (Dispatch.cpp). local: its packet is not in the submission's commands (the
// arrays 0x16 passes, CPU-read, GPU-indirect or fill-kernel; 0x16 drains at the top); window: the
// device has a window (as draws); bda: a BdaPagetable binding (S3.2 takes the load-only ones,
// bdaStores false, which can turn stale on B); imageWrites: it writes a storage image (its ranges
// join the overlay). Handed over, a drain with the census's reason, or neither (A's stale redo).
struct RecordPipeDispatchFacts {
    bool replaying = false, local = false, window = true, labelsPending = false, bda = false, bdaStores = false, imageWrites = false;
};

inline PipeCensusVerdict RecordPipeDispatchVerdict(const RecordPipeDispatchFacts& facts) {
    const auto drain = [](PipeCensusDrain why) { return PipeCensusVerdict{false, why}; };
    if (facts.replaying) return {};
    if (facts.local) return drain(PipeCensusDrain::IndirectDispatch);
    if (!facts.window) return drain(PipeCensusDrain::Other);
    if (facts.labelsPending) return drain(PipeCensusDrain::LabelsPending);
    if (facts.bda) return drain(PipeCensusDrain::Dispatch);
    return {true, std::nullopt};
}

// APS5_PROFILE_DRAW (A, with the pipe on): the verdict joins the [pipe] line's dispatch counts (S3.0).
void NoteRecordPipeDispatch(const RecordPipeDispatchFacts& facts);

// The overlay ranges of a storage image a handed item writes, from its T# (8 dwords): the surface's
// guest bytes + 64 KiB from its base and its DCC (a byte per 256 + 4 KiB); a null T# has none; no
// words or a T# the decoder rejects, everything (each capture drains until the item is recorded).
void RecordPipeImageRanges(std::span<const std::uint32_t> words, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);

// Whether A drains before a packet runs (PacketExecution.cpp, before flushBetweenPackets): draws
// decide in the packet; both dispatches drain (the game always puts a label or wait between draws
// and compute, so it costs nothing, and the fill/copy HLE is covered), except a direct one with
// `dispatches` (APS5_RECORD_PIPE_DISPATCH), which decides in the packet like a draw (Dispatch.cpp;
// the fill/copy HLE drains itself); a sample dump (0x46 event 0x39) and an indirect register load
// (0x63/0x64/0x9f) whose pairs lie in the pruned overlay drain; the rest as the census. wroteOnGpu
// is not known yet and needs no test: it is set only for labels and copies (0x37/0x49/0x40/0x50/0x83,
// PacketMemory.cpp), which drain by opcode.
inline std::optional<PipeCensusDrain> RecordPipeTopDrain(std::span<const std::uint32_t> packet, bool drawPacket, bool flip, bool renderingWait, const RecordPipeOverlay& overlay, bool dispatches = false) {
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if (drawPacket) return std::nullopt;
    if (opcode == 0x15 && !flip && !renderingWait) return dispatches ? std::optional<PipeCensusDrain>{} : PipeCensusDrain::Dispatch;
    if (opcode == 0x46 && packet.size() > 1 && (packet[1] & 0x3fu) == 0x39u) return PipeCensusDrain::Other;
    if ((opcode == 0x63 || opcode == 0x64 || opcode == 0x9f) && packet.size() > 4) {
        const auto begin = packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u);
        const auto end = begin + static_cast<std::uint64_t>(packet[4]) * 8;
        if (end > begin && overlay.Overlaps(begin, end) != 0) return PipeCensusDrain::Overlap;
        return std::nullopt;
    }
    return ClassifyPipeCensus({packet[0], false, flip, renderingWait}, false).drain;
}

// APS5_LAZY_LABELS=1 (P-LABEL; read once, only with the pipe on; default off): a queue-0 label the
// deferred list would take (WRITE_DATA or RELEASE_MEM without an EOP interrupt, <= 32 dword-aligned
// bytes) no longer drains the pipe while it holds unrecorded items: it rides the ring as a label item
// that B records in order after them (parallel-recording-s2-plan.md S2.2, plain labels only), so A
// goes on with the next packet. With the ring empty it joins the deferred list as before; EOP
// interrupts, stores and custom labels drain as before. Declined (old path) under
// APS5_LABEL_LOCK_EACH, APS5_DRAIN_COMPLETION_LABELS and without the queued-label table.
bool RecordPipeLabelsActive();

// APS5_LAZY_LABELS=2 (P-LABEL; default off): labels ride as with =1, and so does a queue-0 wait whose
// awaited bytes a riding label wrote last (RecordPipeRidingValue) with a value it accepts: its label
// is recorded before the items after the wait, so it is satisfied in order, as a same-queue hit in
// the recorder's label table satisfies it today (WaitMemory.cpp), without draining. Any other wait
// drains as before.
bool RecordPipeWaitsActive();

// A label riding the ring (A, from its hand-over until the next drain).
struct RecordPipeRidingLabel {
    std::uint64_t address = 0;
    std::size_t size = 0;
    std::array<std::byte, DeferredLabel::Capacity> bytes{};
    std::uint64_t serial = 0;
};

// A label packet as the ride verdict sees it: its opcode, whether a RELEASE_MEM raises an EOP
// interrupt, and what Pm4::DecodeLabelWrite made of it (decoded false: nothing).
struct RecordPipeLabelFacts {
    std::uint32_t opcode = 0;
    bool interrupt = false, decoded = false;
    std::uint64_t address = 0;
    std::size_t bytes = 0;
};

// Whether the label rides the ring (A's boundary skips the drain and PacketMemory hands it over):
// exactly the labels PacketMemory.cpp defers, so a label that does not ride drains as before.
constexpr bool RecordPipeLabelRides(const RecordPipeLabelFacts& facts) {
    return (facts.opcode == 0x37 || facts.opcode == 0x49) && !facts.interrupt && facts.decoded && facts.bytes != 0 && facts.bytes <= DeferredLabel::Capacity && facts.bytes % 4 == 0 && facts.address % 4 == 0;
}

// The label census (P-LABEL; APS5_PROFILE_DRAW, the [pipe] labels line): each label drain that found
// unrecorded items, by the label's kind, and by what A did next: hand an item over (riding the ring,
// the label would have joined two runs and saved the whole wait) or drain for another reason (riding
// moves the wait there). Labels in a row stay one event. Custom: PM4 0x10 labels.
enum class RecordPipeLabelKind : std::uint8_t { WriteData, ReleaseMem, EopInterrupt, Custom, Count };
inline constexpr std::size_t RecordPipeLabelKinds = static_cast<std::size_t>(RecordPipeLabelKind::Count);
inline constexpr std::array<const char*, RecordPipeLabelKinds> RecordPipeLabelKindNames{"write data", "release mem", "eop interrupt", "custom"};
constexpr RecordPipeLabelKind RecordPipeLabelKindOf(std::uint32_t opcode, bool interrupt) {
    return opcode == 0x37 ? RecordPipeLabelKind::WriteData : opcode == 0x49 ? (interrupt ? RecordPipeLabelKind::EopInterrupt : RecordPipeLabelKind::ReleaseMem) : RecordPipeLabelKind::Custom;
}

struct RecordPipeLabelNext {
    // Buckets: the drain reasons (PipeCensusDrain), then Handed.
    static constexpr std::size_t Handed = PipeCensusDrains, Buckets = PipeCensusDrains + 1;
    std::optional<double> armed;
    std::array<std::uint64_t, Buckets> counts{};
    std::array<double, Buckets> ms{};
    // A label drain waited `waitMs`; an armed one before it had nothing handed in between.
    void Arm(double waitMs) {
        Note(static_cast<std::size_t>(PipeCensusDrain::Label));
        armed = waitMs;
    }
    // The next event after the armed drain (none armed: nothing).
    void Note(std::size_t bucket) {
        if (!armed) return;
        ++counts[bucket];
        ms[bucket] += *armed;
        armed.reset();
    }
};

// APS5_LABEL_CENSUS=1 (P-LABEL; default off): who waits on queue 0's labels. The addresses queue-0
// label packets wrote (forgotten past Limit) and, per address, the queues whose WAIT_REG_MEM polled
// it (bit queue / 8). A label drain is then counted by its address's readers so far.
enum class RecordPipeLabelReader : std::uint8_t { OtherQueue, QueueZero, None, Count };
inline constexpr std::size_t RecordPipeLabelReaderCount = static_cast<std::size_t>(RecordPipeLabelReader::Count);
inline constexpr std::array<const char*, RecordPipeLabelReaderCount> RecordPipeLabelReaderNames{"awaited by another queue", "awaited by queue 0 only", "never awaited"};

struct RecordPipeLabelReaders {
    static constexpr std::size_t Limit = std::size_t{1} << 16u;
    std::unordered_map<std::uint64_t, std::uint32_t> waiters;

    void NoteLabel(std::uint64_t address) {
        if (waiters.size() >= Limit && !waiters.contains(address)) waiters.clear();
        waiters.try_emplace(address, 0u);
    }
    // Whether `address` is a queue-0 label's; the waiting queue joins its readers.
    bool NoteWait(std::uint64_t address, std::uint32_t queue) {
        const auto found = waiters.find(address);
        if (found == waiters.end()) return false;
        found->second |= 1u << ((queue / 8u) & 31u);
        return true;
    }
    RecordPipeLabelReader ReaderOf(std::uint64_t address) const {
        const auto found = waiters.find(address);
        if (found == waiters.end() || found->second == 0) return RecordPipeLabelReader::None;
        return (found->second & ~1u) != 0 ? RecordPipeLabelReader::OtherQueue : RecordPipeLabelReader::QueueZero;
    }
};

// The value a queue-0 wait on [awaited, awaited + bytes) reads in order when a label riding the ring
// wrote those bytes last: the newest unrecorded riding label (serial > consumed) that holds them all,
// with no unrecorded item's overlay range over them newer than it (a later draw, dispatch or label
// writing there; the overlay pruned to `consumed`). nullopt: the wait drains as before.
inline std::optional<std::uint64_t> RecordPipeRidingValue(std::span<const RecordPipeRidingLabel> riding, const RecordPipeOverlay& overlay, std::uint64_t consumed, std::uint64_t awaited, std::size_t bytes) {
    if (bytes == 0 || bytes > sizeof(std::uint64_t)) return std::nullopt;
    for (auto label = riding.rbegin(); label != riding.rend(); ++label) {
        if (label->serial <= consumed) return std::nullopt;
        if (awaited < label->address || awaited + bytes > label->address + label->size) continue;
        for (const auto& range : overlay.ranges) {
            if (awaited < range.end && range.begin < awaited + bytes && range.serial > label->serial) return std::nullopt;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, label->bytes.data() + (awaited - label->address), bytes);
        return value;
    }
    return std::nullopt;
}

// APS5_LABEL_CENSUS, any queue's worker after a WAIT_REG_MEM: the wait on `address` took `ms`.
bool RecordPipeLabelCensus();
void NoteRecordPipeLabelWait(std::uint64_t address, std::uint32_t queue, double ms);
// APS5_PROFILE_DRAW, the queue-0 worker: time it waited for a submission (the [pipe] labels line).
void NoteRecordPipeIdle(std::uint64_t ns);

// APS5_WAIT_PIPE_PROGRESS (s62 R-WAITS; Driver::waitMemory): the ring's serials now ({0, 0} while the
// pipe is off). A cross-queue wait times out after 1000 ms in which no packet finished or ran on any
// queue. B recording a handed draw is queue-0 work in flight, as the draw packet was before the pipe,
// but A sits in a drain outside any packet meanwhile, so a load's long recordings (pipeline builds)
// timed the compute queues out while queue 0 was still on its way to the labels they wait for.
struct RecordPipeProgress {
    std::uint64_t handed = 0, consumed = 0;
};
RecordPipeProgress RecordPipeProgressNow();
// The switch's value: on unless it is exactly "0" (default on since the s62 A/B).
constexpr bool WaitPipeProgressSwitch(const char* text) {
    return text == nullptr || !(text[0] == '0' && text[1] == '\0');
}
// Whether the pipe showed queue-0 work since a wait last saw `seenConsumed`: an item still unrecorded,
// or one recorded since.
constexpr bool RecordPipeProgressed(RecordPipeProgress now, std::uint64_t seenConsumed) {
    return !RecordPipeRing::Empty(now.handed, now.consumed) || now.consumed != seenConsumed;
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

// APS5_RECORD_PIPE_VERIFY=1 (with APS5_RECORD_PIPE=1; default off): the guest ranges the device calls
// of the last Depth recorded items noted as GPU writes (GuestMemory::recordedWrites), by serial. Before
// B records item s it checks s's captures against the items unrecorded when s's draw began, all
// recorded by then (A checks a draw it drained for the same way): an overlap is a write the overlay
// estimate missed (else the draw would have drained and run again).
struct RecordPipeWriteLog {
    struct Item {
        std::uint64_t serial = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    };
    std::array<Item, RecordPipeRing::Depth> items;

    // B, before item s's device call: the list its writes (and A's redo's, if it turns stale) join.
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& Begin(std::uint64_t serial) {
        auto& item = items[RecordPipeRing::Slot(serial)];
        item.serial = serial;
        item.ranges.clear();
        return item.ranges;
    }
    // The newest item in (after, before) whose writes overlap [begin, end), or 0 (items older than
    // the log are not known).
    std::uint64_t Writer(std::uint64_t after, std::uint64_t before, std::uint64_t begin, std::uint64_t end) const {
        for (auto serial = before; serial > after + 1;) {
            const auto& item = items[RecordPipeRing::Slot(--serial)];
            if (item.serial != serial) continue;
            for (const auto& [from, to] : item.ranges) {
                if (begin < to && from < end) return serial;
            }
        }
        return 0;
    }
};

// APS5_RECORD_PIPE_VERIFY, B after recording an item: the first write its device calls logged
// (RecordPipeWriteLog) that its own overlay estimate, the ranges its hand-over left under its serial
// joined end to end, does not cover; none when the estimate covers every write.
inline std::optional<std::pair<std::uint64_t, std::uint64_t>> RecordPipeEstimateMiss(std::span<const std::pair<std::uint64_t, std::uint64_t>> writes, std::span<const std::pair<std::uint64_t, std::uint64_t>> estimate) {
    for (const auto& write : writes) {
        auto covered = write.first;
        for (bool advanced = true; advanced && covered < write.second;) {
            advanced = false;
            for (const auto& [begin, end] : estimate) {
                if (begin > covered || covered >= end) continue;
                covered = end;
                advanced = true;
            }
        }
        if (covered < write.second) return write;
    }
    return std::nullopt;
}

}

#endif
