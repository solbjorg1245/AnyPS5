#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEJOBBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEJOBBUDGET_HPP

#include <cmath>
#include <cstdint>
#include <cstring>

// APS5_JOB_BUDGET (TimestepPatch): the title's main-thread job runner (FUN_1402eb3a0) runs queued jobs until
// 95% of 1/fps after the frame start, fps from FUN_141481b10: 20 or 30 (immediates) in the 20 and 30 FPS
// modes, else the float pair at PE VA 0x142509a28, [0] for enum 0 (Unlocked, 60.0) and [1] for enum 1 (the 60
// FPS mode, 59.94). While jobs are queued a frame cannot end much sooner than 1/60 s, whatever the vblank: a
// vblank above 60 Hz is written into both floats. Only over the image's own values (bit for bit), or over
// tools/patch_game.py --fps120's (60.0, 119.88: its fps120-b, a budget the limit replaces just as well).
namespace TitleJobBudget {

inline constexpr std::uint32_t UnlockedBits = 0x42700000;       // 60.0f
inline constexpr std::uint32_t Mode60Bits = 0x426fc28f;         // 59.94f
inline constexpr std::uint32_t PatchedMode60Bits = 0x42efc28f;  // 119.88f (patch_game.py fps120-b)
// The budget the title already has: a vblank up to this leaves the floats alone.
inline constexpr double TitleHz = 60.0;

enum class Action { Write, AlreadySet, NotAbove, Refuse };

struct Budget {
    Action action = Action::NotAbove;
    // The value for both floats (Write, AlreadySet).
    float value = 0.0f;
};

inline std::uint32_t Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The limit is above the title's budget (else the floats are left alone).
inline bool Above(double limitHz) { return std::isfinite(limitHz) && limitHz > TitleHz + 0.01; }

inline Budget Decide(double limitHz, float unlocked, float mode60) {
    if (!Above(limitHz)) return {};
    const auto value = static_cast<float>(limitHz);
    if (Bits(unlocked) == Bits(value) && Bits(mode60) == Bits(value)) return {Action::AlreadySet, value};
    if (Bits(unlocked) != UnlockedBits || (Bits(mode60) != Mode60Bits && Bits(mode60) != PatchedMode60Bits)) return {Action::Refuse, 0.0f};
    return {Action::Write, value};
}

}

#endif
