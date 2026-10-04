#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_INCLUDE_READTRACE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_INCLUDE_READTRACE_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace File {

inline bool ReadTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_READS") != nullptr;
    return enabled;
}

// Paths of open descriptors, kept only while read tracing is on, so a traced read names its file.
inline std::mutex& TracedPathsMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<int, std::string>& TracedPaths() {
    static std::unordered_map<int, std::string> paths;
    return paths;
}

inline void TraceOpen(int fd, const std::string& path) {
    std::lock_guard lock(TracedPathsMutex());
    TracedPaths()[fd] = path;
}

inline std::string TracedPath(int fd) {
    std::lock_guard lock(TracedPathsMutex());
    const auto found = TracedPaths().find(fd);
    return found != TracedPaths().end() ? found->second : "fd " + std::to_string(fd);
}

// Debug aid: APS5_TRACE_READS=<hex address>:<hex bytes> names every file read whose destination
// overlaps that guest range. File reads store into guest memory from the kernel, which hardware
// watchpoints do not see, so this is how a read that lands in live memory is found.
inline void TraceReadInto(const char* api, const char* source, std::int64_t offset, const void* destination, std::uint64_t bytes, const void* caller = nullptr) {
    struct Range {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };
    static const Range range = [] {
        Range parsed;
        const char* text = std::getenv("APS5_TRACE_READS");
        if (text == nullptr) return parsed;
        char* end = nullptr;
        parsed.begin = std::strtoull(text, &end, 16);
        parsed.end = parsed.begin + (*end == ':' ? std::strtoull(end + 1, nullptr, 16) : 1ull);
        return parsed;
    }();
    if (range.begin == range.end) return;
    const auto begin = reinterpret_cast<std::uint64_t>(destination);
    if (begin >= range.end || begin + bytes <= range.begin) return;
    std::fprintf(stderr, "[reads] %s %s offset 0x%llx -> 0x%llx+0x%llx overlaps the traced range\n", api, source, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(begin), static_cast<unsigned long long>(bytes));
}

}

#endif
