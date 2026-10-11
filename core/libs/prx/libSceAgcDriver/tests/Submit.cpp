#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/NewDrawKeyTally.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/KeyedMemo.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
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
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <optional>
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

// The idle queue worker's sleep between completion reaps (QueueWorker.cpp): on Windows a wake
// signalled before or during the sleep ends it at once, and the high-resolution timer ends it
// otherwise; elsewhere there is no wake event and the worker keeps its condition-variable wait.
void testPollWake() {
    using namespace AgcDriver::DriverDetail;
    using Clock = std::chrono::steady_clock;
    void* wake = PollWakeEvent();
#ifdef _WIN32
    check(wake != nullptr && PollWakeEvent() == wake, "poll wake: no wake event, or a second one, on this thread");
    PollWake(wake);
    auto start = Clock::now();
    check(PollSleepOrWake(wake, std::chrono::seconds(30)) && Clock::now() - start < std::chrono::seconds(10), "poll wake: a wake signalled before the sleep did not end it");
    std::thread waker([wake] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        PollWake(wake);
    });
    start = Clock::now();
    const bool slept = PollSleepOrWake(wake, std::chrono::seconds(30));
    const auto woken = Clock::now() - start;
    waker.join();
    check(slept && woken < std::chrono::seconds(10), "poll wake: a wake signalled during the sleep did not end it");
    start = Clock::now();
    check(PollSleepOrWake(wake, std::chrono::milliseconds(2)) && Clock::now() - start >= std::chrono::milliseconds(1), "poll wake: the timer did not hold an unwoken sleep");
#else
    check(wake == nullptr && !PollSleepOrWake(wake, std::chrono::milliseconds(1)), "poll wake: a wake event outside Windows");
