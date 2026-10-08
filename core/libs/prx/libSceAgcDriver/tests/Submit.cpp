#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/NewDrawKeyTally.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Eq/include/Query.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawSkipReasons.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" int APS5_VABI sceKernelCreateEqueue(KernelEqueue* eq, const char* name);
extern "C" int APS5_VABI sceKernelDeleteEqueue(KernelEqueue eq);

static_assert(sizeof(Packet) == 16);
static_assert(offsetof(Packet, addr) == 0);
static_assert(offsetof(Packet, dw_num) == 8);
static_assert(offsetof(Packet, flags) == 12);

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
std::string expectFailure(TAction action) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw std::runtime_error("expected exception");
}

void testEvents() {
    KernelEvent event{};
    event.filter = -14;
    event.ident = 0x40;
    event.data = 123;
    check(sceAgcDriverGetEqEventType(&event) == 0x40, "graphics event uses wrong field");
    event.filter = -1;
    event.data = -17;
    check(sceAgcDriverGetEqEventType(&event) == -17, "non-graphics event uses wrong field");
    event.data = std::numeric_limits<std::intptr_t>::max();
    expectFailure([&] { sceAgcDriverGetEqEventType(&event); });
    event.filter = -14;
    event.ident = std::numeric_limits<std::uintptr_t>::max();
    expectFailure([&] { sceAgcDriverGetEqEventType(&event); });
    expectFailure([] { sceAgcDriverGetEqEventType(nullptr); });
    expectFailure([&] { sceAgcDriverGetEqEventType(reinterpret_cast<const KernelEvent*>(reinterpret_cast<const std::byte*>(&event) + 1)); });
}

// The draw skip reason heads: numbers that vary per draw fold into '#', small ones and the text stay,
// the " [packet]" suffix and further lines are cut; the tally's order, overflow and reset.
void testDrawSkipReasonTally() {
    using AgcDriver::Graphics::DrawSkipReasonKey;
    using AgcDriver::Graphics::DrawSkipReasonTally;
    check(DrawSkipReasonKey("GetImageResource dword 0 invalid at 0x7f12ab00") == "GetImageResource dword 0 invalid at #", "hex address not folded");
    check(DrawSkipReasonKey("array pitch 4096 != 2048 [DRAW_INDEX_2, color target 0x1234]") == "array pitch # != #", "decimal runs or suffix not folded");
    check(DrawSkipReasonKey("stage 1 differs\nRecompileRequest:\n...") == "stage 1 differs", "second line kept");
    check(DrawSkipReasonKey("0x") == "0x", "bare 0x folded");
    check(DrawSkipReasonKey(std::string(200, 'a')).size() == 72, "head not capped");
    DrawSkipReasonTally tally;
    tally.Add("corner sampling at 0x100", 10.0);
    tally.Add("corner sampling at 0x200", 30.0);
    tally.Add("array pitch 4096", 50.0);
    tally.Add("guest snapshot differs from registered memory", 5.0);
    tally.Add("guest snapshot differs from registered memory", 1.0);
    check(tally.Keys() == 3, "heads not merged");
    const auto top = tally.Top(2);
    check(top.size() == 2 && top[0].key == "corner sampling at #" && top[0].count == 2 && top[0].us == 40.0, "top head wrong");
    check(top[1].key == "guest snapshot differs from registered memory" && top[1].count == 2, "second head wrong");
    for (std::size_t i = 0; i < DrawSkipReasonTally::MaxKeys; ++i) tally.Add("reason " + std::string(1, static_cast<char>('a' + i % 26)) + std::string(1, static_cast<char>('a' + i / 26)), 1.0);
    check(tally.Keys() == DrawSkipReasonTally::MaxKeys && tally.Overflow() == 3 && tally.OverflowUs() == 3.0, "overflow not counted");
    tally.Clear();
    check(tally.Keys() == 0 && tally.Overflow() == 0 && tally.Top(8).empty(), "tally not cleared");
}

