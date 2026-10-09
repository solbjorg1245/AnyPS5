#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::DriverDetail {

// APS5_FAST_STATE=1 (WP12 F1, docs/design/draw-fastpath.md 2.2): the draw key, the register-only
// precheck verdict and the decode of a key without a draw-cache entry come from a memo per draw
// state. The state key hashes the StateKeyRegisters registers and the programs' registered shaders;
// a queue whose stateSerial has not moved since its last draw reuses that state without hashing. The
// draw key mixes the state key with the device serial and the live user words, the base key the same
// without the pointer words (DrawPointerRegisters): the split drawRegisterKey makes, other values.
// APS5_FAST_STATE_VERIFY=1 also hashes every draw's state afresh, checks that equal state keys hold
// equal registers and that the keys separate draws as drawRegisterKey's do, and compares the memo's
// decode and precheck verdict with fresh ones ([fastpath] state, under APS5_PROFILE_DRAW).
struct FastStateEntry {
    struct Precheck {
        // Indexed with primitive restart on: the verdict depends on the device's list restart, so
        // precheckRegisters runs per draw.
        bool live = false;
        std::optional<DrawVerdict> verdict;
        std::string rejected;
    };
    // A program's user words in its decode (decodeDraw's prepare and initializeMerged): a merged
    // stage's 8 prefix words with the user-data pointer pair in the first two (pointerBase 0: not
    // merged), then userCount words from the program's userDataBase.
    struct UserSlots {
        std::uint32_t pointerBase = 0;
        bool pointerRequired = false;
        std::size_t userCount = 0;
    };
    struct Decode {
        std::shared_ptr<const DrawDecode> decode;
        std::vector<UserSlots> slots;
    };
    std::uint64_t key = 0;
    std::array<std::uint64_t, DrawProgramRegisters.size()> programs{};
    // By precheckSlot: not indexed, indexed with 16-bit indices, indexed with 32-bit indices.
    std::array<Precheck, 3> prechecks;
    // The first decode under this state, its user words refilled per draw. Under the memo mutex.
    std::shared_ptr<const Decode> decode;
    // APS5_FAST_STATE_VERIFY: the hashed register words, for the collision check.
    std::vector<std::uint64_t> words;
};

namespace {

bool profileDraw() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

// APS5_FAST_STATE_ENTRIES (default 16384): the memo is emptied when it reaches this many states (a
// queue keeps its current one).
std::size_t fastStateEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_FAST_STATE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{16384};
    }();
    return entries;
}

struct FastStateMemo {
    HostMutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<FastStateEntry>> entries;
};

FastStateMemo& fastStateMemo() {
    static FastStateMemo memo;
    return memo;
}

struct FastStateCounters {
    std::atomic<std::uint64_t> lookups{0}, skips{0}, memoHits{0}, created{0}, cleared{0}, prechecksCached{0}, prechecksLive{0}, decodes{0}, templates{0};
    std::atomic<std::uint64_t> verifyDraws{0}, verifyStale{0}, verifyCollisions{0}, verifyMerged{0}, verifyBaseMerged{0}, verifySplit{0}, verifyWords{0}, verifyDecodes{0}, verifyDecodeMismatches{0}, verifyPrechecks{0}, verifyPrecheckMismatches{0};
};

FastStateCounters& fastStateCounters() {
    static FastStateCounters counters;
    return counters;
}

void bump(std::atomic<std::uint64_t>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}

bool reportVerify(std::atomic<std::uint64_t>& reports) {
    return reports.fetch_add(1, std::memory_order_relaxed) < 20;
}

// StateKeyRegisters and StateUserRegisters split DrawKeyRegisters; StateExtraRegisters (read by the
// precheck only) are state registers outside it.
constexpr bool stateTablesSplitDrawKey() {
    StateRegisterMask draw{};
    for (const auto& range : Graphics::DrawKeyRegisters) {
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) draw[static_cast<std::size_t>(range.bank)][offset / 64] |= std::uint64_t{1} << (offset % 64);
    }
    const auto state = MakeStateRegisterMask(StateKeyRegisters);
    const auto user = MakeStateRegisterMask(StateUserRegisters);
    const auto extra = MakeStateRegisterMask(StateExtraRegisters);
    for (std::size_t bank = 0; bank < 3; ++bank) {
        for (std::size_t word = 0; word < 16; ++word) {
            if ((state[bank][word] & user[bank][word]) != 0 || (extra[bank][word] & draw[bank][word]) != 0 || (extra[bank][word] & ~state[bank][word]) != 0) return false;
            if (((state[bank][word] & ~extra[bank][word]) | user[bank][word]) != draw[bank][word]) return false;
        }
    }
    return true;
}
static_assert(stateTablesSplitDrawKey(), "StateKeyRegisters and StateUserRegisters must split Graphics::DrawKeyRegisters");

