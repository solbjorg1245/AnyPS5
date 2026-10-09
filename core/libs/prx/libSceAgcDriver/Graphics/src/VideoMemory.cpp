#include "prx/libSceAgcDriver/Graphics/include/VideoMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <tuple>

namespace AgcDriver::Graphics::VideoMemory {

namespace {

using Clock = std::chrono::steady_clock;

bool switchOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

std::uint64_t mibFromEnv(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback << 20u;
    return static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10)) << 20u;
}

bool guardFromEnv() {
    const char* value = std::getenv("APS5_VRAM_GUARD");
    if (value != nullptr && *value != '\0') return std::strcmp(value, "0") != 0;
    return switchOn("APS5_RESIDENT_READS");
}

struct State {
    HostMutex mutex;
    int guardOverride = -1;
    Sampler sampler;
    std::uint32_t pollMs = 0;
    std::uint64_t margin = 0;
    std::uint64_t poolFloor = 0;
    std::uint32_t idleMs = 0;
    bool configured = false;
    Clock::time_point lastPoll {};
    Clock::time_point lastReport {};
    Clock::time_point lastOutOfMemory {};
    Clock::time_point pressureSince {};
    Clock::time_point lastRecycle {};
    // Spacing between two recycles: 10 s, doubled (up to 160 s) while they follow each other within
    // a minute, back to 10 s after a quiet minute.
    std::chrono::seconds recycleSpacing {10};
    bool recyclePending = false;
    // The open episode saw usage over the budget or a refused allocation (it recycles at its end).
    bool hard = false;
    // The guard's last trim found nothing left to free (the episode may end in the band).
    bool exhausted = false;
    // Bytes Admits let through since the last sample (not in its usage yet).
    std::uint64_t admitted = 0;
    Counts totals {};
    Counts reported {};
};

State& state() {
    static auto* instance = new State();
    return *instance;
}

std::atomic<bool> pressure{false};
// The resident read cache's trim (GuestBufferMemory registers it with its first copy; targets
// without that cache have none).
std::atomic<ResidentTrimmer> residentTrimmer{nullptr};
std::atomic<std::uint64_t> epoch{0};
// Episodes ended (Rounds).
std::atomic<std::uint64_t> rounds{0};
// Read once from the environment unless a test overrides it (guardOverride).
std::atomic<int> guardCached{-2};

std::uint32_t pollInterval(const State& s) {
    if (s.configured) return s.pollMs;
    static const std::uint32_t ms = [] {
        const char* value = std::getenv("APS5_VRAM_POLL_MS");
        return value != nullptr && *value != '\0' ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 250u;
    }();
    return ms;
}

std::uint64_t marginBytes(const State& s) {
    if (s.configured) return s.margin;
    static const std::uint64_t bytes = mibFromEnv("APS5_VRAM_MARGIN_MIB", 512);
    return bytes;
}

std::uint64_t marginFor(const State& s, std::uint64_t budget) {
    return std::min(marginBytes(s), budget / 16);
}

std::uint64_t poolFloorBytes(const State& s) {
    if (s.configured) return s.poolFloor;
    static const std::uint64_t bytes = mibFromEnv("APS5_VRAM_POOL_FLOOR_MIB", 0);
    return bytes;
}

std::chrono::milliseconds idleTime(const State& s) {
    if (s.configured) return std::chrono::milliseconds(s.idleMs);
    static const std::uint32_t ms = [] {
        const char* value = std::getenv("APS5_VRAM_IDLE_MS");
        return value != nullptr && *value != '\0' ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 1000u;
    }();
    return std::chrono::milliseconds(ms);
}

bool recycleEnabled() {
    static const bool enabled = !switchOn("APS5_VRAM_NO_RECYCLE");
    return enabled;
}

