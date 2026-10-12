#include "prx/libSceAgcDriver/Graphics/include/LaneChain.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LaneEdges.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

// The compute queue's chain (APS5_COMPUTE_QUEUE, docs/design/fewer-submits.md S3), and S4's timeline
// per lane and edges (APS5_ASYNC_COMPUTE), on synthetic submissions: no device, no recorder. Run
// from tests/Submit.cpp's main.

namespace {

using AgcDriver::Graphics::Lane;
using AgcDriver::Graphics::LaneChain;
using AgcDriver::Graphics::LaneEdges;
using AgcDriver::Graphics::LaneHistory;
using AgcDriver::Graphics::SubmitCause;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool none(const LaneChain::Waits& waits) {
    return waits.serial == 0 && waits.oneShot == 0;
}

// What the recorder's Submit does: the waits, then the submission as serial `serial`.
LaneChain::Waits submit(LaneChain& chain, Lane lane, std::uint64_t serial) {
    const auto waits = chain.ForBatch(lane);
    chain.Submitted(lane, serial);
    return waits;
}

// What Recorder::SubmitOneShot does: the wait, then the signal once submitted.
std::uint64_t oneShot(LaneChain& chain, std::uint64_t* signal = nullptr) {
    const auto wait = chain.OneShotWait();
    if (signal != nullptr) *signal = chain.NextOneShot();
    chain.OneShotSubmitted();
    return wait;
}

void testOff() {
    LaneChain chain;
    check(!chain.Enabled(), "lane chain: off by default");
    check(none(submit(chain, Lane::C, 1)) && none(submit(chain, Lane::G, 2)) && none(submit(chain, Lane::C, 3)), "lane chain: off, a lane change waits for nothing");
    check(oneShot(chain) == 0 && oneShot(chain) == 0, "lane chain: off, a one-shot waits for nothing");
    check(none(submit(chain, Lane::C, 4)), "lane chain: off, a compute batch after one-shots waits for nothing");
}

void testBatches() {
    LaneChain chain;
    chain.Enable();
    check(none(submit(chain, Lane::C, 1)), "lane chain: the first batch waits for nothing");
    check(none(submit(chain, Lane::C, 2)) && none(submit(chain, Lane::C, 3)), "lane chain: same-lane batches wait for nothing");
    auto waits = submit(chain, Lane::G, 4);
    check(waits.serial == 3 && waits.oneShot == 0, "lane chain: a lane change waits for serial - 1");
    check(none(submit(chain, Lane::G, 5)), "lane chain: then on the same lane nothing");
    waits = submit(chain, Lane::C, 6);
    check(waits.serial == 5 && waits.oneShot == 0, "lane chain: back to C waits for the G batch before it");
}

void testOneShots() {
    LaneChain chain;
    chain.Enable();
    check(oneShot(chain) == 0, "lane chain: a one-shot before any batch waits for nothing");
    auto waits = submit(chain, Lane::C, 1);
    check(waits.serial == 0 && waits.oneShot == 1, "lane chain: the first compute batch follows an earlier one-shot");
    std::uint64_t signal = 0;
    check(oneShot(chain, &signal) == 1 && signal == 2, "lane chain: a one-shot after a compute batch waits for the newest serial");
    waits = submit(chain, Lane::C, 2);
    check(waits.serial == 0 && waits.oneShot == 2, "lane chain: a compute batch after an un-waited one-shot waits for its value");
    check(none(submit(chain, Lane::C, 3)), "lane chain: the next compute batch follows it already");

    waits = submit(chain, Lane::G, 4);
    check(waits.serial == 3 && waits.oneShot == 0, "lane chain: a lane G batch never waits for a one-shot");
    check(oneShot(chain, &signal) == 0 && signal == 3, "lane chain: a one-shot after a lane G batch waits for nothing (same queue)");
    waits = submit(chain, Lane::C, 5);
    check(waits.serial == 4 && waits.oneShot == 3, "lane chain: a compute batch right after the one-shot waits for both");

    check(oneShot(chain) == 5, "lane chain: a one-shot waits for the newest compute batch");
    check(oneShot(chain) == 5, "lane chain: and so does the next one");
    waits = submit(chain, Lane::G, 6);
    check(waits.serial == 5 && waits.oneShot == 0, "lane chain: a lane G batch after them waits for the compute batch only");
    waits = submit(chain, Lane::C, 7);
    check(waits.serial == 6 && waits.oneShot == 0, "lane chain: a compute batch after that G batch is covered by its serial wait");

    oneShot(chain, &signal);
    oneShot(chain, &signal);
    waits = submit(chain, Lane::C, 8);
    check(waits.serial == 0 && waits.oneShot == signal && signal == 7, "lane chain: two one-shots: the compute batch waits for the newer value");
}

// The switch's submits are counted apart in the APS5_LANE_CENSUS cause line (the last cause).
void testSwitchCause() {
    const auto index = static_cast<std::size_t>(SubmitCause::LaneSwitch);
    const char* name = AgcDriver::Graphics::SubmitCauseNames[index];
    check(index + 1 == AgcDriver::Graphics::SubmitCauses && name != nullptr && std::string(name) == "lane switch", "lane chain: a lane-switch submit is the last named submit cause");
}

// S4a: with a timeline per lane, "every batch up to s" is s on its lane and the other lane's newest
// serial below s; a serial the ring lost waits for min(s, newest) on both lanes.
void testHistory() {
    using Values = std::array<std::uint64_t, 2>;
    LaneHistory history;
    check(history.ValuesFor(1) == Values{0, 0}, "lane history: before any batch nothing is waited for");
    history.Submitted(Lane::G, 1);
    history.Submitted(Lane::G, 2);
    history.Submitted(Lane::C, 3);
    history.Submitted(Lane::G, 4);
    history.Submitted(Lane::C, 5);
    // By LaneIndex: lane G first, lane C second.
    check(history.ValuesFor(2) == Values{2, 0}, "lane history: a lane G serial before any compute batch waits for lane G alone");
    check(history.ValuesFor(3) == Values{2, 3}, "lane history: a compute serial waits for itself and lane G's newest below it");
    check(history.ValuesFor(4) == Values{4, 3}, "lane history: a lane G serial waits for itself and the newest compute serial below it");
    check(history.ValuesFor(5) == Values{4, 5}, "lane history: the newest serial covers both lanes' newest");
    const std::uint64_t last = 6 + LaneHistory::Size - 1;
    for (std::uint64_t serial = 6; serial <= last; ++serial) history.Submitted(Lane::G, serial);
    check(history.ValuesFor(3) == Values{3, 3}, "lane history: a serial the ring lost waits for min(serial, newest) on both lanes");
    check(history.ValuesFor(last) == Values{last, 5}, "lane history: a long lane G run still waits for the last compute batch");
}

// S4a: the waits the edges give each batch (LaneEdges), sticky per lane, against the chain's.
void testEdges() {
    using Source = LaneEdges::Source;
    const auto bit = [](Source source) { return 1u << static_cast<unsigned>(source); };
    {
        // RAW, then sticky, then WAR on the newest reader, then WAW.
        LaneEdges edges;
        edges.Access(Lane::G, 1, 0x10000, 0x10100, true);
        check(edges.Submit(Lane::G, 1, false, false).serial == 0, "lane edges: the first batch waits for nothing");
        edges.Access(Lane::C, 2, 0x10080, 0x10090, false);
        auto wait = edges.Submit(Lane::C, 2, false, false);
        check(wait.serial == 1 && wait.sources == bit(Source::Raw), "lane edges: a compute read of a lane G write waits for it (RAW)");
        edges.Access(Lane::C, 3, 0x10000, 0x10004, false);
        check(edges.Submit(Lane::C, 3, false, false).serial == 0, "lane edges: a lane's later batch is covered by its earlier wait");
        edges.Access(Lane::G, 4, 0x10000, 0x10004, true);
        wait = edges.Submit(Lane::G, 4, false, false);
        check(wait.serial == 3 && wait.sources == bit(Source::War), "lane edges: a write over the other lane's reads waits for the newest reader (WAR)");
        edges.Access(Lane::C, 5, 0x10000, 0x10004, true);
        wait = edges.Submit(Lane::C, 5, false, false);
        check(wait.serial == 4 && wait.sources == bit(Source::Waw), "lane edges: a write over the other lane's write waits for it (WAW)");
    }
    {
        // Disjoint ranges need no edge where the chain waits; a later batch of the run may need one.
        LaneEdges edges;
        edges.Access(Lane::G, 1, 0x10000, 0x10010, true);
        edges.Submit(Lane::G, 1, false, false);
        edges.Access(Lane::C, 2, 0x900000, 0x900010, false);
        check(edges.Submit(Lane::C, 2, false, false).serial == 0, "lane edges: disjoint ranges need no edge (the chain's wait is droppable)");
        edges.Access(Lane::C, 3, 0x10000, 0x10010, false);
        check(edges.Submit(Lane::C, 3, false, false).serial == 1, "lane edges: a later batch of a run that reads the other lane's write waits for it");
    }
    {
        // Producer, image, label and shadow needs; a need beyond the other lane's newest is clamped.
        LaneEdges edges;
        check(edges.Submit(Lane::G, 1, true, false).serial == 0, "lane edges: a producer waits for nothing itself");
        auto wait = edges.Submit(Lane::C, 2, false, true);
        check(wait.serial == 1 && wait.sources == bit(Source::Producer), "lane edges: the other lane's next batch waits for a producer");
        edges.Need(Lane::G, 2, Source::Label);
        wait = edges.Submit(Lane::G, 3, false, false);
        check(wait.serial == 2 && wait.sources == (bit(Source::Label) | bit(Source::Image)), "lane edges: a label taken and an image batch both ask for the compute batch");
        edges.NeedNewest(Lane::C, Source::Shadow);
        wait = edges.Submit(Lane::C, 4, false, false);
        check(wait.serial == 3 && wait.sources == bit(Source::Shadow), "lane edges: a cross-lane shadow take waits for the other lane's newest batch");
        edges.Need(Lane::G, 99, Source::Label);
        check(edges.Submit(Lane::G, 5, false, false).serial == 4, "lane edges: a need above the other lane's newest submitted batch is clamped to it");
    }
    {
        // Retired batches need no wait; ranges wider than the block table; the table is pruned.
        LaneEdges edges;
        edges.Access(Lane::G, 1, 0x10000, 0x10010, true);
        edges.Submit(Lane::G, 1, false, false);
        edges.Retired(1);
        edges.Access(Lane::C, 2, 0x10000, 0x10010, false);
        check(edges.Submit(Lane::C, 2, false, false).serial == 0, "lane edges: a need on a retired batch is dropped");
        edges.Access(Lane::G, 3, 0, std::uint64_t{1} << 40, true);
        edges.Submit(Lane::G, 3, false, false);
        edges.Access(Lane::C, 4, 0x7000000000, 0x7000000010, false);
        check(edges.Submit(Lane::C, 4, false, false).serial == 3, "lane edges: a write wider than the block table covers every block");
        edges.Access(Lane::C, 5, 0x123450000, 0x123460000, true);
        edges.Submit(Lane::C, 5, false, false);
        edges.Access(Lane::G, 6, 0, std::uint64_t{1} << 40, false);
        check(edges.Submit(Lane::G, 6, false, false).serial == 5, "lane edges: a read wider than the block table meets the other lane's newest write anywhere");
        for (std::uint64_t run = 0; run < 70; ++run) edges.Access(Lane::G, 7, run << 26, (run << 26) + (std::uint64_t{1000} << 16), true);
        edges.Submit(Lane::G, 7, false, false);
        check(edges.Blocks() > 65536, "lane edges: the block table holds the marks");
        edges.Retired(7);
        check(edges.Blocks() == 0, "lane edges: blocks only retired batches marked are pruned");
    }
}

}

void testLaneChain() {
    testOff();
    testBatches();
    testOneShots();
    testSwitchCause();
    testHistory();
    testEdges();
}
