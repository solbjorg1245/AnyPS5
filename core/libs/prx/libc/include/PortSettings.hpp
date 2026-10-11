#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_PORTSETTINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_PORTSETTINGS_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Player settings: anyps5-settings.ini next to the executable (APS5_SETTINGS=<file> names another one,
// APS5_SETTINGS=none skips it), one "Key = Value" per line, '#' or ';' starts a comment (not inside a
// "quoted" value), [sections] are ignored. The APS5_* variable after each key overrides it. A key left out keeps today's behaviour: the
// window opens at 60% of the screen and the title's render config (its performance or cinematic mode)
// picks the render resolution and the frame rate.
//   Resolution = 1280x800     APS5_RESOLUTION       output size: window client area (letterboxed), render tier
//   RenderResolution = auto   APS5_RENDER_RESOLUTION 900, 1080, 1440 or 2160: the render tier by hand
//   FpsLimit = 60             APS5_FPS_LIMIT        30 and 60 are the title's own modes; 20-240 otherwise
//   Timestep = fixed          APS5_TIMESTEP         variable (1): the frame step follows the frame time
//   TimestepMinHz = 19.98     APS5_TIMESTEP_MIN_HZ  slowest step rate; slower frames slow the game down
//   WindowMode = windowed     APS5_WINDOW_MODE      or fullscreen (desktop size, the frame scaled into it)
//   ButtonIcons = playstation APS5_BUTTON_ICONS     steamdeck: the prompts show Steam Deck buttons (UiOverlay.hpp)
//   NativeAspect = off        APS5_NATIVE_ASPECT    on (1): render at Resolution's aspect, wider view (Hor+)
//                             APS5_NATIVE_ASPECT_RENDER=1 / APS5_NATIVE_ASPECT_UI=1 (environment only,
//                             experimental): shared render targets / display buffer at the output's shape
//                             APS5_NO_NATIVE_ASPECT_UI_RECT=1 (environment only): the 16:9 UI stretched
//                             with the 3D instead of kept in a centred 16:9 region
//                             APS5_NO_NATIVE_ASPECT_4K_UI=1 (environment only): the smallest slot again,
//                             a 1080p UI below 1440p (its HUD item cluster and menu slots drawn too large)
//                             APS5_NO_RESOLUTION_4K_UI=1 (environment only): Resolution or RenderResolution
//                             up to 1080p runs its tier's own 1080p-display slot again (same HUD fault)
//   Upscaler = off            APS5_UPSCALER         reserved: fsr, dlss (UpscalerQuality and FrameGeneration: file only, reserved)
//   GameDir = "D:\PPSA01341-app0"                   the player's dump (app0), set by the launcher; the game ignores it
// The title takes one fixed physics step of 1/refresh per rendered frame (59.94 Hz in its 60 FPS mode,
// 29.97 Hz in its 30 FPS mode), so any other limit, and any frame rate below the limit, changes the game
// speed unless Timestep = variable (libSceVideoOut TimestepPatch) feeds it the measured frame rate.
namespace PortSettings {

inline constexpr const char* FileName = "anyps5-settings.ini";
inline constexpr const char* OverrideDirectoryName = "anyps5-overrides";
inline constexpr double GameRefresh60 = 60000.0 / 1001.0;
inline constexpr double GameRefresh30 = 30000.0 / 1001.0;
inline constexpr double DefaultTimestepMinHz = 20000.0 / 1001.0;

enum class WindowMode { Auto, Windowed, Fullscreen };
enum class Upscaler { Off, Fsr, Dlss };
enum class ButtonIcons { PlayStation, SteamDeck };

struct Settings {
    std::string source;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t renderHeight = 0;
    double fpsLimit = 0.0;
    bool variableTimestep = false;
    double timestepMinHz = DefaultTimestepMinHz;
    WindowMode windowMode = WindowMode::Auto;
    ButtonIcons buttonIcons = ButtonIcons::PlayStation;
    bool nativeAspect = false;
    bool nativeAspectRender = false;
    bool nativeAspectUi = false;
    // Native aspect keeps the UI on a 3840x2160 display buffer (PlanNativeAspect); APS5_NO_NATIVE_ASPECT_4K_UI.
    bool nativeAspect4kUi = true;
    // A tier up to 1080p keeps the UI on a 3840x2160 display buffer (PlanResolution4kUi); APS5_NO_RESOLUTION_4K_UI.
    bool resolution4kUi = true;
    Upscaler upscaler = Upscaler::Off;
    std::string upscalerQuality;
    bool frameGeneration = false;
    std::string gameDir;  // UTF-8, without the quotes
    std::vector<std::string> warnings;

    bool Any() const { return width != 0 || renderHeight != 0 || fpsLimit != 0.0 || variableTimestep || windowMode != WindowMode::Auto || nativeAspect; }
};

// The title's render tiers (r_Wants4K): internal resolution; the title upscales to its 1080p or 2160p
// output and the presenter scales that into the window. WantsSuperHigh (cinematic mode) is assumed 2160p.
struct RenderTier {
    std::uint32_t width;
    std::uint32_t height;
    const char* convar;
};
inline constexpr std::array<RenderTier, 4> RenderTiers{{
    {1600, 900, "WantsPerf"}, {1920, 1080, "WantsBase"}, {2560, 1440, "WantsHigh"}, {3840, 2160, "WantsSuperHigh"}}};

// Offered sizes (a settings menu lists these; any WIDTHxHEIGHT is accepted).
struct ResolutionPreset {
    std::uint32_t width;
    std::uint32_t height;
    const char* label;
};
inline constexpr std::array<ResolutionPreset, 10> ResolutionPresets{{
    {1280, 720, "1280x720"}, {1280, 800, "1280x800 (Steam Deck)"}, {1600, 900, "1600x900"}, {1920, 1080, "1920x1080"},
    {1920, 1200, "1920x1200"}, {2560, 1080, "2560x1080"}, {2560, 1440, "2560x1440"}, {2560, 1600, "2560x1600"},
    {3440, 1440, "3440x1440"}, {3840, 2160, "3840x2160"}}};

inline std::string Lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return result;
}