// The state key: the StateKeyRegisters hash, then each program's registered shader and offset in it
// (as drawRegisterKey mixes them).
std::uint64_t stateKey(const QueueState& queue, const ShaderRegistry& registry, std::array<std::uint64_t, DrawProgramRegisters.size()>& programs) {
    auto key = StateRegisterHash(queue);
    for (std::size_t program = 0; program < DrawProgramRegisters.size(); ++program) {
        const auto* low = queue.shader.Find(DrawProgramRegisters[program]);
        const auto* high = queue.shader.Find(DrawProgramRegisters[program] + 1);
        programs[program] = 0;
        if (low == nullptr || high == nullptr) {
            key = StateMix(key, 0);
            continue;
        }
        const auto address = (static_cast<std::uint64_t>(*low) << 8u) | (static_cast<std::uint64_t>(*high & 0xffu) << 40u);
        programs[program] = address;
        auto it = registry.upper_bound(address);
        if (it == registry.begin()) {
            key = StateMix(key, 1);
            continue;
        }
        --it;
        key = StateMix(StateMix(key, reinterpret_cast<std::uintptr_t>(it->second.get())), address - it->second->codeAddress);
    }
    return key;
}

std::vector<std::uint64_t> stateWords(const QueueState& queue) {
    const std::array<const Registers*, 3> banks{&queue.context, &queue.shader, &queue.userConfig};
    std::vector<std::uint64_t> words;
    for (const auto& range : StateKeyRegisters) {
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) {
            if (const auto* value = banks[range.bank]->Find(offset)) words.push_back((static_cast<std::uint64_t>(range.bank) << 48u) | (static_cast<std::uint64_t>(offset) << 32u) | *value);
        }
    }
    return words;
}

// The DrawKey::words index of a pointer register (DrawPointerRegisters), or -1.
int pointerWord(std::uint32_t offset) {
    static_assert(DrawPointerRegisters == std::array<std::uint32_t, 8>{0x08c, 0x08d, 0x090, 0x091, 0x094, 0x095, 0x00c, 0x00d});
    switch (offset) {
    case 0x08c: return 0;
    case 0x08d: return 1;
    case 0x090: return 2;
    case 0x091: return 3;
    case 0x094: return 4;
    case 0x095: return 5;
    case 0x00c: return 6;
    case 0x00d: return 7;
    default: return -1;
    }
}

std::size_t precheckSlot(bool indexed, std::uint32_t indexSize) {
    return !indexed ? 0 : indexSize == 2 ? 1 : 2;
}

std::vector<FastStateEntry::UserSlots> userSlots(const DrawDecode& decode) {
    using Role = ShaderRecompiler::ProgramRole;
    std::vector<FastStateEntry::UserSlots> slots(decode.programs.size());
    for (std::size_t i = 0; i < decode.programs.size(); ++i) {
        auto& slot = slots[i];
        const auto& program = decode.programs[i];
        if (decode.roles[i] == Role::Hull) {
            slot.pointerBase = 0x102;
            slot.pointerRequired = true;
        } else if (decode.roles[i] == Role::Main && decode.state.stages.path == Graphics::ShaderPath::Geometry) {
            slot.pointerBase = 0x82;
            slot.pointerRequired = program.snapshot->type == 4;
        }
        slot.userCount = program.userData.size() - (slot.pointerBase != 0 ? 8u : 0u);
    }
    return slots;
}

// decodeDraw's user words from the live registers, with its checks.
void refillUserData(const QueueState& queue, const std::vector<FastStateEntry::UserSlots>& slots, std::vector<DrawProgram>& programs) {
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& slot = slots[i];
        auto& words = programs[i].userData;
        const std::size_t prefix = slot.pointerBase != 0 ? 8 : 0;
        for (std::size_t word = 0; word < slot.userCount; ++word) words[prefix + word] = readRegister(queue.shader, programs[i].userDataBase + static_cast<std::uint32_t>(word));
        if (slot.pointerBase == 0) continue;
        words[0] = 0;
        words[1] = 0;
        if (!slot.pointerRequired && !queue.shader.contains(slot.pointerBase) && !queue.shader.contains(slot.pointerBase + 1)) continue;
        const auto low = readRegister(queue.shader, slot.pointerBase);
        const auto high = readRegister(queue.shader, slot.pointerBase + 1);
        const auto address = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
        require(address != 0 || !slot.pointerRequired, "merged shader user-data address is null");
        if (address == 0) continue;
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), 8, 4);
        words[0] = low;
        words[1] = high;
    }
}

}

