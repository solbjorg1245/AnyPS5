#include <atomic>
#include <chrono>
#include <cpuid.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

#include <windows.h>

// The driver's std::chrono::steady_clock::now. libstdc++-6.dll's calls winpthreads' clock_gettime
// (CLOCK_MONOTONIC), which queries the performance frequency and counter, splits them into a
// timespec and sets errno on the way; the driver reads the clock a few times per packet for its
// phase timings (PacketTimer, DispatchPhaseTiming, TimedAccess, the lookup outcomes), and the
// queue-0 thread spent 4.3% of its samples inside libwinpthread-1.dll (t351 profile), most of it
// under those timing calls. Defined here, the driver's references bind to it instead of the
// libstdc++ import (CMakeLists.txt keeps it out of the exports).
//
// Two ways to read it. The default reads the TSC (invariant TSC required) and maps it to the
// performance counter's nanoseconds: a 32.32 nanoseconds-per-tick factor calibrated against
// QueryPerformanceCounter from the first call on, and an anchor (TSC, ns) re-taken from the
// performance counter every 50 ms behind a seqlock. The anchor stays continuous (the new one starts
// where the old mapping is) and the factor for the next window is slewed so that the mapping meets
// the performance counter again at its end: time points stay monotonic and within a few hundred
// nanoseconds of the ones other modules get from libstdc++. RtlQueryPerformanceCounter was 7.3% and
// this function 3.1% of the queue-0 thread's samples (t355: ~15.5 ns per QPC read here, ~7 ns per
// rdtsc). APS5_NO_TSC_CLOCK=1 reads the performance counter once per call with a cached frequency
// and computes exactly winpthreads' value (whole seconds, then the remainder in nanoseconds rounded
// to nearest; one multiply when the frequency divides 10^9). APS5_NO_FAST_CLOCK=1 forwards every
// call to libstdc++'s, as before.
//
// Under APS5_PROFILE_DRAW every read counts in a per-thread counter (DriverClockReads), printed
// with the [packets] line of the queue's thread.

namespace {

using Now = std::chrono::steady_clock::time_point (*)();

enum Mode : int { Uninitialized = 0, Tsc, Qpc, Libstdcxx };

constexpr std::int64_t NanosecondsPerSecond = 1000000000;

struct alignas(64) TscClock {
    std::atomic<int> mode{Uninitialized};
    bool counting = false;
    std::atomic<std::uint32_t> sequence{0};
    std::atomic<std::int64_t> anchorTsc{0};
    std::atomic<std::int64_t> anchorNs{0};
    std::atomic<std::uint64_t> factor{0};
    std::atomic<std::int64_t> nextAnchor{0};
};

TscClock tscClock;
std::atomic<bool> anchoring{false};
std::int64_t frequency = 0;
std::int64_t nanosecondsPerCount = 0;
std::int64_t originTsc = 0, originNs = 0, windowTicks = 0;
Now libstdcxxNow = nullptr;

thread_local std::uint64_t reads = 0;

std::int64_t qpcNanoseconds() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (nanosecondsPerCount != 0) return counter.QuadPart * nanosecondsPerCount;
    std::int64_t seconds = counter.QuadPart / frequency;
    std::int64_t nanoseconds = ((counter.QuadPart % frequency) * NanosecondsPerSecond + (frequency >> 1)) / frequency;
    if (nanoseconds >= NanosecondsPerSecond) {
        ++seconds;
        nanoseconds -= NanosecondsPerSecond;
    }
    return seconds * NanosecondsPerSecond + nanoseconds;
}

// One (TSC, performance-counter ns) pair, the TSC taken as the midpoint around the counter read.
void readPair(std::int64_t& tsc, std::int64_t& ns) {
    const std::int64_t before = static_cast<std::int64_t>(__rdtsc());
    ns = qpcNanoseconds();
    const std::int64_t after = static_cast<std::int64_t>(__rdtsc());
    tsc = before + ((after - before) >> 1);
}

std::int64_t project(std::int64_t tsc, std::int64_t baseTsc, std::int64_t baseNs, std::uint64_t perTick) {
    const std::int64_t delta = tsc > baseTsc ? tsc - baseTsc : 0;
    return baseNs + static_cast<std::int64_t>((static_cast<unsigned __int128>(delta) * perTick) >> 32);
}

std::uint64_t ratio(std::int64_t ns, std::int64_t ticks) {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(ns) << 32) / static_cast<unsigned __int128>(ticks));
}

bool invariantTsc() {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007u) return false;
    __get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx);
    return (edx & (1u << 8)) != 0;
}

