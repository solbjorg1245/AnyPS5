#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTCENSUS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ShaderRecompiler {
struct RecompileResult;
struct MemoryRegion;
enum class ProgramRole;
}

namespace AgcDriver::Graphics {
struct State;
struct DecodeRead;
enum class IndirectDrawPath : std::uint8_t;
}

namespace AgcDriver::DriverDetail {

struct DrawProgram;
struct StageCapture;

// The fast-path census (APS5_FAST_CENSUS=1 with APS5_PROFILE_DRAW; docs/design/draw-fastpath.md
// F0): what the old path knows about each draw packet that the fast path would decline on, how the
// state key (the draw key without the shader user words) behaves, how many walk reads fall into a
// 64 KiB block with pending GPU writes, and how long the runs of fast-eligible draws between other
// queue-0 packets are. Diagnostic only: nothing changes how a draw is drawn. The pure parts below
// are header-only for tests/Submit.cpp; the hooks are in FastCensus.cpp.

// Why the fast path would decline a draw (every reason that applies is counted; `sole` counts the
// draws only that reason keeps off the fast path).
enum class FastCensusReason : std::uint8_t { Stages, RectList, CpuIndirect, RewritesRecords, Writes, Bda, Lease, CopiedWrites, DeferredFlat, Bindless, Descriptors, PendingBlock, NotResident, ReadsTarget, MetadataPass, Debug, Count };
inline constexpr std::size_t FastCensusReasons = static_cast<std::size_t>(FastCensusReason::Count);
inline constexpr std::array<const char*, FastCensusReasons> FastCensusReasonNames{"stages", "rect list", "cpu indirect", "rewritten records", "writes", "bda", "lease", "copied writes", "deferred flat", "bindless", "descriptors", "pending block", "target not resident", "reads target", "cb metadata", "debug mode"};
inline constexpr std::uint32_t FastCensusBit(FastCensusReason reason) { return 1u << static_cast<unsigned>(reason); }

// maxPushDescriptors of desktop NVIDIA, AMD and Intel: a draw with more declines (design 2.4).
inline constexpr std::uint32_t FastCensusPushLimit = 32;

// A DrawKeyRegisters range by bank (0 context, 1 shader, 2 user-config: Graphics::RegisterBank's order).
struct FastCensusRange {
    std::uint8_t bank;
    std::uint32_t first;
    std::uint32_t count;
};

// The shader user words and merged-stage pointers F1's state mask leaves out: PS 0x00c-0x02b, the
// geometry-back pointer 0x082-0x083, VS/GS front 0x08c-0x0ab, the hull pointer 0x102-0x103, HS 0x10c-0x12b.
inline bool FastCensusUserWord(std::uint8_t bank, std::uint32_t offset) {
    if (bank != 1) return false;
    return (offset >= 0x00c && offset < 0x02c) || (offset >= 0x082 && offset < 0x084) || (offset >= 0x08c && offset < 0x0ac) || (offset >= 0x102 && offset < 0x104) || (offset >= 0x10c && offset < 0x12c);
}

// The color and depth target base words (the churn probe's "CB_COLOR bases" variant made exact):
// DB_HTILE_DATA_BASE, DB_Z/STENCIL_READ/WRITE_BASE and their _HI words, each CB_COLOR slot's BASE,
// CMASK, FMASK and DCC_BASE (words 0, 7, 9 and 13 of its 15), and the slots' *_BASE_EXT words.
inline bool FastCensusTargetBase(std::uint8_t bank, std::uint32_t offset) {
    if (bank != 0) return false;
    if (offset == 0x005 || (offset >= 0x012 && offset <= 0x015) || (offset >= 0x01a && offset <= 0x01e) || (offset >= 0x390 && offset < 0x3b0)) return true;
    if (offset < 0x318 || offset >= 0x318 + 8 * 15) return false;
    const auto word = (offset - 0x318) % 15;
    return word == 0 || word == 7 || word == 9 || word == 13;
}

inline std::uint64_t FastCensusMix(std::uint64_t hash, std::uint64_t word) {
    hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
    return hash ^ (hash >> 29u);
}

// The keys of one draw over the present registers of `ranges`: every word (the draw key's
// registers, without the program registry), the state key (no user words), and the state key
// without the target bases (F4's target-set key would carry them).
struct FastCensusKeys {
    std::uint64_t full = 0;
    std::uint64_t state = 0;
    std::uint64_t stateNoTargets = 0;
};
inline FastCensusKeys ComputeFastCensusKeys(const QueueState& queue, std::span<const FastCensusRange> ranges) {
    constexpr std::uint64_t seed = 0xcbf29ce484222325ull;
    FastCensusKeys keys{seed, seed, seed};
    for (const auto& range : ranges) {
        const auto& bank = range.bank == 0 ? queue.context : range.bank == 1 ? queue.shader : queue.userConfig;
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) {
            if (!bank.contains(offset)) continue;
            const auto word = (static_cast<std::uint64_t>(range.bank) << 56u) | (static_cast<std::uint64_t>(offset) << 32u) | bank.at(offset);
            keys.full = FastCensusMix(keys.full, word);
            if (FastCensusUserWord(range.bank, offset)) continue;
            keys.state = FastCensusMix(keys.state, word);
            if (!FastCensusTargetBase(range.bank, offset)) keys.stateNoTargets = FastCensusMix(keys.stateNoTargets, word);
        }
    }
    return keys;
}

