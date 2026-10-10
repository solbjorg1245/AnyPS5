#include "prx/libc/include/General.hpp"
#include "prx/libc/include/PortSettings.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
}

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "port_settings_tests: %s\n", what);
    std::abort();
}

static bool Near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

static PortSettings::Settings FromText(const char* text) {
    auto settings = PortSettings::Parse(text, "test.ini");
    PortSettings::Normalize(settings);
    return settings;
}

static void TestParse() {
    const auto settings = FromText("\xEF\xBB\xBF# comment\r\n[display]\r\nResolution = 1280x800 ; Steam Deck\r\nfpslimit=45\r\nTimestep = variable\r\nWindowMode = Fullscreen\r\nUpscaler = off\r\n");
    Require(settings.warnings.empty(), "clean file warns");
    Require(settings.width == 1280 && settings.height == 800, "resolution");
    Require(settings.fpsLimit == 45.0 && settings.variableTimestep, "fps limit and timestep");
    Require(settings.windowMode == PortSettings::WindowMode::Fullscreen, "window mode");
    Require(settings.Any(), "Any");

    const auto bad = FromText("Resolution = 100x50\nFpsLimit = 500\nBogus = 1\nno separator\nUpscaler = dlss\n");
    Require(bad.width == 0 && bad.fpsLimit == 0.0, "refused values keep the defaults");
    Require(bad.upscaler == PortSettings::Upscaler::Off, "unimplemented upscaler stays off");
    Require(bad.warnings.size() == 5, "one warning per bad line");
    Require(!bad.Any(), "nothing set");

    Require(!FromText("").Any(), "empty file is today's behaviour");
    Require(PortSettings::GameArgs(FromText("")).empty(), "empty file has no game args");
}

static void TestEnvironment() {
    auto settings = PortSettings::Parse("Resolution = 1920x1080\nFpsLimit = 60\n", "test.ini");
    const std::map<std::string, std::string> environment{{"APS5_RESOLUTION", "1280x800"}, {"APS5_FPS_LIMIT", "40"}, {"APS5_TIMESTEP", "1"}};
    PortSettings::ApplyEnvironment(settings, [&](const char* name) -> const char* {
        const auto found = environment.find(name);
        return found == environment.end() ? nullptr : found->second.c_str();
    });
    PortSettings::Normalize(settings);
    Require(settings.width == 1280 && settings.height == 800, "environment overrides the file");
    Require(settings.fpsLimit == 40.0 && settings.variableTimestep, "environment fps and timestep");
}

static void TestRenderTier() {
    const auto tierFor = [](const char* resolution) {
        const auto* tier = PortSettings::SelectRenderTier(FromText((std::string("Resolution = ") + resolution).c_str()));
        return tier == nullptr ? std::string("none") : std::string(tier->convar);
    };
    Require(tierFor("1280x800") == "WantsPerf", "Steam Deck renders 900p (smallest tier)");
    Require(tierFor("1280x720") == "WantsPerf", "720p");
    Require(tierFor("1600x900") == "WantsPerf", "900p");
    Require(tierFor("1920x1080") == "WantsBase", "1080p");
    Require(tierFor("1920x1200") == "WantsBase", "16:10 1200p shows a 1080p image");
    Require(tierFor("2560x1080") == "WantsBase", "ultrawide 1080p");
    Require(tierFor("2560x1440") == "WantsHigh", "1440p");
    Require(tierFor("3840x2160") == "WantsHigh", "4K output stays WantsHigh unless RenderResolution = 2160");
    Require(tierFor("auto") == "none", "auto keeps the render config");
    const auto manual = FromText("Resolution = 1280x800\nRenderResolution = 2160\n");
    Require(std::string(PortSettings::SelectRenderTier(manual)->convar) == "WantsSuperHigh", "RenderResolution wins");
    Require(FromText("RenderResolution = 720").warnings.size() == 1, "unknown render tier refused");
}