inline std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    return value;
}

// Decimal numbers only, and never the C locale's decimal separator (no strtod: it follows setlocale
// and accepts hex floats).
inline bool ParseNumber(std::string_view text, double& value) {
    if (text.empty()) return false;
    const char* const first = text.data();
    const char* const last = first + text.size();
    double parsed = 0.0;
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

// UTF-8 text of a host path (logs, Settings::source); path.string() would use the ANSI code page.
inline std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The only game build the timestep patch's table layout is known for (gameversion.txt of the title).
inline constexpr const char* SupportedBuildVersion = "2025-10-15.877562";

// The id of gameversion.txt text's "BuildVersion=<id>" line (a leading '+' is the title's own argument
// syntax), empty without one.
inline std::string_view BuildVersionOf(std::string_view text) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.starts_with('+')) line.remove_prefix(1);
        constexpr std::string_view key = "BuildVersion=";
        if (line.starts_with(key)) return Trim(line.substr(key.size()));
    }
    return {};
}

// True when gameversion.txt text names exactly the supported build.
inline bool BuildVersionSupported(std::string_view text) {
    return BuildVersionOf(text) == SupportedBuildVersion;
}

// Where a line's comment starts ('#' or ';' outside "double quotes", so a quoted path may hold them), npos without one.
inline std::size_t CommentStart(std::string_view line) {
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') quoted = !quoted;
        else if (!quoted && (line[index] == '#' || line[index] == ';')) return index;
    }
    return std::string_view::npos;
}

// A value without its surrounding double quotes (GameDir is written quoted).
inline std::string_view Unquote(std::string_view value) {
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    return value;
}

