#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CRASHMEMORY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CRASHMEMORY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// What a FATAL report says about guest memory that reads back zeros (CrashReport.cpp): the run of
// zeros around a register (APS5_CRASH_ZERO_EXTENT) and the recent page replacements of the arena
// (APS5_MAP_RING, recorded by WindowsMappings). A page reset or remap zeroes whole 16 KiB pages, a
// freed and zero-filled block an object-sized run, a GPU store an image's extent.
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

// Why a zero run stopped on one side.
enum class ZeroEdge : std::uint32_t {
    NonZero = 0,    // a non-zero byte (the run's beforeWord / afterWord holds its 8-byte word)
    Cap = 1,        // the scan's limit
    Unreadable = 2, // memory the reader could not read
};

inline const char* ZeroEdgeName(ZeroEdge edge) {
    switch (edge) {
    case ZeroEdge::NonZero: return "non-zero";
    case ZeroEdge::Cap: return "cap";
    case ZeroEdge::Unreadable: return "unreadable";
    }
    return "?";
}

struct ZeroRun {
    bool readable = false;   // the 8-byte word at the address could be read
    bool zero = false;       // and is zero (else the rest is unset)
    std::uint64_t begin = 0; // [begin, end): the zero bytes around the address
    std::uint64_t end = 0;
    ZeroEdge beforeEdge = ZeroEdge::Cap;
    ZeroEdge afterEdge = ZeroEdge::Cap;
    std::uint64_t beforeWord = 0;
    std::uint64_t afterWord = 0;
};

// The run of zero bytes holding the 8-byte word at `address` (rounded down to 8), scanned up to
// `cap` bytes each way in 8-byte words; `read(address, &word)` reads one aligned word, or returns
// false where memory cannot be read.
template <class Read>
ZeroRun MeasureZeroRun(std::uint64_t address, std::uint64_t cap, Read&& read) {
    ZeroRun run;
    const auto start = address & ~std::uint64_t{7};
    std::uint64_t word = 0;
    if (!read(start, &word)) return run;
    run.readable = true;
    if (word != 0) return run;
    run.zero = true;
    run.begin = start;
    run.end = start + 8;
    for (;;) {
        if (run.end - start >= cap) break;
        if (!read(run.end, &word)) {
            run.afterEdge = ZeroEdge::Unreadable;
            break;
        }
        if (word != 0) {
            // Little endian: the low bytes of the word follow the run.
            run.afterEdge = ZeroEdge::NonZero;
            run.afterWord = word;
            run.end += static_cast<std::uint64_t>(__builtin_ctzll(word)) / 8;
            break;
        }
        run.end += 8;
    }
    for (;;) {
        if (start - run.begin >= cap || run.begin < 8) break;
        if (!read(run.begin - 8, &word)) {
            run.beforeEdge = ZeroEdge::Unreadable;
            break;
        }
        if (word != 0) {
            // The high bytes of the word precede the run.
            run.beforeEdge = ZeroEdge::NonZero;
            run.beforeWord = word;
            run.begin -= static_cast<std::uint64_t>(__builtin_clzll(word)) / 8;
            break;
        }
        run.begin -= 8;
    }
    return run;
}

}

#endif