static void TestFrameRate() {
    const auto fixed45 = FromText("FpsLimit = 45\n");
    Require(fixed45.fpsLimit == 60.0 && fixed45.warnings.size() == 1, "45 without the variable timestep falls back to 60");
    Require(PortSettings::VblankHz(fixed45) == 0.0, "60 keeps the default vblank");

    const auto variable45 = FromText("FpsLimit = 45\nTimestep = variable\n");
    Require(PortSettings::GameFramesPerSecond(variable45) == 60 && PortSettings::VblankHz(variable45) == 45.0, "45: 60 FPS mode, 45 Hz vblank");

    const auto thirty = FromText("FpsLimit = 30\n");
    Require(thirty.warnings.empty() && PortSettings::GameFramesPerSecond(thirty) == 30 && PortSettings::VblankHz(thirty) == 0.0, "30 is the title's mode");
    Require(PortSettings::GameFramesPerSecond(FromText("FpsLimit = 59.94\n")) == 60, "59.94 is the 60 mode");
    Require(PortSettings::GameFramesPerSecond(FromText("FpsLimit = auto\n")) == 0, "auto");

    const auto args = PortSettings::GameArgs(FromText("Resolution = 1280x800\nFpsLimit = 30\n"));
    Require(args.size() == 2 && args[0] == "+r_Wants4K=WantsPerf" && args[1] == "+TargetFramesPerSecond=30", "game args");
    Require(PortSettings::AppendLines("+a = 1\r\n+b = 2", args) == "+a = 1\r\n+b = 2\r\n+r_Wants4K=WantsPerf\r\n+TargetFramesPerSecond=30\r\n", "render config append");
    Require(PortSettings::AppendWords("+x=false", args) == "+x=false +r_Wants4K=WantsPerf +TargetFramesPerSecond=30", "command line append");
}

static void TestPacer() {
    constexpr std::uint64_t ms = 1000000ULL;
    PortSettings::TimestepPacer pacer(PortSettings::DefaultTimestepMinHz, 45.0);
    Require(Near(pacer.Flip(1000 * ms), 45.0, 1e-9), "first flip assumes the cap");
    // A steady 40 FPS converges to 40 Hz.
    std::uint64_t now = 1000 * ms;
    for (int frame = 0; frame < 60; ++frame) pacer.Flip(now += 25 * ms);
    Require(Near(pacer.Rate(), 40.0, 0.01), "steady 40 FPS");
    // Faster than the cap clamps to the cap, slower than the floor to the floor.
    for (int frame = 0; frame < 60; ++frame) pacer.Flip(now += 10 * ms);
    Require(Near(pacer.Rate(), 45.0, 0.01), "capped at the limit");
    for (int frame = 0; frame < 60; ++frame) pacer.Flip(now += 250 * ms);
    Require(Near(pacer.Rate(), PortSettings::DefaultTimestepMinHz, 0.01) && pacer.Floored(), "floored at TimestepMinHz");
    // A single hitch moves the step by a quarter of the (clamped) difference only.
    for (int frame = 0; frame < 60; ++frame) pacer.Flip(now += 25 * ms);
    pacer.Flip(now += 1000 * ms);
    Require(pacer.Rate() > 31.0 && pacer.Rate() < 33.0, "hitch smoothed");
    // Over a variable run the steps add up to the elapsed time (the game speed stays 1x).
    PortSettings::TimestepPacer timed(PortSettings::DefaultTimestepMinHz, 60.0);
    now = 5000 * ms;
    double gameSeconds = 0.0;
    double step = 1.0 / timed.Flip(now);
    const std::uint64_t pattern[] = {16, 20, 33, 25, 17, 30, 22, 28};
    for (int frame = 0; frame < 4000; ++frame) {
        now += pattern[frame % 8] * ms;
        gameSeconds += step;
        step = 1.0 / timed.Flip(now);
    }
    Require(Near(gameSeconds / (static_cast<double>(now - 5000 * ms) / 1e9), 1.0, 0.01), "variable steps keep the game speed");
}

static std::string ReadGuest(const char* path) {
    const int fd = sceKernelOpen(path, SCE_KERNEL_O_RDONLY, 0);
    Require(fd >= 0, "open guest file");
    std::string text(4096, '\0');
    const auto size = sceKernelRead(fd, text.data(), text.size());
    Require(size >= 0 && sceKernelClose(fd) == 0, "read guest file");
    text.resize(static_cast<std::size_t>(size));
    return text;
}

static void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, "set variable");
#else
    Require(setenv(name, value, 1) == 0, "set variable");
#endif
}