bool Driver::fastState() {
    static const bool enabled = std::getenv("APS5_FAST_STATE") != nullptr;
    return enabled;
}

bool Driver::fastStateVerify() {
    static const bool verify = fastState() && std::getenv("APS5_FAST_STATE_VERIFY") != nullptr;
    return verify;
}

FastStateEntry& Driver::fastStateEntry(const QueueState& queue, const Submission& submission) {
    auto& counters = fastStateCounters();
    const bool profile = profileDraw();
    if (profile) bump(counters.lookups);
    auto& memo = queue.fastState;
    if (memo.entry != nullptr && memo.serial == queue.stateSerial && memo.registry.get() == submission.shaders.get()) {
        if (profile) bump(counters.skips);
        return *memo.entry;
    }
    std::array<std::uint64_t, DrawProgramRegisters.size()> programs{};
    const auto key = stateKey(queue, *submission.shaders, programs);
    auto& shared = fastStateMemo();
    std::shared_ptr<FastStateEntry> entry;
    {
        std::lock_guard lock(shared.mutex);
        if (const auto found = shared.entries.find(key); found != shared.entries.end()) entry = found->second;
    }
    if (entry != nullptr) {
        if (profile) bump(counters.memoHits);
    } else {
        auto fresh = std::make_shared<FastStateEntry>();
        fresh->key = key;
        fresh->programs = programs;
        const auto* restart = queue.userConfig.Find(0x24b);
        for (std::size_t slot = 0; slot < fresh->prechecks.size(); ++slot) {
            auto& precheck = fresh->prechecks[slot];
            const bool indexed = slot != 0;
            precheck.live = indexed && restart != nullptr && *restart != 0;
            if (!precheck.live) precheck.verdict = precheckRegisters(queue, indexed, slot == 1 ? 2u : 4u, precheck.rejected);
        }
        if (fastStateVerify()) fresh->words = stateWords(queue);
        std::lock_guard lock(shared.mutex);
        if (shared.entries.size() >= fastStateEntries()) {
            shared.entries.clear();
            if (profile) bump(counters.cleared);
        }
        entry = shared.entries.try_emplace(key, std::move(fresh)).first->second;
        if (profile) bump(counters.created);
    }
    memo.serial = queue.stateSerial;
    memo.registry = submission.shaders;
    memo.entry = std::move(entry);
    return *memo.entry;
}

std::optional<DrawVerdict> Driver::fastPrecheckRegisters(const QueueState& queue, const Submission& submission, const Pm4::DrawParameters& drawParameters, std::string& rejected) {
    const auto& entry = fastStateEntry(queue, submission);
    const auto& precheck = entry.prechecks[precheckSlot(drawParameters.indexed, drawParameters.indexSize)];
    auto& counters = fastStateCounters();
    const bool profile = profileDraw();
    if (precheck.live) {
        if (profile) bump(counters.prechecksLive);
        return precheckRegisters(queue, drawParameters.indexed, drawParameters.indexSize, rejected);
    }
    if (profile) bump(counters.prechecksCached);
    if (fastStateVerify()) {
        std::string fresh;
        const auto verdict = precheckRegisters(queue, drawParameters.indexed, drawParameters.indexSize, fresh);
        bump(counters.verifyPrechecks);
        if (verdict != precheck.verdict || (verdict == DrawVerdict::Rejected && fresh != precheck.rejected)) {
            bump(counters.verifyPrecheckMismatches);
            static std::atomic<std::uint64_t> reports{0};
            if (reportVerify(reports)) std::fprintf(stderr, "[fastpath] verify: the state's precheck verdict differs from a fresh one (state 0x%llx: fresh %d '%s', memo %d '%s')\n", static_cast<unsigned long long>(entry.key), verdict ? static_cast<int>(*verdict) : -1, fresh.c_str(), precheck.verdict ? static_cast<int>(*precheck.verdict) : -1, precheck.rejected.c_str());
        }
    }
    if (precheck.verdict == DrawVerdict::Rejected) rejected = precheck.rejected;
    return precheck.verdict;
}

