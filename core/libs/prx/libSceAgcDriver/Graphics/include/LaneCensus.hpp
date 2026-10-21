#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANECENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANECENSUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace AgcDriver::Graphics {

// The lane census (APS5_LANE_CENSUS=1, read once; default off; docs/design/fewer-submits.md S1):
// what a second VkQueue would have to honour, counted on today's one queue. Lane C is a GPU-lock
// thread tagged 0x20-0x57 (the compute queue workers), lane G the rest (queue 0, the recording
// pipe's thread B, untagged threads). Recorder.cpp, WaitMemory.cpp and GuestBufferMemory.cpp feed
// it (each hook one branch on the switch) and print its two lines after "[recorder] submits" every
// 10 s under APS5_PROFILE_DRAW. It never changes what is recorded or when anything submits.
inline bool LaneCensusEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_LANE_CENSUS");
        const bool on = value != nullptr && std::strcmp(value, "1") == 0;
        if (on) std::fprintf(stderr, "[recorder] lane census: submit causes and lane switches, label edges and hazards per 10 s (APS5_LANE_CENSUS=1)\n");
        return on;
    }();
    return enabled;
}
// Read at static initialization (the launcher sets the game's environment before it starts), so
// every hook is one load and a predicted-false branch.
inline const bool LaneCensusOn = LaneCensusEnabled();

enum class Lane : std::uint8_t { G = 0, C = 1 };

constexpr Lane LaneOf(std::uint32_t tag) {
    return tag >= 0x20 && tag <= 0x57 ? Lane::C : Lane::G;
}

// Why a submit happened, by the call that reached Recorder::Submit (a SubmitCauseScope around it).
enum class SubmitCause : std::uint8_t {
    Other, Deadline, SizeCap, NextPacket, LabelPacket, Suspend, Flip, SubmissionEnd, DeferredEnd, Eop, Drain,
    WaitEntry, WaitPoll, WaitPollDue, PipeDeadline, Sync, SyncThrough, Land, KeptBound, Lease, CommandBatch, Present, Presync, LaneSwitch,
};
inline constexpr std::size_t SubmitCauses = static_cast<std::size_t>(SubmitCause::LaneSwitch) + 1;
inline constexpr std::array<const char*, SubmitCauses> SubmitCauseNames{
    "other", "deadline", "size cap", "next packet", "label packet", "suspend", "flip", "submission end", "deferred end", "eop", "drain",
    "wait entry", "wait poll", "wait poll due", "pipe deadline", "sync", "sync through", "land", "kept bound", "lease", "command batch", "present", "presync", "lane switch",
};

inline SubmitCause& CurrentSubmitCause() {
    static thread_local SubmitCause cause = SubmitCause::Other;
    return cause;
}

// Names the submits inside it on this thread. The outermost scope wins, so Sync's own scope does
// not rename a SyncThrough or a caller's submit; with the census off it does nothing.
class SubmitCauseScope {
public:
    explicit SubmitCauseScope(SubmitCause cause) {
        if (!LaneCensusOn) return;
        auto& current = CurrentSubmitCause();
        if (current != SubmitCause::Other) return;
        current = cause;
        owner = true;
    }
    ~SubmitCauseScope() {
        if (owner) CurrentSubmitCause() = SubmitCause::Other;
    }
    SubmitCauseScope(const SubmitCauseScope&) = delete;
    SubmitCauseScope& operator=(const SubmitCauseScope&) = delete;

private:
    bool owner = false;
};

// The counts (GPU-free: the hooks pass lanes, serials and ranges; it locks itself, as label hits
// arrive without the GPU mutex). Items are the recorded draws and dispatches (CountRecordedWork):
// a lane switch is an item of the other lane than the one before, a run the items between two.
// Batch serials: the open batch's is Submissions() + 1, a batch is retired once finished.
class LaneCensus {
public:
    enum class LabelState : std::uint8_t { Open, InFlight, Late };
    enum class Hazard : std::uint8_t { Raw, Waw, War };
    // Hazards are tracked per 64 KiB block (an approximation: two ranges of one block overlap), a
    // range over more blocks marks its first MaxBlocks; at most one of each kind per access.
    static constexpr unsigned BlockShift = 16;
    static constexpr std::uint64_t MaxBlocks = 1024;
    static constexpr std::size_t MaxShadows = 4096;

    struct Counts {
        std::array<std::uint64_t, SubmitCauses> causes{};
        std::array<std::uint64_t, 2> submits{};
        // Submits that ended a render pass, recorded the host-read barrier, flushed deferred copies.
        std::uint64_t passEnded = 0, hostRead = 0, copiesFlushed = 0;
        // Per lane: items, runs that ended in the window, the longest of them; lane switches.
        std::array<std::uint64_t, 2> items{}, runs{}, longest{};
        std::uint64_t switches = 0;
        // Commands() by a lane after the other lane's item: the open pass it ended, the queued copies it flushed.
        std::array<std::uint64_t, 2> crossPasses{}, crossCopies{};
        // Label hits of the waiter lane on the other lane's label, by the entry's state.
        std::array<std::array<std::uint64_t, 3>, 2> crossHits{};
        // Accesses of a lane over the other lane's unretired access that no label edge covers.
        std::array<std::array<std::uint64_t, 3>, 2> hazards{};
        std::uint64_t shadowTakes = 0, crossShadowTakes = 0;
        // Slack: lane-C runs whose G items were followed by a G edge on C (and those items), runs
        // the next C run followed with no such edge (and their G items).
        std::uint64_t slackEdges = 0, slackItems = 0, slackUnmet = 0, slackUnmetItems = 0;
    };

    void Submit(SubmitCause cause, Lane lane, bool passEnded, bool hostRead, bool copiesFlushed);
    void Item(Lane lane);
    void Commands(Lane lane, bool passOpen, bool copiesQueued);
    void LabelHit(Lane waiter, Lane producer, LabelState state, std::uint64_t producerSerial);
    void Access(Lane lane, std::uint64_t serial, std::uint64_t begin, std::uint64_t end, bool write);
    void Accesses(Lane lane, std::uint64_t serial, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, bool write);
    void Retired(std::uint64_t serial);
    void ShadowRegistered(Lane lane, std::uint64_t begin, std::uint64_t end);
    void ShadowTaken(Lane lane, std::uint64_t begin, std::uint64_t end);
    // The window's counts, zeroed for the next one (run, edge and block state carries over);
    // blocks no unretired access marks are dropped.
    Counts Take();
    std::size_t Blocks();

    static std::string CausesLine(const Counts& counts);
    static std::string LanesLine(const Counts& counts);
    static LaneCensus& Shared();

private:
    struct Block {
        std::array<std::uint64_t, 2> write{}, read{};
    };
    void access(Lane lane, std::uint64_t serial, std::uint64_t begin, std::uint64_t end, bool write);

    std::mutex mutex;
    Counts counts;
    bool started = false;
    Lane current = Lane::G;
    std::uint64_t run = 0;
    // The current C run (or the last one, while G records) already met a G edge; a G slack window is open.
    bool cRunEdged = false, slackOpen = false;
    std::uint64_t slackCount = 0;
    // edge[waiter][producer]: the newest producer batch serial the waiter lane took a label from.
    std::array<std::array<std::uint64_t, 2>, 2> edge{};
    std::uint64_t retired = 0;
    std::unordered_map<std::uint64_t, Block> blocks;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Lane> shadows;
};

}

#endif