#endif
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
    std::uint64_t flat = 0;
    std::uint64_t deferred = 0;
    auto walked = old;
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == 0, "fast dispatch verify: equal results differ");
    // The T# streaming-feedback fields (word 5 bit 25, word 6 bits 0-7) are no difference.
    walked.bindings[1].guestDescriptor[5] ^= 1u << 25u;
    walked.bindings[1].guestDescriptor[6] ^= 0x3u;
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == 0 && feedback == 1, "fast dispatch verify: T# feedback bits differ");
    walked.bindings[1].guestDescriptor[6] ^= 0x100u;
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Image) && first.binding == 1 && first.word == 6, "fast dispatch verify: a T# word outside the feedback bits is no image mismatch");
    // A flat copy of a sampled T# word that differs only in its don't-care bits (FlatTsharpFeedbackCopy):
    // excused and counted; outside those bits it is a flat mismatch.
    auto copying = old;
    copying.bindings[2].guestDescriptor[0] = 5;
    walked = copying;
    walked.bindings[1].guestDescriptor[5] ^= 1u << 25u;
    walked.bindings[2].guestDescriptor[0] ^= 1u << 25u;
    feedback = 0;
    check(CompareWalkedResults(copying, walked, first, feedback, flat, deferred) == 0 && feedback == 1 && flat == 1, "fast dispatch verify: a flat T# copy differing in the feedback bits differs");
    check(CompareWalkedResult(copying, walked, true) == 0, "fast draw verify: a flat T# copy differing in the feedback bits differs");
    walked.bindings[2].guestDescriptor[0] ^= 1u << 24u;
    check(CompareWalkedResults(copying, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Flat) && flat == 1 && first.binding == 2 && first.word == 0, "fast dispatch verify: a flat word beyond the feedback bits is no flat mismatch");
    // A flat word the capture left to the GPU holds a placeholder: skipped, counted.
    walked = old;
    auto deferring = old;
    deferring.bindings[2].deferredWords.push_back({1, 0x10000});
    walked.bindings[2].guestDescriptor[1] = 5;
    check(CompareWalkedResults(deferring, walked, first, feedback, flat, deferred) == 0 && deferred == 1, "fast dispatch verify: a deferred flat word differs");
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Flat), "fast dispatch verify: a flat word is no flat mismatch");
    // The fast dispatch binds the walked word: a deferred word differs even where the walk read the
    // placeholder, as its own kind (not a wrong flat word), and nothing is skipped.
    deferred = 0;
    check(CompareWalkedResults(deferring, deferring, first, feedback, flat, deferred, true) == mismatch(WalkMismatch::Deferred) && first.kind == WalkMismatch::Deferred && first.binding == 2 && first.word == 1 && deferred == 0, "fast dispatch verify: a deferred word is no mismatch for the fast dispatch");
    // The fast draw's verify (F3b, strictDeferred): walked results carry no deferred words of
    // their own (WalkResources claims no pure leaf), and the binding the old capture deferred
    // differs whatever the walk read; without the strict mode the deferred word is skipped.
    check(CompareWalkedResult(deferring, old, true) == mismatch(WalkMismatch::Deferred), "fast draw verify: a binding the old capture deferred is no mismatch");
    check(CompareWalkedResult(deferring, walked, true) == mismatch(WalkMismatch::Deferred), "fast draw verify: a deferred binding the walk read differently is no mismatch");
    check(CompareWalkedResult(deferring, walked, false) == 0, "fast walk compare: a deferred flat word differs without the strict mode");
    check(CompareWalkedResult(old, deferring, true) == 0, "fast draw verify: deferred words on the walked side alone differ");
    check(std::string(FastWalkMismatchNames()[static_cast<std::size_t>(WalkMismatch::Deferred)]) == "deferred", "fast walk: the deferred mismatch kind is not named");
    for (const auto* name : WalkMismatchNames) check(name != nullptr && *name != '\0', "fast walk: a mismatch kind without a name");
    walked = old;
    walked.bindings[0].guestDescriptor[0] = 0x100;
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Buffer) && first.kind == WalkMismatch::Buffer && first.binding == 0 && first.word == 0 && first.old == 1 && first.walked == 0x100, "fast dispatch verify: a moved V# is no buffer mismatch");
    walked = old;
    walked.variantId = 8;
    check((CompareWalkedResults(old, walked, first, feedback, flat, deferred) & mismatch(WalkMismatch::Variant)) != 0, "fast dispatch verify: another variant is no variant mismatch");
    walked = old;
    walked.pushConstants[3] = std::byte{1};
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Push) && first.word == 3, "fast dispatch verify: a push constant byte is no push mismatch");
    walked = old;
    walked.bindings.pop_back();
    check(CompareWalkedResults(old, walked, first, feedback, flat, deferred) == mismatch(WalkMismatch::Layout), "fast dispatch verify: a missing binding is no layout mismatch");

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

