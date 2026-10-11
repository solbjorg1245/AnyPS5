#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CRASHMEMORY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CRASHMEMORY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// What a FATAL report says about guest memory that reads back as a fill (CrashReport.cpp): the run of
// one value around a register (APS5_CRASH_RUN_EXTENT: zeros, all ones, a pixel-like pattern) and the
// recent page replacements of the arena (APS5_MAP_RING, recorded by WindowsMappings). A page reset or
// remap zeroes whole 16 KiB pages, a freed and filled block an object-sized run, a GPU store an
// image's extent.
namespace CrashMemory {

enum class MapKind : std::uint32_t {
    Commit = 1, // private memory over a placeholder (fresh zero pages)
    Reset = 2,  // a release back to a placeholder (munmap, heap cache eviction, fixed remap)
    Map = 3,    // a shared section view (direct memory) over the range
};

inline const char* MapKindName(std::uint32_t kind) {
    switch (static_cast<MapKind>(kind)) {
    case MapKind::Commit: return "commit";
    case MapKind::Reset: return "reset";
    case MapKind::Map: return "map";
    }
    return "?";
}

constexpr std::size_t MapCallers = 4;

struct MapEntry {
    std::uint64_t index = 0; // the record's number since the start (0 for the first)
    std::uint64_t begin = 0;
    std::uint64_t bytes = 0;
    std::int64_t time = 0; // steady clock, nanoseconds
    std::uint32_t thread = 0;
    std::uint32_t kind = 0;
    std::uint64_t callers[MapCallers] = {};
};

// Whether the entry's range lies within `window` bytes of `address`.
inline bool MapEntryNear(const MapEntry& entry, std::uint64_t address, std::uint64_t window) {
    const auto low = entry.begin > window ? entry.begin - window : 0;
    const auto end = entry.begin + entry.bytes;
    const auto high = end > ~std::uint64_t{0} - window ? ~std::uint64_t{0} : end + window;
    return address >= low && address < high;
}

// A lock-free ring of the last `Capacity` records: a writer takes a slot with one fetch_add and
// publishes it through the slot's sequence (a seqlock), so the crash report reads it on the faulting
// thread without a lock and skips a slot a writer is still filling.
template <std::size_t Capacity>
class MapRing {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0, "the ring's capacity is a power of two");

public:
    constexpr MapRing() = default;

    void Record(MapKind kind, std::uint64_t begin, std::uint64_t bytes, std::int64_t time, std::uint32_t thread, const std::uint64_t* callers, std::size_t callerCount) {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        auto& slot = slots[index & (Capacity - 1)];
        slot.sequence.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.begin.store(begin, std::memory_order_relaxed);
        slot.bytes.store(bytes, std::memory_order_relaxed);
        slot.time.store(time, std::memory_order_relaxed);
        slot.what.store((static_cast<std::uint64_t>(thread) << 32u) | static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
        for (std::size_t caller = 0; caller < MapCallers; ++caller) slot.callers[caller].store(caller < callerCount ? callers[caller] : 0, std::memory_order_relaxed);
        slot.sequence.store(index + 1, std::memory_order_release);
    }

    // The records taken so far (the ring keeps the last Capacity of them).
    std::uint64_t Recorded() const {
        return next.load(std::memory_order_acquire);
    }

    // Record `index` while the ring still holds it; false once overwritten or while being written.
    bool Read(std::uint64_t index, MapEntry* entry) const {
        const auto& slot = slots[index & (Capacity - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != index + 1) return false;
        entry->index = index;
        entry->begin = slot.begin.load(std::memory_order_relaxed);
        entry->bytes = slot.bytes.load(std::memory_order_relaxed);
        entry->time = slot.time.load(std::memory_order_relaxed);
        const auto what = slot.what.load(std::memory_order_relaxed);
        entry->thread = static_cast<std::uint32_t>(what >> 32u);
        entry->kind = static_cast<std::uint32_t>(what);
        for (std::size_t caller = 0; caller < MapCallers; ++caller) entry->callers[caller] = slot.callers[caller].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot.sequence.load(std::memory_order_relaxed) == index + 1;
    }

    static constexpr std::size_t Size() {
        return Capacity;
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> begin{0};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::int64_t> time{0};
        std::atomic<std::uint64_t> what{0}; // thread << 32 | kind
        std::atomic<std::uint64_t> callers[MapCallers];
    };

    std::atomic<std::uint64_t> next{0};
    Slot slots[Capacity];
};

// The arena's ring: WindowsMappings records into it and the crash report reads it, both in libc.
// Constant initialised (no guard, no heap), 72 KiB.
using ProcessMapRing = MapRing<1024>;
inline ProcessMapRing& MapHistory() {
    static ProcessMapRing ring;
    return ring;
}

// How the words of a run match the word at its address: the same 8-byte value (zeros, all ones, a
// repeated fill), or every byte within NearSpread of that word's first byte (a low-entropy, pixel-like
// fill such as 2121212122222121, t582).
enum class RunClass : std::uint32_t {
    Same = 0,
    Near = 1,
};

constexpr unsigned NearSpread = 8;

inline bool NearByte(std::uint8_t byte, std::uint8_t reference) {
    return static_cast<unsigned>(byte > reference ? byte - reference : reference - byte) <= NearSpread;
}

// Whether every byte of `word` lies within NearSpread of `reference`.
inline bool NearWord(std::uint64_t word, std::uint8_t reference) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        if (!NearByte(static_cast<std::uint8_t>(word >> (8 * byte)), reference)) return false;
    }
    return true;
}