void testValidation() {
    std::array<std::uint32_t, 3> commands{0xc0017600, 0x20c, 0};
    Packet packet{commands.data(), 3, 0, {}};
    expectFailure([] { sceAgcDriverSubmitDcb(nullptr); });
    expectFailure([] { sceAgcDriverAgrSubmitDcb(nullptr); });
    expectFailure([] { sceAgcDriverSubmitAcb(0x20, nullptr); });
    expectFailure([&] { sceAgcDriverSubmitAcb(0, &packet); });
    expectFailure([&] { sceAgcDriverSubmitAcb(0x58, &packet); });
    packet.dw_num = 2;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.dw_num = 3;
    packet.flags = 1;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.flags = 0;
    commands[0] = 0xc001ff00;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc001105c;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc0017601;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc0017600;
    commands[1] = 0x10000;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(commands.data()) + 1);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(std::numeric_limits<std::uintptr_t>::max() - 3);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(0x1000);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    AgcDriverWaitIdle_nid_postfix();
}

void testClearState() {
    AgcDriver::QueueState graphics{{{0x20c, 1}}, {{0x10, 17}, {0x11, 23}}, {{0x242, 5}}};
    const auto shader = graphics.shader;
    const auto userConfig = graphics.userConfig;
    graphics.ClearContext();
    check(graphics.context == AgcDriver::InitialContextRegisters(), "CLEAR_STATE retained context registers");
    check(graphics.shader == shader && graphics.userConfig == userConfig, "CLEAR_STATE reset unrelated registers");
    graphics.context.emplace(0x10, 31);
    graphics.ClearContext();
    check(graphics.context == AgcDriver::InitialContextRegisters(), "repeated CLEAR_STATE retained context registers");

    std::array<std::uint32_t, 3> words{0xc0001200, 0, 0};
    Packet packet{words.data(), 2, 0, {}};
    expectFailure([&] { sceAgcDriverSubmitAcb(0x20, &packet); });
    words[1] = 0x10;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[1] = 0;
    words[0] = 0xc0011200;
    packet.dw_num = 3;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[0] = 0xc0001201;
    packet.dw_num = 2;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[0] = 0xc0001200;
    packet.dw_num = 1;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.dw_num = 2;
    for (std::uint32_t state = 0; state <= 0xf; ++state) {
        words[1] = state;
        check(sceAgcDriverSubmitDcb(&packet) == 0, "CLEAR_STATE submit failed");
    }
    AgcDriverWaitIdle_nid_postfix();
}

void testSubmissions() {
    std::vector<std::thread> producers;
    std::array<std::exception_ptr, 4> errors{};
    for (std::uint32_t i = 0; i < errors.size(); ++i) {
        producers.emplace_back([&, i] {
            try {
                for (std::uint32_t j = 0; j < 100; ++j) {
                    std::array<std::uint32_t, 5> words{0xc0017600, 0x240, j, 0xc0001000, 0};
                    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
                    if (i == 0) check(sceAgcDriverSubmitDcb(&packet) == 0, "DCB submit failed");
                    else if (i == 1) check(sceAgcDriverAgrSubmitDcb(&packet) == 0, "AGR submit failed");
                    else check(sceAgcDriverSubmitAcb(i == 2 ? 0x20 : 0x57, &packet) == 0, "ACB submit failed");
                    words.fill(0xffffffffu);
                }
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    for (auto& producer : producers) producer.join();
    for (auto& error : errors) if (error) std::rethrow_exception(error);
    AgcDriverWaitIdle_nid_postfix();
    Packet empty{};
    check(sceAgcDriverSubmitDcb(&empty) == 0, "empty submit failed");
    AgcDriverWaitIdle_nid_postfix();
}

void testEndOfPipeInterrupts() {
    KernelEqueue eq = 0;
    check(sceKernelCreateEqueue(&eq, "AGC test") == 0, "event queue creation failed");
    auto owner = EqueuePin_nid_postfix(eq);
    int graphicsTag = 0;
    int computeTag = 0;
    check(sceAgcDriverAddEqEvent(eq, 0, &graphicsTag) == 0, "graphics event registration failed");
    check(sceAgcDriverAddEqEvent(eq, 0x20, &computeTag) == 0, "compute event registration failed");
    expectFailure([] { sceAgcDriverAddEqEvent(0, 0, nullptr); });
    std::array<std::uint32_t, 8> words{0xc0064900, 0, 1u << 24u, 0, 0, 0, 0, 0};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0 && sceAgcDriverSubmitDcb(&packet) == 0, "interrupt submit failed");
    AgcDriverWaitIdle_nid_postfix();
    std::array<KernelEvent, 2> events{};
    check(owner->GetTriggeredEvents(events.data(), 2) == 1, "graphics end-of-pipe interrupt was not delivered to its queue only");
    check(events[0].filter == -14 && events[0].udata == &graphicsTag && events[0].data == 2 && sceAgcDriverGetEqEventType(events.data()) == 0, "graphics end-of-pipe event encoding is wrong");
    check(owner->GetTriggeredEvents(events.data(), 2) == 0, "delivered interrupt was not cleared");
    check(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute interrupt submit failed");
    AgcDriverWaitIdle_nid_postfix();
    check(owner->GetTriggeredEvents(events.data(), 2) == 1 && events[0].udata == &computeTag && sceAgcDriverGetEqEventType(events.data()) == 0x20, "compute end-of-pipe interrupt missing");
    words[2] = 0;
    check(sceAgcDriverSubmitDcb(&packet) == 0, "plain release submit failed");
    AgcDriverWaitIdle_nid_postfix();
    check(owner->GetTriggeredEvents(events.data(), 2) == 0, "release without INT_SEL raised an interrupt");
    check(sceAgcDriverDeleteEqEvent(eq, 0) == 0, "graphics event deletion failed");
    expectFailure([&] { sceAgcDriverDeleteEqEvent(eq, 0); });
    words[2] = 1u << 24u;
    check(sceAgcDriverSubmitDcb(&packet) == 0, "interrupt submit after deletion failed");
    AgcDriverWaitIdle_nid_postfix();
    check(owner->GetTriggeredEvents(events.data(), 2) == 0, "deleted event still received interrupts");
    check(sceAgcDriverDeleteEqEvent(eq, 0x20) == 0, "compute event deletion failed");
    owner.reset();
    check(sceKernelDeleteEqueue(eq) == 0, "event queue deletion failed");
}

std::array<std::uint32_t, 5> writeData(volatile std::uint32_t* address, std::uint32_t value) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0033700, 0x00100200, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), value};
}

std::array<std::uint32_t, 7> waitEqual(volatile std::uint32_t* address, std::uint32_t value) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0053c00, 0x13, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), value, 0xffffffffu, 0x19};
}