// The fast paths' direct reader (FastSrtRead, shared by the F2 shadow walk, the fast draw and the
// fast dispatch): a draw program's registered region is served first, then the reader's own
// regions (a dispatch's code and header); a word a region holds only partly, or a misaligned one,
// declines Boundary; the null page reads zero; a deferred label of this thread over the word
// declines QueuedLabel; anything else is a plain load of a readable page.
void testFastReader() {
    using namespace AgcDriver::DriverDetail;
    const std::array<std::uint32_t, 4> programWords{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    const std::array<std::uint32_t, 4> regionWords{0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu};
    // Guest addresses the regions serve: never dereferenced.
    constexpr std::uint64_t base = 0x7f0000000000ull;
    std::vector<DrawProgram> programs(1);
    programs[0].memory[0] = {base, std::as_bytes(std::span(programWords))};
    const std::array<ShaderRecompiler::MemoryRegion, 1> regions{{{base + 8, std::as_bytes(std::span(regionWords))}}};
    const std::vector<DeferredLabel> noLabels;
    const auto read = [&](FastReader& reader, std::uint64_t address, std::uint32_t& value) {
        reader.labels = &noLabels;
        value = 0xdeadbeefu;
        return FastSrtRead(&reader, address, &value);
    };
    std::uint32_t value = 0;
    {
        FastReader reader{programs, regions};
        check(read(reader, base + 4, value) && value == 0x22222222u && !reader.declined, "fast reader: a program word is not served from its region");
        check(read(reader, base + 8, value) && value == 0x33333333u, "fast reader: the reader's own region won over a program's");
        check(read(reader, base + 20, value) && value == 0xddddddddu && !reader.declined, "fast reader: a word only the reader's region holds is not served");
        check(read(reader, 0x100, value) && value == 0, "fast reader: the null page does not read zero");
        check(reader.reads == 4, "fast reader: reads are not counted");
        check(!read(reader, base + 2, value) && reader.declined == WalkDecline::Boundary, "fast reader: a misaligned word did not decline");
    }
    {
        // A region of six bytes holds the word at +4 only partly: Boundary, also when another
        // region would hold it.
        const std::array<ShaderRecompiler::MemoryRegion, 1> wide{{{base, std::as_bytes(std::span(regionWords))}}};
        FastReader reader{programs, wide};
        programs[0].memory[0].bytes = programs[0].memory[0].bytes.first(6);
        check(!read(reader, base + 4, value) && reader.declined == WalkDecline::Boundary, "fast reader: a word a program region holds partly did not decline");
        programs[0].memory[0].bytes = std::as_bytes(std::span(programWords));
        const std::array<ShaderRecompiler::MemoryRegion, 1> partial{{{base + 0x100, std::as_bytes(std::span(regionWords)).first(6)}}};
        FastReader own{{}, partial};
        check(!read(own, base + 0x104, value) && own.declined == WalkDecline::Boundary, "fast reader: a word the reader's region holds partly did not decline");
    }
    {
        // A label an earlier packet of this thread deferred (written only when its packet records
        // its labels): the old path captures again, the reader declines.
        const std::vector<DeferredLabel> labels{{base + 0x100002, 4, {}}};
        FastReader reader{{}, {}};
        value = 0;
        reader.labels = &labels;
        check(!FastSrtRead(&reader, base + 0x100000, &value) && reader.declined == WalkDecline::QueuedLabel, "fast reader: a word under a deferred label did not decline");
        FastReader unlabeled{programs, {}};
        unlabeled.labels = nullptr;
        check(FastSrtRead(&unlabeled, base, &value) && value == 0x11111111u, "fast reader: a reader without a label list did not read");
    }
    {
        // A readable host page: a plain load of the live word, the page then known readable.
        const std::vector<std::uint32_t> host{0x01020304u, 0x05060708u};
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(host.data()));
        FastReader reader{{}, {}};
        check(read(reader, address, value) && value == 0x01020304u && !reader.declined, "fast reader: a readable host word was not loaded");
        check(reader.page == (address & ~std::uint64_t{0xfff}), "fast reader: the readable page is not remembered");
    }
}

