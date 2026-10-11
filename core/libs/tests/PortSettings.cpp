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
    Require(args.size() == 2 && args[0] == "+r_Wants4K=WantsHigh" && args[1] == "+TargetFramesPerSecond=30", "game args (4K UI slot, r6)");
    Require(PortSettings::AppendLines("+a = 1\r\n+b = 2", args) == "+a = 1\r\n+b = 2\r\n+r_Wants4K=WantsHigh\r\n+TargetFramesPerSecond=30\r\n", "render config append");
    Require(PortSettings::AppendWords("+x=false", args) == "+x=false +r_Wants4K=WantsHigh +TargetFramesPerSecond=30", "command line append");
}

static void TestWindowSize() {
    const auto size = [](std::uint32_t w, std::uint32_t h, std::uint32_t uw, std::uint32_t uh) {
        const auto client = PortSettings::WindowClientSize(w, h, uw, uh, 320, 180);
        return std::to_string(client.width) + "x" + std::to_string(client.height);
    };
    Require(size(1280, 800, 2560, 1392) == "1280x800", "1280x800 fits a 1440p work area as is");
    Require(size(1280, 800, 1280, 800) == "1280x800", "Steam Deck: exactly the screen");
    Require(size(1920, 1080, 1920, 1032) == "1835x1032", "taller than the work area: shrunk, aspect kept");
    Require(size(3840, 2160, 2560, 1392) == "2475x1392", "4K on a 1440p work area");
    Require(size(1280, 800, 1024, 1392) == "1024x640", "wider than the work area");
    Require(size(3840, 400, 1920, 1032) == "1920x200", "extreme aspect contained");
    Require(size(7680, 400, 1920, 1032) == "1920x180", "below the minimum height: raised");
    Require(size(1280, 800, 0, 0) == "1280x800", "no usable area: the request");

    const auto with = FromText("Resolution = 1280x800\n");
    const auto without = FromText("");
    Require(PortSettings::DpiAwareWindow(with, nullptr) && PortSettings::DpiAwareWindow(with, "1"), "Resolution makes the window DPI aware");
    Require(!PortSettings::DpiAwareWindow(with, "0"), "APS5_DPI_AWARE=0 keeps it unaware");
    Require(!PortSettings::DpiAwareWindow(without, nullptr) && !PortSettings::DpiAwareWindow(without, "1"), "no Resolution: unchanged");
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

    Require(ReadGuest("/app0/misc/RenderConfig_PS5_Perf.txt") == "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60\r\n+r_Wants4K=WantsHigh\r\n+TargetFramesPerSecond=30\r\n", "render config overlay");
    Require(ReadGuest("/app0/packagecmdlineargs.txt") == "+cp11_callHomeLockDownEnabled=false +r_Wants4K=WantsHigh +TargetFramesPerSecond=30", "command line overlay");
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

static void TestNativeAspect() {
    const auto planFor = [](const std::string& text) { return PortSettings::PlanNativeAspect(FromText(text.c_str())); };
    const auto native = [&](const char* resolution) { return planFor(std::string("NativeAspect = on\nResolution = ") + resolution); };
    const auto is = [](const PortSettings::NativeAspectPlan& plan, std::uint32_t width, std::uint32_t height, const char* convar) {
        return plan.Active() && plan.renderWidth == width && plan.renderHeight == height && std::string(plan.slot->convar) == convar;
    };
    Require(!planFor("Resolution = 2560x1080").Active(), "off by default");
    Require(PortSettings::GameArgs(FromText("Resolution = 2560x1080"))[0] == "+r_Wants4K=WantsHigh", "off: the WantsBase tier in the 4K UI slot (r6)");
    Require(is(native("2560x1080"), 2560, 1080, "WantsHigh"), "21:9 1080p renders 2560x1080 in the 4K-display slot");
    Require(is(native("1280x800"), 1280, 800, "WantsHigh"), "Steam Deck renders 1280x800 in the 4K-display slot (4K UI)");
    Require(is(native("1920x1080"), 1920, 1080, "WantsHigh"), "16:9 1080p renders 1080p in the 4K-display slot (4K UI)");
    Require(is(native("1024x768"), 1024, 768, "WantsHigh"), "4:3 in the 4K-display slot");
    Require(native("1280x800").slot->displayHeight == 2160 && native("1920x1200").slot->displayHeight == 2160, "UI on a 2160p display buffer");
    Require(is(native("3840x2160"), 2560, 1440, "WantsHigh"), "4K renders 1440p as before");
    Require(is(native("1920x1200"), 1920, 1200, "WantsHigh"), "16:10 1200p");
    Require(is(native("3440x1440"), 3440, 1440, "WantsSuperHigh"), "21:9 1440p needs the 3840-wide slot");
    Require(is(native("5120x1440"), 3840, 1080, "WantsSuperHigh"), "32:9 is held to 3840 wide");
    Require(is(planFor("NativeAspect = on\nResolution = 2560x1080\nRenderResolution = 900"), 2136, 900, "WantsHigh"), "RenderResolution sets the height");
    Require(std::abs(native("2560x1080").Aspect() - 2560.0f / 1080.0f) < 1e-6f, "aspect is the output's");
    Require(PortSettings::GameArgs(FromText("NativeAspect = on\nResolution = 2560x1080"))[0] == "+r_Wants4K=WantsHigh", "native slot forced");
    Require(PortSettings::Describe(FromText("NativeAspect = on\nResolution = 2560x1080")).find("native aspect 2.3704: render 2560x1080 in the r_Wants4K=WantsHigh slot") != std::string::npos, "describe");

    const auto alone = FromText("NativeAspect = on");
    Require(!alone.nativeAspect && alone.warnings.size() == 1 && !PortSettings::PlanNativeAspect(alone).Active(), "needs Resolution");
    Require(!FromText("NativeAspect = maybe").nativeAspect && FromText("NativeAspect = maybe").warnings.size() == 1, "bad value refused");
    auto environment = PortSettings::Parse("Resolution = 2560x1080\n", "test.ini");
    PortSettings::ApplyEnvironment(environment, [](const char* name) -> const char* { return std::string(name) == "APS5_NATIVE_ASPECT" ? "1" : nullptr; });
    PortSettings::Normalize(environment);
    Require(environment.nativeAspect && environment.Any(), "APS5_NATIVE_ASPECT");

    // APS5_NO_NATIVE_ASPECT_4K_UI=1 / APS5_NATIVE_ASPECT_4K_UI=0: the smallest slot holding the render size (s60).
    const auto smallUi = [](const char* resolution, const char* variable, const char* value) {
        auto settings = PortSettings::Parse(std::string("NativeAspect = on\nResolution = ") + resolution + "\n", "test.ini");
        PortSettings::ApplyEnvironment(settings, [&](const char* name) -> const char* { return std::string(name) == variable ? value : nullptr; });
        PortSettings::Normalize(settings);
        return PortSettings::PlanNativeAspect(settings);
    };
    Require(is(smallUi("1280x800", "APS5_NO_NATIVE_ASPECT_4K_UI", "1"), 1280, 800, "WantsPerf"), "APS5_NO_NATIVE_ASPECT_4K_UI=1: Steam Deck in the 1080p-display slot");
    Require(is(smallUi("1920x1080", "APS5_NATIVE_ASPECT_4K_UI", "0"), 1920, 1080, "WantsBase"), "APS5_NATIVE_ASPECT_4K_UI=0: 16:9 1080p is the WantsBase tier");
    Require(is(smallUi("1280x800", "APS5_NO_NATIVE_ASPECT_4K_UI", "0"), 1280, 800, "WantsHigh"), "APS5_NO_NATIVE_ASPECT_4K_UI=0 keeps the 4K UI");
    Require(is(smallUi("2560x1080", "APS5_NO_NATIVE_ASPECT_4K_UI", "1"), 2560, 1080, "WantsHigh"), "21:9 1080p is WantsHigh either way");
    Require(is(smallUi("3440x1440", "APS5_NO_NATIVE_ASPECT_4K_UI", "1"), 3440, 1440, "WantsSuperHigh"), "3440x1440 is WantsSuperHigh either way");
    Require(PortSettings::GameArgs(FromText("NativeAspect = on\nResolution = 1280x800"))[0] == "+r_Wants4K=WantsHigh", "4K UI slot forced");

    // The table rewrite touches only the slot's render width and height.
    auto table = PortSettings::TierTable;
    for (const auto& write : PortSettings::TierTableWrites(native("2560x1080"))) table[write.entry] = write.value;
    Require(table[2] == 2560 && table[7] == 1080, "WantsHigh render entries");
    for (std::size_t entry = 0; entry < table.size(); ++entry) {
        if (entry != 2 && entry != 7) Require(table[entry] == PortSettings::TierTable[entry], "other entries untouched");
    }
    const auto deck = PortSettings::TierTableWrites(native("1280x800"));
    Require(deck[0].entry == 2 && deck[0].value == 1280 && deck[1].entry == 7 && deck[1].value == 800, "WantsHigh render entries at 16:10");
    const auto smallDeck = PortSettings::TierTableWrites(smallUi("1280x800", "APS5_NO_NATIVE_ASPECT_4K_UI", "1"));
    Require(smallDeck[0].entry == 0 && smallDeck[0].value == 1280 && smallDeck[1].entry == 5 && smallDeck[1].value == 800, "WantsPerf render entries");
    Require(PortSettings::AspectBits(16.0f / 9.0f) == PortSettings::Aspect16x9Bits, "16:9 bits");

    // The aspect field follows the display mode, and is left alone when it holds anything else.
    const auto plan = native("2560x1080");
    const auto wide = PortSettings::AspectBits(plan.Aspect());
    Require(PortSettings::DisplayAspectBits(PortSettings::Aspect16x9Bits, 3, plan) == wide, "slot mode: native aspect");
    Require(PortSettings::DisplayAspectBits(wide, 3, plan) == wide, "kept");
    Require(PortSettings::DisplayAspectBits(wide, 2, plan) == PortSettings::Aspect16x9Bits, "other mode: back to 16:9");
    Require(PortSettings::DisplayAspectBits(PortSettings::Aspect16x9Bits, 2, plan) == PortSettings::Aspect16x9Bits, "other mode: 16:9 kept");
    Require(!PortSettings::DisplayAspectBits(0x3f800000, 3, plan), "unknown value left alone");
    Require(!PortSettings::DisplayAspectBits(PortSettings::Aspect16x9Bits, 9, plan), "unknown mode left alone");

    // Part 2 (experimental halves, off by default): the shared targets at the render size, the display
    // buffer at the output's shape inside the slot's display size; nothing where the slot's own size
    // already is that, or SetMode never runs. Checked with the s60 slot choice (APS5_NO_NATIVE_ASPECT_4K_UI=1);
    // `fourK` the default 4K-UI slot.
    const auto halves = [](const char* resolution, bool render, bool ui, bool fourK = false) {
        auto settings = PortSettings::Parse(std::string("NativeAspect = on\nResolution = ") + resolution + "\n", "test.ini");
        PortSettings::ApplyEnvironment(settings, [&](const char* name) -> const char* {
            if (std::string(name) == "APS5_NATIVE_ASPECT_RENDER") return render ? "1" : "0";
            if (std::string(name) == "APS5_NATIVE_ASPECT_UI") return ui ? "1" : nullptr;
            if (std::string(name) == "APS5_NO_NATIVE_ASPECT_4K_UI") return fourK ? nullptr : "1";
            return nullptr;
        });
        PortSettings::Normalize(settings);
        return PortSettings::PlanNativeAspect(settings);
    };
    const auto targets = [&](const char* resolution, std::uint32_t sharedWidth, std::uint32_t sharedHeight, std::uint32_t displayWidth, std::uint32_t displayHeight) {
        const auto result = halves(resolution, true, true);
        return result.sharedWidth == sharedWidth && result.sharedHeight == sharedHeight && result.displayWidth == displayWidth && result.displayHeight == displayHeight;
    };
    Require(!native("2560x1080").SharedNative() && !native("2560x1080").DisplayNative() && !native("3440x1440").SharedNative(), "NativeAspect alone is part 1");
    Require(targets("2560x1080", 2560, 1080, 3840, 1620), "21:9 in WantsHigh: shared 2560x1080, display 3840x1620");
    Require(targets("1280x800", 1280, 800, 1728, 1080), "16:10 in WantsPerf: shared 1280x800, display 1728x1080");
    Require(targets("1920x1200", 1920, 1200, 3456, 2160), "16:10 in WantsHigh: display 3456x2160");
    Require(targets("1024x768", 1024, 768, 1440, 1080), "4:3 in WantsPerf: display 1440x1080");
    Require(targets("3440x1440", 3440, 1440, 3440, 1440), "WantsSuperHigh: one size for both");
    Require(targets("5120x1440", 3840, 1080, 3840, 1080), "32:9: 3840x1080 for both");
    Require(targets("3840x2160", 0, 0, 0, 0), "16:9 4K: the slot's own sizes");
    Require(targets("1920x1080", 0, 0, 0, 0), "WantsBase: Init's sizes kept");
    const auto deck4k = halves("1280x800", true, true, true);
    Require(std::string(deck4k.slot->convar) == "WantsHigh" && deck4k.sharedWidth == 1280 && deck4k.sharedHeight == 800 && deck4k.displayWidth == 3456
        && deck4k.displayHeight == 2160, "4K UI: 16:10 in WantsHigh, display 3456x2160");
    const auto base4k = halves("1920x1080", true, true, true);
    Require(std::string(base4k.slot->convar) == "WantsHigh" && base4k.sharedWidth == 1920 && base4k.sharedHeight == 1080 && !base4k.DisplayNative(), "4K UI: 16:9 1080p keeps the 16:9 display");
    for (const char* resolution : {"2560x1080", "1280x800", "1920x1200", "3440x1440", "5120x1440", "1024x768", "800x1280", "7680x2160"}) {
        const auto result = halves(resolution, true, true);
        Require(result.Active() && result.sharedWidth <= 3840 && result.sharedHeight <= 2160 && result.displayWidth <= 3840 && result.displayHeight <= 2160
            && result.displayWidth % 8 == 0 && result.displayHeight % 4 == 0, "every size within 3840x2160, aligned");
    }
    const auto renderOnly = halves("2560x1080", true, false);
    Require(renderOnly.SharedNative() && !renderOnly.DisplayNative() && renderOnly.renderWidth == 2560, "APS5_NATIVE_ASPECT_RENDER alone");
    const auto uiOnly = halves("2560x1080", false, true);
    Require(!uiOnly.SharedNative() && uiOnly.DisplayNative(), "APS5_NATIVE_ASPECT_UI alone ('0' keeps the render half off)");
    const auto superRenderOnly = halves("3440x1440", true, false);
    Require(superRenderOnly.Active() && !superRenderOnly.SharedNative() && !superRenderOnly.DisplayNative(), "WantsSuperHigh needs both halves");
    Require(PortSettings::TierTableWrites(halves("2560x1080", true, true)).size() == 2, "display entries only once the display buffer is patched");
    auto displayTable = PortSettings::TierTable;
    for (const auto& write : PortSettings::TierTableWrites(halves("2560x1080", true, true), true)) displayTable[write.entry] = write.value;
    Require(displayTable[2] == 2560 && displayTable[7] == 1080 && displayTable[12] == 3840 && displayTable[17] == 1620, "WantsHigh render and display entries");
    for (std::size_t entry = 0; entry < displayTable.size(); ++entry) {
        if (entry != 2 && entry != 7 && entry != 12 && entry != 17) Require(displayTable[entry] == PortSettings::TierTable[entry], "other entries untouched");
    }
    Require(PortSettings::Describe(FromText("NativeAspect = on\nResolution = 2560x1080")).find("(shared targets 2560x1440, display buffer 3840x2160)") != std::string::npos, "describe: part 1 by default");

    // The UI rect: on inside NativeAspect, off without it or with the display half; the environment
    // turns it off (APS5_NO_NATIVE_ASPECT_UI_RECT) or forces it (APS5_NATIVE_ASPECT_UI_RECT).
    const auto wideSettings = FromText("NativeAspect = on\nResolution = 2560x1080");
    const auto plainSettings = FromText("Resolution = 2560x1080");
    Require(PortSettings::NativeAspectUiRect(wideSettings, nullptr, nullptr), "UI rect on inside NativeAspect");
    Require(PortSettings::NativeAspectUiRect(FromText("NativeAspect = on\nResolution = 1280x800"), nullptr, ""), "UI rect on at 16:10; an empty APS5_NO_NATIVE_ASPECT_UI_RECT is unset");
    Require(!PortSettings::NativeAspectUiRect(plainSettings, nullptr, nullptr), "UI rect off without NativeAspect");
    Require(!PortSettings::NativeAspectUiRect(wideSettings, nullptr, "1"), "APS5_NO_NATIVE_ASPECT_UI_RECT=1");
    Require(PortSettings::NativeAspectUiRect(wideSettings, nullptr, "0"), "APS5_NO_NATIVE_ASPECT_UI_RECT=0 keeps it");
    Require(!PortSettings::NativeAspectUiRect(wideSettings, "0", nullptr), "APS5_NATIVE_ASPECT_UI_RECT=0");
    Require(PortSettings::NativeAspectUiRect(plainSettings, "1", nullptr), "APS5_NATIVE_ASPECT_UI_RECT=1 forces it as before");
    Require(!PortSettings::NativeAspectUiRect(wideSettings, "1", "1"), "APS5_NO_NATIVE_ASPECT_UI_RECT wins");
    auto uiHalf = PortSettings::Parse("NativeAspect = on\nResolution = 2560x1080\n", "test.ini");
    PortSettings::ApplyEnvironment(uiHalf, [](const char* name) -> const char* { return std::string(name) == "APS5_NATIVE_ASPECT_UI" ? "1" : nullptr; });
    PortSettings::Normalize(uiHalf);
    Require(!PortSettings::NativeAspectUiRect(uiHalf, nullptr, nullptr), "the display half leaves nothing to move");
}

// Plain Resolution's 4K UI (r6): a tier up to 1080p runs in the WantsHigh slot at the tier's render size.
static void TestResolution4kUi() {
    const auto withEnvironment = [](const std::string& text, const char* variable, const char* value) {
        auto settings = PortSettings::Parse(text, "test.ini");
        PortSettings::ApplyEnvironment(settings, [&](const char* name) -> const char* { return std::string(name) == variable ? value : nullptr; });
        PortSettings::Normalize(settings);
        return settings;
    };
    const auto is = [](const PortSettings::NativeAspectPlan& plan, std::uint32_t width, std::uint32_t height, const char* convar) {
        return plan.Active() && plan.uiOnly && plan.renderWidth == width && plan.renderHeight == height && std::string(plan.slot->convar) == convar
            && plan.slot->displayHeight == 2160 && plan.Aspect() == 16.0f / 9.0f && !plan.SharedNative() && !plan.DisplayNative();
    };
    const auto ui = [](const char* text) { return PortSettings::PlanResolution4kUi(FromText(text)); };
    Require(is(ui("Resolution = 1920x1080"), 1920, 1080, "WantsHigh"), "1080p: WantsBase's size in the 4K-display slot");
    Require(is(ui("Resolution = 1280x720"), 1600, 900, "WantsHigh"), "720p: WantsPerf's size in the 4K-display slot");
    Require(is(ui("Resolution = 1280x800"), 1600, 900, "WantsHigh"), "Steam Deck: WantsPerf's size");
    Require(is(ui("Resolution = 2560x1080"), 1920, 1080, "WantsHigh"), "ultrawide 1080p: WantsBase's size");
    Require(is(ui("RenderResolution = 900"), 1600, 900, "WantsHigh"), "RenderResolution = 900 alone");
    Require(is(ui("Resolution = 3840x2160\nRenderResolution = 1080"), 1920, 1080, "WantsHigh"), "RenderResolution = 1080 at 4K");
    Require(!ui("Resolution = 2560x1440").Active() && !ui("Resolution = 3840x2160").Active(), "1440p and up keep their own slot");
    Require(!ui("Resolution = 1280x720\nRenderResolution = 2160").Active(), "RenderResolution = 2160 keeps WantsSuperHigh");
    Require(!ui("").Active() && PortSettings::GameArgs(FromText("")).empty(), "no Resolution: nothing");
    Require(!ui("NativeAspect = on\nResolution = 1920x1080").Active(), "NativeAspect plans its own slot");
    Require(!PortSettings::PlanTierRewrite(FromText("NativeAspect = on\nResolution = 1280x800")).uiOnly, "native aspect's plan wins");
    Require(PortSettings::PlanTierRewrite(FromText("Resolution = 1920x1080")).uiOnly, "plain: the 4K UI plan");
    Require(PortSettings::GameArgs(FromText("Resolution = 1920x1080"))[0] == "+r_Wants4K=WantsHigh", "1080p forces WantsHigh");
    Require(PortSettings::GameArgs(FromText("Resolution = 2560x1440"))[0] == "+r_Wants4K=WantsHigh", "1440p unchanged");
    Require(PortSettings::Describe(FromText("Resolution = 1280x720")).find("render 1600x900 (the WantsPerf tier) in the r_Wants4K=WantsHigh slot (display buffer 3840x2160: 4K UI") != std::string::npos, "describe");
    Require(!PortSettings::NativeAspectUiRect(FromText("Resolution = 1280x720"), nullptr, nullptr), "no UI rect: the image stays 16:9");
    const auto writes = PortSettings::TierTableWrites(ui("Resolution = 1280x720"));
    Require(writes.size() == 2 && writes[0].entry == 2 && writes[0].value == 1600 && writes[1].entry == 7 && writes[1].value == 900, "WantsHigh render entries take the tier's size");

    // APS5_RESOLUTION_4K_UI=0 / APS5_NO_RESOLUTION_4K_UI=1: the tier's own slot (s58-s61).
    const auto off = withEnvironment("Resolution = 1280x720\n", "APS5_NO_RESOLUTION_4K_UI", "1");
    Require(!off.resolution4kUi && !PortSettings::PlanResolution4kUi(off).Active() && PortSettings::GameArgs(off)[0] == "+r_Wants4K=WantsPerf", "APS5_NO_RESOLUTION_4K_UI=1: WantsPerf");
    const auto offBase = withEnvironment("Resolution = 1920x1080\n", "APS5_RESOLUTION_4K_UI", "0");
    Require(PortSettings::GameArgs(offBase)[0] == "+r_Wants4K=WantsBase", "APS5_RESOLUTION_4K_UI=0: WantsBase");
    Require(PortSettings::Describe(offBase).find("render r_Wants4K=WantsBase (1920x1080)") != std::string::npos, "describe: the old line");
    Require(withEnvironment("Resolution = 1920x1080\n", "APS5_NO_RESOLUTION_4K_UI", "0").resolution4kUi, "APS5_NO_RESOLUTION_4K_UI=0 keeps it");
    Require(withEnvironment("Resolution = 1920x1080\n", "APS5_RESOLUTION_4K_UI", "1").resolution4kUi, "APS5_RESOLUTION_4K_UI=1");
    const auto nativeOff = withEnvironment("NativeAspect = on\nResolution = 1280x800\n", "APS5_NO_RESOLUTION_4K_UI", "1");
    Require(std::string(PortSettings::PlanTierRewrite(nativeOff).slot->convar) == "WantsHigh", "native aspect's 4K UI is its own switch");
}

int main() {
    TestReviewFixes();
    TestResolution4kUi();
    TestNativeAspect();
    TestParse();
    TestEnvironment();
    TestRenderTier();
    TestFrameRate();
    TestPacer();
    TestWindowSize();
    TestOverlay();
    std::printf("port_settings_tests: ok\n");
    return 0;
}
