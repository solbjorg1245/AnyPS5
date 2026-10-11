// Linux (and any non-MinGW) stand-ins for the MinGW-only host replacements: FastClock.cpp's read
// counter and OperatorNew.cpp's heap report. glibc's clock_gettime and malloc need no replacement,
// so the [packets] and [heap] profile lines read zero clock reads and the plain heap.
// The commit report reads /proc on Linux: Committed_AS of CommitLimit, and VmRSS for the process
// (Linux has no private-commit figure); zeros elsewhere.

#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

std::uint64_t DriverClockReads() {
    return 0;
}

namespace {

// The value of the "<key>:" line of a /proc file, in bytes (the files give kB); 0 when absent.
std::uint64_t procKilobytes(const char* path, const char* key) {
#if defined(__linux__)
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) return 0;
    const auto keyLength = std::strlen(key);
    char line[256];
    std::uint64_t bytes = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, keyLength) != 0 || line[keyLength] != ':') continue;
        unsigned long long kilobytes = 0;
        if (std::sscanf(line + keyLength + 1, "%llu", &kilobytes) == 1) bytes = static_cast<std::uint64_t>(kilobytes) * 1024;
        break;
    }
    std::fclose(file);
    return bytes;
#else
    (void)path;
    (void)key;
    return 0;
#endif
}

}

namespace AgcDriver::HostHeap {

Counters ThreadCounters() {
    return {};
}

const char* ModeName() {
    return "heap";
}

ArenaStatus Arena() {
    return {};
}

CommitStatus Commit() {
    CommitStatus status;
    status.systemBytes = procKilobytes("/proc/meminfo", "Committed_AS");
    status.limitBytes = procKilobytes("/proc/meminfo", "CommitLimit");
    status.privateBytes = procKilobytes("/proc/self/status", "VmRSS");
    return status;
}

}