// Queued-label words in the fast reader (FastKnownLabels, s53-known-values): a word this thread's
// deferred labels write is served with what the old path reads there once it recorded them and
// waited (the labels stored over memory in queue order, so a later label over an earlier one
// wins); a word the last such label covers only partly declines QueuedLabel, as every label word
// does with either switch off. A word without a label is the plain load.
void testFastReaderLabels() {
    using namespace AgcDriver::DriverDetail;
    std::vector<std::uint32_t> host(8, 0x11111111u);
    for (std::size_t i = 0; i < host.size(); ++i) host[i] = 0x11111111u * static_cast<std::uint32_t>(i + 1);
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(host.data()));
    const auto label = [](std::uint64_t address, std::initializer_list<std::uint32_t> words) {
        DeferredLabel made{address, words.size() * 4, {}};
        std::size_t at = 0;
        for (const auto word : words) {
            std::memcpy(made.bytes.data() + at, &word, 4);
            at += 4;
        }
        return made;
    };
    // Word 0 by one label; words 2-3 by an 8-byte label, then word 2 again by a later one; words 5
    // and 6 each half covered by a label at word 5 + 2 bytes; word 7 by one label; words 1 and 4
    // by none.
    std::vector<DeferredLabel> labels;
    labels.push_back(label(base, {0xa0a0a0a0u}));
    labels.push_back(label(base + 8, {0xb0b0b0b0u, 0xc0c0c0c0u}));
    labels.push_back(label(base + 8, {0xd0d0d0d0u}));
    labels.push_back(label(base + 22, {0xe0e0e0e0u}));
    labels.push_back(label(base + 28, {0xf0f0f0f0u}));
    // The old path's words: the labels recorded over memory in queue order, then read.
    auto recorded = host;
    for (const auto& made : labels) std::memcpy(reinterpret_cast<std::byte*>(recorded.data()) + (made.address - base), made.bytes.data(), made.size);
    const auto readWord = [&](FastReader& reader, std::size_t word, std::uint32_t& value) {
        reader.labels = &labels;
        reader.declined.reset();
        value = 0xdeadbeefu;
        return FastSrtRead(&reader, base + word * 4, &value);
    };
    std::uint32_t value = 0;
    {
        FastReader reader{{}, {}};
        reader.knownValues = true;
        reader.knownLabels = true;
        for (std::size_t word = 0; word < host.size(); ++word) {
            const bool partial = word == 5 || word == 6;
            const bool served = readWord(reader, word, value);
            if (partial) {
                check(!served && reader.declined == WalkDecline::QueuedLabel, "fast reader labels: a word a label covers only partly did not decline");
                continue;
            }
            check(served && !reader.declined, "fast reader labels: a label word or a plain word declined");
            check(value == recorded[word], "fast reader labels: a served word differs from the old path's word after the labels landed");
        }
        check(value == recorded[7] && recorded[7] == 0xf0f0f0f0u && recorded[2] == 0xd0d0d0d0u && recorded[3] == 0xc0c0c0c0u && recorded[4] == host[4], "fast reader labels: the test's label layout is not the one described");
        check(reader.servedLabels == 4, "fast reader labels: the served label words are not counted");
    }
    {
        FastReader reader{{}, {}};
        reader.knownValues = true;
        reader.knownLabels = false;
        check(!readWord(reader, 0, value) && reader.declined == WalkDecline::QueuedLabel, "fast reader labels: a label word was served with APS5_FAST_KNOWN_LABELS=0");
        check(readWord(reader, 4, value) && value == host[4], "fast reader labels: a word without a label declined");
        FastReader off{{}, {}};
        off.knownValues = false;
        off.knownLabels = true;
        check(!readWord(off, 2, value) && off.declined == WalkDecline::QueuedLabel, "fast reader labels: a label word was served with APS5_FAST_KNOWN_VALUES=0");
    }
    {
        // The served-word log (the verify modes'), enabled by its scope only.
        FastReader reader{{}, {}};
        reader.knownValues = true;
        reader.knownLabels = true;
        {
            const FastServedLogScope scope(true);
            check(readWord(reader, 0, value) && readWord(reader, 4, value), "fast reader labels: a logged read declined");
            const auto& words = FastServedWords().words;
            check(words.size() == 1 && words[0].address == base && words[0].value == recorded[0] && words[0].source == FastServedSource::Label, "fast reader labels: the served label word was not logged alone");
        }
        check(!FastServedWords().enabled, "fast reader labels: the log stayed enabled after its scope");
        FastServedWords().words.clear();
        check(readWord(reader, 0, value) && FastServedWords().words.empty(), "fast reader labels: a word was logged without a scope");
        // The verify's compare with the old capture's reads: the old words (the labels landed)
        // agree, the live memory (before the record) does not; a word the capture did not read.
        const std::array<FastServedWord, 2> served{{{base, recorded[0], FastServedSource::Label}, {base + 0x100000, 1, FastServedSource::Known}}};
        const std::array<ShaderRecompiler::MemoryRegion, 1> landed{{{base, std::as_bytes(std::span(recorded))}}};
        const std::array<ShaderRecompiler::MemoryRegion, 1> before{{{base, std::as_bytes(std::span(host))}}};
        std::uint64_t unread = 0;
        check(CompareServedWords(served, landed, unread) == 0 && unread == 1, "fast reader labels: the served words differ from the old capture's");
        unread = 0;
        check(CompareServedWords(served, before, unread) == 1 && unread == 1, "fast reader labels: a served word unlike the old capture's was not counted");
    }
}

