#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

namespace AgcDriver::FrameTrace {
namespace {

struct State {
    std::mutex mutex;
    std::atomic<bool> active{false};
    std::FILE* file = nullptr;
    std::string directory;
    std::uint64_t present = 0;
    std::uint64_t next = 0;
    // Command index -> whether every live image is saved after it.
    std::map<std::uint64_t, bool> dumps;
};

// Never destroyed: worker threads may record while the process exits.
State& Trace() {
    static auto* state = new State();
    return *state;
}

}

void AtPresent(const std::string& directory, std::uint64_t present) {
    auto& trace = Trace();
    std::lock_guard lock(trace.mutex);
    if (trace.file != nullptr) {
        std::fprintf(trace.file, "# end at present %llu\n", static_cast<unsigned long long>(present));
        std::fclose(trace.file);
        trace.file = nullptr;
        trace.active.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "[capture] frame trace of present %llu: %llu commands\n", static_cast<unsigned long long>(trace.present), static_cast<unsigned long long>(trace.next));
    }
    const std::filesystem::path root(directory);
    std::error_code error;
    if (!std::filesystem::remove(root / "trace", error)) return;
    trace.directory = directory;
    trace.present = present;
    trace.next = 0;
    trace.dumps.clear();
    if (std::FILE* list = std::fopen((root / "after.txt").string().c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), list) != nullptr) {
            char* end = nullptr;
            const auto index = std::strtoull(line, &end, 10);
            if (end == line) continue;
            trace.dumps[index] = std::strstr(end, "all") != nullptr;
        }
        std::fclose(list);
    }
    trace.file = std::fopen((root / ("p" + std::to_string(present) + "_trace.txt")).string().c_str(), "w");
    trace.active.store(trace.file != nullptr, std::memory_order_relaxed);
}

bool Active() {
    return Trace().active.load(std::memory_order_relaxed);
}

Entry Record(const std::string& line) {
    auto& trace = Trace();
    std::lock_guard lock(trace.mutex);
    Entry entry;
    if (trace.file == nullptr) return entry;
    entry.index = trace.next++;
    std::fprintf(trace.file, "%llu %s\n", static_cast<unsigned long long>(entry.index), line.c_str());
    if (const auto found = trace.dumps.find(entry.index); found != trace.dumps.end()) {
        entry.dump = true;
        entry.all = found->second;
        entry.prefix = (std::filesystem::path(trace.directory) / ("p" + std::to_string(trace.present) + "_c" + std::to_string(entry.index) + "_")).string();
    }
    return entry;
}

}