// Why a run stopped on one side.
enum class RunEdge : std::uint32_t {
    Differs = 0,    // a byte that does not match (the run's beforeWord / afterWord holds its 8-byte word)
    Cap = 1,        // the scan's limit
    Unreadable = 2, // memory the reader could not read
};

inline const char* RunEdgeName(RunEdge edge) {
    switch (edge) {
    case RunEdge::Differs: return "differs";
    case RunEdge::Cap: return "cap";
    case RunEdge::Unreadable: return "unreadable";
    }
    return "?";
}

struct ValueRun {
    bool readable = false;   // the 8-byte word at the address could be read
    bool matches = false;    // and belongs to the class (always for Same; else the rest is unset)
    std::uint64_t value = 0; // that word
    std::uint64_t begin = 0; // [begin, end): the matching bytes around the address
    std::uint64_t end = 0;
    RunEdge beforeEdge = RunEdge::Cap;
    RunEdge afterEdge = RunEdge::Cap;
    std::uint64_t beforeWord = 0;
    std::uint64_t afterWord = 0;
};

// The run of matching bytes holding the 8-byte word at `address` (rounded down to 8), scanned up to
// `cap` bytes each way in 8-byte words; `read(address, &word)` reads one aligned word, or returns
// false where memory cannot be read. A Same run's edges are byte exact against the value's byte at
// the same offset (the fill repeats every 8 bytes), a Near run's against the first byte.
template <class Read>
ValueRun MeasureRun(std::uint64_t address, std::uint64_t cap, RunClass runClass, Read&& read) {
    ValueRun run;
    const auto start = address & ~std::uint64_t{7};
    if (!read(start, &run.value)) return run;
    run.readable = true;
    const auto reference = static_cast<std::uint8_t>(run.value);
    const auto byteMatches = [&](std::uint64_t word, unsigned byte) {
        const auto value = static_cast<std::uint8_t>(word >> (8 * byte));
        return runClass == RunClass::Same ? value == static_cast<std::uint8_t>(run.value >> (8 * byte)) : NearByte(value, reference);
    };
    const auto wordMatches = [&](std::uint64_t word) {
        return runClass == RunClass::Same ? word == run.value : NearWord(word, reference);
    };
    if (!wordMatches(run.value)) return run;
    run.matches = true;
    run.begin = start;
    run.end = start + 8;
    std::uint64_t word = 0;
    for (;;) {
        if (run.end - start >= cap) break;
        if (!read(run.end, &word)) {
            run.afterEdge = RunEdge::Unreadable;
            break;
        }
        if (!wordMatches(word)) {
            // Little endian: the low bytes of the word follow the run.
            run.afterEdge = RunEdge::Differs;
            run.afterWord = word;
            unsigned byte = 0;
            while (byteMatches(word, byte)) ++byte;
            run.end += byte;
            break;
        }
        run.end += 8;
    }
    for (;;) {
        if (start - run.begin >= cap || run.begin < 8) break;
        if (!read(run.begin - 8, &word)) {
            run.beforeEdge = RunEdge::Unreadable;
            break;
        }
        if (!wordMatches(word)) {
            // The high bytes of the word precede the run.
            run.beforeEdge = RunEdge::Differs;
            run.beforeWord = word;
            unsigned byte = 0;
            while (byteMatches(word, 7 - byte)) ++byte;
            run.begin -= byte;
            break;
        }
        run.begin -= 8;
    }
    return run;
}

}

#endif
