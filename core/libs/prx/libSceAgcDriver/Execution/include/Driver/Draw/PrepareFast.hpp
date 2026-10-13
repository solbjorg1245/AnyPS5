#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREFAST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREFAST_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace AgcDriver::DriverDetail::PrepareFast {

// APS5_PREPARE_FAST (P-PREPARE): the queue-0 thread's per-draw prepare without its avoidable heap
// work. A data hit's patched result comes from a small pool per variant (any copy no earlier draw
// still holds) instead of a deep copy whenever the variant's one reused copy is still held by an
// item in the recording pipe's ring; lookupDraw's per-draw vectors (variant ranks, live data words,
// the hit's live words) keep their buffers. =0 (or unset while measured) is the old path.
// APS5_PREPARE_FAST_VERIFY=1 makes a fresh copy beside every pooled one, patches both, and counts
// the copies whose descriptors differ ([pipe] A fast line).
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PREPARE_FAST");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline bool Verify() {
    static const bool verify = Enabled() && std::getenv("APS5_PREPARE_FAST_VERIFY") != nullptr && std::strcmp(std::getenv("APS5_PREPARE_FAST_VERIFY"), "1") == 0;
    return verify;
}

// Copies a variant's pool may hold besides its `patched`: a variant drawn in every slot of the
// 32-item ring needs 33 (each slot keeps its draw's results until A refills it).
inline constexpr std::size_t PoolLimit = 40;

// The first copy of `pool` no one else holds (use count 1: only the pool), or null. A pool entry is
// only ever copied out on this thread, so a count of 1 cannot rise behind the check.
template <typename T>
std::shared_ptr<T> TakeUnheld(const std::vector<std::shared_ptr<T>>& pool) {
    for (const auto& copy : pool) {
        if (copy != nullptr && copy.use_count() == 1) return copy;
    }
    return nullptr;
}

struct Counters {
    std::atomic<std::uint64_t> pooledReused{0}, pooledMade{0}, unpooled{0}, verified{0}, mismatches{0};
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

// With the [pipe] queue 0 line (every 10 s, APS5_PROFILE_DRAW): the pool's work in the window,
// then resets; nothing while the switch is off.
inline void Report() {
    if (!Enabled()) return;
    auto& counters = Get();
    const auto reused = counters.pooledReused.exchange(0, std::memory_order_relaxed);
    const auto made = counters.pooledMade.exchange(0, std::memory_order_relaxed);
    const auto unpooled = counters.unpooled.exchange(0, std::memory_order_relaxed);
    const auto verified = counters.verified.exchange(0, std::memory_order_relaxed);
    const auto mismatches = counters.mismatches.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[pipe] A fast (APS5_PREPARE_FAST, 10 s): patched results from the pool %llu, new pooled copies %llu, copies beyond the pool %llu; verify (APS5_PREPARE_FAST_VERIFY) %llu compared, %llu mismatches\n", static_cast<unsigned long long>(reused), static_cast<unsigned long long>(made), static_cast<unsigned long long>(unpooled), static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches));
}

}

#endif