// The fast reader's newest-writer memo (NewestWriterMemos, Driver::fastPendingWord): over a ring
// of written buffers that grows, overflows (its oldest entries leave) and gets entries beside,
// inside and over a remembered writer, every lookup gives the plain scan's answer (the newest entry
// overlapping the range), and the memo answers a later word of a writer only while no newer entry
// overlaps that writer's range.
void testNewestWriterMemo() {
    using namespace AgcDriver::DriverDetail;
    constexpr std::size_t Capacity = 64;
    std::deque<WrittenBuffer> ring;
    std::uint64_t pushes = 0;
    const auto push = [&](std::uint64_t begin, std::uint64_t end) {
        ++pushes;
        ring.push_back({1, begin, end, pushes, 0, false});
        while (ring.size() > Capacity) ring.pop_front();
    };
    const auto scan = [&](std::uint64_t begin, std::uint64_t end) -> std::optional<WrittenBuffer> {
        for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
            if (begin < it->end && it->begin < end) return *it;
        }
        return std::nullopt;
    };
    const auto same = [](const std::optional<WrittenBuffer>& left, const std::optional<WrittenBuffer>& right) {
        if (left.has_value() != right.has_value()) return false;
        return !left || (left->serial == right->serial && left->begin == right->begin && left->end == right->end);
    };
    NewestWriterMemos memos;
    check(!memos.Lookup(ring, pushes, 0x1000, 0x1004) && memos.last == nullptr, "writer memo: an empty ring answered");
    push(0x1000, 0x1100);
    check(same(memos.Lookup(ring, pushes, 0x1010, 0x1014), scan(0x1010, 0x1014)) && memos.last != nullptr && memos.last->wholeRange, "writer memo: a lone writer is not remembered for its range");
    // A newer writer beside it leaves the memo holding (checked at the new push count).
    push(0x2000, 0x2100);
    const auto beside = memos.Lookup(ring, pushes, 0x1020, 0x1024);
    check(same(beside, scan(0x1020, 0x1024)) && beside->serial == 1 && memos.last->wholeRange && memos.last->checked == pushes, "writer memo: a writer beside the remembered one ended the memo");
    // One inside its range: a word the newer one does not cover still has the old writer as its
    // newest, but the old writer no longer answers for its whole range.
    push(0x1080, 0x1090);
    check(same(memos.Lookup(ring, pushes, 0x1020, 0x1024), scan(0x1020, 0x1024)) && !memos.last->wholeRange, "writer memo: a writer inside the remembered range was missed");
    check(same(memos.Lookup(ring, pushes, 0x1084, 0x1088), scan(0x1084, 0x1088)) && memos.last->writer.serial == 3, "writer memo: the newer writer inside the range was not the answer");
    // The writer leaves the ring: no answer for its words.
    for (std::size_t i = 0; i < Capacity; ++i) push(0x100000 + i * 0x100, 0x100000 + i * 0x100 + 0x80);
    check(!memos.Lookup(ring, pushes, 0x1010, 0x1014) && !scan(0x1010, 0x1014), "writer memo: a writer that left the ring answered");
    // Interleaved pushes and lookups over a small space: always the scan's answer.
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 33u;
    };
    std::uint64_t remembered = 0;
    for (int step = 0; step < 50000; ++step) {
        const auto r = next();
        if (r % 4 == 0) {
            const auto begin = 0x10000 + (next() % 0x400) * 4;
            push(begin, begin + 4 + (next() % 0x40) * 4);
            continue;
        }
        const auto word = 0x10000 + (next() % 0x480) * 4;
        check(same(memos.Lookup(ring, pushes, word, word + 4), scan(word, word + 4)), "writer memo: a lookup differs from the plain scan");
        if (memos.last != nullptr && memos.last->wholeRange) ++remembered;
    }
    check(remembered != 0, "writer memo: no interleaved lookup left a writer remembered for its range");
}

