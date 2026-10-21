#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CPUTOPOLOGY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CPUTOPOLOGY_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <unistd.h>
#endif

// Hybrid CPU layout (Intel P/E cores) from the OS: which logical processors belong to the lowest and
// the highest efficiency class. Non-hybrid machines report no layout and every pin is a no-op unless
// a mask is given explicitly. Header-only so libkernel and the AGC driver share it without a link.
namespace CpuTopology {

// One physical core: its logical processors (SMT siblings) and its efficiency class.
struct Core {
    std::uint64_t mask = 0;
    unsigned char efficiencyClass = 0;
};

struct Layout {
    std::uint64_t process = 0;
    std::uint64_t efficient = 0;
    std::uint64_t performant = 0;
    bool hybrid = false;
    Core cores[64] = {};
    unsigned coreCount = 0;
};

#ifdef __linux__
inline std::uint64_t MaskFromCpuList(const char* path) {
    std::ifstream file(path);
    std::string text;
    if (!std::getline(file, text)) return 0;
    std::uint64_t mask = 0;
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        char* end = nullptr;
        const unsigned long first = std::strtoul(cursor, &end, 10);
        if (end == cursor) return 0;
        unsigned long last = first;
        cursor = end;
        if (*cursor == '-') {
            last = std::strtoul(cursor + 1, &end, 10);
            if (end == cursor + 1 || last < first) return 0;
            cursor = end;
        }
        for (unsigned long cpu = first; cpu <= last && cpu < 64; ++cpu) mask |= std::uint64_t{1} << cpu;
        if (*cursor == ',') ++cursor;
        else if (*cursor != '\0') return 0;
    }
    return mask;
}
#endif

inline const Layout& Get() {
    static const Layout layout = [] {
        Layout result{};
#ifdef _WIN32
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) result.process = static_cast<std::uint64_t>(processMask);
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) return result;
        std::vector<unsigned char> buffer(length);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) return result;
        std::uint64_t byClass[256] = {};
        bool seen[256] = {};
        for (DWORD offset = 0; offset + 8 <= length;) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (entry->Size == 0 || offset + entry->Size > length) break;
            if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount >= 1 && entry->Processor.GroupMask[0].Group == 0) {
                const unsigned cls = entry->Processor.EfficiencyClass;
                byClass[cls] |= static_cast<std::uint64_t>(entry->Processor.GroupMask[0].Mask);
                seen[cls] = true;
                if (result.coreCount < 64) result.cores[result.coreCount++] = {static_cast<std::uint64_t>(entry->Processor.GroupMask[0].Mask), static_cast<unsigned char>(cls)};
            }
            offset += entry->Size;
        }
        int lowest = -1;
        int highest = -1;
        for (int cls = 0; cls < 256; ++cls) {
            if (!seen[cls]) continue;
            if (lowest < 0) lowest = cls;
            highest = cls;
        }
        if (lowest < 0 || lowest == highest) return result;
        result.efficient = byClass[lowest];
        result.performant = byClass[highest];
        result.hybrid = result.efficient != 0 && result.performant != 0;
#elif defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            for (unsigned cpu = 0; cpu < 64; ++cpu)
                if (CPU_ISSET(cpu, &set)) result.process |= std::uint64_t{1} << cpu;
        }
        result.performant = MaskFromCpuList("/sys/devices/cpu_core/cpus");
        result.efficient = MaskFromCpuList("/sys/devices/cpu_atom/cpus");
        result.hybrid = result.efficient != 0 && result.performant != 0 && (result.efficient & result.performant) == 0;
        std::uint64_t counted = 0;
        for (unsigned cpu = 0; cpu < 64 && result.coreCount < 64; ++cpu) {
            if (((result.process >> cpu) & 1u) == 0 || ((counted >> cpu) & 1u) != 0) continue;
            const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list";
            const auto siblings = MaskFromCpuList(path.c_str());
            if (siblings == 0) continue;
            counted |= siblings;
            result.cores[result.coreCount++] = {siblings, static_cast<unsigned char>(result.hybrid && (siblings & result.performant) != 0 ? 1 : 0)};
        }
#endif
        return result;
    }();
    return layout;
}

// A hex mask from the environment, 0 when unset or empty.
inline std::uint64_t MaskFromEnvironment(const char* name) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return 0;
    return std::strtoull(text, nullptr, 16);
}