DrawKey Driver::fastDrawKey(const QueueState& queue, const Submission& submission, std::uint64_t deviceSerial) {
    const auto& entry = fastStateEntry(queue, submission);
    DrawKey result;
    result.programs = entry.programs;
    auto key = StateMix(entry.key, deviceSerial);
    auto base = key;
    for (const auto& range : StateUserRegisters) {
        for (std::uint32_t offset = range.first; offset < range.first + range.count; ++offset) {
            const auto* value = queue.shader.Find(offset);
            if (value == nullptr) continue;
            const auto word = (static_cast<std::uint64_t>(offset) << 32u) | *value;
            key = StateMix(key, word);
            const int pointer = pointerWord(offset);
            if (pointer < 0) {
                base = StateMix(base, word);
                continue;
            }
            result.words[static_cast<std::size_t>(pointer)] = *value;
            result.present |= 1u << static_cast<unsigned>(pointer);
        }
    }
    result.key = key;
    result.base = base;
    if (!fastStateVerify()) return result;

    auto& counters = fastStateCounters();
    bump(counters.verifyDraws);
    std::array<std::uint64_t, DrawProgramRegisters.size()> programs{};
    if (stateKey(queue, *submission.shaders, programs) != entry.key || programs != entry.programs) {
        bump(counters.verifyStale);
        static std::atomic<std::uint64_t> reports{0};
        if (reportVerify(reports)) std::fprintf(stderr, "[fastpath] verify: the state key of serial %llu differs from a fresh hash (a state register changed without a serial bump)\n", static_cast<unsigned long long>(queue.stateSerial));
    } else if (stateWords(queue) != entry.words) {
        bump(counters.verifyCollisions);
        static std::atomic<std::uint64_t> reports{0};
        if (reportVerify(reports)) std::fprintf(stderr, "[fastpath] verify: state key 0x%llx holds different registers (hash collision)\n", static_cast<unsigned long long>(entry.key));
    }
    // The keys must separate draws as drawRegisterKey's do: a key standing for two old keys would
    // be a wrong draw-cache hit; an old key split in two only costs hits.
    const auto old = drawRegisterKey(queue, *submission.shaders, deviceSerial);
    if (old.words != result.words || old.present != result.present || old.programs != result.programs) bump(counters.verifyWords);
    {
        static HostMutex mutex;
        static std::unordered_map<std::uint64_t, std::uint64_t> newToOld, oldToNew, baseNewToOld;
        std::lock_guard lock(mutex);
        if (newToOld.size() > (1u << 18u) || oldToNew.size() > (1u << 18u) || baseNewToOld.size() > (1u << 18u)) {
            newToOld.clear();
            oldToNew.clear();
            baseNewToOld.clear();
        }
        if (const auto [it, inserted] = newToOld.try_emplace(result.key, old.key); !inserted && it->second != old.key) {
            bump(counters.verifyMerged);
            static std::atomic<std::uint64_t> reports{0};
            if (reportVerify(reports)) std::fprintf(stderr, "[fastpath] verify: key 0x%llx stands for draw keys 0x%llx and 0x%llx\n", static_cast<unsigned long long>(result.key), static_cast<unsigned long long>(it->second), static_cast<unsigned long long>(old.key));
            it->second = old.key;
        }
        if (const auto [it, inserted] = oldToNew.try_emplace(old.key, result.key); !inserted && it->second != result.key) {
            bump(counters.verifySplit);
            it->second = result.key;
        }
        if (const auto [it, inserted] = baseNewToOld.try_emplace(result.base, old.base); !inserted && it->second != old.base) {
            bump(counters.verifyBaseMerged);
            it->second = old.base;
        }
    }
    std::shared_ptr<const DrawDecode> fresh, memo;
    std::string freshError, memoError;
    try {
        fresh = decodeDraw(queue, submission);
    } catch (const std::exception& error) {
        freshError = error.what();
    }
    try {
        memo = fastDrawDecode(queue, submission);
    } catch (const std::exception& error) {
        memoError = error.what();
    }
    bump(counters.verifyDecodes);
    if (freshError != memoError || (fresh != nullptr && memo != nullptr && !sameDecode(*memo, *fresh))) {
        bump(counters.verifyDecodeMismatches);
        static std::atomic<std::uint64_t> reports{0};
        if (reportVerify(reports)) std::fprintf(stderr, "[fastpath] verify: the state's decode differs from a fresh decode (state 0x%llx, key 0x%llx; fresh '%s', memo '%s')\n", static_cast<unsigned long long>(entry.key), static_cast<unsigned long long>(result.key), freshError.c_str(), memoError.c_str());
    }
    return result;
}