// Writes `content` to a temporary sibling and renames it over `target`, so a reader (or a second
// instance writing the same name) sees the old or the new file, never a half-written one.
inline bool WriteFileAtomic(const std::filesystem::path& target, std::string_view content, std::string& error) {
    static std::atomic<std::uint64_t> counter{0};
    auto temporary = target;
    temporary += ".tmp" + std::to_string(counter.fetch_add(1)) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (file) file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (file) file.close();
        if (!file) {
            error = "cannot write " + PathText(temporary);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code rename;
    std::filesystem::rename(temporary, target, rename);
    if (rename) {
        error = "cannot replace " + PathText(target) + ": " + rename.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

// The per-user folder for files the executable's folder cannot take: %LOCALAPPDATA%\AnyPS5 on Windows,
// $XDG_CONFIG_HOME/anyps5 or ~/.config/anyps5 elsewhere; empty when the variables are unset.
// `lookup` returns the variable's value as a path, empty when unset.
inline std::filesystem::path UserDirectory(bool windows, const std::function<std::filesystem::path(const char*)>& lookup) {
    if (windows) {
        const auto base = lookup("LOCALAPPDATA");
        return base.empty() ? std::filesystem::path{} : base / "AnyPS5";
    }
    if (const auto config = lookup("XDG_CONFIG_HOME"); !config.empty()) return config / "anyps5";
    const auto home = lookup("HOME");
    return home.empty() ? std::filesystem::path{} : home / ".config" / "anyps5";
}

// Where the merged copies go, in order of preference: next to the executable, then the user folder.
inline std::vector<std::filesystem::path> OverrideDirectories(const std::filesystem::path& executableDirectory, const std::filesystem::path& userDirectory) {
    std::vector<std::filesystem::path> directories{executableDirectory / OverrideDirectoryName};
    if (!userDirectory.empty()) directories.push_back(userDirectory / OverrideDirectoryName);
    return directories;
}

// Writes `content` as `name` into the first of `directories` that takes it; returns the file written.
inline std::filesystem::path WriteOverrideFile(const std::vector<std::filesystem::path>& directories, const std::filesystem::path& name, std::string_view content, std::string& error) {
    for (const auto& directory : directories) {
        std::error_code created;
        std::filesystem::create_directories(directory, created);
        if (created) {
            error = "cannot create " + PathText(directory) + ": " + created.message();
            continue;
        }
        const auto target = directory / name;
        if (WriteFileAtomic(target, content, error)) return target;
    }
    return {};
}

inline bool ParseResolution(std::string_view text, std::uint32_t& width, std::uint32_t& height) {
    const auto separator = text.find_first_of("xX*");
    if (separator == std::string_view::npos) return false;
    double w = 0.0;
    double h = 0.0;
    if (!ParseNumber(Trim(text.substr(0, separator)), w) || !ParseNumber(Trim(text.substr(separator + 1)), h)) return false;
    if (w != std::floor(w) || h != std::floor(h) || w < 320.0 || h < 180.0 || w > 7680.0 || h > 4320.0) return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

inline const RenderTier* FindRenderTier(std::uint32_t height) {
    for (const auto& tier : RenderTiers) if (tier.height == height) return &tier;
    return nullptr;
}

// 29.97/30 and 59.94/60 are the title's own modes, which need no timestep change.
inline bool IsGameRate(double fps) {
    return std::abs(fps - 30.0) < 0.1 || std::abs(fps - 60.0) < 0.1;
}

inline void SetKey(Settings& settings, std::string_view key, std::string_view value, const std::string& origin) {
    const auto name = Lower(Trim(key));
    const auto text = Lower(Trim(value));
    const auto refuse = [&](const char* expected) {
        settings.warnings.push_back(origin + ": " + std::string(Trim(key)) + " = '" + std::string(Trim(value)) + "' refused (" + expected + "); kept the previous value");
    };
    double number = 0.0;
    if (name == "resolution") {
        if (text == "auto") {
            settings.width = 0;
            settings.height = 0;
        } else if (!ParseResolution(text, settings.width, settings.height)) {
            refuse("auto or WIDTHxHEIGHT from 320x180 to 7680x4320");
        }
    } else if (name == "renderresolution") {
        if (text == "auto") settings.renderHeight = 0;
        else if (ParseNumber(text, number) && number == std::floor(number) && number > 0.0 && number < 10000.0 && FindRenderTier(static_cast<std::uint32_t>(number)) != nullptr) settings.renderHeight = static_cast<std::uint32_t>(number);
        else refuse("auto, 900, 1080, 1440 or 2160");
    } else if (name == "fpslimit") {
        if (text == "auto" || text == "0") settings.fpsLimit = 0.0;
        else if (ParseNumber(text, number) && number >= 20.0 && number <= 240.0) settings.fpsLimit = number;
        else refuse("auto or 20 to 240");
    } else if (name == "timestep") {
        if (text == "fixed" || text == "0" || text == "off") settings.variableTimestep = false;
        else if (text == "variable" || text == "1" || text == "on") settings.variableTimestep = true;
        else refuse("fixed or variable");
    } else if (name == "timestepminhz") {
        if (ParseNumber(text, number) && number >= 10.0 && number <= 240.0) settings.timestepMinHz = number;
        else refuse("10 to 240");
    } else if (name == "windowmode") {
        if (text == "auto") settings.windowMode = WindowMode::Auto;
        else if (text == "windowed") settings.windowMode = WindowMode::Windowed;
        else if (text == "fullscreen") settings.windowMode = WindowMode::Fullscreen;
        else refuse("auto, windowed or fullscreen");
    } else if (name == "buttonicons") {
        if (text == "playstation" || text == "ps" || text == "ps5") settings.buttonIcons = ButtonIcons::PlayStation;
        else if (text == "steamdeck" || text == "deck") settings.buttonIcons = ButtonIcons::SteamDeck;
        else refuse("playstation or steamdeck");
    } else if (name == "nativeaspect") {
        if (text == "off" || text == "0" || text == "false") settings.nativeAspect = false;
        else if (text == "on" || text == "1" || text == "true") settings.nativeAspect = true;
        else refuse("on or off");
    } else if (name == "upscaler") {
        if (text == "off") settings.upscaler = Upscaler::Off;
        else if (text == "fsr" || text == "dlss") settings.warnings.push_back(origin + ": Upscaler = " + text + " is not implemented yet; using off");
        else refuse("off, fsr or dlss");
    } else if (name == "upscalerquality") {
        settings.upscalerQuality = text;
    } else if (name == "framegeneration") {
        if (text == "off" || text == "0") settings.frameGeneration = false;
        else settings.warnings.push_back(origin + ": FrameGeneration is not implemented yet; using off");
    } else if (name == "gamedir") {
        settings.gameDir = std::string(Unquote(value));
    } else {
        settings.warnings.push_back(origin + ": unknown key '" + std::string(Trim(key)) + "'");
    }
}

inline Settings Parse(std::string_view text, const std::string& origin) {
    Settings settings;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t lineNumber = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++lineNumber;
        line = Trim(line.substr(0, CommentStart(line)));
        if (line.empty() || line.front() == '[') continue;
        const auto separator = line.find('=');
        const auto where = origin + ":" + std::to_string(lineNumber);
        if (separator == std::string_view::npos) {
            settings.warnings.push_back(where + ": expected Key = Value");
            continue;
        }
        SetKey(settings, line.substr(0, separator), line.substr(separator + 1), where);
    }
    return settings;
}

// The variable that overrides each key (the launcher names the ones set, which win over its choices).
inline constexpr std::array<std::pair<const char*, const char*>, 9> EnvironmentKeys{{
    {"APS5_RESOLUTION", "Resolution"}, {"APS5_RENDER_RESOLUTION", "RenderResolution"}, {"APS5_FPS_LIMIT", "FpsLimit"},
    {"APS5_TIMESTEP", "Timestep"}, {"APS5_TIMESTEP_MIN_HZ", "TimestepMinHz"}, {"APS5_WINDOW_MODE", "WindowMode"},
    {"APS5_NATIVE_ASPECT", "NativeAspect"}, {"APS5_UPSCALER", "Upscaler"}, {"APS5_BUTTON_ICONS", "ButtonIcons"}}};

inline void ApplyEnvironment(Settings& settings, const std::function<const char*(const char*)>& lookup) {
    for (const auto& [variable, key] : EnvironmentKeys) {
        const char* value = lookup(variable);
        if (value != nullptr && value[0] != '\0') SetKey(settings, key, value, variable);
    }
    // The experimental halves of native aspect part 2 (s59: the render half breaks the post effects,
    // t557/t558/t560; the UI half cuts the bottom HUD off wider than 16:9): any value but 0 turns one on.
    for (const auto& [variable, field] : {std::pair{"APS5_NATIVE_ASPECT_RENDER", &Settings::nativeAspectRender}, std::pair{"APS5_NATIVE_ASPECT_UI", &Settings::nativeAspectUi}}) {
        const char* value = lookup(variable);
        if (value != nullptr && value[0] != '\0') settings.*field = std::string_view(value) != "0";
    }
    // Native aspect's 4K UI (s61): APS5_NATIVE_ASPECT_4K_UI=0 or APS5_NO_NATIVE_ASPECT_4K_UI=1 (any value
    // but 0) brings back the smallest slot that holds the render size (WantsPerf at 1280x800).
    if (const char* value = lookup("APS5_NATIVE_ASPECT_4K_UI"); value != nullptr && value[0] != '\0') settings.nativeAspect4kUi = std::string_view(value) != "0";
    if (const char* value = lookup("APS5_NO_NATIVE_ASPECT_4K_UI"); value != nullptr && value[0] != '\0' && std::string_view(value) != "0") settings.nativeAspect4kUi = false;
    // The plain path's 4K UI (r6): APS5_RESOLUTION_4K_UI=0 or APS5_NO_RESOLUTION_4K_UI=1 (any value but 0)
    // brings back the tier's own slot (WantsPerf at 1280x720, WantsBase at 1920x1080).
    if (const char* value = lookup("APS5_RESOLUTION_4K_UI"); value != nullptr && value[0] != '\0') settings.resolution4kUi = std::string_view(value) != "0";
    if (const char* value = lookup("APS5_NO_RESOLUTION_4K_UI"); value != nullptr && value[0] != '\0' && std::string_view(value) != "0") settings.resolution4kUi = false;
}

// A limit other than the title's 30 or 60 without the variable timestep would change the game speed
// (one 1/59.94 s step per frame): it falls back to 60.
inline void Normalize(Settings& settings) {
    if (settings.fpsLimit != 0.0 && !IsGameRate(settings.fpsLimit) && !settings.variableTimestep) {
        char text[200];
        std::snprintf(text, sizeof(text), "FpsLimit = %g needs Timestep = variable (APS5_TIMESTEP=1): with the fixed 1/59.94 s step the game would run at %.2fx; using 60",
            settings.fpsLimit, settings.fpsLimit / GameRefresh60);
        settings.warnings.push_back(text);
        settings.fpsLimit = 60.0;
    }
    if (settings.nativeAspect && settings.width == 0) {
        settings.warnings.push_back("NativeAspect = on needs Resolution = WIDTHxHEIGHT (the aspect to render at); using off");
        settings.nativeAspect = false;
    }
}

// 30 or 60 for TargetFramesPerSecond, 0 to leave the render config's choice.
inline int GameFramesPerSecond(const Settings& settings) {
    if (settings.fpsLimit == 0.0) return 0;
    return std::abs(settings.fpsLimit - 30.0) < 0.1 ? 30 : 60;
}

// The vblank rate that enforces the limit (the title flips once per vblank in its 60 FPS mode), 0 for
// the default 59.94 Hz.
inline double VblankHz(const Settings& settings) {
    if (settings.fpsLimit == 0.0 || IsGameRate(settings.fpsLimit)) return 0.0;
    return settings.fpsLimit;
}

// The render tier for the output size: the smallest tier at least as tall as the 16:9 image letterboxed
// into Resolution (1280x800 shows a 1280x720 image: WantsPerf), WantsHigh above 1440; nullptr when
// neither Resolution nor RenderResolution is set.
inline const RenderTier* SelectRenderTier(const Settings& settings) {
    if (settings.renderHeight != 0) return FindRenderTier(settings.renderHeight);
    if (settings.width == 0 || settings.height == 0) return nullptr;
    const auto imageHeight = std::min<std::uint64_t>(settings.height, (static_cast<std::uint64_t>(settings.width) * 9 + 8) / 16);
    for (std::size_t index = 0; index + 1 < RenderTiers.size(); ++index) {
        if (RenderTiers[index].height >= imageHeight) return &RenderTiers[index];
    }
    return &RenderTiers[RenderTiers.size() - 2];
}

// Native aspect (NativeAspect = on): the title renders at Resolution's aspect instead of a 16:9 tier.
// Its r_Wants4K tiers come from a table (render and display width/height per mode, read for every view's
// render targets); libSceVideoOut NativeAspectPatch rewrites one slot's render size there and sets the
// display's aspect, which the camera's projection reads (its FOV is vertical: a wider aspect widens the
// view, Hor+). The slot's display buffer keeps its 16:9 size and holds the wider image squeezed; the
// presenter shows it at Resolution's aspect. The slot is the first whose own render size holds the
// planned one (the title also sizes shared targets from the slot's 16:9 render size) and whose display
// buffer is at least the output in both directions (else the first that holds the render size).
// With Settings::nativeAspect4kUi (the default) only slots with a 3840x2160 display buffer count
// (WantsHigh, WantsSuperHigh): the 1080p-display slots (WantsPerf, WantsBase) lay the UI out wrong,
// with or without native aspect (s58-s61: every r_Wants4K=WantsPerf run, t475 t550 t551 t565 t567
// t588, draws the HUD item cluster with a second, larger set of diamond frames and the equipment
// menu's slot frames about twice their size; every 2160p-display run, t549 t585 t587, is right).
// The render size is the plan's either way: the slot's table entry is rewritten to it.
inline constexpr std::uint32_t NativeAspectUiDisplayHeight = 2160;
struct NativeAspectSlot {
    std::uint32_t renderWidth;
    std::uint32_t renderHeight;
    std::uint32_t displayWidth;
    std::uint32_t displayHeight;
    std::uint32_t mode;
    const char* convar;
};
inline constexpr std::array<NativeAspectSlot, 4> NativeAspectSlots{{
    {1600, 900, 1920, 1080, 1, "WantsPerf"}, {1920, 1080, 1920, 1080, 2, "WantsBase"}, {2560, 1440, 3840, 2160, 3, "WantsHigh"},
    {3840, 2160, 3840, 2160, 4, "WantsSuperHigh"}}};

struct NativeAspectPlan {
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t aspectWidth = 0;
    std::uint32_t aspectHeight = 0;
    const NativeAspectSlot* slot = nullptr;
    // Part 2 (PlanNativeTargets), 0 where the slot keeps its own 16:9 size: the shared render targets
    // and the display buffer the UI is drawn on.
    std::uint32_t sharedWidth = 0;
    std::uint32_t sharedHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    // Plain Resolution's 4K UI (PlanResolution4kUi): only the table rewrite; the aspect field and the
    // presentation are left alone (the image stays 16:9).
    bool uiOnly = false;

    bool Active() const { return slot != nullptr; }
    bool SharedNative() const { return sharedWidth != 0; }
    bool DisplayNative() const { return displayWidth != 0; }
    float Aspect() const { return aspectHeight == 0 ? 0.0f : static_cast<float>(static_cast<double>(aspectWidth) / aspectHeight); }
};

// Native aspect part 2. CRenderDisplay::SetMode (FUN_14091a600) sizes the shared render targets (depth,
// G-buffer: ViewSharedRenderTargets::Resize FUN_140927c30 reads the configuration's render size) and the
// display buffer, UI targets and upscale (display size) from one render and one display size per mode,
// not from the tier table, so with the table alone the 3D renders into 16:9 targets and the UI is drawn
// on a 16:9 canvas the presenter stretches. Here the shared targets take the planned render size and the
// display buffer Resolution's shape inside the slot's display size (2560x1080 in WantsHigh: 3840x1620;
// 1280x800 in WantsPerf: 1728x1080). Only the modes SetMode switches to: the title starts in WantsBase,
// sized by CRenderDisplay::Init. WantsSuperHigh loads one size for both, so it takes the render size
// for both and only with both halves on. A size equal to the slot's own stays 0 (nothing to patch).
// Both halves are experimental and off unless APS5_NATIVE_ASPECT_RENDER / APS5_NATIVE_ASPECT_UI ask:
// with the shared targets at 2560x1080 the bloom and temporal passes break (s59 t557, t558, t560;
// cause open), and the UI scales by width and anchors its bottom for a 16:9 height, so a wider
// display buffer cuts the bottom HUD off (t557).
inline void PlanNativeTargets(const Settings& settings, NativeAspectPlan& plan) {
    const auto& slot = *plan.slot;
    std::uint32_t displayWidth = slot.displayWidth;
    std::uint32_t displayHeight = slot.displayHeight;
    if (static_cast<std::uint64_t>(plan.aspectWidth) * slot.displayHeight >= static_cast<std::uint64_t>(plan.aspectHeight) * slot.displayWidth) {
        displayHeight = std::min<std::uint32_t>(slot.displayHeight, static_cast<std::uint32_t>(std::lround(static_cast<double>(slot.displayWidth) * plan.aspectHeight / plan.aspectWidth / 4.0)) * 4);
    } else {
        displayWidth = std::min<std::uint32_t>(slot.displayWidth, static_cast<std::uint32_t>(std::lround(static_cast<double>(slot.displayHeight) * plan.aspectWidth / plan.aspectHeight / 8.0)) * 8);
    }
    const bool renderDiffers = plan.renderWidth != slot.renderWidth || plan.renderHeight != slot.renderHeight;
    if (slot.mode == 4) {
        if (!settings.nativeAspectRender || !settings.nativeAspectUi || !renderDiffers) return;
        plan.sharedWidth = plan.displayWidth = plan.renderWidth;
        plan.sharedHeight = plan.displayHeight = plan.renderHeight;
        return;
    }
    if (slot.mode != 1 && slot.mode != 3) return;
    if (settings.nativeAspectRender && renderDiffers) {
        plan.sharedWidth = plan.renderWidth;
        plan.sharedHeight = plan.renderHeight;
    }
    if (settings.nativeAspectUi && (displayWidth != slot.displayWidth || displayHeight != slot.displayHeight)) {
        plan.displayWidth = displayWidth;
        plan.displayHeight = displayHeight;
    }
}

// The render size for Resolution WxH: RenderResolution's height, else the output height up to 1440 (as
// the tiers: a 2160p output renders 1440p), the width from the aspect; at most 3840x2160 (the title's
// shared render-target memory is sized for that), width a multiple of 8, height of 4.
inline NativeAspectPlan PlanNativeAspect(const Settings& settings) {
    NativeAspectPlan plan;
    if (!settings.nativeAspect || settings.width == 0 || settings.height == 0) return plan;
    const double aspect = static_cast<double>(settings.width) / settings.height;
    double height = settings.renderHeight != 0 ? settings.renderHeight : std::min<std::uint32_t>(settings.height, 1440);
    double width = height * aspect;
    if (width > 3840.0) {
        width = 3840.0;
        height = width / aspect;
    }
    if (height > 2160.0) {
        height = 2160.0;
        width = height * aspect;
    }
    plan.renderWidth = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(width / 8.0)) * 8, 320, 3840);
    plan.renderHeight = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(height / 4.0)) * 4, 180, 2160);
    plan.aspectWidth = settings.width;
    plan.aspectHeight = settings.height;
    const auto holds = [&](const NativeAspectSlot& slot) {
        return plan.renderWidth <= slot.renderWidth && plan.renderHeight <= slot.renderHeight
            && (!settings.nativeAspect4kUi || slot.displayHeight >= NativeAspectUiDisplayHeight);
    };
    for (const auto& slot : NativeAspectSlots) {
        if (holds(slot) && settings.width <= slot.displayWidth && settings.height <= slot.displayHeight) {
            plan.slot = &slot;
            PlanNativeTargets(settings, plan);
            return plan;
        }
    }
    for (const auto& slot : NativeAspectSlots) {
        if (holds(slot)) {
            plan.slot = &slot;
            PlanNativeTargets(settings, plan);
            return plan;
        }
    }
    return plan;
}