inline bool Trace() {
    static const bool trace = std::getenv("APS5_TRACE_AFFINITY") != nullptr;
    return trace;
}

inline unsigned long ThreadId(void* handle) {
#ifdef _WIN32
    return handle != nullptr ? GetThreadId(static_cast<HANDLE>(handle)) : GetCurrentThreadId();
#elif defined(__linux__)
    return handle == nullptr ? static_cast<unsigned long>(gettid()) : 0;
#else
    (void)handle;
    return 0;
#endif
}

// Restricts the thread (`nullptr` is the calling thread) to `mask` clipped to the process affinity;
// returns the mask applied, 0 when nothing was.
inline std::uint64_t Pin(void* handle, std::uint64_t mask) {
#ifdef _WIN32
    const auto& layout = Get();
    if (layout.process != 0) mask &= layout.process;
    if (mask == 0) return 0;
    HANDLE thread = handle != nullptr ? static_cast<HANDLE>(handle) : GetCurrentThread();
    return SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(mask)) != 0 ? mask : 0;
#elif defined(__linux__)
    const auto& layout = Get();
    if (layout.process != 0) mask &= layout.process;
    if (mask == 0 || handle != nullptr) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if ((mask >> cpu) & 1u) CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0 ? mask : 0;
#else
    (void)handle;
    (void)mask;
    return 0;
#endif
}

// Pin plus the APS5_TRACE_AFFINITY=1 line naming the thread.
inline std::uint64_t PinTraced(const char* role, void* handle, std::uint64_t mask) {
    const auto applied = Pin(handle, mask);
    if (Trace()) std::fprintf(stderr, "[affinity] %s tid=%lu mask=0x%llx%s\n", role, ThreadId(handle), static_cast<unsigned long long>(mask), applied != 0 ? "" : " (not applied)");
    return applied;
}

// Helper threads (deferred release, mirror refresh) are pinned only when APS5_HELPER_AFFINITY_MASK
// names their cores.
inline void PinHelperThread(const char* role) {
    static const std::uint64_t mask = MaskFromEnvironment("APS5_HELPER_AFFINITY_MASK");
    if (mask != 0) PinTraced(role, nullptr, mask);
}

// APS5_CORE_SPLIT=<n>: the title's 13 job workers spin at full load on every core, so the driver's
// queue-0 worker and recording pipe share a core with one and pay ~20 % more per draw. The split gives
// the driver's queue workers, recording pipe and presenter the n physical cores (every SMT sibling) of
// the highest efficiency class with the highest processor numbers, and the job workers the other cores
// of the process. Empty when n is 0 or fewer than n + 2 cores would remain.
struct Split {
    std::uint64_t driver = 0;
    std::uint64_t jobs = 0;
};

inline Split SplitCores(const Core* cores, unsigned count, std::uint64_t process, unsigned reserve) {
    Split split{};
    if (process == 0) process = ~std::uint64_t{0};
    std::uint64_t all = 0;
    unsigned usable = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto mask = cores[i].mask & process;
        if (mask == 0 || (all & mask) != 0) continue;
        all |= mask;
        ++usable;
    }
    if (reserve == 0 || usable < reserve + 2) return split;
    for (unsigned picked = 0; picked < reserve; ++picked) {
        int best = -1;
        std::uint64_t bestMask = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto mask = cores[i].mask & process;
            if (mask == 0 || (split.driver & mask) != 0) continue;
            // Core masks are disjoint, so the larger mask holds the higher processor numbers.
            if (best < 0 || cores[i].efficiencyClass > cores[best].efficiencyClass || (cores[i].efficiencyClass == cores[best].efficiencyClass && mask > bestMask)) {
                best = static_cast<int>(i);
                bestMask = mask;
            }
        }
        split.driver |= bestMask;
    }
    split.jobs = all & ~split.driver;
    return split;
}

inline const Split& CoreSplit() {
    static const Split split = [] {
        const char* text = std::getenv("APS5_CORE_SPLIT");
        if (text == nullptr || *text == '\0') return Split{};
        const auto& layout = Get();
        const auto result = SplitCores(layout.cores, layout.coreCount, layout.process, static_cast<unsigned>(std::strtoul(text, nullptr, 10)));
        if (result.driver == 0) std::fprintf(stderr, "[affinity] APS5_CORE_SPLIT=%s not applied (%u cores)\n", text, layout.coreCount);
        return result;
    }();
    return split;
}

}

#endif