std::shared_ptr<const DrawDecode> Driver::fastDrawDecode(const QueueState& queue, const Submission& submission) {
    return fastDrawDecodeFor(fastStateEntry(queue, submission), queue, submission);
}

std::shared_ptr<const DrawDecode> Driver::fastDrawDecodeFor(FastStateEntry& entry, const QueueState& queue, const Submission& submission) {
    auto& shared = fastStateMemo();
    std::shared_ptr<const FastStateEntry::Decode> memo;
    {
        std::lock_guard lock(shared.mutex);
        memo = entry.decode;
    }
    const bool profile = profileDraw();
    if (memo == nullptr) {
        auto fresh = decodeDraw(queue, submission);
        auto built = std::make_shared<FastStateEntry::Decode>();
        built->slots = userSlots(*fresh);
        built->decode = fresh;
        {
            std::lock_guard lock(shared.mutex);
            if (entry.decode == nullptr) entry.decode = std::move(built);
        }
        if (profile) bump(fastStateCounters().templates);
        return fresh;
    }
    auto decode = std::make_shared<DrawDecode>(*memo->decode);
    refillUserData(queue, memo->slots, decode->programs);
    if (profile) bump(fastStateCounters().decodes);
    return decode;
}

const DrawDecode& Driver::fastDrawDecodeInto(const QueueState& queue, const Submission& submission, DrawDecode& scratch, std::shared_ptr<const DrawDecode>& held) {
    static const bool reuse = std::getenv("APS5_NO_DECODE_REUSE") == nullptr;
    if (!reuse) {
        held = fastDrawDecode(queue, submission);
        return *held;
    }
    auto& entry = fastStateEntry(queue, submission);
    std::shared_ptr<const FastStateEntry::Decode> memo;
    {
        auto& shared = fastStateMemo();
        std::lock_guard lock(shared.mutex);
        memo = entry.decode;
    }
    if (memo == nullptr) {
        // The state's first decode (it becomes the template), for the entry found above: one
        // fastStateEntry lookup per draw, as fastDrawDecode makes.
        held = fastDrawDecodeFor(entry, queue, submission);
        return *held;
    }
    // fastDrawDecode's copy of the template, by copy assignment.
    scratch = *memo->decode;
    refillUserData(queue, memo->slots, scratch.programs);
    if (profileDraw()) bump(fastStateCounters().decodes);
    return scratch;
}

void Driver::reportFastState() {
    auto& counters = fastStateCounters();
    const auto take = [](std::atomic<std::uint64_t>& value) { return value.exchange(0, std::memory_order_relaxed); };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::size_t entries = 0;
    {
        auto& shared = fastStateMemo();
        std::lock_guard lock(shared.mutex);
        entries = shared.entries.size();
    }
    const auto lookups = take(counters.lookups);
    const auto skips = take(counters.skips);
    std::fprintf(stderr, "[fastpath] state (10 s): %llu lookups: %llu serial skips, %llu hashes (%llu memo hits, %llu new states, %llu memo clears), %zu entries; prechecks %llu from the state, %llu live (primitive restart); decodes from the state %llu, %llu first decodes kept\n", count(lookups), count(skips), count(lookups > skips ? lookups - skips : 0), count(take(counters.memoHits)), count(take(counters.created)), count(take(counters.cleared)), entries, count(take(counters.prechecksCached)), count(take(counters.prechecksLive)), count(take(counters.decodes)), count(take(counters.templates)));
    if (!fastStateVerify()) return;
    std::fprintf(stderr, "[fastpath] state verify (10 s): %llu draws; stale state keys (a change without a serial bump) %llu, state key collisions %llu; keys standing for two draw keys %llu (base keys %llu), draw keys split %llu, pointer words or programs differing %llu; decodes %llu compared, %llu differed; prechecks %llu compared, %llu differed\n", count(take(counters.verifyDraws)), count(take(counters.verifyStale)), count(take(counters.verifyCollisions)), count(take(counters.verifyMerged)), count(take(counters.verifyBaseMerged)), count(take(counters.verifySplit)), count(take(counters.verifyWords)), count(take(counters.verifyDecodes)), count(take(counters.verifyDecodeMismatches)), count(take(counters.verifyPrechecks)), count(take(counters.verifyPrecheckMismatches)));
}

}
