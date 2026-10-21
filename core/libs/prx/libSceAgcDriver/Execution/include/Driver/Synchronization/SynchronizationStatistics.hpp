#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATIONSTATISTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATIONSTATISTICS_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>

namespace AgcDriver::DriverDetail {

struct SubmissionCosts {
    std::atomic<std::uint64_t> submissions{0}, validateNs{0}, copyNs{0}, dequeueNs{0}, completeNs{0}, suspendNs{0}, suspends{0}, suspendsSkipped{0}, endSubmits{0}, endSkipped{0}, notifiesSkipped{0};
    // APS5_COMPUTE_END_DEFER: deferred ends the deferring worker sent at the deadline (sendDeferredEnd).
    std::atomic<std::uint64_t> endDeferredSent{0};
};

extern std::atomic<std::uint64_t> gpuLabels, completionLabels, notImportedLabels, noOpLabels, unlockedDrains;

extern std::atomic<std::uint64_t> labelFallbacks[5];

extern std::atomic<std::uint64_t> queuedLabels, labelGroups, immediateLabels;

extern std::atomic<std::uint64_t> packetLockRecords, packetLockSubmits, packetLockDeferred, packetSubmitDeferred, captureTryRecords, captureRetries, suspendPoints;

extern std::atomic<std::uint64_t> recordTriesSkipped;

extern std::atomic<bool> queue0Dormant;

// APS5_COMPUTE_END_DEFER (ComputeEndDefers): queue 0's worker's queued count, set once it runs, and
// whether it runs a submission (set before its dequeue lowers the count, cleared after it executed).
extern std::atomic<const std::atomic<std::uint64_t>*> queue0Queued;
extern std::atomic<bool> queue0Running;

extern std::atomic<std::uint64_t> packetLockRecordUs, packetLockSubmitUs;

enum TrySite { TryLabel = 0, TryFlush, TryReap, TryPoll, TryIdle, TryCapture, TryPollRecord, TrySites };

extern std::atomic<std::uint64_t> triesFailed[TrySites];

extern std::atomic<std::uint64_t> storesOnGpu, storesBehindCompletions, storesOnCpu, storesDrained;

extern std::map<std::uint32_t, std::uint64_t> drainCounts;

extern std::uint64_t drainTotal;

extern std::chrono::steady_clock::time_point lastSyncReport;

extern std::atomic<std::uint64_t> pollLabelRecords, hookLabelRecords;

SubmissionCosts& submissionCosts(std::uint32_t queue);

void reportSync();

void countLabelOutcome(int reason);

}

#endif
