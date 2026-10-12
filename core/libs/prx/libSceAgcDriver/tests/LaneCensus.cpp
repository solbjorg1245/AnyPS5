#include "prx/libSceAgcDriver/Graphics/include/LaneCensus.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The lane census (APS5_LANE_CENSUS, docs/design/fewer-submits.md S1) on synthetic events: no
// device, no recorder. Run from tests/Submit.cpp's main.

namespace {

using AgcDriver::Graphics::Lane;
using AgcDriver::Graphics::LaneCensus;
using AgcDriver::Graphics::SubmitCause;
using State = LaneCensus::LabelState;
using Hazard = LaneCensus::Hazard;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

std::uint64_t hazards(const LaneCensus::Counts& counts, Lane lane, Hazard kind) {
    return counts.hazards[static_cast<std::size_t>(lane)][static_cast<std::size_t>(kind)];
}

std::uint64_t hits(const LaneCensus::Counts& counts, Lane waiter, State state) {
    return counts.crossHits[static_cast<std::size_t>(waiter)][static_cast<std::size_t>(state)];
}

void testLanesAndSubmitCauses() {
    static_assert(AgcDriver::Graphics::LaneOf(0) == Lane::G && AgcDriver::Graphics::LaneOf(0x1f) == Lane::G);
    static_assert(AgcDriver::Graphics::LaneOf(0x20) == Lane::C && AgcDriver::Graphics::LaneOf(0x57) == Lane::C);
    static_assert(AgcDriver::Graphics::LaneOf(0x58) == Lane::G && AgcDriver::Graphics::LaneOf(0xffffffffu) == Lane::G);
    check(std::string(AgcDriver::Graphics::SubmitCauseNames[static_cast<std::size_t>(SubmitCause::Presync)]) == "presync", "lane census: every submit cause is named");
    // Off (the tests run without APS5_LANE_CENSUS): a scope names nothing.
    if (!AgcDriver::Graphics::LaneCensusOn) {
        const AgcDriver::Graphics::SubmitCauseScope scope(SubmitCause::Sync);
        check(AgcDriver::Graphics::CurrentSubmitCause() == SubmitCause::Other, "lane census: a scope is inert with the census off");
    }

    LaneCensus census;
    check(has(LaneCensus::CausesLine(census.Take()), "APS5_LANE_CENSUS): none; by lane: G 0, C 0"), "lane census: an empty window lists no cause");
    census.Submit(SubmitCause::Deadline, Lane::G, true, false, true);
    census.Submit(SubmitCause::LabelPacket, Lane::C, false, true, false);
    census.Submit(SubmitCause::Deadline, Lane::C, false, false, false);
    const auto counts = census.Take();
    check(counts.causes[static_cast<std::size_t>(SubmitCause::Deadline)] == 2 && counts.causes[static_cast<std::size_t>(SubmitCause::LabelPacket)] == 1, "lane census: submits counted by cause");
    check(counts.submits[0] == 1 && counts.submits[1] == 2, "lane census: submits counted by lane");
    check(counts.passEnded == 1 && counts.hostRead == 1 && counts.copiesFlushed == 1, "lane census: what each submit closed");
    const auto line = LaneCensus::CausesLine(counts);
    check(has(line, "APS5_LANE_CENSUS): deadline 2, label packet 1; by lane: G 1, C 2; submits that ended a render pass 1, recorded the host-read barrier 1, flushed deferred copies 1"), "lane census: the causes line");
    check(census.Take().causes[static_cast<std::size_t>(SubmitCause::Deadline)] == 0, "lane census: Take starts a new window");
}

void testRunsSwitchesAndSlack() {
    LaneCensus census;
    // Commands() before any item names no lane to have worked for.
    census.Commands(Lane::G, true, true);
    census.Item(Lane::C);
    census.Item(Lane::G);
    census.Item(Lane::G);
    // G's edge on the C run after two G items: two items of slack.
    census.LabelHit(Lane::G, Lane::C, State::Open, 7);
    census.Item(Lane::C);
    census.Commands(Lane::G, true, true);
    census.Commands(Lane::G, false, true);
    census.Commands(Lane::C, true, true);
    census.Item(Lane::G);
    // The next C run began with no G edge on the last: unmet, one G item.
    census.Item(Lane::C);
    // An edge while C still records: no slack; a second edge on the same run counts once.
    census.LabelHit(Lane::G, Lane::C, State::InFlight, 8);
    census.LabelHit(Lane::G, Lane::C, State::Late, 9);
    census.LabelHit(Lane::C, Lane::G, State::InFlight, 3);
    census.LabelHit(Lane::G, Lane::G, State::Open, 10);
    auto counts = census.Take();
    check(counts.items[0] == 3 && counts.items[1] == 3, "lane census: items by lane");
    check(counts.switches == 4, "lane census: lane switches");
    check(counts.runs[0] == 2 && counts.runs[1] == 2, "lane census: ended runs by lane");
    check(counts.longest[0] == 2 && counts.longest[1] == 1, "lane census: longest runs");
    check(counts.crossPasses[0] == 1 && counts.crossCopies[0] == 2 && counts.crossPasses[1] == 0 && counts.crossCopies[1] == 0, "lane census: Commands() after the other lane's item");
    check(hits(counts, Lane::G, State::Open) == 1 && hits(counts, Lane::G, State::InFlight) == 1 && hits(counts, Lane::G, State::Late) == 1, "lane census: G's cross-lane hits by state");
    check(hits(counts, Lane::C, State::InFlight) == 1 && hits(counts, Lane::C, State::Open) == 0, "lane census: same-lane hits are not cross-lane");
    check(counts.slackEdges == 2 && counts.slackItems == 2, "lane census: slack before G's edges");
    check(counts.slackUnmet == 1 && counts.slackUnmetItems == 1, "lane census: a C run G never waited on");
    const auto line = LaneCensus::LanesLine(counts);
    check(has(line, "items G 3 / C 3, 4 switches, runs G 2 / C 2 (mean 1.5 / 1.5 items, longest 2 / 1)"), "lane census: the lanes line's runs");
    check(has(line, "slack: 2 C runs met by a G edge after 1.0 G items (mean), 1 followed by the next C run without one (1.0 G items)"), "lane census: the lanes line's slack");
    // The run in progress carries over to the next window.
    census.Item(Lane::C);
    census.Item(Lane::G);
    counts = census.Take();
    check(counts.runs[1] == 1 && counts.longest[1] == 2 && counts.switches == 1, "lane census: a run spans windows");
}

void testHazardsAndShadows() {
    LaneCensus census;
    census.Access(Lane::G, 1, 0x10, 0x10, true);
    census.Access(Lane::C, 5, 0x0, 0x100, true);
    census.Access(Lane::G, 5, 0x80, 0x90, false);
    census.Access(Lane::C, 5, 0x0, 0x10, true);
    // G's label edge on C's batch 5 covers what C did there.
    census.LabelHit(Lane::G, Lane::C, State::InFlight, 5);
    census.Access(Lane::G, 6, 0x0, 0x10, true);
    // C has no edge on G: G's batch-6 write is a hazard to both a read and a write of C.
    census.Access(Lane::C, 6, 0x0, 0x10, false);
    census.Access(Lane::C, 6, 0x0, 0x10, true);
    // Retired work is ordered by completion.
    census.Retired(6);
    census.Access(Lane::G, 7, 0x0, 0x10, true);
    // Blocks of 64 KiB: a range across a boundary marks both.
    census.Access(Lane::C, 8, 0x1fff0, 0x20010, true);
    census.Access(Lane::G, 8, 0x20000, 0x20004, false);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{0x1fff0, 0x1fff4}, {0x20000, 0x20004}};
    census.Accesses(Lane::G, 8, ranges, true);
    check(census.Blocks() == 3, "lane census: blocks tracked");
    auto counts = census.Take();
    check(hazards(counts, Lane::G, Hazard::Raw) == 2 && hazards(counts, Lane::G, Hazard::Waw) == 2 && hazards(counts, Lane::G, Hazard::War) == 0, "lane census: G's uncovered hazards");
    check(hazards(counts, Lane::C, Hazard::Raw) == 1 && hazards(counts, Lane::C, Hazard::Waw) == 1 && hazards(counts, Lane::C, Hazard::War) == 2, "lane census: C's uncovered hazards");
    check(has(LaneCensus::LanesLine(counts), "(64 KiB blocks): G RAW 2 WAW 2 WAR 0, C RAW 1 WAW 1 WAR 2"), "lane census: the lanes line's hazards");
    check(census.Blocks() == 3, "lane census: blocks with unretired marks stay");
    census.Retired(7);
    census.Take();
    check(census.Blocks() == 2, "lane census: blocks with only retired marks are dropped at Take");
    census.Retired(8);
    counts = census.Take();
    check(census.Blocks() == 0 && hazards(counts, Lane::G, Hazard::Raw) == 0, "lane census: every block retired");

    census.ShadowRegistered(Lane::C, 0x1000, 0x2000);
    census.ShadowTaken(Lane::G, 0x1000, 0x2000);
    census.ShadowTaken(Lane::G, 0x1000, 0x2000);
    census.ShadowRegistered(Lane::G, 0x3000, 0x4000);
    census.ShadowTaken(Lane::G, 0x3000, 0x4000);
    counts = census.Take();
    check(counts.shadowTakes == 3 && counts.crossShadowTakes == 1, "lane census: staged shadow takes by registering lane");
}

}

void testLaneCensus() {
    testLanesAndSubmitCauses();
    testRunsSwitchesAndSlack();
    testHazardsAndShadows();
}
