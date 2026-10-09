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
    bool configured = false;
    Clock::time_point lastPoll {};
    Clock::time_point lastReport {};
    Clock::time_point lastOutOfMemory {};
    Clock::time_point pressureSince {};
    Clock::time_point lastRecycle {};
    bool recyclePending = false;
    bool poolLimited = false;
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

std::uint64_t poolFloorBytes(const State& s) {
    if (s.configured) return s.poolFloor;
    static const std::uint64_t bytes = mibFromEnv("APS5_VRAM_POOL_FLOOR_MIB", 256);
    return bytes;
}

bool recycleEnabled() {
    static const bool enabled = std::getenv("APS5_VRAM_NO_RECYCLE") == nullptr;
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
    ++s.totals.episodes;
}

void endEpisode(State& s, Clock::time_point now) {
    if (!pressure.exchange(false, std::memory_order_acq_rel)) return;
    s.totals.pressureSeconds += std::chrono::duration<double>(now - s.pressureSince).count();
    s.recyclePending = true;
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
    bool restore = false, recycleNow = false;
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
        if (GuardEnabled()) {
            const auto margin = marginBytes(s);
            if (sampled) {
                if (sample.usage + margin > sample.budget) startEpisode(s, now);
                else if (sample.usage + 2 * margin < sample.budget && now - s.lastOutOfMemory > std::chrono::milliseconds(pollInterval(s))) endEpisode(s, now);
            } else if (pressure.load(std::memory_order_acquire) && now - s.lastOutOfMemory > std::chrono::seconds(2)) {
                endEpisode(s, now);
            }
            if (pressure.load(std::memory_order_acquire)) {
                // What exceeds the budget (a refusal without a sample: one margin per poll).
                excess = !sampled ? margin : (sample.usage + margin > sample.budget ? sample.usage + margin - sample.budget : 0);
                floor = poolFloorBytes(s);
                if (excess != 0) s.poolLimited = true;
            } else {
                restore = s.poolLimited;
                s.poolLimited = false;
                if (s.recyclePending && recycleEnabled() && (s.lastRecycle == Clock::time_point{} || now - s.lastRecycle >= std::chrono::seconds(10))) {
                    s.recyclePending = false;
                    s.lastRecycle = now;
                    recycleNow = true;
                }
            }
        }
    }
    std::uint64_t poolTrimmed = 0, residentBytes = 0, residentCopies = 0, recycledBytes = 0;
    if (excess != 0 || restore || recycleNow) {
        auto pool = GetBufferPool(context);
        if (excess != 0) {
            // The pool's idle slots first, then resident copies, least recently used.
            poolTrimmed = pool->TrimDevice(excess, floor);
            if (poolTrimmed < excess) {
                if (const auto trim = residentTrimmer.load(std::memory_order_acquire)) std::tie(residentBytes, residentCopies) = trim(excess - poolTrimmed);
            }
        }
        if (restore) pool->RestoreDeviceLimit();
        if (recycleNow) {
            // Everything made before is recycled as it comes back (Epoch), and what the pool
            // retains now goes at once.
            epoch.fetch_add(1, std::memory_order_acq_rel);
            recycledBytes = pool->ReleaseDevice();
        }
    }
    std::lock_guard lock(s.mutex);
    s.totals.poolTrimmedBytes += poolTrimmed;
    s.totals.residentTrimmedBytes += residentBytes;
    s.totals.residentTrimmedCopies += residentCopies;
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
    if (last.valid && last.usage + s.admitted + bytes + 2 * marginBytes(s) > last.budget) {
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
    if (!GuardEnabled()) return;
    const auto now = Clock::now();
    s.lastOutOfMemory = now;
    startEpisode(s, now);
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
    char text[1024];
    std::snprintf(text, sizeof(text), "[vram] (10 s) device-local heap: usage %.0f MiB of budget %.0f MiB (peak %.0f), host heap %.0f of %.0f MiB%s; guard %s, pressure %s: episodes %llu (%.1f s), refused allocations %llu; trimmed: pool %.0f MiB, resident copies %.0f MiB (%llu); resident copies refused for the budget %llu; recycles %llu (pool %.0f MiB); device allocations made under pressure %llu (%.0f MiB); epoch %llu", mib(c.last.usage), mib(c.last.budget), mib(c.peakUsage), mib(c.last.hostUsage), mib(c.last.hostBudget), c.last.valid ? "" : " (no VK_EXT_memory_budget sample)", GuardEnabled() ? "on" : "off", pressure.load(std::memory_order_relaxed) ? "now" : "no", d(c.episodes, r.episodes), c.pressureSeconds - r.pressureSeconds, d(c.outOfMemory, r.outOfMemory), mib(c.poolTrimmedBytes - r.poolTrimmedBytes), mib(c.residentTrimmedBytes - r.residentTrimmedBytes), d(c.residentTrimmedCopies, r.residentTrimmedCopies), d(c.residentRefused, r.residentRefused), d(c.recycles, r.recycles), mib(c.recycledPoolBytes - r.recycledPoolBytes), d(c.pressuredAllocations, r.pressuredAllocations), mib(c.pressuredBytes - r.pressuredBytes), static_cast<unsigned long long>(epoch.load(std::memory_order_relaxed)));
    s.reported = c;
    return text;
}

void ConfigureForTests(int guard, Sampler sampler, std::uint32_t pollMs, std::uint64_t marginBytes, std::uint64_t poolFloorBytes) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.guardOverride = guard;
    guardCached.store(guard < 0 ? (guardFromEnv() ? 1 : 0) : guard, std::memory_order_relaxed);
    s.sampler = std::move(sampler);
    s.pollMs = pollMs;
    s.margin = marginBytes;
    s.poolFloor = poolFloorBytes;
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
    s.recyclePending = false;
    s.poolLimited = false;
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