// Plain Resolution's 4K UI (r6, Settings::resolution4kUi, on by default): a tier with a 1080p display
// buffer (WantsPerf, WantsBase: Resolution or RenderResolution up to 1080p, NativeAspect off) lays the
// HUD item cluster and the menu slots out wrong (NativeAspectUiDisplayHeight: t475 t550 t565 t588), so
// the title runs the first 3840x2160-display slot that holds the tier's render size (WantsHigh) with that
// slot's render entries rewritten to the tier's size: the 3D renders as in the tier's own slot (1600x900
// at 1280x720, 1920x1080 at 1920x1080), the UI is laid out on the 2160p display buffer and the presenter
// scales it down to the window. The table rewrite is native aspect part 5's (NativeAspectPatch, t592);
// the aspect field is not written. Inactive under NativeAspect (PlanNativeAspect plans the slot), above
// 1080p, and with APS5_RESOLUTION_4K_UI=0 / APS5_NO_RESOLUTION_4K_UI=1 (the tier's own slot, as s58-s61).
inline NativeAspectPlan PlanResolution4kUi(const Settings& settings) {
    NativeAspectPlan plan;
    if (!settings.resolution4kUi || settings.nativeAspect) return plan;
    const auto* tier = SelectRenderTier(settings);
    if (tier == nullptr) return plan;
    const auto own = std::find_if(NativeAspectSlots.begin(), NativeAspectSlots.end(), [&](const NativeAspectSlot& slot) { return std::string_view(slot.convar) == tier->convar; });
    if (own == NativeAspectSlots.end() || own->displayHeight >= NativeAspectUiDisplayHeight) return plan;
    for (const auto& slot : NativeAspectSlots) {
        if (slot.displayHeight >= NativeAspectUiDisplayHeight && tier->width <= slot.renderWidth && tier->height <= slot.renderHeight) {
            plan.slot = &slot;
            break;
        }
    }
    if (!plan.Active()) return plan;
    plan.renderWidth = tier->width;
    plan.renderHeight = tier->height;
    plan.aspectWidth = 16;
    plan.aspectHeight = 9;
    plan.uiOnly = true;
    return plan;
}