// What ends a run of fast-eligible draws on queue 0 (F6's hand-over points): a dispatch, a wait
// (WAIT_REG_MEM, a rendering wait), a label or other memory store (RELEASE_MEM, WRITE_DATA, a
// packet that wrote on the GPU), a memory copy (DMA_DATA, COPY_DATA, DUMP_CONST_RAM), a flip, a draw
// the fast path declines, the end of the submission, or any other packet. Register and state
// packets (SET_*_REG, CLEAR_STATE, the index and instance state, WRITE_CONST_RAM, markers, context
// push/pop, the cache events the port ignores) do not.
enum class FastCensusBreak : std::uint8_t { Dispatch, Wait, Label, Dma, Flip, DeclinedDraw, SubmissionEnd, Other, Count };
inline constexpr std::size_t FastCensusBreaks = static_cast<std::size_t>(FastCensusBreak::Count);
inline constexpr std::array<const char*, FastCensusBreaks> FastCensusBreakNames{"dispatch", "wait", "label", "dma", "flip", "declined draw", "submission end", "other"};

// A non-draw packet's effect on a run; nullopt for a state packet.
inline std::optional<FastCensusBreak> ClassifyFastCensusPacket(std::uint32_t header, bool flip, bool renderingWait, bool wroteOnGpu) {
    if (flip) return FastCensusBreak::Flip;
    if (renderingWait) return FastCensusBreak::Wait;
    const auto opcode = (header >> 8u) & 0xffu;
    switch (opcode) {
    case 0x15: case 0x16: return FastCensusBreak::Dispatch;
    case 0x3c: case 0x93: return FastCensusBreak::Wait;
    case 0x37: case 0x49: return FastCensusBreak::Label;
    case 0x40: case 0x50: case 0x83: return FastCensusBreak::Dma;
    case 0x10:
        if ((header & 0xfcu) == 0) break;
        switch ((header >> 2u) & 0x3fu) {
        case 0x06: return FastCensusBreak::Wait;
        case 0x17: return FastCensusBreak::Flip;
        case 0x15: case 0x18: return FastCensusBreak::Label;
        case 0x19: return FastCensusBreak::Dma;
        case 0x00: case 0x05: case 0x09: case 0x0b: case 0x0c: case 0x1a: break;
        default: return FastCensusBreak::Other;
        }
        break;
    case 0x11: case 0x12: case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f: case 0x42: case 0x46: case 0x58:
    case 0x63: case 0x64: case 0x68: case 0x69: case 0x76: case 0x79: case 0x7a: case 0x81: case 0x9f: break;
    default: return FastCensusBreak::Other;
    }
    if (wroteOnGpu) return FastCensusBreak::Label;
    return std::nullopt;
}

