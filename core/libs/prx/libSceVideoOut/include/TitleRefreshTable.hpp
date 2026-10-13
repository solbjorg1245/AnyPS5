#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEREFRESHTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEREFRESHTABLE_HPP

#include <array>
#include <cmath>
#include <cstring>

// The title's refresh table (TimestepPatch), indexed by TargetFramesPerSecond: 59.94, 59.94 (the 60 FPS mode),
// 29.97, 19.98 at PE VA 0x142507b90 in 2025-10-15.877562, found by its 16 bytes. tools/patch_game.py --fps120
// (fps120-a) turns entry 1 into 119.88; APS5_TABLE_COMPAT=1 accepts that form too. The variable step overwrites
// the entry each flip anyway; with the fixed step the 60 FPS mode then steps 1/119.88 s per flip, so the game
// runs at vblank / 119.88 (0.50x at the default 59.94 Hz vblank).
namespace TitleRefreshTable {

enum class Form { None, Original, Fps120 };

inline constexpr std::array<unsigned char, 16> Original{0x8f, 0xc2, 0x6f, 0x42, 0x8f, 0xc2, 0x6f, 0x42, 0x8f, 0xc2, 0xef, 0x41, 0x0a, 0xd7, 0x9f, 0x41};
// Entry 1 = 119.88f (0x42efc28f, patch_game.py's fps120-a), the others as in Original.
inline constexpr std::array<unsigned char, 16> Fps120{0x8f, 0xc2, 0x6f, 0x42, 0x8f, 0xc2, 0xef, 0x42, 0x8f, 0xc2, 0xef, 0x41, 0x0a, 0xd7, 0x9f, 0x41};
// The patched 60 FPS mode's refresh.
inline constexpr float PatchedRefresh = 119.88f;

// The form the 16 bytes at `bytes` hold.
inline Form Classify(const unsigned char* bytes) {
    if (std::memcmp(bytes, Original.data(), Original.size()) == 0) return Form::Original;
    if (std::memcmp(bytes, Fps120.data(), Fps120.size()) == 0) return Form::Fps120;
    return Form::None;
}

// The game speed a fixed step of 1/titleHz per flip gives at one flip per vblank.
inline double Speed(double titleHz, double vblankHz) { return titleHz > 0.0 ? vblankHz / titleHz : 0.0; }

// Within 2% of 1.0x.
inline bool RightSpeed(double speed) { return std::fabs(speed - 1.0) <= 0.02; }

}

#endif
