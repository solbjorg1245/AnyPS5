#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

void Driver::reportSkip(const char* kind, const std::string& what) {
    static std::mutex reportedMutex;
    static std::set<std::size_t> reported;
    static std::map<std::string, int> requestDumps;

    static const bool traceSkips = std::getenv("APS5_TRACE_SKIPS") != nullptr;
    static const int dumpLimit = [] { const char* text = std::getenv("APS5_SKIP_REQUEST_DUMPS"); return text ? std::atoi(text) : 8; }();
    static constexpr std::string_view marker = "\nRecompileRequest:\n";
    std::lock_guard lock(reportedMutex);
    std::string line;
    static bool announced = false;
    if (!announced) {
        announced = true;
        char text[160];
        std::snprintf(text, sizeof(text), "[gpu] skip reports: one write each, RecompileRequest dumps capped at %d per reason (APS5_SKIP_REQUEST_DUMPS)\n", dumpLimit);
        line = text;
    }
    const std::string prefix = "[gpu] skipped " + std::string(kind);
    if (reported.insert(std::hash<std::string>{}(what)).second) {
        const auto request = what.find(marker);
        if (request == std::string::npos) {
            line += prefix + ": " + what + "\n";
        } else {
            const auto reason = what.substr(0, request);
            const auto suffix = what.find(' ', request + marker.size());
            auto& dumps = requestDumps[reason];
            if (dumpLimit < 0 || dumps < dumpLimit) {
                ++dumps;
                line += prefix + ": " + what + "\n";
            } else {
                if (dumps == dumpLimit) {
                    ++dumps;
                    line += prefix + ": further RecompileRequest dumps for \"" + reason + "\" are left out\n";
                }
                line += prefix + ": " + reason + " (request left out)" + (suffix == std::string::npos ? std::string() : what.substr(suffix)) + "\n";
            }
        }
    } else if (traceSkips) {
        line += prefix + " again: " + what.substr(0, 100) + "\n";
    }
    // Debug aid: APS5_SKIP_COUNTS=1 prints how many draws and dispatches each reason skipped, every
    // 10 s (the reason up to its packet and target suffix, at most 140 characters).
    static const bool countSkips = std::getenv("APS5_SKIP_COUNTS") != nullptr;
    if (countSkips) {
        static std::map<std::string, std::uint64_t> counts;
        static auto lastReport = std::chrono::steady_clock::now();
        auto key = prefix + ": " + what.substr(0, std::min(what.find(" ["), what.find('\n')));
        if (key.size() > 140) key.resize(140);
        ++counts[key];
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(10)) {
            lastReport = now;
            std::vector<std::pair<std::uint64_t, std::string>> sorted;
            for (const auto& [reason, count] : counts) sorted.emplace_back(count, reason);
            std::sort(sorted.rbegin(), sorted.rend());
            line += "[gpu] skips over 10 s:\n";
            for (const auto& [count, reason] : sorted) line += "  " + std::to_string(count) + " x " + reason + "\n";
            counts.clear();
        }
    }
    if (!line.empty()) std::fwrite(line.data(), 1, line.size(), stderr);
}

std::string Driver::dumpRequest(std::uint64_t address, const ShaderRecompiler::RecompileRequest& request) {
    static std::mutex dumpMutex;
    static std::set<std::uint64_t> dumped;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_%llx.req", static_cast<unsigned long long>(address));
    std::lock_guard lock(dumpMutex);
    if (!dumped.insert(address).second) return name;
    try {
        const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
        if (std::FILE* file = std::fopen(name, "wb")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[gpu] could not serialize request for 0x%llx: %s\n", static_cast<unsigned long long>(address), error.what());
    }
    return name;
}

}