// Runs of fast-eligible draws by length (1, 2-3, 4-7, ... 64-127, 128 and more) and what ended them.
struct FastCensusRuns {
    static constexpr std::size_t Buckets = 8;
    static constexpr std::array<const char*, Buckets> BucketNames{"1", "2-3", "4-7", "8-15", "16-31", "32-63", "64-127", "128+"};
    static constexpr std::size_t Bucket(std::uint64_t length) {
        std::size_t bucket = 0;
        while (bucket + 1 < Buckets && length >= (std::uint64_t{2} << bucket)) ++bucket;
        return bucket;
    }
    std::array<std::uint64_t, Buckets> runs{};
    std::array<std::uint64_t, FastCensusBreaks> breaks{};
    // Eligible draws in runs, and those in runs of 16 or more.
    std::uint64_t packets = 0;
    std::uint64_t longPackets = 0;

    void Close(std::uint64_t length, FastCensusBreak why) {
        ++breaks[static_cast<std::size_t>(why)];
        if (length == 0) return;
        ++runs[Bucket(length)];
        packets += length;
        if (length >= 16) longPackets += length;
    }
    std::uint64_t Runs() const {
        std::uint64_t total = 0;
        for (const auto count : runs) total += count;
        return total;
    }
    void Reset() { *this = {}; }
};

// Draws by decline reason: `any` per reason, `sole` where it is the only one.
struct FastCensusTally {
    std::uint64_t draws = 0;
    std::uint64_t eligible = 0;
    std::array<std::uint64_t, FastCensusReasons> any{};
    std::array<std::uint64_t, FastCensusReasons> sole{};

    void Note(std::uint32_t reasons) {
        ++draws;
        if (reasons == 0) {
            ++eligible;
            return;
        }
        const bool single = std::popcount(reasons) == 1;
        for (std::size_t reason = 0; reason < FastCensusReasons; ++reason) {
            if ((reasons & (1u << reason)) == 0) continue;
            ++any[reason];
            if (single) ++sole[reason];
        }
    }
    void Reset() { *this = {}; }
};

// Descriptors per draw (all stages, one push set in F3a): <=8, <=16, <=24, <=32, <=48, <=64, more.
inline constexpr std::array<const char*, 7> FastCensusDescriptorBucketNames{"<=8", "<=16", "<=24", "<=32", "<=48", "<=64", ">64"};
inline std::size_t FastCensusDescriptorBucket(std::uint32_t count) {
    if (count <= 32) return count == 0 ? 0 : (count - 1) / 8;
    return count <= 48 ? 4 : count <= 64 ? 5 : 6;
}

// Hooks (FastCensus.cpp). Active: APS5_FAST_CENSUS and APS5_PROFILE_DRAW both set.
bool FastCensusActive();
// Driver::draw, once its stages' results are known (a nested or retried draw of the packet overwrites).
void NoteFastCensusDraw(const QueueState& queue, const Graphics::State& graphics, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<DrawProgram>& programs, const std::vector<const ShaderRecompiler::RecompileResult*>& results, const std::vector<bool>& recompiled, const std::vector<StageCapture>& captures, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool indirect, const std::optional<Graphics::IndirectDrawPath>& cpuIndirect, bool debug);
// The precheck's CB metadata pass (drawn without the decode).
void NoteFastCensusMetadataPass();
// A draw packet that was not drawn (rejected or thrown after Driver::draw's census point): dropped.
void NoteFastCensusDropped();
// After every packet of a submission (the draw packet's census is committed here), and at its end.
void NoteFastCensusPacket(std::uint32_t queue, std::uint32_t header, bool drawPacket, bool flip, bool renderingWait, bool wroteOnGpu);
void NoteFastCensusSubmissionEnd(std::uint32_t queue);

