#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_FRAMETRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_FRAMETRACE_HPP

#include <cstdint>
#include <string>

namespace AgcDriver::FrameTrace {

// Debug aid (APS5_CAPTURE_DIR, see VulkanDevice::CaptureTargets): a trace of the draws and dispatches
// the queues execute between two presents. Creating <dir>/trace starts one at the next present (the
// file is removed): every command is logged to <dir>/p<present>_trace.txt with its index in the
// frame, and after each index listed in <dir>/after.txt (one per line; "all" after the index saves
// every live image instead of the command's own targets) the targets are saved as
// <dir>/p<present>_c<index>_*.raw. Indices count commands of every queue in execution order.
void AtPresent(const std::string& directory, std::uint64_t present);
bool Active();

struct Entry {
    std::uint64_t index = 0;
    // Whether the command's targets (every live image with `all`) are to be saved, under `prefix`.
    bool dump = false;
    bool all = false;
    std::string prefix;
};
// Logs one command of a traced frame (nothing when no trace runs).
Entry Record(const std::string& line);

}

#endif