// The fast draw's import memo (HostImportMemo, s53-fast-cost-b step 2c): an entry answers a range
// inside its import only under the device, registry generation and import epoch it was noted
// under; a retire (the epoch), a registry change (the generation) or another device makes it miss,
// and a fifth import replaces the oldest of four.
void testHostImportMemo() {
    using namespace AgcDriver::Graphics;
    const auto device = reinterpret_cast<VkDevice>(std::uintptr_t{0x1000});
    const auto otherDevice = reinterpret_cast<VkDevice>(std::uintptr_t{0x2000});
    const HostImport import{0x10000, 0x10000, VK_NULL_HANDLE, VK_NULL_HANDLE, 0};
    HostImportMemo memo;
    check(memo.Find(device, 5, 7, 0x10000, 4) == nullptr, "import memo: an empty memo answered");
    memo.Note(device, 5, 7, import);
    check(memo.Find(device, 5, 7, 0x10000, 0x100) == &import, "import memo: the range at the import's base missed");
    check(memo.Find(device, 5, 7, 0x1ff00, 0x100) == &import, "import memo: the range ending at the import's end missed");
    check(memo.Find(device, 5, 7, 0x10000, 0x10000) == &import, "import memo: the whole import missed");
    check(memo.Find(device, 5, 7, 0x1ff00, 0x101) == nullptr, "import memo: a range past the import's end was answered");
    check(memo.Find(device, 5, 7, 0xff00, 0x200) == nullptr, "import memo: a range before the import's base was answered");
    check(memo.Find(device, 5, 7, std::numeric_limits<std::uint64_t>::max() - 1, 4) == nullptr, "import memo: a range wrapping the address space was answered");
    check(memo.Find(device, 5, 8, 0x10000, 0x100) == nullptr, "import memo: an entry answered after an import retired (epoch changed)");
    check(memo.Find(device, 6, 7, 0x10000, 0x100) == nullptr, "import memo: an entry answered after the registry changed (generation changed)");
    check(memo.Find(otherDevice, 5, 7, 0x10000, 0x100) == nullptr, "import memo: an entry answered for another device");
    // Four more imports noted: the first is replaced.
    std::array<HostImport, 4> others{};
    for (std::size_t i = 0; i < others.size(); ++i) {
        others[i] = {0x100000 * (i + 1), 0x1000, VK_NULL_HANDLE, VK_NULL_HANDLE, 0};
        memo.Note(device, 5, 7, others[i]);
    }
    check(memo.Find(device, 5, 7, 0x10000, 0x100) == nullptr, "import memo: the oldest entry was not replaced");
    for (const auto& other : others) check(memo.Find(device, 5, 7, other.base, 4) == &other, "import memo: a noted import was lost");
    // No import at all without host imports (as HostImportFor).
    Context context{};
    check(HostImportMemoized(context, 0x10000, 4) == nullptr && HostImportFor(context, 0x10000, 4) == nullptr, "import memo: a context without host imports was answered");
}

