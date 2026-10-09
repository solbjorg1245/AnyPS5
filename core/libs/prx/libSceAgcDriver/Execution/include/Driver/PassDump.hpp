#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PASSDUMP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PASSDUMP_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstdint>
#include <string>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::PassDump {

// Debug aid (APS5_PASS_DUMP=1; off, nothing runs): a per-pass dump of one frame, to find the pass
// that first produces a fault (NaN or infinity, blocky holes of zero texels).
//
// Armed at a present when <dir>/PASSDUMP_NOW exists (the file is removed) or at present number
// APS5_PASS_DUMP_AT. The frame runs from the next flip packet to the one after it: its draws and
// dispatches run one at a time (APS5_PASS_DUMP_SERIAL=0 lets queues overlap) and without the fast
// draw and dispatch paths. A draw pass is the draws between two changes of the color and depth
// targets (or a dispatch on the pass's queue); when it ends its color targets, depth target and the
// storage images its draws wrote are read back. After every dispatch the storage images it wrote,
// and the buffer ranges (as F32), are read back. The present after the frame reads back the
// display buffer. Each image is one line of <dir>/passdump/<date-time>-p<present>/index.csv: NaN,
// infinity, zero and negative texel counts, 16x16 tiles that are 90 % zero or NaN and those on a
// hole's edge, the programs and the textures the pass sampled; "#complete,..." ends the file.
// APS5_PASS_DUMP_RAW=1 also writes each image as <seq>.raw (one header line, then the texels;
// 2 GiB per dump at most). <dir> is APS5_PASS_DUMP_DIR, else APS5_CAPTURE_DIR's parent, else the
// working directory. tools/passdump_report.py reads the dump.
bool Enabled();
// Whether the dumped frame is running (the hooks below do nothing otherwise).
bool FrameActive();

// Held around a draw or dispatch packet of the dumped frame (nothing otherwise): its packets run
// one at a time, so a pass's readback sees only its own draws.
class Serial {
public:
    Serial();
    ~Serial();
    Serial(const Serial&) = delete;
    Serial& operator=(const Serial&) = delete;

private:
    bool held = false;
};

// The presenter, under GuestMemory::GpuMutex: arms a dump, or ends the dumped frame with the
// display buffer at `displayAddress`.
void AtPresent(VulkanDevice& device, std::uint64_t displayAddress);
// A flip packet: starts the armed frame, or ends the running one.
void AtFlip(VulkanDevice* device, std::uint32_t queueId);
// Before a draw packet: a change of targets ends the open pass (read back) and opens the next.
void BeforeDraw(VulkanDevice* device, const QueueState& queue, std::uint32_t queueId);
// After it: `note` is the FrameTrace note of its uses (written images, bound resources).
void AfterDraw(const QueueState& queue, std::uint32_t queueId, bool drawn, const std::string& note);
// Before a dispatch packet: ends the open pass when it was opened on the same queue.
void BeforeWork(VulkanDevice* device, std::uint32_t queueId);
// After it: reads back what `note` says it wrote.
void AfterDispatch(VulkanDevice* device, const QueueState& queue, std::uint32_t queueId, const std::string& note);

}

#endif
