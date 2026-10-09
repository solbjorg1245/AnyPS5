#include "prx/libSceAgcDriver/Graphics/include/InPlaceWriteCensus.hpp"
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace AgcDriver::Graphics;
using Reason = InPlaceWriteCensus::Reason;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool Has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// The write tracker as the census asks it (GuestMemory::CpuStampedBytes): a generation every
// collect bumps, and per range the generation of the last CPU store a collect stamped.
struct Tracker {
    std::uint64_t generation = 1;
    std::map<std::uint64_t, std::uint64_t> stamps;
    std::vector<std::uint64_t> asked;

    void Store(std::uint64_t begin) { stamps[begin] = ++generation; }
    auto Since(std::uint64_t begin, std::uint64_t bytes) {
        return [this, begin, bytes](std::uint64_t previous) {
            asked.push_back(previous);
            InPlaceWriteCensus::Change change;
            change.generation = ++generation;
            const auto found = stamps.find(begin);
            if (previous != 0 && found != stamps.end() && found->second > previous) change.dirtyBytes = bytes;
            return change;
        };
    }
};

void reasonTotals() {
    InPlaceWriteCensus census;
    Tracker tracker;
    census.Note(0x1000, 0x2000, Reason::TooSmall, 0xa0, tracker.Since(0x1000, 0x1000));
    census.Note(0x100000, 0x2100000, Reason::TooLarge, 0xa0, tracker.Since(0x100000, 0x2000000));
    census.Note(0x100000, 0x2100000, Reason::TooLarge, 0xb0, tracker.Since(0x100000, 0x2000000));
    census.Note(0x5000, 0x6000, Reason::AtomicCap, 0xc0, tracker.Since(0x5000, 0x1000));
    census.NoteSpace(64ull << 20);
    census.NoteSpace(32ull << 20);
    Expect(census.Uses(Reason::TooLarge) == 2 && census.Uses(Reason::TooSmall) == 1 && census.Uses(Reason::AtomicCap) == 1, "uses were not counted by reason");
    Expect(census.Uses(Reason::AddressBased) == 2 && census.Ranges() == 3, "a space use was kept as a range");
    const auto text = census.Report(10.0);
    Expect(Has(text, "[inplace-writes] (10 s): 6 uses of written regions bound in place (160 MiB logical) by reason: "), "the totals line is wrong");
    Expect(Has(text, "too-large 2, too-small 1, address-based 2 (2 space uses, space writable 64 MiB), mirror 0, sparse 0, unstaged 0, not-allowed 0, atomic-cap 1, no-gpu-copies 0, other 0; 3 distinct ranges (0 uses overflowed)\n"), "the reasons or the range count are wrong");
}

void topOrder() {
    InPlaceWriteCensus census;
    Tracker tracker;
    for (std::uint64_t use = 0; use < 10; ++use) census.Note(0x10000, 0x11000, Reason::TooSmall, 0xa0 + use % 5, tracker.Since(0x10000, 0x1000));
    for (std::uint64_t use = 0; use < 2; ++use) census.Note(0x20000, 0x120000, Reason::TooLarge, 0xb0 + use, tracker.Since(0x20000, 0x100000));
    for (std::uint64_t use = 0; use < 5; ++use) census.Note(0x30000, 0x32000, Reason::Other, 0xc0, tracker.Since(0x30000, 0x2000));
    const auto top = census.Top(3);
    Expect(top.size() == 3 && top[0].begin == 0x20000 && top[1].begin == 0x10000 && top[2].begin == 0x30000, "the ranges are not ordered by uses x bytes, ties by address");
    Expect(census.Top(2).size() == 2 && census.Top(9).size() == 3, "the top count is not bounded");
    Expect(top[0].program == 0xb0 && top[0].programs == 2 && !top[0].morePrograms, "a range's programs were not counted");
    Expect(top[1].program == 0xa0 && top[1].programs == 4 && top[1].morePrograms, "a fifth program was not flagged");
    const auto text = census.Report(10.0, 2);
    Expect(Has(text, "[inplace-writes] top 2 by uses x MiB") && Has(text, " 0x20000 1 MiB too-large x2 (0 changed by the CPU, 0 KiB dirty) prog 0xb0 +1 programs;"), "the top line is wrong");
    Expect(Has(text, "too-small x10") && Has(text, "prog 0xa0 +3 or more programs;\n") && !Has(text, "0x30000"), "the top line lists past its count");
}