// The device's VK_EXT_memory_budget query: the largest device-local heap and the largest other one.
bool deviceSample(const Context& context, VideoMemorySample& sample) {
    if (!context.memoryBudget || context.memoryProperties2 == nullptr || context.physical == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    context.memoryProperties2(context.physical, &properties);
    const auto& heaps = properties.memoryProperties;
    std::int32_t device = -1, host = -1;
    for (std::uint32_t i = 0; i < heaps.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
        auto& pick = (heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 ? device : host;
        if (pick < 0 || heaps.memoryHeaps[i].size > heaps.memoryHeaps[pick].size) pick = static_cast<std::int32_t>(i);
    }
    if (device < 0 || budget.heapBudget[device] == 0) return false;
    sample.valid = true;
    sample.usage = budget.heapUsage[device];
    sample.budget = budget.heapBudget[device];
    if (host >= 0) {
        sample.hostUsage = budget.heapUsage[host];
        sample.hostBudget = budget.heapBudget[host];
    }
    return true;
}

void startEpisode(State& s, Clock::time_point now) {
    if (pressure.exchange(true, std::memory_order_acq_rel)) return;
    s.pressureSince = now;
    s.hard = false;
    s.exhausted = false;
    ++s.totals.episodes;
}

void markHard(State& s) {
    if (s.hard || !pressure.load(std::memory_order_acquire)) return;
    s.hard = true;
    ++s.totals.hardEpisodes;
}

void endEpisode(State& s, Clock::time_point now) {
    if (!pressure.exchange(false, std::memory_order_acq_rel)) return;
    s.totals.pressureSeconds += std::chrono::duration<double>(now - s.pressureSince).count();
    // Only an episode over the budget can have paged anything out: a soft one (within the margin)
    // recycles nothing.
    if (s.hard && GuardEnabled()) s.recyclePending = true;
    s.hard = false;
    s.exhausted = false;
    rounds.fetch_add(1, std::memory_order_acq_rel);
}

void report(State& s, Clock::time_point now) {
    if (!Reporting()) return;
    if (s.lastReport == Clock::time_point{}) {
        s.lastReport = now;
        return;
    }
    if (now - s.lastReport < std::chrono::seconds(10)) return;
    s.lastReport = now;
    const auto line = Report();
    std::fprintf(stderr, "%s\n", line.c_str());
}

}

bool GuardEnabled() {
    auto cached = guardCached.load(std::memory_order_relaxed);
    if (cached == -2) {
        cached = guardFromEnv() ? 1 : 0;
        guardCached.store(cached, std::memory_order_relaxed);
    }
    return cached == 1;
}

bool Reporting() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr || std::getenv("APS5_PROFILE_GPU") != nullptr;
    return profiled || GuardEnabled();
}

bool Wanted() {
    return GuardEnabled() || Reporting();
}

