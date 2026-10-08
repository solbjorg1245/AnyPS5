#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHTIMING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCHTIMING_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::DriverDetail {

enum DriverPhase { PhasePrologue, PhaseKey, PhaseLookup, PhaseValidate, PhaseValidateWait, PhaseRelock, PhaseCapture, PhaseRecompile, PhaseInsert, PhaseQueuedLabels, PhaseSnapshots, PhasePrepareKey, PhasePrepareFind, PhasePreparePrecollect, PhasePreparePresync, PhasePrepareStageA, PhasePrepareOther, PhaseRecipePrecheck, PhaseLockWait, PhaseLabels, PhaseNoteWriters, PhaseDevice, PhaseTail, PhaseEpilogue, DriverPhaseCount };

enum DispatchClass : std::size_t { Queue0Direct, Queue0Indirect, OtherQueues, DispatchClassCount };

// Fast: recorded by the fast dispatch (F5, FastDispatch.cpp), whose own phases are on the
// [fastpath] dispatches line.
enum class DispatchOutcome : std::size_t { Skipped = 0, Real, CopyHle, FillHle, SkippedMemo, Fast, Count };

inline constexpr const char* DriverPhaseNames[DriverPhaseCount] = {"prologue", "key", "lookup", "validate", "validate GPU wait", "relock", "capture", "recompile", "insert", "queued-label check", "snapshots", "prepare: key", "prepare: find", "prepare: precollect", "prepare: presync", "prepare: stage A (miss)", "prepare: other", "recipe pre-check", "lock wait", "labels", "note writers", "device call", "tail", "epilogue"};

inline constexpr const char* DispatchClassNames[DispatchClassCount] = {"queue 0 direct", "queue 0 indirect", "other queues"};

inline constexpr const char* DispatchOutcomeNames[static_cast<std::size_t>(DispatchOutcome::Count)] = {"skipped", "real", "copy HLE", "fill HLE", "skipped (memo)", "fast"};

// The device call by path (VulkanDevice::DevicePath order) and its split rows
// (VulkanDevice::DeviceSplitRow order), per dispatch-cache miss/hit: the [dispatch] device paths line.
inline constexpr std::size_t DevicePathCount = 5;
inline constexpr std::size_t DeviceSplitRows = 7;
inline constexpr const char* DevicePathNames[DevicePathCount] = {"other", "recipe", "resource hit", "stage B (miss)", "full build"};
inline constexpr const char* DeviceSplitNames[DeviceSplitRows] = {"images", "upload", "A locked", "revalidate", "full", "proof", "record"};

struct DevicePathTotals {
    std::uint64_t calls = 0;
    double deviceMs = 0;
    std::array<double, DeviceSplitRows> ms{};
};

struct DriverPhaseTotals {
    std::array<double, DriverPhaseCount> ms{};
    std::uint64_t dispatches = 0, hits = 0, validations = 0;
    std::array<std::array<DevicePathTotals, DevicePathCount>, 2> paths{};
    // Builds by VulkanDevice::DeviceCallSplit::missKind (index 0 unused).
    std::array<std::uint64_t, 11> missKinds{};
};

struct PendingDispatchPhases {
    DispatchOutcome outcome = DispatchOutcome::Skipped;
    bool phases = false;
    bool hit = false;
    bool validated = false;
    std::array<double, DriverPhaseCount> ms{};
    std::chrono::steady_clock::time_point tailAt{};
    std::size_t devicePath = 0;
    std::array<double, DeviceSplitRows> deviceSplit{};
    std::uint8_t missKind = 0;
};

struct DispatchPhaseTiming {
    bool profile;
    std::chrono::steady_clock::time_point& lap;
    std::array<double, DriverPhaseCount>& phaseMs;
    std::chrono::steady_clock::time_point& phaseLap;
    double Elapsed();
    void Phase(DriverPhase which);
};

}

#endif
