#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_PROLITEPATCH_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_PROLITEPATCH_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Upscaling probe (docs/design/dlss.md stage S0; APS5_MFSR_PROBE=1, off by default): "Pro-lite". The
// renderer init FUN_1409f49c0 builds the PS5 Pro upscaler (SuperResolution, FUN_140a621a0) only when IsPro
// FUN_140856930 (sceKernelIsTrinityMode() != 0) answers true; its call at 0x1409f4b33 becomes `mov al,1`
// and a 3-byte nop, so that site alone sees a Pro (the other 14 keep the base render config, shaders and
// memory split). With r_Wants4K=WantsTrinity (mode 5, ForceTrinityTier) the frame then hands its images to
// libScePsml_debug (one NOP packet per frame, MfsrPacket.hpp) instead of running its TAA pass. Written at
// the first guest path resolve, before the renderer starts, for the supported build with the expected
// bytes only; otherwise one line, nothing written, and the Psml stubs stay unimplemented.
namespace ProLitePatch {

inline constexpr const char* SwitchName = "APS5_MFSR_PROBE";
inline constexpr std::uintptr_t SiteVa = 0x1409f4b33;
// call 0x140856930 (IsPro), followed by test al,al; je.
inline constexpr std::array<unsigned char, 5> Original{0xe8, 0xf8, 0x1d, 0xe6, 0xff};
// mov al,1; nop dword [rax].
inline constexpr std::array<unsigned char, 5> Patched{0xb0, 0x01, 0x0f, 0x1f, 0x00};
inline constexpr const char* TrinityArg = "+r_Wants4K=WantsTrinity";

inline bool Enabled(const char* value) { return value != nullptr && value[0] != '\0' && std::string_view(value) != "0"; }

enum class Site { Original, Patched, Unknown };

// What the five bytes at the site hold.
inline Site Classify(const unsigned char* bytes) {
    if (std::equal(Original.begin(), Original.end(), bytes)) return Site::Original;
    if (std::equal(Patched.begin(), Patched.end(), bytes)) return Site::Patched;
    return Site::Unknown;
}

// The title's arguments with its render tier replaced by WantsTrinity, the tier the upscaler gate takes.
inline void ForceTrinityTier(std::vector<std::string>& args) {
    std::erase_if(args, [](const std::string& arg) { return arg.starts_with("+r_Wants4K="); });
    args.emplace_back(TrinityArg);
}

}

// Applies Pro-lite under APS5_MFSR_PROBE; first in the path overlay's call_once (General.cpp), so the
// settings overlay (InstallPortSettingsOverlay) already knows whether to force the tier.
void InstallProLitePatch(const std::function<std::filesystem::path(const char*)>& resolve);

// Pro-lite is written: libScePsml_debug's HLE and the WantsTrinity argument follow it.
extern "C" bool MfsrProbeActive_nid_no_patch();

#endif