void Poll(const Context& context) {
    if (!Wanted()) return;
    auto& s = state();
    const auto now = Clock::now();
    // Decided under the mutex, done without it: a trim takes the resident cache's mutex (whose
    // holders call Admits) and destroys Vulkan objects.
    std::uint64_t excess = 0, floor = 0;
    std::chrono::milliseconds idle{0};
    bool recycleNow = false;
    {
        std::lock_guard lock(s.mutex);
        if (s.lastPoll != Clock::time_point{} && now - s.lastPoll < std::chrono::milliseconds(pollInterval(s))) return;
        s.lastPoll = now;
        ++s.totals.polls;
        VideoMemorySample sample;
        const bool sampled = (s.sampler ? s.sampler(context, sample) : deviceSample(context, sample)) && sample.valid;
        if (sampled) {
            s.totals.last = sample;
            s.totals.peakUsage = std::max(s.totals.peakUsage, sample.usage);
            s.admitted = 0;
        }
        // Episodes are observed in both modes (the [vram] line, the attribution counters); only
        // the guard acts on them.
        const auto margin = sampled ? marginFor(s, sample.budget) : marginBytes(s);
        const bool refusedLately = s.lastOutOfMemory != Clock::time_point{} && now - s.lastOutOfMemory <= std::chrono::milliseconds(pollInterval(s));
        if (sampled) {
            if (sample.usage + margin > sample.budget) {
                startEpisode(s, now);
            } else if (pressure.load(std::memory_order_acquire) && !refusedLately) {
                // Under the end threshold, or under the start one with nothing left to free (the
                // game's own memory keeps it in the band; holding the episode would only refuse
                // copies the headroom check refuses anyway).
                if (sample.usage + 2 * margin < sample.budget) {
                    endEpisode(s, now);
                } else if (s.exhausted) {
                    ++s.totals.exhaustedEnds;
                    endEpisode(s, now);
                }
            }
            if (sample.usage > sample.budget) markHard(s);
        } else if (pressure.load(std::memory_order_acquire) && now - s.lastOutOfMemory > std::chrono::seconds(2)) {
            endEpisode(s, now);
        }
        if (GuardEnabled()) {
            if (pressure.load(std::memory_order_acquire)) {
                // Down to the end threshold, so the guard can end what it started (a refusal
                // without a sample: one margin per poll).
                excess = !sampled ? margin : (sample.usage + 2 * margin > sample.budget ? sample.usage + 2 * margin - sample.budget : 0);
                floor = poolFloorBytes(s);
                idle = idleTime(s);
            } else if (s.recyclePending && recycleEnabled() && (s.lastRecycle == Clock::time_point{} || now - s.lastRecycle >= s.recycleSpacing)) {
                // Recycles following each other within a minute back off; a quiet minute resets.
                if (s.lastRecycle != Clock::time_point{} && now - s.lastRecycle < std::chrono::minutes(1)) s.recycleSpacing = std::min<std::chrono::seconds>(2 * s.recycleSpacing, std::chrono::seconds(160));
                else s.recycleSpacing = std::chrono::seconds(10);
                s.recyclePending = false;
                s.lastRecycle = now;
                recycleNow = true;
            }
        }
    }
    std::uint64_t poolTrimmed = 0, recycledBytes = 0;
    ResidentTrim resident;
    if (excess != 0 || recycleNow) {
        auto pool = GetBufferPool(context);
        if (excess != 0) {
            // The pool's idle slots first, then resident copies, least recently used.
            poolTrimmed = pool->TrimDevice(excess, floor, idle);
            if (poolTrimmed < excess) {
                if (const auto trim = residentTrimmer.load(std::memory_order_acquire)) resident = trim(excess - poolTrimmed);
            }
        }
        if (recycleNow) {
            // Everything made before is recycled as it comes back (Epoch), and what the pool
            // retains now goes at once.
            epoch.fetch_add(1, std::memory_order_acq_rel);
            recycledBytes = pool->ReleaseDevice();
        }
    }
    std::lock_guard lock(s.mutex);
    if (excess != 0) s.exhausted = poolTrimmed == 0 && resident.bytes == 0 && resident.heldCopies == 0;
    s.totals.poolTrimmedBytes += poolTrimmed;
    s.totals.residentTrimmedBytes += resident.bytes;
    s.totals.residentTrimmedCopies += resident.copies;
    s.totals.residentHeldBytes += resident.heldBytes;
    s.totals.residentHeldCopies += resident.heldCopies;
    if (recycleNow) {
        ++s.totals.recycles;
        s.totals.recycledPoolBytes += recycledBytes;
    }
    report(s, now);
}

void SetResidentTrimmer(ResidentTrimmer trimmer) {
    residentTrimmer.store(trimmer, std::memory_order_release);
}

bool UnderPressure() {
    return pressure.load(std::memory_order_relaxed);
}

std::uint64_t Rounds() {
    return rounds.load(std::memory_order_acquire);
}

std::uint64_t Margin(std::uint64_t budget) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return marginFor(s, budget);
}

std::uint64_t Epoch() {
    return epoch.load(std::memory_order_relaxed);
}

bool Admits(std::uint64_t bytes) {
    if (!GuardEnabled()) return true;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (pressure.load(std::memory_order_acquire)) {
        ++s.totals.residentRefused;
        return false;
    }
    const auto& last = s.totals.last;
    if (last.valid && last.usage + s.admitted + bytes + 2 * marginFor(s, last.budget) > last.budget) {
        ++s.totals.residentRefused;
        return false;
    }
    s.admitted += bytes;
    return true;
}

void NoteOutOfMemory() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    ++s.totals.outOfMemory;
    // Without sampling nothing would end the episode: only counted then.
    if (!Wanted()) return;
    const auto now = Clock::now();
    s.lastOutOfMemory = now;
    startEpisode(s, now);
    markHard(s);
}

