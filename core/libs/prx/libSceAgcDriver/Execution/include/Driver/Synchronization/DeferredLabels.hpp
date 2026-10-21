#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AgcDriver::DriverDetail {

inline constexpr std::chrono::microseconds ReapInterval{250};

struct DeferredLabel {
    static constexpr std::size_t Capacity = 32;
    std::uint64_t address;
    std::size_t size;
    std::array<std::byte, Capacity> bytes;
};

struct DeferredLabels {
    std::vector<DeferredLabel> labels;

    std::chrono::steady_clock::time_point since;
};

DeferredLabels& deferredLabels();

bool DeferLabels();

bool QueuedLabelTable();

bool LabelBatchSubmit();

std::size_t& queuedLabelsNoted();

bool NeedsRecordedLabels(std::uint32_t header);

bool PacketLocksItself(std::uint32_t header);

// APS5_COMPUTE_END_DEFER=1 (read once; default off; docs/design/fewer-submits.md S2): a compute
// queue's submission end records its labels but leaves the submit to the label deadline while queue 0
// is busy, so compute ends stop cutting queue 0's batch.
bool ComputeEndDefer();

// Whether such an end defers: a compute queue (0x20-0x57) without a rewind tail, while queue 0 has a
// submission queued or runs one. Queue 0 then passes a submission end after this one recorded its
// labels, and that end submits a pending label (the record pipe holds items only inside a queue-0
// submission: recordPipeSubmissionEnd drains it first); between queue 0's packets the deadline holds
// (flushBetweenPackets, B's submitDue, waitMemory's poll). For a queue 0 blocked in a wait, or past its
// end's check as this one deferred, the deferring worker sends it at the deadline once idle (sendDeferredEnd).
constexpr bool ComputeEndDefers(bool enabled, std::uint32_t queue, std::uint64_t queue0Queued, bool queue0Running, bool rewindTail) {
    return enabled && queue >= 0x20 && queue <= 0x57 && !rewindTail && (queue0Queued != 0 || queue0Running);
}

}

#endif
