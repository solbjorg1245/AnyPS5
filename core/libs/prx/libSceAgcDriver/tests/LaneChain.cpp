#include "prx/libSceAgcDriver/Graphics/include/LaneChain.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

// The compute queue's chain (APS5_COMPUTE_QUEUE, docs/design/fewer-submits.md S3) on synthetic
// submissions: no device, no recorder. Run from tests/Submit.cpp's main.

namespace {

using AgcDriver::Graphics::Lane;
using AgcDriver::Graphics::LaneChain;
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

}

void testLaneChain() {
    testOff();
    testBatches();
    testOneShots();
    testSwitchCause();
}
