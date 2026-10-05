#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
    // "match <text>" lines: commands whose line contains the text are dumped too (at most
    // MaxMatchDumps a frame), so passes are found although async queues reorder the indices.
    std::vector<std::pair<std::string, bool>> matches;
    std::uint32_t matchDumps = 0;
};

constexpr std::uint32_t MaxMatchDumps = 64;

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
    trace.matches.clear();
    trace.matchDumps = 0;
    if (std::FILE* list = std::fopen((root / "after.txt").string().c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), list) != nullptr) {
            if (std::strncmp(line, "match ", 6) == 0) {
                std::string text(line + 6);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
                const bool all = text.size() > 4 && text.compare(text.size() - 4, 4, " all") == 0;
                if (all) text.resize(text.size() - 4);
                if (!text.empty()) trace.matches.emplace_back(text, all);
                continue;
            }
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
    }
    for (const auto& [text, all] : trace.matches) {
        if (trace.matchDumps >= MaxMatchDumps || line.find(text) == std::string::npos) continue;
        ++trace.matchDumps;
        entry.dump = true;
        entry.all = entry.all || all;
    }
    if (entry.dump) {
        entry.prefix = (std::filesystem::path(trace.directory) / ("p" + std::to_string(trace.present) + "_c" + std::to_string(entry.index) + "_")).string();
    }
    return entry;
}

}
