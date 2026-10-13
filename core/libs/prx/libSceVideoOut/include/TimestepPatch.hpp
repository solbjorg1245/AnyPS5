#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TIMESTEPPATCH_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include "prx/libc/include/PortSettings.hpp"
#include "prx/libSceVideoOut/include/TitleRefreshField.hpp"
#include "prx/libSceVideoOut/include/TitleStepWindow.hpp"

// Variable timestep for Demon's Souls (PPSA01341). The title steps physics (and the logic delta floor)
// once per frame by 1/refresh, refresh coming from a table indexed by TargetFramesPerSecond: 59.94,
// 59.94 (60 FPS mode), 29.97 (30 FPS mode), 19.98, read by FUN_14091b3a0 at a mode change (port notes,
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
// APS5_FRAME_PROBE=1 (diagnostic, writes nothing) also looks for the table with the fixed step and reads,
// each flip, the step the title derived from it (FUN_140b380e0's nominal at PE VA 0x142f8b4a0, found by its
// distance from the table): the 10 s line then adds the title's own step range, the game speed those steps
// add up to and the window's vblank ticks, so a run above 60 FPS shows its real speed, not the model's.
// APS5_TIMESTEP_FIELD=1 (with the variable step) makes the entries reach the title: FUN_14091b3a0 copies one
// into the display object's refresh field (CRenderDisplay+0x28, which FUN_140b380e0 divides by) only when the
// mode changes, so each flip also writes there the entry of the mode in effect (TitleRefreshField.hpp).
class TimestepPatch {
public:
    static TimestepPatch& Get();

    // One flip of the title; vblankHz is the vblank clock (0: 59.94 Hz), vblankCount the flip's vblank
    // counter (read under its config's mutex, which the vblank thread holds to advance it).
    void Flip(double vblankHz, std::uint64_t vblankCount);

private:
    TimestepPatch();
    void report(std::uint64_t now, std::uint64_t vblankCount);

    std::mutex mutex;
    bool enabled = false;
    bool profile = false;
    bool probe = false;
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
    // APS5_FRAME_PROBE: the title's step (nullptr: probe off or not readable) over the digest window.
    float* titleStep = nullptr;
    TitleStepWindow titleSteps;
    std::uint64_t windowVblank = 0;
    // APS5_TIMESTEP_FIELD: the display pointer slot (nullptr: off), the field's last known value (none before
    // the first decision) and the digest window's stores, title rewrites (mode changes) and flips left alone.
    void followRefreshField();
    bool refreshField = false;
    bool fieldStarted = false;
    bool fieldRefused = false;
    std::uint64_t* displaySlot = nullptr;
    std::optional<float> fieldKnown;
    std::uint64_t windowFieldWrites = 0;
    std::uint64_t windowModeChanges = 0;
    std::uint64_t windowFieldLeft = 0;
};

#endif