std::array<std::uint32_t, 9> waitEqual64(volatile std::uint32_t* address, std::uint64_t value, std::uint64_t mask) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0079300, 0x13, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32u), static_cast<std::uint32_t>(mask), static_cast<std::uint32_t>(mask >> 32u), 0x19};
}

void submit(std::uint32_t queue, const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    check((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "label submit failed");
}

std::chrono::milliseconds waitFor(volatile std::uint32_t* address, std::uint32_t value, const char* message) {
    const auto start = std::chrono::steady_clock::now();
    while (*address != value) {
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), message);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

template<std::size_t... N>
std::vector<std::uint32_t> commands(const std::array<std::uint32_t, N>&... packets) {
    std::vector<std::uint32_t> words;
    (words.insert(words.end(), packets.begin(), packets.end()), ...);
    return words;
}

std::array<std::uint32_t, 8> endOfPipeLabel(volatile std::uint32_t* address, std::uint32_t value) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0064900, 0x514, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), value, 0, 0};
}

void testEndOfPipeLabelsWithoutWork() {
    alignas(64) static volatile std::uint32_t first = 0, second = 0, done = 0;
    KernelEqueue eq = 0;
    check(sceKernelCreateEqueue(&eq, "AGC EOP labels") == 0, "event queue creation failed");
    auto owner = EqueuePin_nid_postfix(eq);
    int tag = 0;
    check(sceAgcDriverAddEqEvent(eq, 0, &tag) == 0, "graphics event registration failed");
    submit(0x20, commands(waitEqual(&second, 2), writeData(&done, 1)));
    submit(0, commands(endOfPipeLabel(&first, 1), endOfPipeLabel(&second, 2)));
    waitFor(&done, 1, "an end-of-pipe label with no work before it never landed");
    check(first == 1 && second == 2, "end-of-pipe labels with no work before them landed wrong");
    AgcDriverWaitIdle_nid_postfix();
    std::array<KernelEvent, 2> events{};
    check(owner->GetTriggeredEvents(events.data(), 2) == 1 && events[0].udata == &tag && events[0].data == 2, "end-of-pipe labels with no work before them lost their interrupts");
    check(sceAgcDriverDeleteEqEvent(eq, 0) == 0, "graphics event deletion failed");
    owner.reset();
    check(sceKernelDeleteEqueue(eq) == 0, "event queue deletion failed");
}