// The slot whose render entries NativeAspectPatch rewrites and the overlay forces: native aspect's plan,
// else plain Resolution's 4K UI; inactive when the title keeps its tier's own slot.
inline NativeAspectPlan PlanTierRewrite(const Settings& settings) {
    if (const auto plan = PlanNativeAspect(settings); plan.Active()) return plan;
    return PlanResolution4kUi(settings);
}

// Native aspect's UI rect (libSceAgcDriver DepthSurface.cpp, s59-s61): the 16:9 UI is moved into a
// centred 16:9 region of the display buffer at its composite, so the HUD, menus and movies keep their
// proportions while the 3D fills the window. On wherever native aspect is planned and the display
// buffer keeps its 16:9 shape (with APS5_NATIVE_ASPECT_UI it has the window's shape: nothing to move).
// `noUiRect` (APS5_NO_NATIVE_ASPECT_UI_RECT) other than empty or 0 turns it off (part 1: the UI
// stretched with the 3D); `uiRect` (APS5_NATIVE_ASPECT_UI_RECT) forces it on (any value but 0) or off.
inline bool NativeAspectUiRect(const Settings& settings, const char* uiRect, const char* noUiRect) {
    if (noUiRect != nullptr && noUiRect[0] != '\0' && std::string_view(noUiRect) != "0") return false;
    if (uiRect != nullptr && uiRect[0] != '\0') return std::string_view(uiRect) != "0";
    const auto plan = PlanNativeAspect(settings);
    return plan.Active() && !plan.DisplayNative();
}

