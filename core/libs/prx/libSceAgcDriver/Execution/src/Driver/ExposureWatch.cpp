#include "prx/libSceAgcDriver/Execution/include/Driver/ExposureWatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <mutex>
#include <vector>

namespace AgcDriver::ExposureWatch {

namespace {

using Clock = std::chrono::steady_clock;

template <typename TBank>
std::uint32_t reg(const TBank& bank, std::uint32_t offset) {
    const auto it = bank.find(offset);
    return it == bank.end() ? 0u : it->second;
}

std::uint64_t watchedProgram() {
    static const std::uint64_t address = [] {
        const char* value = std::getenv("APS5_EXPOSURE_WATCH_CS");
        return value != nullptr && value[0] != '\0' ? std::strtoull(value, nullptr, 16) : 0x2488a1200ull;
    }();
    return address;
}

float jumpRatio() {
    static const float ratio = [] {
        const char* value = std::getenv("APS5_EXPOSURE_WATCH_RATIO");
        const float parsed = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
        return parsed > 1.0f ? parsed : 1.5f;
    }();
    return ratio;
}

// The last decoded table (the packet loop writes it, the presenter copies it).
struct Watch {
    HostMutex mutex;
    bool decoded = false;
    Table table;
    std::uint64_t tableAddress = 0;
    std::uint32_t queueId = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t unreadable = 0;
};

Watch& watch() {
    static Watch instance;
    return instance;
}

// One present's look at the exposure state.
struct Record {
    std::uint64_t present = 0;
    char stamp[16] = {};
    float exposure = 0, ratio = 0, adaptedEv = 0, targetEv = 0, clampedEv = 0, historyMean = 0, newest = 0;
    std::uint32_t slot = 0;
    ListStats list;
};

void wallStamp(char (&text)[16]) {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    text[0] = '\0';
    if (const auto* local = std::localtime(&seconds)) {
        char clock[12] = {};
        std::strftime(clock, sizeof(clock), "%H:%M:%S", local);
        std::snprintf(text, sizeof(text), "%s.%03d", clock, static_cast<int>(millis));
    }
}

void printRecord(const char* tag, const Record& record) {
    const auto& list = record.list;
    std::fprintf(stderr, "[exposure] %s p%llu %s exp %.4g (x%.3g) ev %.3f target %.3f clamped %.3f mean %.4g ring[%u] %.4g; list %u max %.4g mean %.4g median %.4g window %.4g nan %u inf %u neg %u huge %u descents %u zeros %u nonzero %u..%u (%u in the window)\n", tag, static_cast<unsigned long long>(record.present), record.stamp, record.exposure, record.ratio, record.adaptedEv, record.targetEv, record.clampedEv, record.historyMean, record.slot, record.newest, list.count, list.max, list.mean, list.median, list.windowMean, list.nan, list.inf, list.negative, list.huge, list.descents, list.zeros, list.firstNonZero, list.lastNonZero, list.windowNonZero);
}

// Copies `count` floats `step` bytes apart from guest memory (empty when the range is not mapped).
bool readFloats(std::uint64_t address, std::uint32_t count, std::uint32_t step, std::vector<float>& out) {
    out.clear();
    if (address == 0 || count == 0 || step < 4) return false;
    const auto bytes = static_cast<std::size_t>(count - 1) * step + 4u;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) return false;
    out.resize(count);
    const auto* base = reinterpret_cast<const std::byte*>(address);
    for (std::uint32_t index = 0; index < count; ++index) std::memcpy(&out[index], base + static_cast<std::size_t>(index) * step, sizeof(float));
    return true;
}

// APS5_EXPOSURE_WATCH_DUMP=<dir>: the sorted list and the state as read, <dir>/<present>-<reason>.bin
// (the list's floats, then the 20 state floats), at each jump and each 10 s digest, 60 files at most.
void dumpList(const char* reason, std::uint64_t present, const std::vector<float>& list, const std::vector<float>& state) {
    static const char* folder = std::getenv("APS5_EXPOSURE_WATCH_DUMP");
    static std::uint32_t written = 0;
    if (folder == nullptr || folder[0] == '\0' || written >= 60) return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%llu-%s.bin", folder, static_cast<unsigned long long>(present), reason);
    if (std::FILE* file = std::fopen(path, "wb")) {
        ++written;
        std::fwrite(list.data(), sizeof(float), list.size(), file);
        std::fwrite(state.data(), sizeof(float), state.size(), file);
        std::fclose(file);
    }
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_EXPOSURE_WATCH");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void AtDispatch(const QueueState& queue, std::uint32_t queueId) {
    const auto program = (static_cast<std::uint64_t>(reg(queue.shader, 0x20d) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.shader, 0x20c)) << 8u);
    if (program != watchedProgram()) return;
    // COMPUTE_USER_DATA_0/1 point at the table's address (s_load_dwordx2 s[12:13], s[0:1]).
    const auto pointer = static_cast<std::uint64_t>(reg(queue.shader, 0x240)) | (static_cast<std::uint64_t>(reg(queue.shader, 0x241)) << 32u);
    auto& state = watch();
    std::uint64_t tableAddress = 0;
    if (pointer != 0 && GuestMemory::Accessible(reinterpret_cast<const void*>(pointer), 8)) std::memcpy(&tableAddress, reinterpret_cast<const void*>(pointer), 8);
    if (tableAddress == 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(tableAddress), Table::Bytes)) {
        std::lock_guard lock(state.mutex);
        ++state.unreadable;
        return;
    }
    const auto table = DecodeTable(reinterpret_cast<const std::byte*>(tableAddress));
    std::lock_guard lock(state.mutex);
    if (!state.decoded) std::fprintf(stderr, "[exposure] watching cs 0x%llx q%x: table 0x%llx, state 0x%llx, history 0x%llx (%u), sorted 0x%llx (%u records, stride %u, window %u+%u), key %.4g, EV %.3g..%.3g, speeds %.4g/%.4g, biases %.4g/%.4g\n", static_cast<unsigned long long>(program), queueId, static_cast<unsigned long long>(tableAddress), static_cast<unsigned long long>(table.state.address), static_cast<unsigned long long>(table.history.address), table.historyCount, static_cast<unsigned long long>(table.sorted.address), table.sorted.records, table.sorted.stride, table.sortedOffset, table.sortedCount, table.key, table.minEv, table.maxEv, table.speedDown, table.speedUp, table.targetBias, table.exposureBias);
    state.decoded = true;
    state.table = table;
    state.tableAddress = tableAddress;
    state.queueId = queueId;
    ++state.dispatches;
}