void testLabelStoredSinceSubmission() {
    alignas(64) static volatile std::uint32_t gate = 0, label = 0, done = 0, late = 0;
    submit(0x20, commands(waitEqual(&gate, 1), waitEqual(&label, 1), writeData(&done, 1)));
    submit(0, commands(writeData(&label, 1)));
    waitFor(&label, 1, "producer label never landed");
    label = 0;
    gate = 1;
    check(waitFor(&done, 1, "consumer never passed its waits") < std::chrono::milliseconds(500), "a label stored after the wait's submission did not satisfy it");
    AgcDriverWaitIdle_nid_postfix();
    submit(0x20, commands(waitEqual(&label, 1), writeData(&late, 1)));
    check(waitFor(&late, 1, "consumer never passed its wait") >= std::chrono::milliseconds(900), "a label stored before the wait's submission satisfied it");
    AgcDriverWaitIdle_nid_postfix();
}

void testWideLabelStoredSinceSubmission() {
    alignas(64) static volatile std::uint32_t gate = 0, done = 0, late = 0;
    alignas(64) static volatile std::uint32_t label[2] = {0, 0x5eed};
    submit(0x20, commands(waitEqual(&gate, 1), waitEqual64(label, 1, 0xffffffffu), writeData(&done, 1)));
    submit(0, commands(writeData(label, 1)));
    waitFor(label, 1, "producer label never landed");
    label[0] = 0;
    gate = 1;
    check(waitFor(&done, 1, "consumer never passed its waits") < std::chrono::milliseconds(500), "a 32-bit label stored after a low-dword 64-bit wait's submission did not satisfy it");
    AgcDriverWaitIdle_nid_postfix();
    gate = 0;
    submit(0x20, commands(waitEqual(&gate, 1), waitEqual64(label, 1, ~0ull), writeData(&late, 1)));
    submit(0, commands(writeData(label, 1)));
    waitFor(label, 1, "producer label never landed");
    label[0] = 0;
    gate = 1;
    check(waitFor(&late, 1, "consumer never passed its wait") >= std::chrono::milliseconds(900), "a 32-bit store satisfied a 64-bit wait whose high dword never matched");
    AgcDriverWaitIdle_nid_postfix();
}

void testLabelHeldAtSubmission() {
    alignas(64) static volatile std::uint32_t gate = 0, label = 1, done = 0, reset = 0;
    submit(0x20, commands(waitEqual(&gate, 1), waitEqual(&label, 1), writeData(&done, 1)));
    label = 0;
    gate = 1;
    check(waitFor(&done, 1, "consumer never passed its waits") < std::chrono::milliseconds(500), "a label held when the wait was submitted did not satisfy it");
    AgcDriverWaitIdle_nid_postfix();
    gate = 0;
    label = 1;
    submit(0x20, commands(waitEqual(&gate, 1), writeData(&label, 0), waitEqual(&label, 1), writeData(&reset, 1)));
    gate = 1;
    check(waitFor(&reset, 1, "consumer never passed its wait") >= std::chrono::milliseconds(900), "a label its own queue stored first counted as held at the submission");
    AgcDriverWaitIdle_nid_postfix();
}

// A dispatch the driver cannot run (here: no compute program registers) is reported and skipped,
// as the GPU runs past compute work whose tables are not filled yet (Demon's Souls issues some):
// the queue goes idle and later submissions are accepted.
void testSkippedDispatch() {
    std::array<std::uint32_t, 5> words{0xc0031500, 1, 1, 1, 0x41};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitAcb(0x21, &packet) == 0, "dispatch was not accepted");
    std::vector<std::thread> waiters;
    for (std::size_t waiter = 0; waiter < 4; ++waiter) waiters.emplace_back([] { AgcDriverWaitIdle_nid_postfix(); });
    for (auto& waiter : waiters) waiter.join();
    check(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "a submission after a skipped dispatch was refused");
    AgcDriverWaitIdle_nid_postfix();
}

