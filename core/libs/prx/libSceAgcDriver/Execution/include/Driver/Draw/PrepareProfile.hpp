#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREPROFILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREPROFILE_HPP

#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

namespace AgcDriver::DriverDetail::PrepareProfile {

// APS5_PREPARE_PROFILE=1 (P-PREPARE, default off): where the queue-0 thread's time goes (thread A of
// the recording pipe), finer than the [draw] phases and the [packets] times: one TSC pair and one
// heap-counter pair per span, queue 0 only, printed with the [pipe] queue 0 line every 10 s
// (APS5_PROFILE_DRAW) as "[pipe] A profile". The "A:" spans split a submission's packet loop by packet
// kind (their sum is less than "A: submission" by the loop's own work); the "draw:" spans split a draw
// packet, and nest only where the names say so ("draw: lookup" holds its compares and patches).
enum Span : std::size_t {
    ASubmission,
    ABoundary,
    ARingFull,
    ADrainWait,
    AFlush,
    AMemory,
    ADraw,
    ADispatch,
    AWait,
    ALabel,
    AOther,
    AEnd,
    DPrecheck,
    DKey,
    DFind,
    DDecode,
    DPrepare,
    DSetup,
    DLookup,
    DCompare,
    DPatch,
    DVertex,
    DStages,
    DCache,
    DTail,
    DRecipe,
    DHandOver,
    DLocked,
    SpanCount
};

constexpr std::array<const char*, SpanCount> SpanNames{"A: submission (execute)", "A: boundary (holds the ring-full and drain waits)", "A: ring-full waits", "A: drain waits (anywhere)", "A: flush between packets", "A: packet memory", "A: draw packets", "A: dispatch packets", "A: wait packets", "A: label packets (RELEASE_MEM, WRITE_DATA)", "A: other packets", "A: submission end", "draw: resolve+precheck", "draw: key build", "draw: cache find", "draw: decode", "draw: program prepare", "draw: scratch setup", "draw: lookup", "draw: lookup: stage compares", "draw: lookup: patched results", "draw: vertex infos", "draw: stage results (capture on a miss)", "draw: cache stages", "draw: reads, overlap check, snapshot", "draw: recipe and rejection", "draw: hand-over", "draw: recorded on A"};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PREPARE_PROFILE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Totals {
    std::array<std::atomic<std::uint64_t>, SpanCount> cycles{};
    std::array<std::atomic<std::uint64_t>, SpanCount> calls{};
    std::array<std::atomic<std::uint64_t>, SpanCount> allocations{};
    std::uint64_t lastTsc = __rdtsc();
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

inline Totals& Get() {
    static Totals totals;
    return totals;
}

// One span of queue 0's thread: nothing but a flag test while the switch is off or on another queue.
class Scope {
public:
    Scope(Span span, bool queue0) : span(span), on(queue0 && Enabled()) {
        if (!on) return;
        heap = HostHeap::ThreadCounters().allocations;
        start = __rdtsc();
    }
    ~Scope() { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Stop() {
        if (!on) return;
        on = false;
        const auto end = __rdtsc();
        auto& totals = Get();
        totals.cycles[span].fetch_add(end - start, std::memory_order_relaxed);
        totals.calls[span].fetch_add(1, std::memory_order_relaxed);
        totals.allocations[span].fetch_add(HostHeap::ThreadCounters().allocations - heap, std::memory_order_relaxed);
    }

private:
    Span span;
    bool on;
    std::uint64_t start = 0;
    std::uint64_t heap = 0;
};

// Called with the [pipe] queue 0 line (every 10 s): per span ms in the window, us per draw packet
// ("A: draw packets" calls), calls per draw packet and heap allocations per call; then resets.
inline void Report() {
    if (!Enabled()) return;
    auto& totals = Get();
    const auto nowTsc = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(now - totals.lastReport).count();
    const double cyclesPerUs = us > 0 ? static_cast<double>(nowTsc - totals.lastTsc) / us : 1.0;
    totals.lastTsc = nowTsc;
    totals.lastReport = now;
    std::array<std::uint64_t, SpanCount> cycles{}, calls{}, allocations{};
    for (std::size_t span = 0; span < SpanCount; ++span) {
        cycles[span] = totals.cycles[span].exchange(0, std::memory_order_relaxed);
        calls[span] = totals.calls[span].exchange(0, std::memory_order_relaxed);
        allocations[span] = totals.allocations[span].exchange(0, std::memory_order_relaxed);
    }
    const auto draws = calls[ADraw];
    if (draws == 0) return;
    char line[6144];
    int n = std::snprintf(line, sizeof(line), "[pipe] A profile (APS5_PREPARE_PROFILE, 10 s): %llu draw packets, %.0f cycles/us, %.0f ms window; span: ms / us per draw packet / calls per draw packet / allocations per call:", static_cast<unsigned long long>(draws), cyclesPerUs, us / 1000.0);
    for (std::size_t span = 0; span < SpanCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++span) {
        if (calls[span] == 0) continue;
        const double ms = static_cast<double>(cycles[span]) / cyclesPerUs / 1000.0;
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.1f / %.2f / %.2f / %.1f;", SpanNames[span], ms, ms * 1000.0 / static_cast<double>(draws), static_cast<double>(calls[span]) / static_cast<double>(draws), static_cast<double>(allocations[span]) / static_cast<double>(calls[span]));
    }
    std::fprintf(stderr, "%s\n", line);
}

}

#endif
