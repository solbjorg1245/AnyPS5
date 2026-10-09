#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP

#include <cstddef>
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
// The table is only patched when /app0/gameversion.txt names the build the layout is known for
// (PortSettings::SupportedBuildVersion); the page holding it is writable only while an entry is
// stored, and each entry is one aligned 32-bit store. The game's logic thread reads the table without
// a lock: the 30 FPS mode's entry is stored first and the 60 FPS mode's last, so a reader sees the old
// or the new value of each entry (a frame may mix the two entries' generations, and a value is up to
// one flip old: the step it feeds is smoothed over several flips anyway).
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
    // Stores the two entries (30 FPS mode first); false when the page cannot be made writable.
    bool store(float rate60, float rate30);

    float* table = nullptr;
    void* pageBase = nullptr;
    std::size_t pageBytes = 0;
    unsigned long pageProtection = 0;
    float stored60 = 0.0f;
    float stored30 = 0.0f;
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