// The recording-pipe census (docs/design/parallel-recording.md stage 0; APS5_PROFILE_DRAW, off with
// APS5_NO_PIPE_CENSUS=1 or APS5_FAST_DRAW): the runs of queue-0 packets a recording thread could take
// over under two policies (0: direct and GPU-indirect draws; 1: also recorded direct dispatches), the
// drains that end them (the queue thread waits for the recording thread), and the captures that read
// a range the open run writes (an upper bound: the whole run counts as unrecorded).
enum class PipeCensusDrain : std::uint8_t { LabelsPending, CpuIndirect, Dispatch, IndirectDispatch, Wait, Label, Dma, Flip, Stale, Overlap, SubmissionEnd, Other, Count };
inline constexpr std::size_t PipeCensusDrains = static_cast<std::size_t>(PipeCensusDrain::Count);
inline constexpr std::array<const char*, PipeCensusDrains> PipeCensusDrainNames{"labels pending", "cpu indirect", "dispatch", "indirect dispatch", "wait", "label", "dma", "flip", "stale", "overlap", "submission end", "other"};
// How a draw packet took the GPU lock (None: it recorded nothing; Stale: a SnapshotStale retry; Other:
// the CB metadata pass, a locked prepare) and what a DISPATCH_DIRECT did (Copy: the HLE; Other: fast).
enum class PipeCensusLock : std::uint8_t { None, Direct, CpuIndirect, Stale, Other };
enum class PipeCensusDispatch : std::uint8_t { None, Recorded, Copy, Other };
enum class DispatchOutcome : std::size_t;

// A queue-0 packet as the hooks saw it; labelsPending: deferred labels record inside its lock.
struct PipeCensusPacket {
    std::uint32_t header = 0;
    bool drawPacket = false, flip = false, renderingWait = false, wroteOnGpu = false;
    PipeCensusLock lock = PipeCensusLock::None;
    PipeCensusDispatch dispatch = PipeCensusDispatch::None;
    bool labelsPending = false;
};

// Handed over (eligible), a drain, or neither (state packets, packets that recorded nothing).
struct PipeCensusVerdict {
    bool eligible = false;
    std::optional<PipeCensusDrain> drain;
};

inline PipeCensusVerdict ClassifyPipeCensus(const PipeCensusPacket& packet, bool dispatches) {
    const auto drain = [](PipeCensusDrain why) { return PipeCensusVerdict{false, why}; };
    const auto handed = packet.labelsPending ? drain(PipeCensusDrain::LabelsPending) : PipeCensusVerdict{true, std::nullopt};
    const auto opcode = (packet.header >> 8u) & 0xffu;
    if (packet.drawPacket && packet.lock == PipeCensusLock::None) return {};
    if (packet.drawPacket && packet.lock == PipeCensusLock::Direct) return handed;
    if (packet.drawPacket) return drain(packet.lock == PipeCensusLock::CpuIndirect ? PipeCensusDrain::CpuIndirect : packet.lock == PipeCensusLock::Stale ? PipeCensusDrain::Stale : PipeCensusDrain::Other);
    if (opcode == 0x15 && packet.dispatch == PipeCensusDispatch::None) return {};
    if (opcode == 0x15 && packet.dispatch == PipeCensusDispatch::Recorded) return dispatches ? handed : drain(PipeCensusDrain::Dispatch);
    if (opcode == 0x15) return drain(packet.dispatch == PipeCensusDispatch::Copy ? PipeCensusDrain::Dma : PipeCensusDrain::Other);
    if (opcode == 0x16) return drain(PipeCensusDrain::IndirectDispatch);
    // The rest as they end fast-census runs (FastCensusBreak order; a declined draw never comes here).
    constexpr std::array<PipeCensusDrain, FastCensusBreaks> drains{PipeCensusDrain::Dispatch, PipeCensusDrain::Wait, PipeCensusDrain::Label, PipeCensusDrain::Dma, PipeCensusDrain::Flip, PipeCensusDrain::Other, PipeCensusDrain::SubmissionEnd, PipeCensusDrain::Other};
    const auto why = ClassifyFastCensusPacket(packet.header, packet.flip, packet.renderingWait, packet.wroteOnGpu);
    return why ? drain(drains[static_cast<std::size_t>(*why)]) : PipeCensusVerdict{};
}