// The title's tier table in the supported build (PE VA 0x14251eb48, read by FUN_14091ab90): render width,
// render height, display width, display height, five entries each, indexed by EWantsDisplayMode - 1
// (WantsPerf 1 .. WantsTrinity 5). The render half is the signature that must occur once; the display
// half is only checked for the log (it is never written).
inline constexpr std::array<std::uint32_t, 20> TierTable{{1600, 1920, 2560, 3840, 2688, 900, 1080, 1440, 2160, 1512,
    1920, 1920, 3840, 3840, 3840, 1080, 1080, 2160, 2160, 2160}};
inline constexpr std::size_t TierTableSignatureEntries = 10;
// The CRenderDisplay singleton pointer (DAT_142f1a988) relative to the table, and the object's fields:
// +4 the aspect every camera projection divides by (16:9 from its Init), +0x3c the EWantsDisplayMode.
inline constexpr std::ptrdiff_t DisplaySingletonFromTierTable = 0x142f1a988 - 0x14251eb48;
inline constexpr std::size_t DisplayAspectOffset = 0x4;
inline constexpr std::size_t DisplayModeOffset = 0x3c;
inline constexpr std::uint32_t Aspect16x9Bits = 0x3fe38e39;

struct TableWrite {
    std::size_t entry;
    std::uint32_t value;
};

// The entries the plan rewrites: the slot's render width and render height, and with `display` (the
// display buffer patched, part 2) its display width and height, which every view's configuration
// carries for its upscale into the display buffer.
inline std::vector<TableWrite> TierTableWrites(const NativeAspectPlan& plan, bool display = false) {
    const std::size_t index = plan.slot->mode - 1;
    std::vector<TableWrite> writes{{index, plan.renderWidth}, {5 + index, plan.renderHeight}};
    if (display && plan.DisplayNative()) {
        writes.push_back({10 + index, plan.displayWidth});
        writes.push_back({15 + index, plan.displayHeight});
    }
    return writes;
}

inline std::uint32_t AspectBits(float aspect) { return std::bit_cast<std::uint32_t>(aspect); }

// The value the display's aspect field should hold: the plan's aspect while the display runs the slot's
// mode (its views then render the rewritten size), 16:9 in any other mode; nothing when the field holds
// neither value (another layout: leave it alone).
inline std::optional<std::uint32_t> DisplayAspectBits(std::uint32_t current, std::int32_t mode, const NativeAspectPlan& plan) {
    const auto wide = AspectBits(plan.Aspect());
    if (current != wide && current != Aspect16x9Bits) return std::nullopt;
    if (mode < 0 || mode > 5) return std::nullopt;
    return static_cast<std::uint32_t>(mode) == plan.slot->mode ? wide : Aspect16x9Bits;
}

struct ClientSize {
    std::uint32_t width;
    std::uint32_t height;
};

// The window's client area for Resolution (physical pixels) on a display whose usable area (the work
// area, physical pixels) is usableWidth x usableHeight: Resolution itself when it fits, else the largest
// size of its aspect that does; never below the minimum (3840x400 on a 1920 screen is raised).
inline ClientSize WindowClientSize(std::uint32_t width, std::uint32_t height, std::uint32_t usableWidth, std::uint32_t usableHeight, std::uint32_t minimumWidth, std::uint32_t minimumHeight) {
    if (width == 0 || height == 0 || usableWidth == 0 || usableHeight == 0) return {std::max(width, minimumWidth), std::max(height, minimumHeight)};
    const double scale = std::min({1.0, static_cast<double>(usableWidth) / width, static_cast<double>(usableHeight) / height});
    const auto extent = [scale](std::uint32_t requested, std::uint32_t minimum) {
        return std::max({minimum, std::uint32_t{1}, static_cast<std::uint32_t>(std::lround(requested * scale))});
    };
    return {extent(width, minimumWidth), extent(height, minimumHeight)};
}

