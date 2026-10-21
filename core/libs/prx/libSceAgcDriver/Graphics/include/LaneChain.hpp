#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANECHAIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANECHAIN_HPP

#include "prx/libSceAgcDriver/Graphics/include/LaneCensus.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {

// APS5_COMPUTE_QUEUE=1 (read once; default off; docs/design/fewer-submits.md S3): the device asks
// for a second queue of its queue family, and the recorder submits lane C's batches (LaneOf: the
// compute queue workers) to it, lane G's to the first queue. Off: one queue, as before.
inline bool ComputeQueueAsked() {
    static const bool asked = [] {
        const char* value = std::getenv("APS5_COMPUTE_QUEUE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return asked;
}

// The waits that keep the two queues in the one serial order of a single queue (GPU-free; the
// recorder keeps one under GuestMemory::GpuMutex). Every recorded batch signals the recorder's
// timeline with its serial; a batch whose predecessor went to the other queue first waits for that
// serial (a signal covers every earlier command of its queue, a wait every later one), so batch N
// starts after batch N - 1 finished whichever queue ran it; neighbours on one queue keep their
// recorded barriers, as before. One-shot submissions outside the recorder (a resident land, a
// CommandBatch, the presentation blit) go to the first queue: one waits for the newest batch when
// that went to the compute queue, and signals a second timeline (+1 each) that the next compute
// batch waits for unless a batch submitted since covers it (a lane G batch after it on the same
// queue: the next compute batch waits for that one). Disabled: every wait is 0.
class LaneChain {
public:
    struct Waits {
        // The recorder's timeline value to wait for (0: none).
        std::uint64_t serial = 0;
        // The one-shot timeline value to wait for (0: none).
        std::uint64_t oneShot = 0;
    };

    void Enable() { enabled = true; }
    bool Enabled() const { return enabled; }

    // The waits of the next batch, recorded by `lane`.
    Waits ForBatch(Lane lane) const {
        Waits waits;
        if (!enabled) return waits;
        if (newest != 0 && newestLane != lane) waits.serial = newest;
        if (lane == Lane::C && oneShots > followed) waits.oneShot = oneShots;
        return waits;
    }
    // That batch was submitted with `serial`; it follows every one-shot so far (a compute batch by
    // its wait, a lane G batch by queue order).
    void Submitted(Lane lane, std::uint64_t serial) {
        if (!enabled) return;
        newest = serial;
        newestLane = lane;
        followed = oneShots;
    }
    // The recorder's timeline value a one-shot on the first queue waits for (0: none).
    std::uint64_t OneShotWait() const { return enabled && newestLane == Lane::C ? newest : 0; }
    // The one-shot timeline value the next one-shot signals; OneShotSubmitted once it was submitted.
    std::uint64_t NextOneShot() const { return oneShots + 1; }
    void OneShotSubmitted() {
        if (enabled) ++oneShots;
    }

private:
    bool enabled = false;
    Lane newestLane = Lane::G;
    std::uint64_t newest = 0;
    std::uint64_t oneShots = 0;
    // The one-shot value every later batch follows already.
    std::uint64_t followed = 0;
};

}

#endif