// One policy's runs by length (FastCensusRuns' buckets) and the drains that ended them; a drain with
// no open run costs nothing and is not counted. Apply is true when the run drained: an eligible packet
// whose capture read the open run's writes drains it (Overlap) and starts the next run.
struct PipeCensusRuns {
    std::uint64_t length = 0, count = 0, packets = 0;
    std::array<std::uint64_t, FastCensusRuns::Buckets> runs{};
    std::array<std::uint64_t, PipeCensusDrains> drains{};

    void Close(PipeCensusDrain why) {
        if (length == 0) return;
        ++drains[static_cast<std::size_t>(why)];
        ++runs[FastCensusRuns::Bucket(length)];
        ++count;
        packets += length;
        length = 0;
    }
    bool Apply(const PipeCensusVerdict& verdict, bool overlaps) {
        if (verdict.drain) Close(*verdict.drain);
        const bool drained = verdict.eligible && overlaps && length != 0;
        if (drained) Close(PipeCensusDrain::Overlap);
        if (verdict.eligible) ++length;
        return drained || verdict.drain.has_value();
    }
};

// The ranges the open run writes, by kind (shader-written buffers; color and depth targets). A range
// inside a known one of the same kind is not added again: the known one keeps the newer serial (the
// recording pipe's item, RecordPipe.hpp; the census leaves it 0), and Prune drops recorded ones.
struct PipeCensusOverlay {
    static constexpr std::uint8_t Write = 1, Target = 2;
    struct Range {
        std::uint64_t begin, end;
        std::uint8_t kind;
        std::uint64_t serial = 0;
    };
    std::vector<Range> ranges;

    void Add(const Range& range) {
        for (auto& known : ranges) {
            if (known.begin <= range.begin && range.end <= known.end && (known.kind & range.kind) == range.kind) {
                known.serial = std::max(known.serial, range.serial);
                return;
            }
        }
        ranges.push_back(range);
    }
    // Drops the ranges of the items up to `consumed`.
    void Prune(std::uint64_t consumed) {
        std::erase_if(ranges, [consumed](const Range& range) { return range.serial <= consumed; });
    }
    // The kinds of the known ranges [begin, end) overlaps; 0 for none.
    std::uint8_t Overlaps(std::uint64_t begin, std::uint64_t end) const {
        std::uint8_t kinds = 0;
        for (const auto& known : ranges) {
            if (begin < known.end && known.begin < end) kinds = static_cast<std::uint8_t>(kinds | known.kind);
        }
        return kinds;
    }
};

// Hooks (FastCensus.cpp), queue 0 only: the callers check, so one thread owns the census.
bool PipeCensusActive();
// A draw's or dispatch's captured regions against the open runs' writes.
void NotePipeCensusCapture(const std::vector<ShaderRecompiler::MemoryRegion>& regions);
// The packet takes the GPU lock: how, and whether deferred labels record inside it. The writes noted
// after it are this attempt's (targets: the color ones, depth and stencil sized from the extent).
void NotePipeCensusLock(PipeCensusLock kind);
void NotePipeCensusWrite(std::uint64_t begin, std::uint64_t end, bool target);
void NotePipeCensusTargets(const Graphics::State& graphics);
// After every queue-0 packet, and at each queue-0 submission end (the 10 s report).
void NotePipeCensusPacket(std::uint32_t header, bool drawPacket, bool flip, bool renderingWait, bool wroteOnGpu, DispatchOutcome dispatch);
void NotePipeCensusSubmissionEnd(bool workQueued);

}

#endif
