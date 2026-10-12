#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GRAPHICSMODEPIN_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GRAPHICSMODEPIN_HPP

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "prx/libc/include/PortSettings.hpp"

// Demon's Souls (PPSA01341) has its own "Visual Mode" row in Options > Display (UI property
// Setting_VisualMode: Performance or Cinematic; PS5 Pro and Power Saver are chosen by the system). The
// choice is the options field +0x64 (0 performance, 1 cinematic, 2 Pro) and selects a render-config slot
// (FUN_1400c6220: 0 -> slot 1 RenderConfig_PS5_Perf, 1 -> slot 2 RenderConfig_PS5_High, 2 -> slot 4, the
// Pro config on a Pro and Perf otherwise). It is written by the menu (the UI property-changed handler at
// 0x1400c45ee), by the options save ("RenderConfig" key, 0x140032cdb), by the system game presets on a
// first start (0x1407d9212), and reapplied after the character creator (0x1400865ee); every path ends in
// the render-config manager's slot setter FUN_140051db0, which applies the convars FUN_14004d610 read at
// boot from $/misc/RenderConfig_PS5_*.txt. The slot is read nowhere else.
// The port's settings (Resolution, FpsLimit; InstallPortSettingsOverlay) own the render size and the
// frame rate, so the cinematic slot's file is aliased to the performance one: every path then loads the
// same convars (the overlay's merged copy when the settings add arguments), changing the option changes
// nothing and a choice kept in the options save cannot override the launcher. The row stays visible
// (the menu is compiled UI data, ui/scripts/_cmn/uistartmenu.cgpr). APS5_HIDE_GRAPHICS_MODE=0 keeps the
// title's own configs (unset or any other value: pinned).
namespace GraphicsModePin {

inline constexpr const char* SwitchName = "APS5_HIDE_GRAPHICS_MODE";
// The paths the title opens ($/misc/RenderConfig_PS5_Perf.txt and _High.txt, strings at 0x1423f0e2f and
// 0x14243bd31, lower-cased by the title); aliases match them without case unless APS5_NO_PATH_CASE_FOLD.
inline constexpr const char* PerformanceGuest = "/app0/misc/renderconfig_ps5_perf.txt";
inline constexpr const char* CinematicGuest = "app0/misc/renderconfig_ps5_high.txt";

inline bool Enabled(const char* value) { return value == nullptr || value[0] == '\0' || std::string_view(value) != "0"; }

struct Plan {
    // Alias CinematicGuest to `target`.
    bool alias = false;
    std::filesystem::path target;
    // The one [settings] line.
    std::string line;
};

// `performanceHost`: the host file the performance config resolves to (the merged copy when the overlay
// wrote one); `performanceFound`: it exists.
inline Plan PlanPin(const char* switchValue, const std::filesystem::path& performanceHost, bool performanceFound) {
    Plan plan;
    if (!Enabled(switchValue)) {
        plan.line = "[settings] graphics mode: the title's Visual Mode is live (APS5_HIDE_GRAPHICS_MODE=0): Cinematic loads its own RenderConfig_PS5_High";
        return plan;
    }
    if (!performanceFound) {
        plan.line = "[settings] graphics mode: " + PortSettings::PathText(performanceHost) + " not found: the title's Visual Mode is left live";
        return plan;
    }
    plan.alias = true;
    plan.target = performanceHost;
    plan.line = "[settings] graphics mode pinned: Visual Mode Cinematic loads the performance config (" + PortSettings::PathText(performanceHost) +
        "), so the option changes nothing and a saved choice cannot override Resolution/FpsLimit (APS5_HIDE_GRAPHICS_MODE=0 = the title's own modes)";
    return plan;
}

}

// Applies the pin; called after InstallPortSettingsOverlay, so `resolve` already returns its merged copy.
void InstallGraphicsModePin(const std::function<std::filesystem::path(const char*)>& resolve);

#endif