// Resolution counts physical pixels, so the process declares itself DPI aware (Windows would otherwise
// scale the window by the display scale: 1280x800 at 125% gives a 1600x1000 client, the frame stretched).
// `switchValue` is APS5_DPI_AWARE: "0" keeps the process DPI unaware. Without Resolution nothing changes.
inline bool DpiAwareWindow(const Settings& settings, const char* switchValue) {
    if (settings.width == 0) return false;
    return switchValue == nullptr || std::string_view(switchValue) != "0";
}

// The title's arguments the settings imply; empty when they imply none.
inline std::vector<std::string> GameArgs(const Settings& settings) {
    std::vector<std::string> args;
    if (const auto plan = PlanTierRewrite(settings); plan.Active()) args.push_back(std::string("+r_Wants4K=") + plan.slot->convar);
    else if (const auto* tier = SelectRenderTier(settings)) args.push_back(std::string("+r_Wants4K=") + tier->convar);
    if (const auto fps = GameFramesPerSecond(settings)) args.push_back("+TargetFramesPerSecond=" + std::to_string(fps));
    return args;
}

// A render config with the arguments appended one per line (they load after the file's own lines and win).
inline std::string AppendLines(std::string_view original, const std::vector<std::string>& args) {
    std::string result(Trim(original));
    for (const auto& arg : args) result += "\r\n" + arg;
    return result + "\r\n";
}

// The package command line with the arguments appended, space separated.
inline std::string AppendWords(std::string_view original, const std::vector<std::string>& args) {
    std::string result(Trim(original));
    for (const auto& arg : args) result += (result.empty() ? "" : " ") + arg;
    return result;
}

inline std::string Describe(const Settings& settings) {
    char text[512];
    std::string result;
    if (settings.width != 0) {
        std::snprintf(text, sizeof(text), "Resolution %ux%u, ", settings.width, settings.height);
        result += text;
    }
    if (const auto plan = PlanNativeAspect(settings); plan.Active()) {
        std::snprintf(text, sizeof(text), "native aspect %.4f: render %ux%u in the r_Wants4K=%s slot (shared targets %ux%u, display buffer %ux%u), ", plan.Aspect(),
            plan.renderWidth, plan.renderHeight, plan.slot->convar, plan.SharedNative() ? plan.sharedWidth : plan.slot->renderWidth,
            plan.SharedNative() ? plan.sharedHeight : plan.slot->renderHeight, plan.DisplayNative() ? plan.displayWidth : plan.slot->displayWidth,
            plan.DisplayNative() ? plan.displayHeight : plan.slot->displayHeight);
        result += text;
    } else if (const auto ui = PlanResolution4kUi(settings); ui.Active()) {
        std::snprintf(text, sizeof(text), "render %ux%u (the %s tier) in the r_Wants4K=%s slot (display buffer %ux%u: 4K UI, APS5_NO_RESOLUTION_4K_UI=1 for the tier's own), ",
            ui.renderWidth, ui.renderHeight, SelectRenderTier(settings)->convar, ui.slot->convar, ui.slot->displayWidth, ui.slot->displayHeight);
        result += text;
    } else if (const auto* tier = SelectRenderTier(settings)) {
        std::snprintf(text, sizeof(text), "render r_Wants4K=%s (%ux%u), ", tier->convar, tier->width, tier->height);
        result += text;
    }
    if (settings.fpsLimit != 0.0) {
        const auto vblank = VblankHz(settings);
        std::snprintf(text, sizeof(text), "FpsLimit %g (%d FPS mode, vblank %.2f Hz), ", settings.fpsLimit, GameFramesPerSecond(settings), vblank != 0.0 ? vblank : GameRefresh60);
        result += text;
    }
    std::snprintf(text, sizeof(text), "Timestep %s", settings.variableTimestep ? "variable" : "fixed");
    result += text;
    if (settings.variableTimestep) {
        std::snprintf(text, sizeof(text), " (%.2f Hz and up)", settings.timestepMinHz);
        result += text;
    }
    result += settings.windowMode == WindowMode::Fullscreen ? ", WindowMode fullscreen" : settings.windowMode == WindowMode::Windowed ? ", WindowMode windowed" : "";
    result += ", Upscaler off";
    result += settings.source.empty() ? " (no settings file)" : " (" + settings.source + ")";
    return result;
}

// The settings launcher (s61, core/launcher: anyps5-launcher next to the executable or in its libs folder).
// libc runs it before the settings load (PortSettings.cpp) and waits; it rewrites the file. Exit code 0 =
// Play (the game starts and reads the file), LauncherExitQuit = its window was closed (the game exits), any
// other code = it failed (the game starts with the file as it is).
inline constexpr const char* LauncherName = "anyps5-launcher";
inline constexpr const char* LaunchArgument = "--launch";
inline constexpr int LauncherExitQuit = 3;

// Why the launcher is not shown, nullptr to show it: --launch on the host command line (Steam launch
// options "%command% --launch"), APS5_NO_LAUNCHER=1 (any value but 0) or APS5_LAUNCHER=0 (the test
// harness), APS5_SETTINGS=none (no file to edit). `args` is the host argv, argv[0] first.
inline const char* LauncherSkipReason(const std::vector<std::string>& args, const std::function<const char*(const char*)>& lookup) {
    for (std::size_t index = 1; index < args.size(); ++index) if (args[index] == LaunchArgument) return LaunchArgument;
    const auto set = [&](const char* name) -> const char* { const char* value = lookup(name); return value != nullptr && value[0] != '\0' ? value : nullptr; };
    if (const char* value = set("APS5_NO_LAUNCHER"); value != nullptr && std::string_view(value) != "0") return "APS5_NO_LAUNCHER";
    if (const char* value = set("APS5_LAUNCHER"); value != nullptr && std::string_view(value) == "0") return "APS5_LAUNCHER=0";
    if (const char* value = set("APS5_SETTINGS"); value != nullptr && Lower(value) == "none") return "APS5_SETTINGS=none";
    return nullptr;
}

