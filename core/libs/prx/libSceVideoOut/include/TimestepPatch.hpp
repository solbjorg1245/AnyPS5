#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include "prx/libc/include/PortSettings.hpp"

// Variable timestep for Demon's Souls (PPSA01341). The title steps physics (and the logic delta floor)
// once per frame by 1/refresh, refresh coming from a table indexed by TargetFramesPerSecond: 59.94,
// 59.94 (60 FPS mode), 29.97 (30 FPS mode), 19.98, read every frame by FUN_14091b3a0 (port notes,
// docs/research/frame-pacing.md). With Timestep = variable (APS5_TIMESTEP=1) every flip rewrites the 60
// and 30 FPS entries with the measured frame rate (PortSettings::TimestepPacer), capped at the rate each
// mode can flip at, so the step, the frame-locked game clock and the refresh readers follow the real
// frame time down to TimestepMinHz. Without the table (another title or build) nothing is written.
// APS5_PROFILE_DRAW=1 prints a [timestep] line every 10 s with the game speed either way.
class TimestepPatch {
public:
    static TimestepPatch& Get();

    // One flip of the title; vblankHz is the vblank clock (0: 59.94 Hz).
    void Flip(double vblankHz);

private:
    TimestepPatch();
    void report(std::uint64_t now);

    std::mutex mutex;
    bool enabled = false;
    bool profile = false;
    bool searched = false;
    float* table = nullptr;
    bool mode30 = false;
    double floorHz = 0.0;
    std::unique_ptr<PortSettings::TimestepPacer> pacer;
    std::uint64_t windowStart = 0;
    std::uint64_t windowFlips = 0;
    std::uint64_t windowFloored = 0;
    double windowGameSeconds = 0.0;
    double windowMinHz = 0.0;
    double windowMaxHz = 0.0;
};

#endif
