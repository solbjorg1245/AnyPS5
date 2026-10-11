#include "prx/libc/include/CrashMemory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "crash_memory_tests: %s\n", what);
    std::abort();
}

static void TestRing() {
    static CrashMemory::MapRing<4> ring;
    CrashMemory::MapEntry entry;
    Require(ring.Recorded() == 0 && !ring.Read(0, &entry), "empty ring holds nothing");
    const std::uint64_t callers[] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000};
    ring.Record(CrashMemory::MapKind::Reset, 0x3dbc00000, 0x10000, 42, 7, callers, 2);
    Require(ring.Recorded() == 1 && ring.Read(0, &entry), "a record reads back");
    Require(entry.index == 0 && entry.begin == 0x3dbc00000 && entry.bytes == 0x10000 && entry.time == 42 && entry.thread == 7, "fields");
    Require(entry.kind == static_cast<std::uint32_t>(CrashMemory::MapKind::Reset) && std::strcmp(CrashMemory::MapKindName(entry.kind), "reset") == 0, "kind");
    Require(entry.callers[0] == 0x1000 && entry.callers[1] == 0x2000 && entry.callers[2] == 0 && entry.callers[3] == 0, "missing callers are zero");
    for (std::uint64_t i = 1; i < 6; ++i) ring.Record(CrashMemory::MapKind::Map, 0x1000 * i, 0x4000, static_cast<std::int64_t>(i), 1, callers, 5);
    Require(ring.Recorded() == 6, "count");
    Require(!ring.Read(0, &entry) && !ring.Read(1, &entry), "overwritten records are gone");
    Require(ring.Read(2, &entry) && entry.begin == 0x2000 && ring.Read(5, &entry) && entry.begin == 0x5000, "the last four stay");
    Require(entry.callers[3] == 0x4000, "callers beyond the slot's four are dropped");
    Require(!ring.Read(6, &entry), "a record not yet taken");
}

static void TestNear() {
    CrashMemory::MapEntry entry;
    entry.begin = 0x10000000;
    entry.bytes = 0x4000;
    Require(CrashMemory::MapEntryNear(entry, 0x10002000, 0), "inside");
    Require(!CrashMemory::MapEntryNear(entry, 0x10004000, 0), "end is exclusive");
    Require(CrashMemory::MapEntryNear(entry, 0x10004000 + 0xfff, 0x1000), "after, within the window");
    Require(!CrashMemory::MapEntryNear(entry, 0x10004000 + 0x1000, 0x1000), "after, past the window");
    Require(CrashMemory::MapEntryNear(entry, 0x10000000 - 0x1000, 0x1000) && !CrashMemory::MapEntryNear(entry, 0x10000000 - 0x1001, 0x1000), "before");
    entry.begin = 0x100;
    Require(CrashMemory::MapEntryNear(entry, 0, 0x1000), "window clamps at zero");
    entry.begin = ~std::uint64_t{0} - 0x4000;
    Require(CrashMemory::MapEntryNear(entry, ~std::uint64_t{0} - 1, 0x10000), "window clamps at the top");
}

static void TestZeroRun() {
    // A fake guest range of 0x200 words at 0x7000: readable over [0x7000, 0x8000).
    static std::uint8_t memory[0x1000];
    constexpr std::uint64_t base = 0x7000;
    const auto read = [&](std::uint64_t address, std::uint64_t* word) {
        if (address < base || address + 8 > base + sizeof(memory)) return false;
        std::memcpy(word, memory + (address - base), 8);
        return true;
    };
    memory[0x100 + 5] = 0xab; // the run starts at 0x7106
    memory[0x340 + 2] = 0xcd; // and ends at 0x7342
    auto run = CrashMemory::MeasureZeroRun(base + 0x200, 0x10000, read);
    Require(run.readable && run.zero, "a zero word");
    Require(run.begin == base + 0x106 && run.end == base + 0x342, "byte-exact edges");
    Require(run.beforeEdge == CrashMemory::ZeroEdge::NonZero && run.afterEdge == CrashMemory::ZeroEdge::NonZero, "non-zero edges");
    Require(run.beforeWord == 0xab0000000000ull && run.afterWord == 0xcd0000ull, "edge words");

    run = CrashMemory::MeasureZeroRun(base + 0x200, 0x40, read);
    Require(run.begin == base + 0x1c0 && run.end == base + 0x240, "capped both ways");
    Require(run.beforeEdge == CrashMemory::ZeroEdge::Cap && run.afterEdge == CrashMemory::ZeroEdge::Cap, "cap edges");

    std::memset(memory, 0, sizeof(memory));
    run = CrashMemory::MeasureZeroRun(base + 0x804, 0x10000, read);
    Require(run.begin == base && run.end == base + sizeof(memory), "the whole readable range");
    Require(run.beforeEdge == CrashMemory::ZeroEdge::Unreadable && run.afterEdge == CrashMemory::ZeroEdge::Unreadable, "unreadable edges");

    memory[0x808] = 1;
    run = CrashMemory::MeasureZeroRun(base + 0x808, 0x10000, read);
    Require(run.readable && !run.zero, "a non-zero word has no run");
    run = CrashMemory::MeasureZeroRun(base - 0x100, 0x10000, read);
    Require(!run.readable && !run.zero, "an unreadable address");
}

int main() {
    TestRing();
    TestNear();
    TestZeroRun();
    std::printf("crash_memory_tests: ok\n");
    return 0;
}