__attribute__((noinline)) void reanchor(std::int64_t tsc) {
    if (anchoring.exchange(true, std::memory_order_acquire)) return;
    if (tsc >= tscClock.nextAnchor.load(std::memory_order_relaxed)) {
        std::int64_t pairTsc, pairNs;
        readPair(pairTsc, pairNs);
        const std::int64_t baseTsc = tscClock.anchorTsc.load(std::memory_order_relaxed);
        const std::int64_t baseNs = tscClock.anchorNs.load(std::memory_order_relaxed);
        const std::uint64_t perTick = tscClock.factor.load(std::memory_order_relaxed);
        // Continuity: the new anchor is where the current mapping is at the pair's TSC.
        const std::int64_t continued = project(pairTsc, baseTsc, baseNs, perTick);
        // The long-baseline factor, then slewed so the mapping meets the counter one window later.
        const std::uint64_t calibrated = ratio(pairNs - originNs, pairTsc - originTsc);
        const std::int64_t target = pairNs + static_cast<std::int64_t>((static_cast<unsigned __int128>(windowTicks) * calibrated) >> 32);
        std::uint64_t slewed = target > continued ? ratio(target - continued, windowTicks) : calibrated / 2;
        if (slewed < calibrated / 2) slewed = calibrated / 2;
        if (slewed > calibrated + calibrated / 2) slewed = calibrated + calibrated / 2;
        const std::uint32_t sequence = tscClock.sequence.load(std::memory_order_relaxed);
        tscClock.sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        tscClock.anchorTsc.store(pairTsc, std::memory_order_relaxed);
        tscClock.anchorNs.store(continued, std::memory_order_relaxed);
        tscClock.factor.store(slewed, std::memory_order_relaxed);
        tscClock.nextAnchor.store(pairTsc + windowTicks, std::memory_order_relaxed);
        tscClock.sequence.store(sequence + 2, std::memory_order_release);
    }
    anchoring.store(false, std::memory_order_release);
}

__attribute__((noinline)) int initialize() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel)) {
        int mode;
        while ((mode = tscClock.mode.load(std::memory_order_acquire)) == Uninitialized) YieldProcessor();
        return mode;
    }
    tscClock.counting = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (std::getenv("APS5_NO_FAST_CLOCK") != nullptr) {
        if (HMODULE module = GetModuleHandleA("libstdc++-6.dll")) libstdcxxNow = reinterpret_cast<Now>(reinterpret_cast<void*>(GetProcAddress(module, "_ZNSt6chrono3_V212steady_clock3nowEv")));
        if (libstdcxxNow == nullptr) {
            std::fprintf(stderr, "[clock] libstdc++'s steady_clock::now not found\n");
            std::abort();
        }
        std::fprintf(stderr, "[clock] driver steady_clock: libstdc++'s (APS5_NO_FAST_CLOCK)\n");
        tscClock.mode.store(Libstdcxx, std::memory_order_release);
        return Libstdcxx;
    }
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    frequency = value.QuadPart;
    if (NanosecondsPerSecond % frequency == 0) nanosecondsPerCount = NanosecondsPerSecond / frequency;
    if (std::getenv("APS5_NO_TSC_CLOCK") != nullptr || !invariantTsc()) {
        std::fprintf(stderr, "[clock] driver steady_clock: performance counter at %lld Hz%s\n", static_cast<long long>(frequency), std::getenv("APS5_NO_TSC_CLOCK") != nullptr ? " (APS5_NO_TSC_CLOCK)" : " (no invariant TSC)");
        tscClock.mode.store(Qpc, std::memory_order_release);
        return Qpc;
    }
    // A first factor from a 5 ms pairing; reanchor refines it on the baseline from here on.
    readPair(originTsc, originNs);
    std::int64_t tsc, ns;
    do readPair(tsc, ns);
    while (ns - originNs < 5000000);
    const std::uint64_t perTick = ratio(ns - originNs, tsc - originTsc);
    windowTicks = static_cast<std::int64_t>((static_cast<unsigned __int128>(50000000) << 32) / perTick);
    tscClock.anchorTsc.store(tsc, std::memory_order_relaxed);
    tscClock.anchorNs.store(ns, std::memory_order_relaxed);
    tscClock.factor.store(perTick, std::memory_order_relaxed);
    tscClock.nextAnchor.store(tsc + windowTicks, std::memory_order_relaxed);
    std::fprintf(stderr, "[clock] driver steady_clock: TSC at ~%.0f Hz mapped onto the performance counter (%lld Hz), re-anchored every 50 ms\n", static_cast<double>(tsc - originTsc) * 1e9 / static_cast<double>(ns - originNs), static_cast<long long>(frequency));
    tscClock.mode.store(Tsc, std::memory_order_release);
    return Tsc;
}

}

// This thread's clock reads since the last call (counted under APS5_PROFILE_DRAW only).
std::uint64_t DriverClockReads() {
    const std::uint64_t value = reads;
    reads = 0;
    return value;
}

std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
    int mode = tscClock.mode.load(std::memory_order_acquire);
    if (mode == Uninitialized) [[unlikely]] mode = initialize();
    if (tscClock.counting) ++reads;
    if (mode == Tsc) [[likely]] {
        std::uint32_t sequence;
        std::int64_t baseTsc, baseNs, next;
        std::uint64_t perTick;
        do {
            sequence = tscClock.sequence.load(std::memory_order_acquire);
            baseTsc = tscClock.anchorTsc.load(std::memory_order_relaxed);
            baseNs = tscClock.anchorNs.load(std::memory_order_relaxed);
            perTick = tscClock.factor.load(std::memory_order_relaxed);
            next = tscClock.nextAnchor.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
        } while ((sequence & 1u) != 0 || tscClock.sequence.load(std::memory_order_relaxed) != sequence);
        const std::int64_t tsc = static_cast<std::int64_t>(__rdtsc());
        if (tsc >= next) [[unlikely]] reanchor(tsc);
        return time_point(duration(project(tsc, baseTsc, baseNs, perTick)));
    }
    if (mode == Libstdcxx) [[unlikely]] return libstdcxxNow();
    return time_point(duration(qpcNanoseconds()));
}