// The fast pipeline lookup (s53-fast-cost-b step 2d): the key is the store's bytes, built into a
// reused vector; a state field or a stage's variant changes it (a memoized pipeline of the old key
// is not answered), and no variant id leaves it empty (a private pipeline). The memo (KeyedMemo)
// answers the noted key only while the store dropped nothing since (its removal count), for the
// same live device instance and while the object lives; a ninth key replaces the oldest of eight.
void testFastPipelineMemo() {
    using namespace AgcDriver::Graphics;
    Context context{};
    context.device = reinterpret_cast<VkDevice>(std::uintptr_t{0x1000});
    State state{};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.cullMode = VK_CULL_MODE_NONE;
    state.blends.resize(1);
    VertexInputLayout input;
    input.bindings.push_back({0, 16, VK_VERTEX_INPUT_RATE_VERTEX});
    input.attributes.push_back({0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    vertex.variantId = 5;
    fragment.variantId = 6;
    const std::array<CompiledShader, 2> shaders{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}}};
    const std::array<std::uint32_t, 4> layoutKey{0, 7, 1, 1};
    std::vector<std::byte> key;
    FastPipelineKey(context, state, input, 3, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, key);
    check(!key.empty(), "fast pipeline key: the key is empty");
    const auto noted = key;
    std::vector<std::byte> again(500, std::byte{0x77});
    FastPipelineKey(context, state, input, 3, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, again);
    check(again == noted, "fast pipeline key: a key built into a used vector differs");

    // The memo over that key.
    auto pool = std::make_shared<int>(1);
    auto pipeline = std::make_shared<int>(42);
    KeyedMemo<int, 8> memo;
    memo.Note(noted, KeyHash(noted), 3, pool.get(), pool, pipeline);
    check(memo.Find(noted, KeyHash(noted), 3, pool.get()) == pipeline, "fast pipeline memo: the noted key missed");
    // A state change: another key.
    state.cullMode = VK_CULL_MODE_BACK_BIT;
    FastPipelineKey(context, state, input, 3, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, key);
    check(key != noted && memo.Find(key, KeyHash(key), 3, pool.get()) == nullptr, "fast pipeline memo: a state change was answered by the old pipeline");
    state.cullMode = VK_CULL_MODE_NONE;
    // A stage's variant: another key.
    fragment.variantId = 9;
    FastPipelineKey(context, state, input, 3, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, key);
    check(key != noted && memo.Find(key, KeyHash(key), 3, pool.get()) == nullptr, "fast pipeline memo: a new fragment variant was answered by the old pipeline");
    // Another push layout: another key.
    fragment.variantId = 6;
    FastPipelineKey(context, state, input, 4, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, key);
    check(key != noted, "fast pipeline key: the push layout is not in the key");
    // No variant id: empty (the store's private pipeline, never memoized).
    vertex.variantId = 0;
    FastPipelineKey(context, state, input, 3, layoutKey, shaders, VK_IMAGE_LAYOUT_GENERAL, key);
    check(key.empty(), "fast pipeline key: a stage without a variant id has a key");
    vertex.variantId = 5;
    // Equal hashes are not enough: the bytes are compared.
    auto forged = noted;
    forged.back() ^= std::byte{1};
    check(memo.Find(forged, KeyHash(noted), 3, pool.get()) == nullptr, "fast pipeline memo: a key with the noted hash but other bytes was answered");
    // The store dropped a pipeline since (pipeline change): no entry answers.
    check(memo.Find(noted, KeyHash(noted), 4, pool.get()) == nullptr, "fast pipeline memo: an entry answered after the store removed one");
    // Another device instance, or the noted one gone.
    auto otherPool = std::make_shared<int>(2);
    check(memo.Find(noted, KeyHash(noted), 3, otherPool.get()) == nullptr, "fast pipeline memo: an entry answered for another device instance");
    const auto* poolAddress = pool.get();
    pool.reset();
    check(memo.Find(noted, KeyHash(noted), 3, poolAddress) == nullptr, "fast pipeline memo: an entry answered after its device instance died");
    // The pipeline gone: no entry answers.
    auto livePool = std::make_shared<int>(3);
    memo.Note(noted, KeyHash(noted), 3, livePool.get(), livePool, pipeline);
    check(memo.Find(noted, KeyHash(noted), 3, livePool.get()) == pipeline, "fast pipeline memo: a re-noted key missed");
    std::weak_ptr<int> watched = pipeline;
    pipeline.reset();
    check(watched.expired() && memo.Find(noted, KeyHash(noted), 3, livePool.get()) == nullptr, "fast pipeline memo: the memo kept a pipeline alive or answered a dead one");
    // Eight more keys: the oldest goes.
    KeyedMemo<int, 8> ring;
    std::vector<std::shared_ptr<int>> values;
    std::vector<std::vector<std::byte>> keys;
    for (int i = 0; i < 9; ++i) {
        keys.push_back(std::vector<std::byte>(16, static_cast<std::byte>(i)));
        values.push_back(std::make_shared<int>(i));
        ring.Note(keys.back(), KeyHash(keys.back()), 0, nullptr, {}, values.back());
    }
    check(ring.Find(keys[0], KeyHash(keys[0]), 0, nullptr) == nullptr, "fast pipeline memo: the oldest of nine keys was kept");
    for (int i = 1; i < 9; ++i) check(ring.Find(keys[i], KeyHash(keys[i]), 0, nullptr) == values[i], "fast pipeline memo: a recent key was lost");
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
        testPollWake();
        testFastDispatchVerify();
        testFastReader();
        testFastReaderLabels();
        testNewestWriterMemo();
        testHostImportMemo();
        testFastPipelineMemo();
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