// The never-seen draw key tally: keys per base key, the pointer words that changed between them,
// a key absent again with the same words, the top order and the reset.
void testNewDrawKeyTally() {
    using AgcDriver::DriverDetail::NewDrawKeyTally;
    NewDrawKeyTally tally;
    const std::array<std::uint64_t, 5> programs{0x1000, 0, 0, 0, 0x2000};
    std::array<std::uint32_t, 8> words{0x10, 0x20, 0, 0, 0, 0, 0x30, 0x40};
    const std::uint32_t present = 0xc3;
    tally.Note(7, true, words, present, programs);
    words[0] = 0x110;
    tally.Note(7, true, words, present, programs);
    tally.Note(7, false, words, present, programs);
    words[6] = 0x130;
    words[2] = 0x99; // not present: ignored
    tally.Note(7, false, words, present, programs);
    tally.Note(9, false, words, present, programs);
    check(tally.keys == 5 && tally.bases.size() == 2 && tally.untracked == 0, "tally counts keys and bases");
    const auto top = tally.Top(10);
    check(top.size() == 2 && top[0].first == 7 && top[1].first == 9, "tally top orders by keys");
    const auto& base = *top[0].second;
    check(base.keys == 4 && base.known == 2 && base.sameWords == 1, "tally counts known and same-word keys");
    check(base.changed == 0x41, "tally marks the changed present pointer words");
    check(base.programs[0] == 0x1000 && base.programs[4] == 0x2000, "tally keeps the program addresses");
    check(tally.Top(1).size() == 1, "tally top keeps the count asked for");
    tally.Reset();
    check(tally.keys == 0 && tally.bases.empty() && tally.Top(10).empty(), "tally reset");
}