// The overlay: settings file -> merged render config and command line, served for the guest paths.
static void TestOverlay() {
    const auto root = std::filesystem::canonical(std::filesystem::current_path());
    Require(!std::filesystem::exists(root / "app0"), "run from a directory without app0");
    const auto settingsPath = root / ("anyps5-settings-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    { std::ofstream file(settingsPath, std::ios::binary); file << "Resolution = 1280x800\nFpsLimit = 30\n"; }
    std::filesystem::create_directories(root / "app0" / "misc");
    { std::ofstream file(root / "app0" / "PackageCmdLineArgs.txt", std::ios::binary); file << "+cp11_callHomeLockDownEnabled=false"; }
    { std::ofstream file(root / "app0" / "misc" / "renderconfig_ps5_perf.txt", std::ios::binary); file << "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60"; }
    { std::ofstream file(root / "app0" / "misc" / "gamecommandlineargs.txt", std::ios::binary); file << "+untouched"; }
    SetVariable("APS5_SETTINGS", settingsPath.string().c_str());

    Require(ReadGuest("/app0/misc/RenderConfig_PS5_Perf.txt") == "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60\r\n+r_Wants4K=WantsPerf\r\n+TargetFramesPerSecond=30\r\n", "render config overlay");
    Require(ReadGuest("/app0/packagecmdlineargs.txt") == "+cp11_callHomeLockDownEnabled=false +r_Wants4K=WantsPerf +TargetFramesPerSecond=30", "command line overlay");
    Require(ReadGuest("/app0/misc/gamecommandlineargs.txt") == "+untouched", "other files untouched");
    Require(GetPortSettings_nid_no_patch().width == 1280, "process settings");

    std::filesystem::remove_all(root / "app0");
    std::filesystem::remove(settingsPath);
}

// Review fixes: number parsing, build identity, atomic writes, override folders, non-ASCII paths.
static void TestReviewFixes() {
    double value = 0.0;
    Require(PortSettings::ParseNumber("29.97", value) && Near(value, 29.97, 1e-9), "decimal number");
    Require(!PortSettings::ParseNumber("0x14", value), "hex refused");
    Require(!PortSettings::ParseNumber("29,97", value), "comma refused");
    Require(!PortSettings::ParseNumber("20abc", value) && !PortSettings::ParseNumber("", value) && !PortSettings::ParseNumber("nan", value), "junk refused");

    Require(PortSettings::BuildVersionSupported("+BuildVersion=2025-10-15.877562\r\n\r\n"), "supported build");
    Require(PortSettings::BuildVersionSupported("BuildVersion=2025-10-15.877562"), "supported build, no plus");
    Require(!PortSettings::BuildVersionSupported("+BuildVersion=2025-10-15.877563\r\n"), "other build refused");
    Require(!PortSettings::BuildVersionSupported("") && !PortSettings::BuildVersionSupported("something else"), "no build refused");

    const auto windows = PortSettings::UserDirectory(true, [](const char* name) { return std::string(name) == "LOCALAPPDATA" ? std::filesystem::path("C:/Users/x/AppData/Local") : std::filesystem::path{}; });
    Require(windows == std::filesystem::path("C:/Users/x/AppData/Local") / "AnyPS5", "windows user folder");
    const auto xdg = PortSettings::UserDirectory(false, [](const char* name) { return std::string(name) == "XDG_CONFIG_HOME" ? std::filesystem::path("/cfg") : std::filesystem::path("/home/x"); });
    Require(xdg == std::filesystem::path("/cfg") / "anyps5", "XDG folder");
    const auto home = PortSettings::UserDirectory(false, [](const char* name) { return std::string(name) == "HOME" ? std::filesystem::path("/home/x") : std::filesystem::path{}; });
    Require(home == std::filesystem::path("/home/x") / ".config" / "anyps5", "home folder");
    Require(PortSettings::UserDirectory(false, [](const char*) { return std::filesystem::path{}; }).empty(), "no user folder");
    Require(PortSettings::OverrideDirectories("/exe", {}).size() == 1 && PortSettings::OverrideDirectories("/exe", "/user").size() == 2, "override folders");

    // A non-ASCII folder: written atomically, read back, no temporary left behind, no narrow round trip.
    const auto base = std::filesystem::temp_directory_path() / std::filesystem::path(u8"aps5-\u00e4\u00f6-test");
    std::filesystem::remove_all(base);
    std::string error;
    std::filesystem::create_directories(base);
    { std::ofstream file(base / "blocked-by-file", std::ios::binary); file << "x"; }
    const auto written = PortSettings::WriteOverrideFile({base / "blocked-by-file" / "inner", base / "ok"}, "a.txt", "first", error);
    Require(written == base / "ok" / "a.txt", "falls through to the next folder");
    Require(PortSettings::WriteFileAtomic(written, "second", error), "overwrite");
    { std::ifstream file(written, std::ios::binary); std::string text((std::istreambuf_iterator<char>(file)), {}); Require(text == "second", "overwritten content"); }
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(base / "ok")) ++entries;
    Require(entries == 1, "no temporary file left");
    Require(!PortSettings::PathText(base).empty() && PortSettings::PathText(base).find('?') == std::string::npos, "path text keeps non-ASCII");
    std::filesystem::remove_all(base);
}

int main() {
    TestReviewFixes();
    TestParse();
    TestEnvironment();
    TestRenderTier();
    TestFrameRate();
    TestPacer();
    TestOverlay();
    std::printf("port_settings_tests: ok\n");
    return 0;
}
