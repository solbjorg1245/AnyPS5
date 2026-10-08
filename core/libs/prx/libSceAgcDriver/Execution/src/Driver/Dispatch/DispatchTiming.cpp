#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"

namespace AgcDriver::DriverDetail {

void Driver::addDriverPhases(DispatchClass which, const PendingDispatchPhases& pending) {
    const auto& ms = pending.ms;
    std::lock_guard lock(driverPhasesMutex);
    auto& totals = driverPhaseTotals[which];
    for (std::size_t i = 0; i < DriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.dispatches;
    if (pending.hit) ++totals.hits;
    if (pending.validated) ++totals.validations;
    auto& path = totals.paths[pending.hit ? 1 : 0][std::min(pending.devicePath, DevicePathCount - 1)];
    ++path.calls;
    path.deviceMs += ms[PhaseDevice];
    for (std::size_t i = 0; i < DeviceSplitRows; ++i) path.ms[i] += pending.deviceSplit[i];
    ++totals.missKinds[std::min<std::size_t>(pending.missKind, 10)];
    const auto now = std::chrono::steady_clock::now();
    if (now - driverPhasesReport < std::chrono::seconds(10)) return;
    driverPhasesReport = now;
    for (std::size_t cls = 0; cls < DispatchClassCount; ++cls) {
        auto& line = driverPhaseTotals[cls];
        if (line.dispatches == 0) continue;
        std::string report;
        double total = 0;
        for (std::size_t i = 0; i < DriverPhaseCount; ++i) {
            char text[64];
            std::snprintf(text, sizeof(text), " %s %.1f", DriverPhaseNames[i], line.ms[i] * 1000 / static_cast<double>(line.dispatches));
            report += text;
            total += line.ms[i];
        }
        const double perValidation = line.validations != 0 ? 1000 / static_cast<double>(line.validations) : 0.0;
        std::fprintf(stderr, "[dispatch] driver phases %s (10 s, %llu dispatches, %llu cache hits), us per dispatch:%s, total %.1f (%.1f ms); validate %.1f us per validation (%llu validations; GPU waits inside it %.1f us per validation apart)\n", DispatchClassNames[cls], static_cast<unsigned long long>(line.dispatches), static_cast<unsigned long long>(line.hits), report.c_str(), total * 1000 / static_cast<double>(line.dispatches), total, line.ms[PhaseValidate] * perValidation, static_cast<unsigned long long>(line.validations), line.ms[PhaseValidateWait] * perValidation);
        // The device call by path, dispatch-cache misses and hits apart: calls, us per call, and
        // the split rows in us per call.
        std::string paths;
        for (std::size_t hit = 0; hit < 2; ++hit) {
            paths += hit == 0 ? " | dispatch-cache misses:" : " | hits:";
            for (std::size_t p = 0; p < DevicePathCount; ++p) {
                const auto& row = line.paths[hit][p];
                if (row.calls == 0) continue;
                const auto perCall = 1000 / static_cast<double>(row.calls);
                char text[96];
                std::snprintf(text, sizeof(text), " %s %llu at %.1f (", DevicePathNames[p], static_cast<unsigned long long>(row.calls), row.deviceMs * perCall);
                paths += text;
                bool first = true;
                for (std::size_t i = 0; i < DeviceSplitRows; ++i) {
                    if (row.ms[i] == 0) continue;
                    std::snprintf(text, sizeof(text), "%s%s %.1f", first ? "" : ", ", DeviceSplitNames[i], row.ms[i] * perCall);
                    paths += text;
                    first = false;
                }
                paths += ");";
            }
        }
        std::fprintf(stderr, "[dispatch] device paths %s (10 s, us per call)%s; builds: read-only-rebased key built before %llu, variant built before %llu, new variant %llu; not reusable: lease %llu (cached space alone %llu, + per-build regions %llu, writable or heap mirror %llu, own lease %llu), copied writes %llu, other completion %llu, other %llu\n", DispatchClassNames[cls], paths.c_str(), static_cast<unsigned long long>(line.missKinds[1]), static_cast<unsigned long long>(line.missKinds[2]), static_cast<unsigned long long>(line.missKinds[3]), static_cast<unsigned long long>(line.missKinds[4] + line.missKinds[8] + line.missKinds[9] + line.missKinds[10]), static_cast<unsigned long long>(line.missKinds[4]), static_cast<unsigned long long>(line.missKinds[8]), static_cast<unsigned long long>(line.missKinds[9]), static_cast<unsigned long long>(line.missKinds[10]), static_cast<unsigned long long>(line.missKinds[5]), static_cast<unsigned long long>(line.missKinds[6]), static_cast<unsigned long long>(line.missKinds[7]));
        line = {};
    }
}

PendingDispatchPhases& Driver::pendingDispatchPhases() {
    static thread_local PendingDispatchPhases pending;
    return pending;
}

std::chrono::steady_clock::time_point& Driver::packetStartedAt() {
    static thread_local std::chrono::steady_clock::time_point started{};
    return started;
}

double DispatchPhaseTiming::Elapsed() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - lap).count();
    lap = now;
    return ms;
}

void DispatchPhaseTiming::Phase(DriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