// The launcher's choices made consistent the way Normalize would refuse them otherwise: an FpsLimit other
// than the title's 30 or 60 turns the variable timestep on, native aspect needs an explicit Resolution.
inline void ApplyLauncherRules(Settings& settings) {
    if (settings.fpsLimit != 0.0 && !IsGameRate(settings.fpsLimit)) settings.variableTimestep = true;
    if (settings.width == 0) settings.nativeAspect = false;
}

// Shortest text that parses back to the same number (locale independent, like ParseNumber).
inline std::string FormatNumber(double value) {
    char text[32];
    const auto result = std::to_chars(text, text + sizeof(text), value);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string("0");
}

// The keys the launcher writes, as values Parse reads back to the same settings.
inline std::vector<std::pair<std::string, std::string>> LauncherValues(const Settings& settings) {
    return {
        {"Resolution", settings.width == 0 ? std::string("auto") : std::to_string(settings.width) + "x" + std::to_string(settings.height)},
        {"WindowMode", settings.windowMode == WindowMode::Fullscreen ? "fullscreen" : "windowed"}, // auto is windowed (DisplayWindow)
        {"FpsLimit", settings.fpsLimit == 0.0 ? std::string("auto") : FormatNumber(settings.fpsLimit)},
        {"Timestep", settings.variableTimestep ? "variable" : "fixed"},
        {"NativeAspect", settings.nativeAspect ? "on" : "off"},
        {"ButtonIcons", settings.buttonIcons == ButtonIcons::SteamDeck ? "steamdeck" : "playstation"}};
}

// LauncherValues plus the game folder, quoted (a path may hold '#' or ';'). An empty folder is written as
// GameDir = "" with `keepEmpty` (the launcher's own window: a cleared field clears the saved folder), else
// left out.
inline std::vector<std::pair<std::string, std::string>> LauncherValuesWithGameDir(const Settings& settings, bool keepEmpty = false) {
    auto values = LauncherValues(settings);
    if (keepEmpty || !settings.gameDir.empty()) values.emplace_back("GameDir", "\"" + settings.gameDir + "\"");
    return values;
}

// The file's text with each key set to its value. Every active "Key = Value" line of a key (any case) gets
// the new value and keeps its indentation, key spelling and trailing comment; a key without such a line is
// appended. Comments (also commented-out keys), sections, other keys, a UTF-8 BOM and the line endings stay
// byte for byte; appended lines use the file's line ending (LF for a new file).
inline std::string SetIniValues(std::string_view text, const std::vector<std::pair<std::string, std::string>>& values) {
    const char* const newline = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    std::vector<bool> written(values.size(), false);
    std::string result;
    result.reserve(text.size() + 32 * values.size());
    std::size_t start = 0;
    if (text.starts_with("\xEF\xBB\xBF")) {
        result += text.substr(0, 3);
        start = 3;
    }
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        std::string_view ending = end == std::string_view::npos ? "" : "\n";
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
            ending = end == std::string_view::npos ? "\r" : "\r\n";
        }
        start = end == std::string_view::npos ? text.size() : end + 1;
        const auto comment = CommentStart(line);
        const auto body = line.substr(0, comment);
        const auto separator = body.find('=');
        const auto trimmed = Trim(body);
        std::size_t match = values.size();
        if (separator != std::string_view::npos && !trimmed.empty() && trimmed.front() != '[') {
            const auto key = Lower(Trim(body.substr(0, separator)));
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (Lower(values[index].first) == key) {
                    match = index;
                    break;
                }
            }
        }
        if (match == values.size()) {
            result += line;
            result += ending;
            continue;
        }
        const auto indent = body.find_first_not_of(" \t");
        result += body.substr(0, indent);
        result += Trim(body.substr(0, separator));
        result += " = ";
        result += values[match].second;
        if (comment != std::string_view::npos) {
            result += body.substr(body.find_last_not_of(" \t") + 1);
            result += line.substr(comment);
        }
        result += ending;
        written[match] = true;
    }
    bool open = !result.empty() && result.back() != '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (written[index]) continue;
        if (open) result += newline;
        open = false;
        result += values[index].first + " = " + values[index].second + newline;
    }
    return result;
}

// The variable timestep's rate: the flip-to-flip time, clamped to [1/maxHz, 1/minHz] and smoothed
// (exponential average, `smoothing` per flip), as the frame rate the title's next step should assume.
class TimestepPacer {
public:
    TimestepPacer(double minHz, double maxHz, double smoothing = 0.25)
        : shortest(1.0 / std::max(minHz, maxHz)), longest(1.0 / std::min(minHz, maxHz)), weight(smoothing), period(shortest) {}

    double Flip(std::uint64_t nanos) {
        floored = false;
        if (last != 0 && nanos > last) {
            const double interval = static_cast<double>(nanos - last) / 1e9;
            floored = interval > longest;
            period += weight * (std::clamp(interval, shortest, longest) - period);
        }
        last = nanos;
        return 1.0 / period;
    }

    double Rate() const { return 1.0 / period; }
    bool Floored() const { return floored; }

private:
    double shortest;
    double longest;
    double weight;
    double period;
    std::uint64_t last = 0;
    bool floored = false;
};

}

// The process's settings: the file, then the APS5_* overrides, loaded and logged once ([settings] lines).
extern "C" const PortSettings::Settings& GetPortSettings_nid_no_patch();
// Aliases the title's /app0/packagecmdlineargs.txt and /app0/misc/renderconfig_ps5_*.txt to copies with
// the settings' arguments appended (anyps5-overrides next to the executable); `resolve` maps a guest path
// to its host file. Nothing happens when the settings imply no arguments.
void InstallPortSettingsOverlay(const std::function<std::filesystem::path(const char*)>& resolve);
// The folders override files go to, in order of preference (OverrideDirectories for this executable).
std::vector<std::filesystem::path> PortOverrideDirectories();

#endif