void AtPresent() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    // APS5_EXPOSURE_WATCH=2: a line per present (correlates the scene's luminance with screenshots).
    static const bool everyPresent = [] {
        const char* value = std::getenv("APS5_EXPOSURE_WATCH");
        return value != nullptr && std::strcmp(value, "2") == 0;
    }();
    static std::uint64_t presents = 0;
    static std::array<Record, 6> recent{};
    static std::uint32_t follow = 0;
    static float previous = std::numeric_limits<float>::quiet_NaN();
    // The 10 s window.
    static auto windowStart = Clock::now();
    static std::vector<float> exposures;
    static float evLow = 0, evHigh = 0, targetLow = 0, targetHigh = 0, listMax = 0;
    static std::uint32_t windowNonZeroMax = 0, hugeMax = 0, unsorted = 0, nonFinite = 0, jumps = 0, printed = 0, unread = 0;
    static std::vector<float> values, stateValues, listValues;

    Table table;
    std::uint64_t dispatches = 0, unreadable = 0;
    {
        auto& state = watch();
        std::lock_guard lock(state.mutex);
        if (!state.decoded) return;
        table = state.table;
        dispatches = state.dispatches;
        unreadable = state.unreadable;
    }
    Record record;
    record.present = ++presents;
    wallStamp(record.stamp);
    if (readFloats(table.state.address, 20, 4, stateValues)) {
        record.exposure = stateValues[0];
        record.ratio = stateValues[6];
        record.adaptedEv = stateValues[3];
        record.historyMean = stateValues[8];
        record.clampedEv = stateValues[9];
        record.targetEv = stateValues[10];
    } else {
        ++unread;
        record.exposure = std::numeric_limits<float>::quiet_NaN();
    }
    const auto ringCount = std::min<std::uint32_t>(table.historyCount, 64);
    if (ringCount != 0 && readFloats(table.history.address, ringCount, table.history.Step(4), values)) {
        record.slot = table.historyIndex % ringCount;
        record.newest = values[record.slot];
    }
    if (readFloats(table.sorted.address, table.sorted.Elements(4, 1u << 16u), table.sorted.Step(4), listValues)) record.list = Summarize(listValues, table.sortedOffset, table.sortedCount);

    const float before = previous;
    const bool jumped = presents > 1 && Jumped(before, record.exposure, jumpRatio());
    previous = record.exposure;
    if (jumped) ++jumps;
    if (jumped && printed < 20) {
        ++printed;
        std::fprintf(stderr, "[exposure] jump at p%llu %s: exposure %.4g -> %.4g (EV range %.3g..%.3g, key %.4g, speeds %.4g/%.4g, window %u+%u of %u)\n", static_cast<unsigned long long>(record.present), record.stamp, before, record.exposure, table.minEv, table.maxEv, table.key, table.speedDown, table.speedUp, table.sortedOffset, table.sortedCount, table.sorted.records);
        for (std::uint64_t back = std::min<std::uint64_t>(presents - 1, recent.size()); back != 0; --back) printRecord("before", recent[(presents - 1 - back) % recent.size()]);
        printRecord("now", record);
        follow = static_cast<std::uint32_t>(recent.size());
        dumpList("jump", record.present, listValues, stateValues);
    } else if (follow != 0) {
        --follow;
        printRecord("after", record);
    } else if (everyPresent) {
        printRecord("frame", record);
    }
    recent[(presents - 1) % recent.size()] = record;

    if (std::isfinite(record.exposure)) {
        const bool first = exposures.empty();
        exposures.push_back(record.exposure);
        evLow = first ? record.adaptedEv : std::min(evLow, record.adaptedEv);
        evHigh = first ? record.adaptedEv : std::max(evHigh, record.adaptedEv);
        targetLow = first ? record.targetEv : std::min(targetLow, record.targetEv);
        targetHigh = first ? record.targetEv : std::max(targetHigh, record.targetEv);
    } else {
        ++nonFinite;
    }
    listMax = std::max(listMax, record.list.max);
    hugeMax = std::max(hugeMax, record.list.huge);
    windowNonZeroMax = std::max(windowNonZeroMax, record.list.windowNonZero);
    if (record.list.descents != 0) ++unsorted;
    const auto now = Clock::now();
    if (now - windowStart < std::chrono::seconds(10)) return;
    windowStart = now;
    dumpList("digest", record.present, listValues, stateValues);
    std::sort(exposures.begin(), exposures.end());
    const auto at = [&](double fraction) { return exposures.empty() ? 0.0f : exposures[std::min(exposures.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(exposures.size())))]; };
    if (profile) std::fprintf(stderr, "[exposure] 10 s: %zu presents read, exposure min %.4g p10 %.4g median %.4g max %.4g, adapted EV %.3f..%.3f, target EV %.3f..%.3f (clamp %.3g..%.3g), non-finite %u, unread %u, jumps %u, list max %.4g, huge tiles max %u, unsorted presents %u, window non-zero max %u; %llu dispatches decoded (%llu unreadable tables), state 0x%llx\n", exposures.size(), at(0.0), at(0.1), at(0.5), exposures.empty() ? 0.0f : exposures.back(), evLow, evHigh, targetLow, targetHigh, table.minEv, table.maxEv, nonFinite, unread, jumps, listMax, hugeMax, unsorted, windowNonZeroMax, static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(unreadable), static_cast<unsigned long long>(table.state.address));
    exposures.clear();
    listMax = 0;
    windowNonZeroMax = hugeMax = unsorted = nonFinite = jumps = printed = unread = 0;
}

}
