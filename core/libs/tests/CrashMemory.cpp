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

// A fake guest range at 0x7000, readable over [0x7000, 0x8000).
static std::uint8_t g_memory[0x1000];
constexpr std::uint64_t MemoryBase = 0x7000;

static bool ReadFake(std::uint64_t address, std::uint64_t* word) {
    if (address < MemoryBase || address + 8 > MemoryBase + sizeof(g_memory)) return false;
    std::memcpy(word, g_memory + (address - MemoryBase), 8);
    return true;
}

static CrashMemory::ValueRun Measure(std::uint64_t offset, std::uint64_t cap, CrashMemory::RunClass runClass = CrashMemory::RunClass::Same) {
    return CrashMemory::MeasureRun(MemoryBase + offset, cap, runClass, ReadFake);
}

static void TestZeroRun() {
    using CrashMemory::RunEdge;
    std::memset(g_memory, 0, sizeof(g_memory));
    g_memory[0x100 + 5] = 0xab; // the run starts at 0x7106
    g_memory[0x340 + 2] = 0xcd; // and ends at 0x7342
    auto run = Measure(0x200, 0x10000);
    Require(run.readable && run.matches && run.value == 0, "a zero word");
    Require(run.begin == MemoryBase + 0x106 && run.end == MemoryBase + 0x342, "byte-exact edges");
    Require(run.beforeEdge == RunEdge::Differs && run.afterEdge == RunEdge::Differs, "differing edges");
    Require(run.beforeWord == 0xab0000000000ull && run.afterWord == 0xcd0000ull, "edge words");

    run = Measure(0x200, 0x40);
    Require(run.begin == MemoryBase + 0x1c0 && run.end == MemoryBase + 0x240, "capped both ways");
    Require(run.beforeEdge == RunEdge::Cap && run.afterEdge == RunEdge::Cap, "cap edges");

    std::memset(g_memory, 0, sizeof(g_memory));
    run = Measure(0x804, 0x10000);
    Require(run.begin == MemoryBase && run.end == MemoryBase + sizeof(g_memory), "the whole readable range");
    Require(run.beforeEdge == RunEdge::Unreadable && run.afterEdge == RunEdge::Unreadable, "unreadable edges");

    run = CrashMemory::MeasureRun(MemoryBase - 0x100, 0x10000, CrashMemory::RunClass::Same, ReadFake);
    Require(!run.readable && !run.matches, "an unreadable address");
}

static void TestValueRuns() {
    using CrashMemory::RunClass;
    // All ones (t582's [rbx]) over [0x400, 0x600), a pointer-sized word right before it.
    std::memset(g_memory, 0, sizeof(g_memory));
    std::memset(g_memory + 0x400, 0xff, 0x200);
    const std::uint64_t pointer = 0x00007ff7ac3d37b0ull;
    std::memcpy(g_memory + 0x3f8, &pointer, 8);
    auto run = Measure(0x4f8, 0x10000);
    Require(run.matches && run.value == ~std::uint64_t{0}, "an all-ones word");
    Require(run.begin == MemoryBase + 0x400 && run.end == MemoryBase + 0x600, "all-ones run");
    Require(run.beforeWord == pointer && run.afterWord == 0, "all-ones edge words");

    // A repeated 8-byte fill; the edges match byte for byte against the fill's own offsets.
    const std::uint64_t fill = 0x1122334455667788ull;
    for (std::size_t offset = 0x100; offset < 0x300; offset += 8) std::memcpy(g_memory + offset, &fill, 8);
    g_memory[0x300] = 0x88; // fill byte 0 continues the run
    g_memory[0x301] = 0x77; // and byte 1
    g_memory[0xff] = 0x11;  // fill byte 7 before it
    run = Measure(0x180, 0x10000);
    Require(run.value == fill && run.begin == MemoryBase + 0xff && run.end == MemoryBase + 0x302, "a repeated fill, byte exact");

    // Pixel-like bytes (t582's [rdi]): not one value, but every byte within 8 of the first.
    std::memset(g_memory, 0, sizeof(g_memory));
    const std::uint8_t pixels[] = {0x21, 0x21, 0x21, 0x21, 0x22, 0x22, 0x21, 0x21, 0x22, 0x23, 0x23, 0x22, 0x23, 0x23, 0x23, 0x23};
    for (std::size_t offset = 0x200; offset < 0x600; offset += sizeof(pixels)) std::memcpy(g_memory + offset, pixels, sizeof(pixels));
    g_memory[0x600] = 0x28;  // within 8 of 0x21
    g_memory[0x601] = 0x2a;  // not
    g_memory[0x1ff] = 0x1a;  // within 8
    g_memory[0x1fe] = 0x10;  // not
    run = Measure(0x300, 0x10000);
    Require(run.matches && run.begin == MemoryBase + 0x300 && run.end == MemoryBase + 0x308, "pixels are no single value");
    Require(CrashMemory::NearWord(run.value, static_cast<std::uint8_t>(run.value)), "pixels are low entropy");
    run = Measure(0x300, 0x10000, RunClass::Near);
    Require(run.matches && run.begin == MemoryBase + 0x1ff && run.end == MemoryBase + 0x601, "the pixel-like run, byte exact");
    Require(!CrashMemory::NearWord(pointer, static_cast<std::uint8_t>(pointer)), "a pointer is not low entropy");
    std::memcpy(g_memory + 0x300, &pointer, 8);
    run = Measure(0x300, 0x10000, RunClass::Near);
    Require(run.readable && !run.matches, "a word outside the class has no near run");
}

int main() {
    TestRing();
    TestNear();
    TestZeroRun();
    TestValueRuns();
    std::printf("crash_memory_tests: ok\n");
    return 0;
}
