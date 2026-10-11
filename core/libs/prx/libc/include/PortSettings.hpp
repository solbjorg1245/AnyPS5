#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_PORTSETTINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_PORTSETTINGS_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Player settings: anyps5-settings.ini next to the executable (APS5_SETTINGS=<file> names another one,
// APS5_SETTINGS=none skips it), one "Key = Value" per line, '#' or ';' starts a comment, [sections] are
// ignored. The APS5_* variable after each key overrides it. A key left out keeps today's behaviour: the
// window opens at 60% of the screen and the title's render config (its performance or cinematic mode)
// picks the render resolution and the frame rate.
//   Resolution = 1280x800     APS5_RESOLUTION       output size: window client area (letterboxed), render tier
//   RenderResolution = auto   APS5_RENDER_RESOLUTION 900, 1080, 1440 or 2160: the render tier by hand
//   FpsLimit = 60             APS5_FPS_LIMIT        30 and 60 are the title's own modes; 20-240 otherwise
//   Timestep = fixed          APS5_TIMESTEP         variable (1): the frame step follows the frame time
//   TimestepMinHz = 19.98     APS5_TIMESTEP_MIN_HZ  slowest step rate; slower frames slow the game down
//   WindowMode = windowed     APS5_WINDOW_MODE      or fullscreen (desktop size, the frame scaled into it)
//   Upscaler = off            APS5_UPSCALER         reserved: fsr, dlss (UpscalerQuality and FrameGeneration: file only, reserved)
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

struct Settings {
    std::string source;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t renderHeight = 0;
    double fpsLimit = 0.0;
    bool variableTimestep = false;
    double timestepMinHz = DefaultTimestepMinHz;
    WindowMode windowMode = WindowMode::Auto;
    Upscaler upscaler = Upscaler::Off;
    std::string upscalerQuality;
    bool frameGeneration = false;
    std::vector<std::string> warnings;

    bool Any() const { return width != 0 || renderHeight != 0 || fpsLimit != 0.0 || variableTimestep || windowMode != WindowMode::Auto; }
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

// True when gameversion.txt text has a "BuildVersion=<id>" line (a leading '+' is the title's own
// argument syntax) with exactly the supported id.
inline bool BuildVersionSupported(std::string_view text) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.starts_with('+')) line.remove_prefix(1);
        constexpr std::string_view key = "BuildVersion=";
        if (line.starts_with(key)) return Trim(line.substr(key.size())) == SupportedBuildVersion;
    }
    return false;
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
    } else if (name == "upscaler") {
        if (text == "off") settings.upscaler = Upscaler::Off;
        else if (text == "fsr" || text == "dlss") settings.warnings.push_back(origin + ": Upscaler = " + text + " is not implemented yet; using off");
        else refuse("off, fsr or dlss");
    } else if (name == "upscalerquality") {
        settings.upscalerQuality = text;
    } else if (name == "framegeneration") {
        if (text == "off" || text == "0") settings.frameGeneration = false;
        else settings.warnings.push_back(origin + ": FrameGeneration is not implemented yet; using off");
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
        line = Trim(line.substr(0, line.find_first_of("#;")));
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

inline void ApplyEnvironment(Settings& settings, const std::function<const char*(const char*)>& lookup) {
    static constexpr std::array<std::pair<const char*, const char*>, 7> variables{{
        {"APS5_RESOLUTION", "Resolution"}, {"APS5_RENDER_RESOLUTION", "RenderResolution"}, {"APS5_FPS_LIMIT", "FpsLimit"},
        {"APS5_TIMESTEP", "Timestep"}, {"APS5_TIMESTEP_MIN_HZ", "TimestepMinHz"}, {"APS5_WINDOW_MODE", "WindowMode"},
        {"APS5_UPSCALER", "Upscaler"}}};
    for (const auto& [variable, key] : variables) {
        const char* value = lookup(variable);
        if (value != nullptr && value[0] != '\0') SetKey(settings, key, value, variable);
    }
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
    if (const auto* tier = SelectRenderTier(settings)) args.push_back(std::string("+r_Wants4K=") + tier->convar);
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
    if (const auto* tier = SelectRenderTier(settings)) {
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

#endif