void NoteDeviceAllocation(std::uint64_t bytes) {
    if (!pressure.load(std::memory_order_relaxed)) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    ++s.totals.pressuredAllocations;
    s.totals.pressuredBytes += bytes;
}

Counts Read() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto counts = s.totals;
    if (pressure.load(std::memory_order_acquire)) counts.pressureSeconds += std::chrono::duration<double>(Clock::now() - s.pressureSince).count();
    return counts;
}

std::string Report() {
    auto& s = state();
    // Called by Poll under the mutex or by tests without it: HostMutex is not recursive, so the
    // counts are read without taking it again (relaxed: only reported).
    const auto& c = s.totals;
    const auto& r = s.reported;
    const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; };
    const auto d = [](std::uint64_t now, std::uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char text[1280];
    std::snprintf(text, sizeof(text), "[vram] (10 s) device-local heap: usage %.0f MiB of budget %.0f MiB (peak %.0f, margin %.0f), host heap %.0f of %.0f MiB%s; guard %s, pressure %s: episodes %llu (%.1f s; %llu over the budget or refused, %llu ended with nothing left to free), refused allocations %llu; trimmed: pool %.0f MiB, resident copies %.0f MiB (%llu) and %.0f MiB held by builds (%llu, rebuilt); resident copies refused for the budget %llu; recycles %llu (pool %.0f MiB); device allocations made under pressure %llu (%.0f MiB); epoch %llu", mib(c.last.usage), mib(c.last.budget), mib(c.peakUsage), mib(c.last.valid ? marginFor(s, c.last.budget) : marginBytes(s)), mib(c.last.hostUsage), mib(c.last.hostBudget), c.last.valid ? "" : " (no VK_EXT_memory_budget sample)", GuardEnabled() ? "on" : "off (observed only)", pressure.load(std::memory_order_relaxed) ? "now" : "no", d(c.episodes, r.episodes), c.pressureSeconds - r.pressureSeconds, d(c.hardEpisodes, r.hardEpisodes), d(c.exhaustedEnds, r.exhaustedEnds), d(c.outOfMemory, r.outOfMemory), mib(c.poolTrimmedBytes - r.poolTrimmedBytes), mib(c.residentTrimmedBytes - r.residentTrimmedBytes), d(c.residentTrimmedCopies, r.residentTrimmedCopies), mib(c.residentHeldBytes - r.residentHeldBytes), d(c.residentHeldCopies, r.residentHeldCopies), d(c.residentRefused, r.residentRefused), d(c.recycles, r.recycles), mib(c.recycledPoolBytes - r.recycledPoolBytes), d(c.pressuredAllocations, r.pressuredAllocations), mib(c.pressuredBytes - r.pressuredBytes), static_cast<unsigned long long>(epoch.load(std::memory_order_relaxed)));
    s.reported = c;
    return text;
}

void ConfigureForTests(int guard, Sampler sampler, std::uint32_t pollMs, std::uint64_t marginBytes, std::uint64_t poolFloorBytes, std::uint32_t idleMs) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.guardOverride = guard;
    guardCached.store(guard < 0 ? (guardFromEnv() ? 1 : 0) : guard, std::memory_order_relaxed);
    s.sampler = std::move(sampler);
    s.pollMs = pollMs;
    s.margin = marginBytes;
    s.poolFloor = poolFloorBytes;
    s.idleMs = idleMs;
    s.configured = guard >= 0;
}

void ResetForTests() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    pressure.store(false, std::memory_order_release);
    s.lastPoll = {};
    s.lastReport = {};
    s.lastOutOfMemory = {};
    s.pressureSince = {};
    s.lastRecycle = {};
    s.recycleSpacing = std::chrono::seconds(10);
    s.recyclePending = false;
    s.hard = false;
    s.exhausted = false;
    s.admitted = 0;
    s.totals = {};
    s.reported = {};
}

void RecycleNowForTests(const Context& context) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.recyclePending = false;
        s.lastRecycle = Clock::now();
        ++s.totals.recycles;
    }
    epoch.fetch_add(1, std::memory_order_acq_rel);
    const auto bytes = GetBufferPool(context)->ReleaseDevice();
    std::lock_guard lock(s.mutex);
    s.totals.recycledPoolBytes += bytes;
}

}
