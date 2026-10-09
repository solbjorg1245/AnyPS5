#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WRITEEVIDENCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WRITEEVIDENCE_HPP

#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace AgcDriver::DriverDetail {

inline constexpr std::size_t WrittenBufferRing = 4096;

inline constexpr std::size_t DwordEvidenceEntries = 65536;

struct WrittenBuffer {
    std::uint64_t program;
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t serial;
    std::uint32_t queue;
    bool atomic;
    std::shared_ptr<const std::vector<std::byte>> value;
    std::uint64_t generation = 0;
};

struct DwordEvidence {
    std::uint32_t streak = 0;
    std::uint32_t changed = 0;

    std::uint64_t program = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    // The word's value at the last observation (ShaderMemory::PendingWrite::RawExpected: a raw
    // read that finds another value re-reads through the hook and ends the streak). After the
    // writer fields: observeDword resets an entry with DwordEvidence{0, 0, program, begin, end}.
    std::uint32_t lastValue = 0;
    bool valueKnown = false;
};

class SampledReadScope {
public:
    explicit SampledReadScope(std::atomic<std::uint64_t>& reads);
    ~SampledReadScope();
    SampledReadScope(const SampledReadScope&) = delete;
    SampledReadScope& operator=(const SampledReadScope&) = delete;

private:
    bool previous;
};

struct ValidateCounters {
    std::uint64_t pending = 0, unsyncedMisses = 0, skipped = 0, syncedNoWriter = 0, syncedForeign = 0, syncedLargeRange = 0, syncedLabel = 0, syncedEvidence = 0, syncedWriterChanged = 0, syncedSample = 0, syncedImage = 0, syncedShadow = 0, syncedOff = 0, verified = 0, mismatches = 0, verifiedMissesHit = 0, knownValue = 0;
    double syncedWaitMs = 0, verifiedWaitMs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

struct PendingView {
    std::shared_ptr<const Graphics::Recorder::WriteRanges> snapshot;
    std::uint64_t generation = 0;
    bool loaded = false;

    void Load();
    bool Overlaps(std::uint64_t address, std::size_t bytes) const;
};

// Entries pushed into the driver's written-buffer ring so far (Driver::writtenBuffers), counted
// under its mutex: the ring holds the last ring.size() of them, the newest at the back. Read
// lock-free by the fast reader, whose known words hold only while no writer was noted since they
// were classified (FastRead.cpp). Bumped by the driver alone (a test may bump it to stand for a
// new writer).
std::atomic<std::uint64_t>& WrittenBufferPushes();

// Driver::newestWriterLocked's answer remembered for one thread (the fast reader's, through
// Driver::fastPendingWord): the newest ring entry overlapping a word, and whether no newer entry
// overlaps any of that entry's own range, in which case the same entry answers every range inside
// it until a newer overlapping entry is pushed or the entry leaves the ring. Lookup checks only
// the entries pushed since the memo was last checked (a full scan from the newest entry
// otherwise), so a run of reads of one copy's destination costs one scan, not one per word.
struct NewestWriterMemo {
    WrittenBuffer writer{};
    // The push count at which `writer` entered the ring (WrittenBufferPushes after its push) and
    // the one the memo was last checked at; `wholeRange`: no newer entry overlapped the writer's
    // range at `checked`.
    std::uint64_t pushed = 0;
    std::uint64_t checked = 0;
    bool wholeRange = false;
    // newestWriterLocked over the ring after `pushes` pushes (the caller holds the ring's mutex):
    // exactly its answer, since a memo hit is a range inside a writer no newer entry overlaps.
    std::optional<WrittenBuffer> Lookup(const std::deque<WrittenBuffer>& ring, std::uint64_t pushes, std::uint64_t begin, std::uint64_t end);
    // Whether the memo still answers for the writer's whole range after `pushes` pushes: the
    // writer is still in the ring and no entry pushed since `checked` overlaps its range.
    bool Holds(const std::deque<WrittenBuffer>& ring, std::uint64_t pushes);
};

// A few memos (a walk reads several copies' destinations): the one whose writer holds over a
// range containing the query answers, otherwise the next slot scans. At most one can hold for a
// range (of two writers containing it the newer overlaps the older, which then fails Holds).
struct NewestWriterMemos {
    std::array<NewestWriterMemo, 4> entries{};
    std::size_t next = 0;
    // The memo that gave the last answer, null when no writer overlapped the range.
    const NewestWriterMemo* last = nullptr;
    std::optional<WrittenBuffer> Lookup(const std::deque<WrittenBuffer>& ring, std::uint64_t pushes, std::uint64_t begin, std::uint64_t end);
};

// The old capture's answer for one word a pending GPU write overlaps (Driver::fastPendingWord,
// the fast reader's query: the ShaderMemory::PendingWrite queryPendingWrite gives for those 4
// bytes) and the value it serves with: the known word for KnownValue, the value the write evidence
// saw last for RawExpected. A KnownValue answer from a writer whose known bytes cover a range no
// newer writer and no label overlaps also names that range and its bytes: every word inside it
// gets the same answer while the pending snapshot (its publish generation), the writer ring (its
// push count, `writers`) and the recorder's write notes (Recorder::WriteGeneration, `writes`,
// loaded before the classification) are unchanged. The write notes cover a label or store noted
// inside a range the snapshot already holds, which publishes nothing (Recorder::noteWrite).
struct FastPendingAnswer {
    ShaderMemory::PendingWrite policy = ShaderMemory::PendingWrite::Sync;
    std::uint32_t word = 0;
    std::uint64_t rangeBegin = 0;
    std::uint64_t rangeEnd = 0;
    std::shared_ptr<const std::vector<std::byte>> rangeBytes;
    std::uint64_t writers = 0;
    std::uint64_t writes = 0;
};
using FastPendingQuery = void (*)(std::uint64_t address, const PendingView& view, FastPendingAnswer& answer);

}

#endif
