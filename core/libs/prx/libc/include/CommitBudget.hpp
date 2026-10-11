#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_COMMITBUDGET_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_COMMITBUDGET_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

// Cache budgets whose defaults were sized on the development machine (65 GB of system commit) are
// scaled down from the machine's commit limit (Windows: RAM plus the current pagefile; Linux: RAM
// plus swap), so a 16-32 GB player machine does not run out of commit: the full default at 48 GiB
// or more, half from 28 GiB, a quarter below. An explicit size in the cache's own APS5_* variable
// always wins; APS5_NO_COMMIT_SCALE=1 keeps the fixed defaults on any machine.
namespace CommitBudget {

constexpr std::uint64_t FullLimitMiB = 48 * 1024;
constexpr std::uint64_t HalfLimitMiB = 28 * 1024;

// The budget for a default of `full` under a commit limit of `limitMiB` (0: unknown, keep `full`).
constexpr std::size_t Scale(std::size_t full, std::uint64_t limitMiB) {
    if (limitMiB == 0 || limitMiB >= FullLimitMiB) return full;
    if (limitMiB >= HalfLimitMiB) return full / 2;
    return full / 4;
}

inline std::uint64_t LimitMiB() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return status.ullTotalPageFile >> 20;
#else
    struct sysinfo info{};
    if (sysinfo(&info) != 0) return 0;
    return (static_cast<std::uint64_t>(info.totalram) + info.totalswap) * info.mem_unit >> 20;
#endif
}

// The commit limit the budgets are scaled from, read once (0 with APS5_NO_COMMIT_SCALE=1).
inline std::uint64_t ScalingLimitMiB() {
    static const std::uint64_t limit = std::getenv("APS5_NO_COMMIT_SCALE") != nullptr ? 0 : LimitMiB();
    return limit;
}

inline std::size_t Scaled(std::size_t full) {
    return Scale(full, ScalingLimitMiB());
}

}

#endif
