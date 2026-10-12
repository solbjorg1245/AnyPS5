#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDPROFILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDPROFILE_HPP

#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

namespace AgcDriver::Graphics {

// APS5_RECORD_FAST (P-RECORD, default on; =0 the old path): the recording thread's largest per-draw
// cost trimmed, nothing else changed: a draw's set copy (ShaderResources::PrepareDrawBindings) comes
// from the open batch's transient pool, sized without a map, with its lists in thread scratch. The
// old path takes the chain pools and frees each draw's set on its own (t812-t818: +4% presents).
inline bool RecordFast() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_RECORD_FAST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

}

namespace AgcDriver::Graphics::RecordProfile {

// APS5_RECORD_PROFILE=1 (P-RECORD, default off): where a recorded draw's time goes inside
// Graphics::Draw, finer than the [draws] phases: one TSC pair and one heap-counter pair per span,
// totalled over all threads (in the steady scene that is the recording pipe's thread B) and printed
// with the [draws] line every 10 s (APS5_PROFILE_DRAW) as "[draws] record profile". A span's time
// includes the spans nested in it only where the names say so ("total" holds everything).
enum Span : std::size_t {
    DrawTotal,
    Inputs,
    TargetRefresh,
    TargetRefreshWalked,
    TargetClear,
    ClearKeys,
    ClearKeysPending,
    ClearKeysSync,
    ClearKeysRead,
    ClearMaterialize,
    TargetView,
    LookupKey,
    LookupFind,
    LookupRebase,
    LookupRevalidate,
    LookupAliases,
    Build,
    IndirectDecide,
    PipelineLookup,
    PipelineFramebuffer,
    StoreFlushes,
    SetSelect,
    SetAllocate,
    SetCopy,
    SetWrite,
    SetKeep,
    DeferredFlat,
    Commands,
    BindPush,
    DrawCommands,
    Keep,
    Recipe,
    Bookkeeping,
    SpanCount
};

constexpr std::array<const char*, SpanCount> SpanNames{"total (Graphics::Draw)", "inputs (validate+vertex)", "target refresh", "target refresh with a page walk", "target clear check", "clear check: keys", "clear check: keys pending", "clear check: keys sync", "clear check: keys read", "clear check: materialize", "target view", "lookup: key", "lookup: find", "lookup: rebase check", "lookup: revalidate", "lookup: aliases", "resources build", "indirect decision", "pipeline lookup", "framebuffer", "store flushes", "set copy: select", "set copy: allocate", "set copy: copy", "set copy: write", "set copy: keep", "deferred flat", "commands (pass, barrier, indirect args)", "bind+push", "draw command and notes", "keep", "recipe", "profile bookkeeping"};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_RECORD_PROFILE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Totals {
    std::array<std::atomic<std::uint64_t>, SpanCount> cycles{};
    std::array<std::atomic<std::uint64_t>, SpanCount> calls{};
    std::array<std::atomic<std::uint64_t>, SpanCount> allocations{};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> fastHits{0};
    // The TSC and the clock at the last report: the window's cycles per microsecond.
    std::uint64_t lastTsc = __rdtsc();
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

inline Totals& Get() {
    static Totals totals;
    return totals;
}

inline void Add(Span span, std::uint64_t cycles, std::uint64_t allocations) {
    auto& totals = Get();
    totals.cycles[span].fetch_add(cycles, std::memory_order_relaxed);
    totals.calls[span].fetch_add(1, std::memory_order_relaxed);
    totals.allocations[span].fetch_add(allocations, std::memory_order_relaxed);
}

// One span: nothing but a flag test while the switch is off.
class Scope {
public:
    explicit Scope(Span span) : span(span), on(Enabled()) {
        if (!on) return;
        heap = HostHeap::ThreadCounters().allocations;
        start = __rdtsc();
    }
    ~Scope() { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    // Ends the span early (once); `as` books it under another span (a walked target refresh).
    void Stop(Span as) {
        span = as;
        Stop();
    }
    void Stop() {
        if (!on) return;
        on = false;
        const auto end = __rdtsc();
        Add(span, end - start, HostHeap::ThreadCounters().allocations - heap);
    }

private:
    Span span;
    bool on;
    std::uint64_t start = 0;
    std::uint64_t heap = 0;
};

inline void NoteDraw() {
    if (Enabled()) Get().draws.fetch_add(1, std::memory_order_relaxed);
}

// Called with the [draws] line (every 10 s): per span us per draw, share of the total span, calls
// and heap allocations per call; then resets the window.
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
    const auto draws = totals.draws.exchange(0, std::memory_order_relaxed);
    const auto fastHits = totals.fastHits.exchange(0, std::memory_order_relaxed);
    if (draws == 0) return;
    const double total = static_cast<double>(cycles[DrawTotal]);
    char line[4096];
    int n = std::snprintf(line, sizeof(line), "[draws] record profile (APS5_RECORD_PROFILE, 10 s): %llu draws, %.0f cycles/us, fast hits %llu; span: us per draw / share of total / calls / allocations per call:", static_cast<unsigned long long>(draws), cyclesPerUs, static_cast<unsigned long long>(fastHits));
    for (std::size_t span = 0; span < SpanCount && n > 0 && static_cast<std::size_t>(n) < sizeof(line); ++span) {
        if (calls[span] == 0) continue;
        const double perDraw = static_cast<double>(cycles[span]) / cyclesPerUs / static_cast<double>(draws);
        n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " %s %.2f / %.1f%% / %llu / %.1f;", SpanNames[span], perDraw, total > 0 ? 100.0 * static_cast<double>(cycles[span]) / total : 0.0, static_cast<unsigned long long>(calls[span]), static_cast<double>(allocations[span]) / static_cast<double>(calls[span]));
    }
    std::fprintf(stderr, "%s\n", line);
}

}

#endif