// The fast-path census core (docs/design/draw-fastpath.md F0): the state key leaves out the shader
// user words and merged-stage pointers, the target-free state key also the color and depth bases;
// which packets end a run of eligible draws; the run, decline and descriptor tallies.
void testFastCensus() {
    using namespace AgcDriver::DriverDetail;
    const std::array<FastCensusRange, 6> ranges{{{0, 0x012, 4}, {0, 0x1e0, 8}, {0, 0x318, 0x78}, {0, 0x390, 8}, {1, 0x008, 0x24}, {1, 0x082, 2}}};
    auto queue = std::make_unique<AgcDriver::QueueState>();
    queue->shader.insert_or_assign(0x008, 0x1000);
    queue->shader.insert_or_assign(0x00c, 0x10);
    const auto base = ComputeFastCensusKeys(*queue, ranges);
    check(base.full == ComputeFastCensusKeys(*queue, ranges).full, "census keys are deterministic");
    const auto after = [&](std::uint8_t bank, std::uint32_t offset, std::uint32_t value) {
        auto changed = std::make_unique<AgcDriver::QueueState>(*queue);
        (bank == 0 ? changed->context : changed->shader).insert_or_assign(offset, value);
        return ComputeFastCensusKeys(*changed, ranges);
    };
    const auto expect = [&](const FastCensusKeys& keys, bool full, bool state, bool stateNoTargets, const char* what) {
        check((keys.full != base.full) == full && (keys.state != base.state) == state && (keys.stateNoTargets != base.stateNoTargets) == stateNoTargets, what);
    };
    expect(after(1, 0x00c, 0x20), true, false, false, "census: a pixel user word changes the full key alone");
    expect(after(1, 0x082, 0x1234), true, false, false, "census: the geometry-back pointer changes the full key alone");
    expect(after(1, 0x008, 0x2000), true, true, true, "census: a program address is state");
    expect(after(0, 0x1e0, 0x12345678), true, true, true, "census: a blend control is state");
    expect(after(0, 0x318, 0x4000), true, true, false, "census: CB_COLOR0_BASE is a target base");
    expect(after(0, 0x318 + 15 + 13, 0x4000), true, true, false, "census: CB_COLOR1_DCC_BASE is a target base");
    expect(after(0, 0x391, 0x1), true, true, false, "census: CB_COLOR1_BASE_EXT is a target base");
    expect(after(0, 0x014, 0x4000), true, true, false, "census: DB_Z_WRITE_BASE is a target base");
    expect(after(0, 0x31c, 0x4000), true, true, true, "census: CB_COLOR0_INFO is state");
    expect(after(0, 0x2ff, 0x1), false, false, false, "census: a register outside the ranges changes no key");

    const auto header = [](std::uint32_t opcode) { return 0xc0000000u | (opcode << 8u); };
    const auto custom = [&](std::uint32_t kind) { return header(0x10) | (kind << 2u); };
    check(!ClassifyFastCensusPacket(header(0x69), false, false, false), "census: SET_CONTEXT_REG keeps a run");
    check(!ClassifyFastCensusPacket(header(0x76), false, false, false), "census: SET_SH_REG keeps a run");
    check(!ClassifyFastCensusPacket(header(0x12), false, false, false), "census: CLEAR_STATE keeps a run");
    check(!ClassifyFastCensusPacket(custom(0x0b), false, false, false), "census: a marker keeps a run");
    check(!ClassifyFastCensusPacket(custom(0x1a), false, false, false), "census: a context push keeps a run");
    check(!ClassifyFastCensusPacket(header(0x81), false, false, false), "census: WRITE_CONST_RAM keeps a run");
    check(ClassifyFastCensusPacket(header(0x15), false, false, false) == FastCensusBreak::Dispatch, "census: a dispatch ends a run");
    check(ClassifyFastCensusPacket(header(0x3c), false, false, false) == FastCensusBreak::Wait, "census: WAIT_REG_MEM ends a run");
    check(ClassifyFastCensusPacket(header(0x49), false, false, false) == FastCensusBreak::Label, "census: RELEASE_MEM ends a run");
    check(ClassifyFastCensusPacket(header(0x50), false, false, false) == FastCensusBreak::Dma, "census: DMA_DATA ends a run");
    check(ClassifyFastCensusPacket(header(0x40), false, false, false) == FastCensusBreak::Dma && ClassifyFastCensusPacket(header(0x83), false, false, false) == FastCensusBreak::Dma, "census: COPY_DATA and DUMP_CONST_RAM are memory copies");
    check(ClassifyFastCensusPacket(custom(0x18), false, false, false) == FastCensusBreak::Label, "census: RELEASE_MEM_CUSTOM ends a run");
    check(ClassifyFastCensusPacket(header(0x69), false, false, true) == FastCensusBreak::Label, "census: a packet that wrote on the GPU ends a run");
    check(ClassifyFastCensusPacket(custom(0x17), true, false, false) == FastCensusBreak::Flip, "census: a flip ends a run");
    check(ClassifyFastCensusPacket(custom(0x06), false, true, false) == FastCensusBreak::Wait, "census: a rendering wait ends a run");
    check(ClassifyFastCensusPacket(header(0x22), false, false, false) == FastCensusBreak::Other, "census: an unlisted packet ends a run");

    check(FastCensusRuns::Bucket(1) == 0 && FastCensusRuns::Bucket(2) == 1 && FastCensusRuns::Bucket(3) == 1 && FastCensusRuns::Bucket(4) == 2, "census: short run buckets");
    check(FastCensusRuns::Bucket(127) == 6 && FastCensusRuns::Bucket(128) == 7 && FastCensusRuns::Bucket(1u << 20u) == 7, "census: long run buckets");
    FastCensusRuns runs;
    runs.Close(0, FastCensusBreak::Wait);
    runs.Close(3, FastCensusBreak::Dispatch);
    runs.Close(20, FastCensusBreak::SubmissionEnd);
    check(runs.Runs() == 2 && runs.packets == 23 && runs.longPackets == 20 && runs.runs[1] == 1 && runs.runs[4] == 1, "census: runs by length");
    check(runs.breaks[static_cast<std::size_t>(FastCensusBreak::Wait)] == 1 && runs.breaks[static_cast<std::size_t>(FastCensusBreak::Dispatch)] == 1 && runs.breaks[static_cast<std::size_t>(FastCensusBreak::SubmissionEnd)] == 1, "census: what ended the runs");
    runs.Reset();
    check(runs.Runs() == 0 && runs.packets == 0 && runs.breaks[static_cast<std::size_t>(FastCensusBreak::Wait)] == 0, "census: runs reset");

    FastCensusTally decline;
    const auto writes = static_cast<std::size_t>(FastCensusReason::Writes);
    const auto bindless = static_cast<std::size_t>(FastCensusReason::Bindless);
    decline.Note(0);
    decline.Note(FastCensusBit(FastCensusReason::Writes));
    decline.Note(FastCensusBit(FastCensusReason::Writes) | FastCensusBit(FastCensusReason::Bindless));
    check(decline.draws == 3 && decline.eligible == 1, "census: eligible draws");
    check(decline.any[writes] == 2 && decline.sole[writes] == 1 && decline.any[bindless] == 1 && decline.sole[bindless] == 0, "census: declines any and sole");

    check(FastCensusDescriptorBucket(0) == 0 && FastCensusDescriptorBucket(8) == 0 && FastCensusDescriptorBucket(9) == 1 && FastCensusDescriptorBucket(32) == 3, "census: descriptor buckets to the push limit");
    check(FastCensusDescriptorBucket(33) == 4 && FastCensusDescriptorBucket(64) == 5 && FastCensusDescriptorBucket(65) == 6, "census: descriptor buckets over the push limit");
}