void changedCounts() {
    InPlaceWriteCensus census;
    Tracker tracker;
    // A store before the range's first use in the window is no change between two uses.
    tracker.Store(0x40000);
    census.Note(0x40000, 0x50000, Reason::TooLarge, 0xd0, tracker.Since(0x40000, 0x10000));
    census.Note(0x40000, 0x50000, Reason::TooLarge, 0xd0, tracker.Since(0x40000, 0x10000));
    tracker.Store(0x40000);
    census.Note(0x40000, 0x50000, Reason::TooLarge, 0xd0, tracker.Since(0x40000, 0x10000));
    tracker.Store(0x60000);
    census.Note(0x40000, 0x50000, Reason::TooLarge, 0xd0, tracker.Since(0x40000, 0x10000));
    const auto top = census.Top(1);
    Expect(top.size() == 1 && top[0].uses == 4 && top[0].changed == 1 && top[0].dirtyBytes == 0x10000, "the CPU changes between uses were miscounted");
    Expect(tracker.asked == std::vector<std::uint64_t>{0, 3, 4, 6}, "a use did not ask since the generation its range's previous use got");
    Expect(Has(census.Report(10.0), "x4 (1 changed by the CPU, 64 KiB dirty) prog 0xd0;"), "the changes are not reported");
}

void overflowAndWindows() {
    InPlaceWriteCensus census(4);
    Tracker tracker;
    for (std::uint64_t index = 1; index <= 6; ++index) census.Note(index << 20, (index << 20) + 0x1000, Reason::Mirror, 0xe0, tracker.Since(index << 20, 0x1000));
    census.Note(1 << 20, (1 << 20) + 0x1000, Reason::Mirror, 0xe0, tracker.Since(1 << 20, 0x1000));
    Expect(census.Ranges() == 4 && census.Overflowed() == 2 && census.Uses(Reason::Mirror) == 7, "the range cap did not hold");
    Expect(tracker.asked.size() == 5 && census.Top(1).at(0).uses == 2, "an overflowed use asked the tracker, or a kept range stopped counting");
    census.NoteSpace(1 << 20);
    Expect(Has(census.Report(10.0), "; 4 distinct ranges (2 uses overflowed)\n"), "the overflow is not reported");
    Expect(census.Ranges() == 0 && census.Uses(Reason::Mirror) == 0 && census.Uses(Reason::AddressBased) == 0 && census.Overflowed() == 0, "the window did not restart");
    // A range's previous use does not outlive its window: a store between the windows is no change.
    tracker.Store(1 << 20);
    census.Note(1 << 20, (1 << 20) + 0x1000, Reason::Mirror, 0xe0, tracker.Since(1 << 20, 0x1000));
    Expect(tracker.asked.size() == 6 && tracker.asked.back() == 0 && census.Top(1).at(0).changed == 0, "a range's previous use outlived its window");
    const auto text = census.Report(9.6);
    Expect(Has(text, "(10 s): 1 uses") && Has(text, "(0 space uses, space writable 0 MiB)") && Has(text, " mirror x1 "), "the next window kept the previous one's counts");
    const auto empty = census.Report(10.0);
    Expect(Has(empty, "(10 s): 0 uses") && !Has(empty, "] top "), "an empty window listed ranges");
}

}

int main() {
    reasonTotals();
    topOrder();
    changedCounts();
    overflowAndWindows();
    if (failures != 0) {
        std::fprintf(stderr, "%d in-place write census checks failed\n", failures);
        return 1;
    }
    std::printf("In-place write census tests passed\n");
    return 0;
}
