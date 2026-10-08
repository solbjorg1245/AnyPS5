#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <windows.h>

// The driver's std::chrono::steady_clock::now. libstdc++-6.dll's calls winpthreads' clock_gettime
// (CLOCK_MONOTONIC), which queries the performance frequency and counter, splits them into a
// timespec and sets errno on the way; the driver reads the clock a few times per packet for its
// phase timings (PacketTimer, DispatchPhaseTiming, TimedAccess, the lookup outcomes), and the
// queue-0 thread spent 4.3% of its samples inside libwinpthread-1.dll (t351 profile), most of it
// under those timing calls. This one reads the counter once with a cached frequency and computes
// exactly winpthreads' value (whole seconds, then the remainder in nanoseconds rounded to nearest),
// so its time points compare with the ones other modules get from libstdc++. Defined here, the
// driver's references bind to it instead of the libstdc++ import (CMakeLists.txt keeps it out of
// the exports). APS5_NO_FAST_CLOCK=1 forwards every call to libstdc++'s, as before.

namespace {

using Now = std::chrono::steady_clock::time_point (*)();

std::atomic<std::int64_t> frequency{0};
Now libstdcxxNow = nullptr;

__attribute__((noinline)) std::int64_t initialize() {
    if (std::getenv("APS5_NO_FAST_CLOCK") != nullptr) {
        if (HMODULE module = GetModuleHandleA("libstdc++-6.dll")) libstdcxxNow = reinterpret_cast<Now>(reinterpret_cast<void*>(GetProcAddress(module, "_ZNSt6chrono3_V212steady_clock3nowEv")));
        if (libstdcxxNow == nullptr) {
            std::fprintf(stderr, "[clock] libstdc++'s steady_clock::now not found\n");
            std::abort();
        }
        std::fprintf(stderr, "[clock] driver steady_clock: libstdc++'s (APS5_NO_FAST_CLOCK)\n");
        frequency.store(-1, std::memory_order_release);
        return -1;
    }
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    std::fprintf(stderr, "[clock] driver steady_clock: performance counter at %lld Hz\n", static_cast<long long>(value.QuadPart));
    frequency.store(value.QuadPart, std::memory_order_release);
    return value.QuadPart;
}

}

std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
    std::int64_t hz = frequency.load(std::memory_order_acquire);
    if (hz == 0) [[unlikely]] hz = initialize();
    if (hz < 0) [[unlikely]] return libstdcxxNow();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    std::int64_t seconds = counter.QuadPart / hz;
    std::int64_t nanoseconds = ((counter.QuadPart % hz) * 1000000000 + (hz >> 1)) / hz;
    if (nanoseconds >= 1000000000) {
        ++seconds;
        nanoseconds -= 1000000000;
    }
    return time_point(duration(seconds * 1000000000 + nanoseconds));
}