// The fast dispatch (F5): the compare its verify mode makes between the old path's capture and the
// walked variant (APS5_FAST_DISPATCH_VERIFY, CompareWalkedResults), and the push layout key a
// variant's bindings give, with the bindings it declines (Graphics::FastComputeLayoutKey).
void testFastDispatchVerify() {
    using namespace AgcDriver::DriverDetail;
    using AgcDriver::Graphics::FastDispatchDecline;
    using Kind = ShaderRecompiler::DescriptorKind;
    using Role = ShaderRecompiler::DescriptorRole;
    ShaderRecompiler::RecompileResult old;
    old.variantId = 7;
    old.bindings.push_back({Kind::StorageBuffer, Role::GuestBuffers, 0, 0, 1, {1, 2, 3, 4}});
    old.bindings.push_back({Kind::SampledImage, Role::GuestImages, 0, 1, 1, {0, 1, 2, 3, 4, 5, 6, 7}});
    old.bindings.push_back({Kind::StorageBuffer, Role::FlattenedSrt, 0, 2, 1, {9, 9}});
    old.pushConstants.resize(8);
    const auto mismatch = [](WalkMismatch kind) { return 1u << static_cast<unsigned>(kind); };
    WalkDifference first;
    std::uint64_t feedback = 0;
    std::uint64_t deferred = 0;
    auto walked = old;
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == 0, "fast dispatch verify: equal results differ");
    // The T# streaming-feedback fields (word 5 bit 25, word 6 bits 0-7) are no difference.
    walked.bindings[1].guestDescriptor[5] ^= 1u << 25u;
    walked.bindings[1].guestDescriptor[6] ^= 0x3u;
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == 0 && feedback == 1, "fast dispatch verify: T# feedback bits differ");
    walked.bindings[1].guestDescriptor[6] ^= 0x100u;
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == mismatch(WalkMismatch::Image) && first.binding == 1 && first.word == 6, "fast dispatch verify: a T# word outside the feedback bits is no image mismatch");
    // A flat word the capture left to the GPU holds a placeholder: skipped, counted.
    walked = old;
    auto deferring = old;
    deferring.bindings[2].deferredWords.push_back({1, 0x10000});
    walked.bindings[2].guestDescriptor[1] = 5;
    check(CompareWalkedResults(deferring, walked, first, feedback, deferred) == 0 && deferred == 1, "fast dispatch verify: a deferred flat word differs");
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == mismatch(WalkMismatch::Flat), "fast dispatch verify: a flat word is no flat mismatch");
    // The fast dispatch binds the walked word: a deferred word differs even where the walk read the placeholder.
    deferred = 0;
    check(CompareWalkedResults(deferring, deferring, first, feedback, deferred, true) == mismatch(WalkMismatch::Flat) && first.binding == 2 && first.word == 1 && deferred == 0, "fast dispatch verify: a deferred word is no mismatch for the fast dispatch");
    walked = old;
    walked.bindings[0].guestDescriptor[0] = 0x100;
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == mismatch(WalkMismatch::Buffer) && first.kind == WalkMismatch::Buffer && first.binding == 0 && first.word == 0 && first.old == 1 && first.walked == 0x100, "fast dispatch verify: a moved V# is no buffer mismatch");
    walked = old;
    walked.variantId = 8;
    check((CompareWalkedResults(old, walked, first, feedback, deferred) & mismatch(WalkMismatch::Variant)) != 0, "fast dispatch verify: another variant is no variant mismatch");
    walked = old;
    walked.pushConstants[3] = std::byte{1};
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == mismatch(WalkMismatch::Push) && first.word == 3, "fast dispatch verify: a push constant byte is no push mismatch");
    walked = old;
    walked.bindings.pop_back();
    check(CompareWalkedResults(old, walked, first, feedback, deferred) == mismatch(WalkMismatch::Layout), "fast dispatch verify: a missing binding is no layout mismatch");

    std::vector<std::uint32_t> key;
    const std::vector<std::uint32_t> expected{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    check(!AgcDriver::Graphics::FastComputeLayoutKey(old, key) && key == expected, "fast dispatch layout key: not the ShaderResources key");
    auto declined = old;
    declined.bindings.push_back({Kind::StorageBuffer, Role::BdaPagetable, 0, 3, 1, {}});
    check(AgcDriver::Graphics::FastComputeLayoutKey(declined, key) == FastDispatchDecline::Bda, "fast dispatch layout key: a BDA table is bound");
    declined = old;
    declined.bindings.push_back({Kind::StorageBuffer, Role::Gds, 0, 3, 1, {}});
    check(AgcDriver::Graphics::FastComputeLayoutKey(declined, key) == FastDispatchDecline::Role, "fast dispatch layout key: GDS is bound");
    declined = old;
    declined.bindings[2].binding = 0;
    check(AgcDriver::Graphics::FastComputeLayoutKey(declined, key) == FastDispatchDecline::Invalid, "fast dispatch layout key: a duplicate binding is bound");
    declined = old;
    declined.bindings[2].count = 2;
    check(AgcDriver::Graphics::FastComputeLayoutKey(declined, key) == FastDispatchDecline::Invalid, "fast dispatch layout key: an array of flat words is bound");
    for (const auto* name : AgcDriver::Graphics::FastDispatchDeclineNames) check(name != nullptr && *name != '\0', "fast dispatch: a decline without a name");
    for (const auto* name : WalkDeclineNames) check(name != nullptr && *name != '\0', "fast walk: a decline without a name");

    // The declines made before the lock (Graphics::FastDispatchPrecheck): what a build rejects of a
    // read-only V# and of the data words; the adjustment slot is required of every element.
    using AgcDriver::Graphics::FastDispatchPrecheck;
    AgcDriver::Graphics::Context context{};
    context.limits.maxStorageBufferRange = 1u << 20u;
    ShaderRecompiler::RecompileResult shader;
    shader.bindings.push_back({Kind::StorageBuffer, Role::GuestBuffers, 0, 0, 1, {0x10000u, 4u << 16u, 64u, 0x01016facu}});
    shader.bindings[0].bufferWritten = {false};
    shader.bindings.push_back({Kind::StorageBuffer, Role::FlattenedSrt, 0, 1, 1, {9, 9}});
    check(!FastDispatchPrecheck(context, shader), "fast dispatch precheck: a read V# and flat words declined");
    auto checked = shader;
    checked.pushConstants.resize(4);
    checked.memoryOffsetDword = 1;
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Invalid, "fast dispatch precheck: an adjustment byte outside the push constants");
    checked.memoryOffsetDword = 0;
    check(!FastDispatchPrecheck(context, checked), "fast dispatch precheck: an adjustment byte inside the push constants declined");
    checked = shader;
    checked.bindings[0].guestDescriptor[1] |= 0x40000000u;
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Invalid, "fast dispatch precheck: a reserved V# bit");
    checked = shader;
    checked.bindings[0].guestDescriptor = {0, 0, 0, 0};
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Invalid, "fast dispatch precheck: an empty V# without the placeholder buffer");
    checked.pushConstants.resize(4);
    checked.memoryOffsetDword = 1;
    context.emptyBuffer = reinterpret_cast<VkBuffer>(std::uintptr_t{0x10});
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Invalid, "fast dispatch precheck: an empty V# outside the push constants");
    checked.memoryOffsetDword = 0;
    check(!FastDispatchPrecheck(context, checked), "fast dispatch precheck: an empty V# declined");
    checked = shader;
    checked.bindings[1].deferredWords.push_back({1, 0x20000});
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Deferred, "fast dispatch precheck: deferred flat words");
    checked = shader;
    checked.bindings[1].guestDescriptor.assign((1u << 18u) + 1u, 0u);
    check(FastDispatchPrecheck(context, checked) == FastDispatchDecline::Invalid, "fast dispatch precheck: flat words over the range limit");
}

}

int main() {
    try {
        testEvents();
        testDrawSkipReasonTally();
        testValidation();
        testClearState();
        testSubmissions();
        testEndOfPipeInterrupts();
        testLabelStoredSinceSubmission();
        testEndOfPipeLabelsWithoutWork();
        testLabelHeldAtSubmission();
        testWideLabelStoredSinceSubmission();
        testSkippedDispatch();
        testNewDrawKeyTally();
        testFastCensus();
        testFastDispatchVerify();
        LibcRunShutdown_nid_postfix();
        std::puts("AGC driver submit tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try { LibcRunShutdown_nid_postfix(); }
        catch (const std::exception& shutdown) { std::fprintf(stderr, "shutdown: %s\n", shutdown.what()); }
        return 1;
    }
}
