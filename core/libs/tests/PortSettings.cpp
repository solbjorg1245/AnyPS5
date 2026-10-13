#include "prx/libc/include/General.hpp"
#include "prx/libc/include/AddonContent.hpp"
#include "prx/libc/include/PortSettings.hpp"
#include "prx/libc/include/MovieWatch.hpp"
#include "prx/libSceVideoOut/include/TitleJobBudget.hpp"
#include "prx/libSceVideoOut/include/TitleRefreshField.hpp"
#include "prx/libSceVideoOut/include/TitleRefreshTable.hpp"
#include "prx/libSceVideoOut/include/TitleStepWindow.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "GameFolder.hpp"
#include "HostText.hpp"

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

// APS5_FRAME_PROBE's window (libSceVideoOut TimestepPatch): the title's own float step, read once per flip.
static void TestTitleStep() {
    const TitleStepWindow empty{};
    Require(empty.Steps() == 0 && empty.Unreadable() == 0 && empty.Speed(10.0) == 0.0 && empty.MinHz() == 0.0 && empty.MaxHz() == 0.0, "empty window");
    // Ten seconds of flips at flipHz, each reading a step of 1/stepHz.
    const auto speed = [](double stepHz, double flipHz) {
        TitleStepWindow window;
        const long flips = std::lround(flipHz * 10.0);
        for (long flip = 0; flip < flips; ++flip) window.Add(static_cast<float>(1.0 / stepHz));
        return window.Speed(static_cast<double>(flips) / flipHz);
    };
    Require(Near(speed(PortSettings::GameRefresh60, PortSettings::GameRefresh60), 1.0, 0.001), "60 FPS mode at the 59.94 Hz vblank: real time");
    Require(Near(speed(120.0, 120.0), 1.0, 0.001), "1/120 s steps at 120 flips/s: real time");
    Require(Near(speed(PortSettings::GameRefresh60, 120.0), 120.0 / PortSettings::GameRefresh60, 0.001), "1/59.94 s steps at 120 flips/s (APS5_VBLANK_HZ alone): 2x");
    TitleStepWindow mixed;
    for (const double value : {0.0, -1.0 / 60.0, 0.0005, 1.5, std::nan(""), HUGE_VAL}) mixed.Add(value);
    Require(mixed.Steps() == 0 && mixed.Unreadable() == 6 && mixed.Speed(1.0) == 0.0 && mixed.MaxHz() == 0.0, "implausible values ignored");
    for (const double hz : {120.0, 29.97, PortSettings::GameRefresh60}) mixed.Add(1.0 / hz);
    Require(mixed.Steps() == 3 && Near(mixed.MinHz(), 29.97, 1e-9) && Near(mixed.MaxHz(), 120.0, 1e-9), "step range");
    Require(Near(mixed.Speed(2.0), (1.0 / 120.0 + 1.0 / 29.97 + 1.0 / PortSettings::GameRefresh60) / 2.0, 1e-12), "speed sums the plausible steps only");
}

// APS5_TIMESTEP_FIELD's decision (libSceVideoOut TimestepPatch): the title's refresh field once per flip, with
// the table's 60 FPS entry at 41 Hz and its 30 FPS entry at 25 Hz.
static void TestRefreshField() {
    using Action = TitleRefreshField::Action;
    const auto decide = [](std::int32_t mode, float current, std::optional<float> known) { return TitleRefreshField::Decide(mode, current, known, 41.0f, 25.0f); };
    const auto first = decide(1, 59.94f, std::nullopt);
    Require(first.action == Action::Write && first.value == 41.0f && !first.titleRewrote, "60 FPS mode, first sight of 59.94: the 60 FPS entry");
    Require(decide(2, 29.97f, std::nullopt).action == Action::Write && decide(2, 29.97f, std::nullopt).value == 25.0f, "30 FPS mode: the 30 FPS entry");
    Require(decide(0, 59.94f, std::nullopt).action == Action::Write && decide(0, 59.94f, std::nullopt).value == 41.0f, "Unlocked (the 59.94 constant): the 60 FPS entry");
    Require(decide(3, 19.98f, std::nullopt).action == Action::OtherMode && decide(3, 19.98f, 41.0f).action == Action::OtherMode, "20 FPS mode: left alone");
    Require(decide(7, 41.0f, 41.0f).action == Action::OtherMode, "another enum once known: left alone");
    for (const float value : {1234.5f, std::nanf(""), 0.0f, 41.0f, 59.96f}) Require(decide(1, value, std::nullopt).action == Action::Refuse, "not a title refresh before the first write: refused");
    Require(decide(4, 59.94f, std::nullopt).action == Action::Refuse && decide(-1, 19.98f, std::nullopt).action == Action::Refuse, "enum outside 0-3 before the first write: refused");
    Require(decide(1, 29.975f, std::nullopt).action == Action::Write, "a title refresh within 0.01");
    const auto rewrite = decide(2, 29.97f, 41.0f);
    Require(rewrite.action == Action::Write && rewrite.value == 25.0f && rewrite.titleRewrote, "the title's rewrite at a mode change: written and counted");
    const auto copied = decide(1, 37.5f, 41.0f);
    Require(copied.action == Action::Write && copied.value == 41.0f && copied.titleRewrote, "the title's copy of an older entry: written and counted");
    const auto same = decide(1, 41.0f, 41.0f);
    Require(same.action == Action::AlreadySet && !same.titleRewrote, "equal value: no store");
    Require(TitleRefreshField::Decide(1, 59.94f, std::nullopt, 59.94f, 29.97f).action == Action::AlreadySet, "first sight already holding the entry: no store");
    Require(decide(1, std::nanf(""), 41.0f).action == Action::Write, "any value once known: written");
}

static void TestJobBudget() {
    using Action = TitleJobBudget::Action;
    const auto decide = [](double limit, float unlocked, float mode60) { return TitleJobBudget::Decide(limit, unlocked, mode60); };
    Require(TitleJobBudget::Bits(60.0f) == TitleJobBudget::UnlockedBits && TitleJobBudget::Bits(59.94f) == TitleJobBudget::Mode60Bits && TitleJobBudget::Bits(119.88f) == TitleJobBudget::PatchedMode60Bits,
        "the image's bits: 60.0, 59.94 and patch_game.py's 119.88");
    const auto at120 = decide(120.0, 60.0f, 59.94f);
    Require(at120.action == Action::Write && at120.value == 120.0f, "120 over the title's 60 / 59.94: 120 into both");
    Require(decide(144.0, 60.0f, 59.94f).action == Action::Write && decide(144.0, 60.0f, 59.94f).value == 144.0f, "144: 144");
    for (const double limit : {PortSettings::GameRefresh60, 60.0, 60.005, 30.0, 0.0, std::nan("")}) Require(decide(limit, 60.0f, 59.94f).action == Action::NotAbove, "at or below the title's 60: left alone");
    Require(decide(60.5, 60.0f, 59.94f).action == Action::Write, "just above 60: written");
    Require(decide(120.0, 60.0f, 119.88f).action == Action::Write && decide(120.0, 60.0f, 119.88f).value == 120.0f, "patch_game.py --fps120's 119.88: replaced by the limit");
    const auto already = decide(120.0, 120.0f, 120.0f);
    Require(already.action == Action::AlreadySet && already.value == 120.0f, "both holding the limit: already set");
    for (const auto& pair : {std::pair{59.94f, 59.94f}, std::pair{60.0f, 60.0f}, std::pair{60.0f, 59.95f}, std::pair{0.0f, 0.0f}, std::pair{120.0f, 59.94f}, std::pair{60.0f, 120.0f}, std::pair{std::nanf(""), 59.94f}})
        Require(decide(120.0, pair.first, pair.second).action == Action::Refuse, "anything else: refused");
}

// APS5_TABLE_COMPAT (libSceVideoOut TimestepPatch): the refresh table's two forms, the refresh field's guard on a
// patch_game.py --fps120 table and the game speed the fixed step gives such a table.
static void TestTableCompat() {
    using Form = TitleRefreshTable::Form;
    const float original[4] = {59.94f, 59.94f, 29.97f, 19.98f};
    const float patched[4] = {59.94f, 119.88f, 29.97f, 19.98f};
    Require(std::memcmp(original, TitleRefreshTable::Original.data(), sizeof(original)) == 0 && std::memcmp(patched, TitleRefreshTable::Fps120.data(), sizeof(patched)) == 0,
        "the signatures are the floats' bits");
    Require(TitleJobBudget::Bits(TitleRefreshTable::PatchedRefresh) == TitleJobBudget::PatchedMode60Bits, "patch_game.py's 119.88: 0x42efc28f");
    Require(TitleRefreshTable::Classify(TitleRefreshTable::Original.data()) == Form::Original, "the image's table: original");
    Require(TitleRefreshTable::Classify(TitleRefreshTable::Fps120.data()) == Form::Fps120, "patch_game.py --fps120's table: fps120");
    for (std::size_t bit = 0; bit < 128; ++bit) {
        auto bytes = TitleRefreshTable::Fps120;
        bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
        // Bit 55 is entry 1's lowest exponent bit: 119.88 back to 59.94.
        Require(TitleRefreshTable::Classify(bytes.data()) == (bit == 55 ? Form::Original : Form::None), "one bit off the fps120 form: none");
    }
    using Action = TitleRefreshField::Action;
    Require(TitleRefreshField::Decide(1, 119.88f, std::nullopt, 41.0f, 25.0f).action == Action::Refuse, "119.88 on an original table: refused");
    const auto patchedField = TitleRefreshField::Decide(1, 119.88f, std::nullopt, 41.0f, 25.0f, true);
    Require(patchedField.action == Action::Write && patchedField.value == 41.0f, "119.88 on a patched table: a title refresh, the 60 FPS entry");
    Require(TitleRefreshField::Decide(1, 119.885f, std::nullopt, 41.0f, 25.0f, true).action == Action::Write, "within 0.01 of 119.88");
    Require(TitleRefreshField::Decide(1, 119.9f, std::nullopt, 41.0f, 25.0f, true).action == Action::Refuse, "0.02 off 119.88: refused");
    Require(TitleRefreshField::Decide(2, 29.97f, std::nullopt, 41.0f, 25.0f, true).action == Action::Write, "the other refreshes still on a patched table");
    char text[16];
    const double slow = TitleRefreshTable::Speed(TitleRefreshTable::PatchedRefresh, PortSettings::GameRefresh60);
    std::snprintf(text, sizeof(text), "%.2f", slow);
    Require(std::string(text) == "0.50" && !TitleRefreshTable::RightSpeed(slow), "the 59.94 Hz vblank: 0.50x");
    const double right = TitleRefreshTable::Speed(TitleRefreshTable::PatchedRefresh, 120000.0 / 1001.0);
    std::snprintf(text, sizeof(text), "%.2f", right);
    Require(std::string(text) == "1.00" && TitleRefreshTable::RightSpeed(right), "the 119.88 Hz vblank: 1.00x");
    Require(TitleRefreshTable::Speed(0.0, 59.94) == 0.0, "no title refresh: 0");
}

static void TestPresentMode() {
    using PortSettings::PresentMode;
    Require(FromText("").presentMode == PresentMode::Auto, "PresentMode: auto by default");
    Require(FromText("PresentMode = fifo").presentMode == PresentMode::Fifo && FromText("PresentMode = Mailbox").presentMode == PresentMode::Mailbox, "PresentMode: fifo, mailbox (any case)");
    Require(FromText("presentmode = IMMEDIATE").presentMode == PresentMode::Immediate && FromText("PresentMode = fifo\nPresentMode = AUTO").presentMode == PresentMode::Auto, "PresentMode: immediate, auto");
    const auto bad = FromText("PresentMode = fifo\nPresentMode = vsync");
    Require(bad.presentMode == PresentMode::Fifo && bad.warnings.size() == 1 && bad.warnings[0].find("refused") != std::string::npos, "PresentMode: a refused value warns and keeps the previous one");
    const auto badOnly = FromText("PresentMode = vsync");
    Require(badOnly.presentMode == PresentMode::Auto && badOnly.warnings.size() == 1, "PresentMode: a refused value keeps auto");
    Require(!FromText("PresentMode = mailbox").Any(), "PresentMode alone is not Any()");
    Require(PortSettings::Describe(FromText("Resolution = 1920x1080\nFpsLimit = 120\nTimestep = variable\nPresentMode = mailbox")) == PortSettings::Describe(FromText("Resolution = 1920x1080\nFpsLimit = 120\nTimestep = variable")), "PresentMode leaves Describe byte-identical");

    auto settings = PortSettings::Parse("PresentMode = fifo\n", "test.ini");
    const std::map<std::string, std::string> environment{{"APS5_PRESENT_MODE", "mailbox"}};
    PortSettings::ApplyEnvironment(settings, [&](const char* name) -> const char* {
        const auto found = environment.find(name);
        return found == environment.end() ? nullptr : found->second.c_str();
    });
    Require(settings.presentMode == PresentMode::Mailbox && settings.warnings.empty(), "APS5_PRESENT_MODE overrides the file");
    auto refused = PortSettings::Parse("PresentMode = fifo\n", "test.ini");
    PortSettings::ApplyEnvironment(refused, [](const char* name) -> const char* { return std::string(name) == "APS5_PRESENT_MODE" ? "tearing" : nullptr; });
    Require(refused.presentMode == PresentMode::Fifo && refused.warnings.size() == 1 && refused.warnings[0].find("APS5_PRESENT_MODE") != std::string::npos, "an invalid APS5_PRESENT_MODE warns and keeps the file's");

    const auto choose = [](PresentMode wanted, double vblank, double display, bool mailbox, bool immediate, PresentMode expected) {
        const auto choice = PortSettings::ChoosePresentMode(wanted, vblank, display, mailbox, immediate);
        return choice.mode == expected && !choice.reason.empty();
    };
    Require(choose(PresentMode::Auto, 120.0, 60.0, true, true, PresentMode::Mailbox), "auto: vblank 120 on a 60 Hz display, mailbox offered");
    Require(PortSettings::ChoosePresentMode(PresentMode::Auto, 120.0, 60.0, true, false).reason == "auto: vblank 120.00 Hz above the display's 60 Hz", "auto: the reason");
    Require(choose(PresentMode::Auto, 120.0, 60.0, false, true, PresentMode::Fifo), "auto: mailbox not offered, fifo");
    Require(PortSettings::ChoosePresentMode(PresentMode::Auto, 120.0, 60.0, false, true).reason.find("mailbox is not offered") != std::string::npos, "auto: why not mailbox");
    Require(choose(PresentMode::Auto, 0.0, 60.0, true, true, PresentMode::Fifo), "auto: the default 59.94 on a 60 Hz display, fifo");
    Require(PortSettings::ChoosePresentMode(PresentMode::Auto, 0.0, 60.0, true, true).reason == "auto: vblank 59.94 Hz within the display's 60 Hz", "auto: the default vblank's reason");
    Require(choose(PresentMode::Auto, 60.0, 59.0, true, true, PresentMode::Fifo), "auto: within 1 Hz, fifo");
    Require(choose(PresentMode::Auto, 120.0, 0.0, true, true, PresentMode::Fifo), "auto: display refresh unknown, fifo");
    Require(choose(PresentMode::Auto, 120.0, 144.0, true, true, PresentMode::Fifo), "auto: vblank 120 on a 144 Hz display, fifo");
    for (const double vblank : {0.0, 20.0, 30.0, 60.0, 120.0, 240.0}) {
        for (const double display : {0.0, 30.0, 60.0, 144.0}) {
            for (const bool mailbox : {false, true}) Require(PortSettings::ChoosePresentMode(PresentMode::Auto, vblank, display, mailbox, true).mode != PresentMode::Immediate, "auto never tears");
        }
    }
    Require(choose(PresentMode::Immediate, 0.0, 60.0, true, true, PresentMode::Immediate), "immediate offered: immediate");
    Require(choose(PresentMode::Immediate, 0.0, 60.0, true, false, PresentMode::Fifo), "immediate not offered: fifo");
    Require(choose(PresentMode::Mailbox, 0.0, 60.0, true, false, PresentMode::Mailbox) && choose(PresentMode::Mailbox, 120.0, 60.0, false, true, PresentMode::Fifo), "mailbox: as asked when offered, else fifo");
    Require(PortSettings::ChoosePresentMode(PresentMode::Mailbox, 0.0, 60.0, false, true).reason == "PresentMode mailbox is not offered by the surface; using fifo", "mailbox not offered: the reason");
    Require(choose(PresentMode::Fifo, 120.0, 60.0, true, true, PresentMode::Fifo), "fifo: fifo even above the display refresh");
}

// The launcher's "Display (N Hz)" preset and its PresentMode line (frame rate S3, APS5_PRESENT_AUTO=1).
static void TestDisplayFpsLimit() {
    Require(PortSettings::DisplayFpsLimit(0) == 0.0 && PortSettings::DisplayFpsLimit(-1) == 0.0, "display limit: unknown refresh");
    Require(PortSettings::DisplayFpsLimit(59) == 60.0 && PortSettings::DisplayFpsLimit(60) == 60.0 && PortSettings::DisplayFpsLimit(61) == 60.0, "display limit: 59-61 is 60");
    Require(PortSettings::DisplayFpsLimit(29) == 30.0 && PortSettings::DisplayFpsLimit(30) == 30.0 && PortSettings::DisplayFpsLimit(31) == 30.0, "display limit: 29-31 is 30");
    Require(PortSettings::DisplayFpsLimit(119) == 120.0 && PortSettings::DisplayFpsLimit(120) == 120.0 && PortSettings::DisplayFpsLimit(121) == 120.0, "display limit: 119-121 is 120");
    Require(PortSettings::DisplayFpsLimit(144) == 144.0 && PortSettings::DisplayFpsLimit(165) == 165.0 && PortSettings::DisplayFpsLimit(75) == 75.0, "display limit: other refreshes as they are");
    Require(PortSettings::DisplayFpsLimit(10) == 0.0 && PortSettings::DisplayFpsLimit(360) == 0.0, "display limit: outside 20-240");

    PortSettings::Settings chosen;
    chosen.fpsLimit = 120.0;
    chosen.presentMode = PortSettings::PresentMode::Mailbox;
    PortSettings::ApplyLauncherRules(chosen);
    auto values = PortSettings::LauncherValues(chosen);
    values.emplace_back("PresentMode", PortSettings::PresentModeName(chosen.presentMode));
    const auto reread = FromText(PortSettings::SetIniValues("", values).c_str());
    Require(reread.warnings.empty() && reread.presentMode == PortSettings::PresentMode::Mailbox && reread.fpsLimit == 120.0 && reread.variableTimestep,
            "round trip: 120 with the variable timestep and PresentMode mailbox");
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
    // No settings window here (APS5_NO_LAUNCHER=0 with anyps5-launcher.exe next to the test shows it: a
    // manual check of the spawn, wait and read-back path).
    // The overlay is installed once per process, so the engine switches stay unset here (default: culling off).
    // APS5_ENGINE_SWITCHES=0 (no engine arguments) is covered by engine_switches_tests.
    SetVariable("APS5_ENGINE_SWITCHES", "");
    if (std::getenv("APS5_NO_LAUNCHER") == nullptr) SetVariable("APS5_NO_LAUNCHER", "1");

    Require(ReadGuest("/app0/misc/RenderConfig_PS5_Perf.txt") == "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60\r\n+r_Wants4K=WantsHigh\r\n+TargetFramesPerSecond=30\r\n+doIndexBufferCulling=false\r\n", "render config overlay (default engine switch: culling off)");
    Require(ReadGuest("/app0/packagecmdlineargs.txt") == "+cp11_callHomeLockDownEnabled=false +r_Wants4K=WantsHigh +TargetFramesPerSecond=30 +doIndexBufferCulling=false", "command line overlay");
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

// The settings launcher (s61): when it is skipped, and the ini read/modify/write round trip it saves with.
static void TestLauncher() {
    const auto lookup = [](std::map<std::string, std::string> environment) {
        return [environment](const char* name) -> const char* {
            const auto found = environment.find(name);
            return found == environment.end() ? nullptr : found->second.c_str();
        };
    };
    const auto reason = [](const char* text) { return text == nullptr ? std::string("shown") : std::string(text); };
    Require(reason(PortSettings::LauncherSkipReason({"DemonsSouls.exe"}, lookup({}))) == "shown", "launcher shown by default");
    Require(reason(PortSettings::LauncherSkipReason({"DemonsSouls.exe", "--launch"}, lookup({}))) == "--launch", "--launch skips it");
    Require(reason(PortSettings::LauncherSkipReason({"--launch"}, lookup({}))) == "shown", "argv[0] is not an option");
    Require(reason(PortSettings::LauncherSkipReason({"x", "--launcher"}, lookup({}))) == "shown", "only --launch itself");
    Require(reason(PortSettings::LauncherSkipReason({"x"}, lookup({{"APS5_NO_LAUNCHER", "1"}}))) == "APS5_NO_LAUNCHER", "APS5_NO_LAUNCHER=1");
    Require(reason(PortSettings::LauncherSkipReason({"x"}, lookup({{"APS5_NO_LAUNCHER", "0"}}))) == "shown", "APS5_NO_LAUNCHER=0 shows it");
    Require(reason(PortSettings::LauncherSkipReason({"x"}, lookup({{"APS5_LAUNCHER", "0"}}))) == "APS5_LAUNCHER=0", "APS5_LAUNCHER=0");
    Require(reason(PortSettings::LauncherSkipReason({"x"}, lookup({{"APS5_LAUNCHER", "1"}}))) == "shown", "APS5_LAUNCHER=1 shows it");
    Require(reason(PortSettings::LauncherSkipReason({"x"}, lookup({{"APS5_SETTINGS", "None"}}))) == "APS5_SETTINGS=none", "no file to edit");

    // Existing keys change in place (indentation, spelling, trailing comment kept, every duplicate), comments,
    // commented-out keys, sections, unknown keys, the BOM and CRLF stay; missing keys are appended with CRLF.
    const std::string original = "\xEF\xBB\xBF# player settings\r\n[display]\r\n  resolution=1920x1080   ; desk\r\n# FpsLimit = 45\r\nBogus = 7\r\n"
                                 "TimestepMinHz = 25\r\n\r\nRESOLUTION = 640x360\r\nWindowMode = auto";
    const std::string edited = PortSettings::SetIniValues(original, {{"Resolution", "1280x800"}, {"WindowMode", "fullscreen"}, {"FpsLimit", "40"}});
    Require(edited == "\xEF\xBB\xBF# player settings\r\n[display]\r\n  resolution = 1280x800   ; desk\r\n# FpsLimit = 45\r\nBogus = 7\r\n"
                      "TimestepMinHz = 25\r\n\r\nRESOLUTION = 1280x800\r\nWindowMode = fullscreen\r\nFpsLimit = 40\r\n", "ini edited in place");
    Require(PortSettings::SetIniValues(edited, {{"Resolution", "1280x800"}, {"WindowMode", "fullscreen"}, {"FpsLimit", "40"}}) == edited, "same values leave the text alone");
    Require(PortSettings::SetIniValues("", {{"Resolution", "auto"}}) == "Resolution = auto\n", "new file uses LF");
    Require(PortSettings::SetIniValues("A = 1\n", {{"a", "2"}}) == "A = 2\n", "the line keeps its key spelling");
    Require(PortSettings::SetIniValues("Timestep = fixed;x\n", {{"Timestep", "variable"}}) == "Timestep = variable;x\n", "comment right after the value");
    Require(PortSettings::SetIniValues("Timestep\n", {{"Timestep", "variable"}}) == "Timestep\nTimestep = variable\n", "a line without = is not the key");

    // Launcher values -> file -> parser gives the same settings back; the file's other keys survive.
    const PortSettings::Settings defaults0;
    PortSettings::Settings chosen;
    chosen.width = 2560;
    chosen.height = 1080;
    chosen.windowMode = PortSettings::WindowMode::Fullscreen;
    chosen.fpsLimit = 47.5;
    chosen.nativeAspect = true;
    chosen.buttonIcons = PortSettings::ButtonIcons::SteamDeck;
    PortSettings::ApplyLauncherRules(chosen);
    Require(chosen.variableTimestep, "a custom limit turns the variable timestep on");
    const auto saved = PortSettings::SetIniValues(original, PortSettings::LauncherValues(chosen));
    auto reread = PortSettings::Parse(saved, "test.ini");
    PortSettings::Normalize(reread);
    Require(reread.width == 2560 && reread.height == 1080 && reread.windowMode == PortSettings::WindowMode::Fullscreen, "round trip resolution and window");
    Require(reread.fpsLimit == 47.5 && reread.variableTimestep && reread.nativeAspect, "round trip fps, timestep, native aspect");
    Require(PortSettings::Describe(chosen).find(", ButtonIcons steamdeck") != std::string::npos && PortSettings::Describe(defaults0).find("ButtonIcons") == std::string::npos, "describe: button icons");
    Require(reread.buttonIcons == PortSettings::ButtonIcons::SteamDeck && saved.find("ButtonIcons = steamdeck\r\n") != std::string::npos, "round trip button icons");
    Require(Near(reread.timestepMinHz, 25.0, 1e-9), "other keys survive the save");
    Require(saved.find("Bogus = 7\r\n") != std::string::npos && saved.find("# FpsLimit = 45\r\n") != std::string::npos, "unknown key and comment kept");
    Require(saved.find("NativeAspect = on\r\n") != std::string::npos && saved.find("Timestep = variable\r\n") != std::string::npos, "keys appended with CRLF");

    PortSettings::Settings defaults;
    PortSettings::ApplyLauncherRules(defaults);
    const auto plain = PortSettings::Parse(PortSettings::SetIniValues("", PortSettings::LauncherValues(defaults)), "test.ini");
    // Auto is saved as windowed (what DisplayWindow does for auto), so only the window mode is set.
    Require(plain.warnings.empty() && plain.windowMode == PortSettings::WindowMode::Windowed && plain.width == 0 && plain.fpsLimit == 0.0 && !plain.variableTimestep && !plain.nativeAspect,
            "the launcher's defaults are today's behaviour");
    PortSettings::Settings sixty;
    sixty.fpsLimit = 60.0;
    sixty.nativeAspect = true;
    PortSettings::ApplyLauncherRules(sixty);
    Require(!sixty.variableTimestep && !sixty.nativeAspect, "60 keeps the fixed step; native aspect needs a resolution");
    Require(PortSettings::FormatNumber(59.94) == "59.94" && PortSettings::FormatNumber(45.0) == "45", "numbers written short");
}

// A minimal ELF64 eboot.bin: one executable PT_LOAD at 0x83b000 holding ds_patch.py's patch site.
static std::string FakeEboot(bool knownVersion) {
    std::string data(0x2000, '\0');
    const auto put = [&](std::size_t offset, std::uint64_t value, int size) {
        for (int index = 0; index < size; ++index) data[offset + static_cast<std::size_t>(index)] = static_cast<char>((value >> (8 * index)) & 0xff);
    };
    data[0] = 0x7f;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 2;
    data[5] = 1;
    put(0x20, 64, 8);
    put(0x36, 56, 2);
    put(0x38, 1, 2);
    put(64 + 0x00, 1, 4);
    put(64 + 0x04, 5, 4);
    put(64 + 0x08, 0x1000, 8);
    put(64 + 0x10, 0x83b000, 8);
    put(64 + 0x20, 0x1000, 8);
    for (std::size_t index = 0; index < Launcher::PatchSiteBytes.size(); ++index) data[0x16b3 + index] = static_cast<char>(knownVersion ? Launcher::PatchSiteBytes[index] : 0x90);
    return data;
}

static void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static const std::string ElfModule = std::string("\x7f" "ELF", 4) + std::string(60, '\0');
static const std::string SelfModule = std::string("\x54\x14\xf5\xee", 4) + std::string(60, '\0');

static void MakeDump(const std::filesystem::path& app0, const std::string& eboot, const char* title = "PPSA01341") {
    WriteFile(app0 / "eboot.bin", eboot);
    WriteFile(app0 / "sce_sys" / "param.json", std::string("{\n  \"contentVersion\": \"01.007.000\",\n  \"titleId\" : \"") + title + "\"\n}\n");
    WriteFile(app0 / "gameversion.txt", "+BuildVersion=2025-10-15.877562\r\n");
    WriteFile(app0 / "sce_module" / "libc.prx", ElfModule);
}

static bool Mentions(const std::vector<std::string>& lines, const char* text) {
    for (const auto& line : lines) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

// GameDir in the settings file and the launcher's game folder checks and setup stamp (core/launcher/GameFolder.hpp).
static void TestGameFolder() {
    // A quoted path keeps '#' and ';'; the comment after it stays a comment.
    const auto parsed = FromText("GameDir = \"D:\\Games #1; old\\PPSA01341-app0\" ; my dump\n");
    Require(parsed.gameDir == "D:\\Games #1; old\\PPSA01341-app0" && parsed.warnings.empty(), "GameDir parsed with # and ; inside quotes");
    PortSettings::Settings chosen;
    chosen.gameDir = "E:\\New #2\\app0";
    const auto saved = PortSettings::SetIniValues("GameDir = \"D:\\Old;1\" ; my dump\nBogus = 1\n", PortSettings::LauncherValuesWithGameDir(chosen));
    Require(saved.starts_with("GameDir = \"E:\\New #2\\app0\" ; my dump\nBogus = 1\n"), "GameDir replaced in place, comment kept");
    Require(PortSettings::Parse(saved, "test.ini").gameDir == "E:\\New #2\\app0", "GameDir round trip");
    Require(PortSettings::SetIniValues("", PortSettings::LauncherValuesWithGameDir(PortSettings::Settings{})).find("GameDir") == std::string::npos, "no GameDir key while unset");
    // The launcher's window writes an empty folder too: clearing the field clears the file's folder.
    const auto cleared = PortSettings::SetIniValues("GameDir = \"D:\\Old\"\n", PortSettings::LauncherValuesWithGameDir(PortSettings::Settings{}, true));
    Require(cleared.starts_with("GameDir = \"\"\n"), "cleared GameDir written as \"\"");
    const auto reread = PortSettings::Parse(cleared, "test.ini");
    Require(reread.gameDir.empty() && reread.warnings.empty(), "an empty GameDir reads back empty, without a warning");
    Require(FromText("FpsLimit = 45 # \"quoted\" comment\nTimestep = variable\n").fpsLimit == 45.0, "comment with quotes after a plain value");

    Require(Launcher::CleanFolderText("  \"C:\\Games\\dump\\\"  ") == "C:\\Games\\dump", "pasted path: quotes and trailing separator dropped");
    Require(Launcher::CleanFolderText("C:\\") == "C:\\" && Launcher::CleanFolderText("/home/deck/app0/") == "/home/deck/app0", "roots kept, trailing slash dropped");
    Require(Launcher::CleanFolderText("D:\\Games\n\\app0\r\n") == "D:\\Games\\app0", "pasted line breaks dropped");

    const auto base = std::filesystem::temp_directory_path() / "aps5-gamefolder-test";
    std::error_code error;
    std::filesystem::remove_all(base, error);
    const auto install = base / "install";
    std::filesystem::create_directories(install);

    Require(Launcher::CheckGameFolder("", install).state == Launcher::FolderState::Empty, "empty folder text");
    Require(Launcher::CheckGameFolder(PortSettings::PathText(base / "missing"), install).state == Launcher::FolderState::Invalid, "missing folder");
    // A relative path names another folder for the setup (its working directory differs): refused.
    auto check = Launcher::CheckGameFolder("PPSA01341/app0", install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "full path"), "relative path refused");
    check = Launcher::CheckGameFolder(PortSettings::PathText(base) + "/dump \"1\"", install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "double quote"), "a path with a double quote refused");
#ifdef _WIN32
    check = Launcher::CheckGameFolder("\\\\nas\\games\\PPSA01341-app0", install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "network folder"), "network share refused without touching it");
#endif

    // The parent of app0 is taken as its app0; a supported dump has no warnings.
    const auto good = base / "PPSA01341" / "app0";
    MakeDump(good, FakeEboot(true));
    const auto bytes = Launcher::ReadTextBytes(good / "eboot.bin", Launcher::PatchSiteVaddr, Launcher::PatchSiteBytes.size());
    Require(bytes.size() == Launcher::PatchSiteBytes.size() && std::equal(bytes.begin(), bytes.end(), Launcher::PatchSiteBytes.begin()), "patch site read from the ELF");
    check = Launcher::CheckGameFolder("\"" + PortSettings::PathText(base / "PPSA01341") + "/\"", install);
    Require(check.state == Launcher::FolderState::Valid && check.errors.empty() && check.warnings.empty(), "valid dump through its parent folder");
    Require(std::filesystem::equivalent(check.folder, good) && check.summary.find("PPSA01341") != std::string::npos && check.summary.find("(supported)") != std::string::npos, "app0 used, title and version shown");
    Require(Launcher::CheckGameFolder(PortSettings::PathText(good / "eboot.bin"), install).state == Launcher::FolderState::Valid, "eboot.bin itself picked");

    // A folder name outside the ANSI code page (the relinker reads it through its UTF-8 manifest).
    const auto cyrillic = base / Launcher::PathFromText("\xd0\x94\xd0\xb5\xd0\xbc\xd0\xbe\xd0\xbd") / "app0";
    MakeDump(cyrillic, FakeEboot(true));
    check = Launcher::CheckGameFolder(PortSettings::PathText(cyrillic), install);
    Require(check.state == Launcher::FolderState::Valid && PortSettings::PathText(check.folder) == PortSettings::PathText(cyrillic), "non-ANSI folder name");

    // An unsupported version or another game is refused before any setup (the setup would relink it first).
    const auto unknown = base / "unknown";
    MakeDump(unknown, FakeEboot(false));
    check = Launcher::CheckGameFolder(PortSettings::PathText(unknown), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "Unsupported game version"), "unknown version refused");
    const auto other = base / "other";
    MakeDump(other, FakeEboot(true), "PPSA01342");
    check = Launcher::CheckGameFolder(PortSettings::PathText(other), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "PPSA01342, not Demon's Souls"), "another title refused");

    // sce_module must hold decrypted modules only.
    const auto selfModules = base / "selfmodules";
    MakeDump(selfModules, FakeEboot(true));
    WriteFile(selfModules / "sce_module" / "libSceNpCppWebApi.prx", SelfModule);
    check = Launcher::CheckGameFolder(PortSettings::PathText(selfModules), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "1 encrypted module"), "an encrypted module refused");
    std::filesystem::remove(selfModules / "sce_module" / "libSceNpCppWebApi.prx", error);
    std::filesystem::remove(selfModules / "sce_module" / "libc.prx", error);
    check = Launcher::CheckGameFolder(PortSettings::PathText(selfModules), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "no decrypted game module"), "an empty sce_module refused");

    // AnyPS5 unpacked inside the dump would be written into it.
    Require(Launcher::IsInside(good / "AnyPS5", good) && Launcher::IsInside(good, good) && !Launcher::IsInside(base / "PPSA013410", base / "PPSA01341"), "IsInside");
    std::filesystem::create_directories(good / "AnyPS5");
    check = Launcher::CheckGameFolder(PortSettings::PathText(good), good / "AnyPS5");
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "inside this dump"), "AnyPS5 inside the dump refused");
    std::filesystem::remove(good / "AnyPS5", error);

    const auto encrypted = base / "encrypted";
    MakeDump(encrypted, std::string("\x4f\x15\x3d\x1d", 4) + std::string(60, '\0'));
    check = Launcher::CheckGameFolder(PortSettings::PathText(encrypted), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "encrypted"), "encrypted SELF refused");

    const auto partial = base / "partial";
    WriteFile(partial / "eboot.bin", FakeEboot(true));
    std::filesystem::create_directories(partial / "sce_sys");
    check = Launcher::CheckGameFolder(PortSettings::PathText(partial), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "sce_module is missing"), "missing sce_module named");

    MakeDump(install / "app0", FakeEboot(true));
    check = Launcher::CheckGameFolder(PortSettings::PathText(install / "app0"), install);
    Require(check.state == Launcher::FolderState::Invalid && Mentions(check.errors, "AnyPS5's own folder"), "AnyPS5's own app0 refused");
    std::filesystem::remove_all(install / "app0", error);

    // The release folder, the setup stamp and why the setup is due.
    Require(Launcher::FindInstallDir(install).empty(), "no release folder without the setup script and relinker");
    WriteFile(install / Launcher::SetupScript, "setup");
    WriteFile(install / "tools" / Launcher::RelinkerName, "relinker");
    WriteFile(install / "tools" / "ds_patch.py", "patch");
    std::filesystem::create_directories(install / "libs");
    Require(Launcher::FindInstallDir(install) == install && Launcher::FindInstallDir(install / "libs") == install, "release folder from the root or libs/");
    Require(Launcher::SetupReason(install, good, true).find("first start") != std::string::npos, "setup due: no executable");
    WriteFile(install / Launcher::GameExecutable, "exe");
    std::filesystem::create_directories(install / "app0" / "sce_sys");
    Require(Launcher::SetupReason(install, good, true).find("no record") != std::string::npos, "setup due: no stamp");
    const auto stamp = [&](bool patched) { return Launcher::SetupStamp(good, Launcher::ToolsFingerprint(install), Launcher::DumpFingerprint(good), patched); };
    WriteFile(install / Launcher::StampName, stamp(true));
    Require(Launcher::SetupReason(install, good, true).empty() && Launcher::SetupReason(install, good, false).empty(), "patched stamp holds");
    WriteFile(install / Launcher::StampName, stamp(false));
    Require(Launcher::SetupReason(install, good, false).empty(), "unpatched stamp holds without Python 3.8+");
    Require(Launcher::SetupReason(install, good, true).find("Python 3.8") != std::string::npos, "setup due: Python 3.8+ installed since");
    Require(Launcher::SetupReason(install, unknown, false).find("game folder changed") != std::string::npos, "setup due: other game folder");
    WriteFile(good / "eboot.bin", FakeEboot(true) + "updated in place");
    Require(Launcher::SetupReason(install, good, false).find("eboot.bin changed") != std::string::npos, "setup due: dump updated in place");
    WriteFile(install / Launcher::StampName, stamp(false));
    WriteFile(install / "tools" / Launcher::RelinkerName, "a newer relinker");
    Require(Launcher::SetupReason(install, good, false).find("updated") != std::string::npos, "setup due: AnyPS5 updated");
    // Patched=no only when the setup said so (its own Python check decides).
    Require(Launcher::SetupLeftUnpatched({"== relinking x", "WARNING: Python 3.8+ not found: DemonsSouls.exe is NOT patched and will run much slower."}) &&
                !Launcher::SetupLeftUnpatched({"== patching DemonsSouls.exe", "applied  job-worker-spin"}),
        "unpatched setup output recognised");

    // Python as DemonsSouls.ps1 / .sh find it, never the Store alias.
#ifdef _WIN32
    const char* pythonName = "python.exe";
    const std::string pathText = PortSettings::PathText(base / "WindowsApps") + ";" + PortSettings::PathText(base / "bin");
#else
    const char* pythonName = "python3";
    const std::string pathText = PortSettings::PathText(base / "WindowsApps") + ":" + PortSettings::PathText(base / "bin");
#endif
    const auto lookup = [&](const char* name) -> const char* { return std::string_view(name) == "PATH" ? pathText.c_str() : nullptr; };
    WriteFile(base / "WindowsApps" / pythonName, "");
    Require(Launcher::PythonCandidates(lookup).empty(), "the WindowsApps alias is not Python");
    WriteFile(base / "bin" / pythonName, "");
    auto candidates = Launcher::PythonCandidates(lookup);
    Require(candidates.size() == 1 && candidates[0].size() == 1 && std::filesystem::equivalent(Launcher::PathFromText(candidates[0][0]), base / "bin" / pythonName), "Python on PATH");
#ifdef _WIN32
    WriteFile(base / "bin" / "py.exe", "");
    candidates = Launcher::PythonCandidates(lookup);
    Require(candidates.size() == 2 && candidates[0].size() == 2 && candidates[0][1] == "-3" && candidates[0][0].ends_with("py.exe"), "py -3 tried first, as DemonsSouls.ps1 does");
#endif
    std::filesystem::remove_all(base, error);
}

// The launcher's process and picker text helpers (core/launcher/HostText.hpp).
static void TestHostText() {
    using Launcher::QuoteArgument;
    Require(QuoteArgument(std::string("plain")) == "plain" && QuoteArgument(std::string("")) == "\"\"", "plain and empty arguments");
    Require(QuoteArgument(std::string("D:\\")) == "D:\\" && QuoteArgument(std::string("D:\\My Games\\")) == "\"D:\\My Games\\\\\"", "a trailing backslash doubled only when quoted");
    Require(QuoteArgument(std::string("say \"hi\"")) == "\"say \\\"hi\\\"\"" && QuoteArgument(std::string("a\\\"b")) == "\"a\\\\\\\"b\"", "quotes escaped, backslashes before them doubled");
    Require(QuoteArgument(std::wstring(L"C:\\Program Files\\x")) == L"\"C:\\Program Files\\x\"", "wide text");
    Require(Launcher::PercentDecode("/home/deck/My%20Games/%41") == "/home/deck/My Games/A" && Launcher::PercentDecode("50%zz%4") == "50%zz%4", "percent decoding, a trailing escape too");
    std::string folder;
    Require(Launcher::ParsePortalResponse("/org/freedesktop/portal/desktop/request/1_2/t: org.freedesktop.portal.Request.Response (uint32 0, {'uris': <['file:///run/media/deck/SD%20card/app0']>, 'choices': <@a(ss) []>})", folder) ==
                    Launcher::PortalAnswer::Picked &&
                folder == "/run/media/deck/SD card/app0",
        "portal answer: picked folder");
    Require(Launcher::ParsePortalResponse("x: org.freedesktop.portal.Request.Response (uint32 1, @a{sv} {})", folder) == Launcher::PortalAnswer::Cancelled, "portal answer: cancelled");
    Require(Launcher::ParsePortalResponse("garbage", folder) == Launcher::PortalAnswer::Unreadable, "portal answer: unreadable");
}

// DeluxeEdition / AddonDir and the add-on search the entitlement query answers from (AddonContent.hpp, s61).
static void TestAddonContent() {
    Require(FromText("").deluxeEdition == PortSettings::DeluxeEdition::Auto, "DeluxeEdition defaults to auto");
    Require(FromText("DeluxeEdition = off").deluxeEdition == PortSettings::DeluxeEdition::Off && FromText("DeluxeEdition = on").deluxeEdition == PortSettings::DeluxeEdition::Auto, "DeluxeEdition off / on");
    const auto bad = FromText("DeluxeEdition = owned");
    Require(bad.deluxeEdition == PortSettings::DeluxeEdition::Auto && bad.warnings.size() == 1, "DeluxeEdition: a bad value refused");
    auto environment = PortSettings::Parse("DeluxeEdition = auto\n", "test.ini");
    PortSettings::ApplyEnvironment(environment, [](const char* name) -> const char* { return std::string(name) == "APS5_DELUXE_EDITION" ? "0" : nullptr; });
    Require(environment.deluxeEdition == PortSettings::DeluxeEdition::Off, "APS5_DELUXE_EDITION=0");
    Require(FromText("AddonDir = \"D:\\Dumps #2\\PPSA01341-ac\" ; mine\n").addonDir == "D:\\Dumps #2\\PPSA01341-ac", "AddonDir quoted");
    PortSettings::Settings off;
    off.deluxeEdition = PortSettings::DeluxeEdition::Off;
    const auto saved = PortSettings::SetIniValues("", PortSettings::LauncherValues(off));
    Require(saved.find("DeluxeEdition = off\n") != std::string::npos && PortSettings::Parse(saved, "test.ini").deluxeEdition == PortSettings::DeluxeEdition::Off, "launcher round trip: off");
    Require(PortSettings::SetIniValues("", PortSettings::LauncherValues(PortSettings::Settings{})).find("DeluxeEdition = auto\n") != std::string::npos, "launcher writes auto");
    Require(PortSettings::Describe(off).find("Deluxe Edition content off") != std::string::npos && PortSettings::Describe(PortSettings::Settings{}).find("Deluxe") == std::string::npos, "describe");

    Require(AddonContent::ContentIdIs("EP9000-PPSA01341_00-DEMONSSOULSGIFT2", AddonContent::DeluxeEditionLabel) && AddonContent::ContentIdIs("up9000-ppsa01342_00-demonssoulsgift2", AddonContent::DeluxeEditionLabel), "content ids of both regions");
    Require(!AddonContent::ContentIdIs("EP9000-PPSA99999_00-DEMONSSOULSGIFT2", AddonContent::DeluxeEditionLabel) && !AddonContent::ContentIdIs("EP9000-PPSA01341_00-XDEMONSSOULSGIFT2", AddonContent::DeluxeEditionLabel), "another title's or label's id");

    const auto base = std::filesystem::temp_directory_path() / "aps5-addon-test";
    std::error_code error;
    std::filesystem::remove_all(base, error);
    const auto dumps = base / "dumps";
    const auto app0 = dumps / "PPSA01341-app0";
    MakeDump(app0, FakeEboot(true));
    MakeDump(dumps / "PPSA21567-app0", FakeEboot(true), "PPSA21567");
    const auto find = [&](const std::filesystem::path& addonDir = {}) { return AddonContent::Find(AddonContent::DumpFolders(app0), addonDir); };
    Require(find().empty() && AddonContent::DumpFolders(app0).size() == 1, "a plain dump holds no add-on");

    // Not an add-on: an empty folder named after the label, a non-empty one with only the name (no
    // metadata, also "...GIFT20" and a prefixed name), the wrong title id, GIFT1 metadata in a GIFT2-named
    // folder, the base game folder, another title's content id, a folder inside another dump (another
    // region's app0 is not looked into).
    std::filesystem::create_directories(dumps / "DEMONSSOULSGIFT2");
    WriteFile(dumps / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2" / "readme.txt", "just a file");
    WriteFile(dumps / "XDEMONSSOULSGIFT2 old" / "license.dat", "x");
    WriteFile(dumps / "DEMONSSOULSGIFT20" / "sce_sys" / "param.json", "{}");
    WriteFile(dumps / "wrongtitle-DEMONSSOULSGIFT2" / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA99999_00-DEMONSSOULSGIFT2\"}");
    WriteFile(dumps / "gift1-DEMONSSOULSGIFT2" / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA01341_00-DEMONSSOULSGIFT1\"}");
    WriteFile(dumps / "sfo-wrongtitle" / "sce_sys" / "param.sfo", std::string(8, '\0') + "CONTENT_ID" + std::string(2, '\0') + "EP9000-PPSA99999_00-DEMONSSOULSGIFT2" + std::string(3, '\0'));
    WriteFile(dumps / "other" / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA99999_00-DEMONSSOULSGIFT2\"}");
    MakeDump(dumps / "PPSA01342-app0", FakeEboot(true), "PPSA01342");
    WriteFile(dumps / "PPSA01342-app0" / "DEMONSSOULSGIFT2" / "license.dat", "x");
    auto junk = find();
    Require(junk.size() == 1 && junk[0].label == AddonContent::PreOrderLabel && junk[0].folder.filename() == "gift1-DEMONSSOULSGIFT2", "name-only, wrong-title, empty, foreign and in-dump folders are no add-on; GIFT1 metadata is only GIFT1");
    Require(AddonContent::FindLabel(find(dumps / "DEMONSSOULSGIFT2"), AddonContent::DeluxeEditionLabel) == nullptr && AddonContent::FindLabel(find(dumps / "gift1-DEMONSSOULSGIFT2"), AddonContent::DeluxeEditionLabel) == nullptr, "AddonDir: a bare named folder, or GIFT1 metadata in a GIFT2-named folder, is not the Deluxe Edition");
    std::filesystem::remove_all(dumps / "gift1-DEMONSSOULSGIFT2", error);
    // The base game folder is not an add-on even with the label in its metadata.
    WriteFile(app0 / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA01341_00-DEMONSSOULSGIFT2\", \"titleId\": \"PPSA01341\"}");
    Require(AddonContent::FindLabel(find(), AddonContent::DeluxeEditionLabel) == nullptr, "the base game folder is no add-on");
    MakeDump(app0, FakeEboot(true));

    // Next to app0 by its metadata (content id in param.json); the pre-order gift by param.sfo two levels into an addcont folder.
    std::filesystem::remove_all(dumps / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2", error);
    WriteFile(dumps / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2" / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA01341_00-DEMONSSOULSGIFT2\"}");
    WriteFile(dumps / "addcont" / "PPSA01341" / "gift" / "sce_sys" / "param.sfo", std::string(8, '\0') + "CONTENT_ID" + std::string(2, '\0') + "EP9000-PPSA01341_00-DEMONSSOULSGIFT1" + std::string(3, '\0'));
    auto found = find();
    const auto* deluxe = AddonContent::FindLabel(found, AddonContent::DeluxeEditionLabel);
    const auto* preOrder = AddonContent::FindLabel(found, AddonContent::PreOrderLabel);
    Require(found.size() == 2 && deluxe != nullptr && deluxe->reason == "its sce_sys/param.json" && deluxe->folder.filename() == "EP9000-PPSA01341_00-DEMONSSOULSGIFT2", "Deluxe Edition next to app0 by param.json");
    Require(preOrder != nullptr && preOrder->reason == "its param.sfo" && preOrder->folder.filename() == "gift", "pre-order gift by param.sfo in addcont");

    // By param.json in a folder named with the title id; and in the player's AddonDir anywhere else.
    std::filesystem::remove_all(dumps / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2", error);
    std::filesystem::remove_all(dumps / "addcont", error);
    WriteFile(dumps / "PPSA01341-ac" / "1" / "sce_sys" / "param.json", "{\n  \"contentId\" : \"EP9000-PPSA01341_00-DEMONSSOULSGIFT2\",\n  \"titleId\": \"PPSA01341\"\n}\n");
    found = find();
    Require(found.size() == 1 && found[0].label == AddonContent::DeluxeEditionLabel && found[0].reason == "its sce_sys/param.json", "Deluxe Edition by param.json in PPSA01341-ac");
    std::filesystem::remove_all(dumps / "PPSA01341-ac", error);
    WriteFile(base / "elsewhere" / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2" / "license.dat", "x");
    Require(find().empty() && find(base / "elsewhere").empty(), "AddonDir: a named folder without metadata is not it");
    WriteFile(base / "elsewhere" / "EP9000-PPSA01341_00-DEMONSSOULSGIFT2" / "sce_sys" / "param.json", "{\"contentId\": \"EP9000-PPSA01341_00-DEMONSSOULSGIFT2\"}");
    Require(find().empty() && find(base / "elsewhere").size() == 1, "AddonDir names a folder elsewhere");

    // What the query answers: the file's labels but its Deluxe Edition, then the dump's (Deluxe only when on).
    const std::vector<AddonContent::Found> both{{AddonContent::PreOrderLabel, "a", "its name"}, {AddonContent::DeluxeEditionLabel, "b", "its name"}};
    std::vector<std::string> notes;
    auto labels = AddonContent::AnsweredLabels({"OTHERADDON", AddonContent::DeluxeEditionLabel}, both, true, notes);
    Require(labels == std::vector<std::string>{"OTHERADDON", AddonContent::PreOrderLabel, AddonContent::DeluxeEditionLabel} && notes.size() == 1, "found: owned");
    notes.clear();
    labels = AddonContent::AnsweredLabels({AddonContent::DeluxeEditionLabel}, {}, true, notes);
    Require(labels.empty() && notes.size() == 1, "the entitlements file alone does not make it owned");
    notes.clear();
    labels = AddonContent::AnsweredLabels({}, both, false, notes);
    Require(labels == std::vector<std::string>{AddonContent::PreOrderLabel} && notes.size() == 1, "DeluxeEdition = off: not owned");
    std::filesystem::remove_all(base, error);
}

// APS5_MOVIE_WATCH (MovieWatch.hpp): the Bink 2 header and the speed of one pass from its reads.
static void TestMovieWatch() {
    // logo_intro.bk2's header: 360 frames, 3840x2160, 30/1 fps.
    unsigned char header[MovieWatch::HeaderBytes] = {'K', 'B', '2', 'm'};
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) header[at + i] = static_cast<unsigned char>(value >> (8u * i));
    };
    put(4, 71054660u - 8u);
    put(8, 360);
    put(12, 1u << 20u);
    put(16, 360);
    put(20, 3840);
    put(24, 2160);
    put(28, 30);
    put(32, 1);
    const auto parsed = MovieWatch::ParseHeader(header, sizeof(header));
    Require(parsed.valid && parsed.frames == 360 && parsed.width == 3840 && parsed.height == 2160, "Bink 2 header fields");
    Require(Near(parsed.Fps(), 30.0, 1e-9) && Near(parsed.Seconds(), 12.0, 1e-9), "Bink 2 frame rate and length");
    Require(!MovieWatch::ParseHeader(header, MovieWatch::HeaderBytes - 1).valid, "a short header is no header");
    header[0] = 'B';
    Require(!MovieWatch::ParseHeader(header, sizeof(header)).valid, "another signature is not followed");
    Require(MovieWatch::IsMovie("movies/logo_intro.bk2") && MovieWatch::IsMovie("A/B.BK2") && !MovieWatch::IsMovie("a.bk2x") && !MovieWatch::IsMovie("bk2"),
        "movie file names");

    // A 12 s, 72 MB movie streamed in 1 MB reads after a 6 MB preload, at 1x (6 MB/s) and at 2x.
    const std::uint64_t mb = 1u << 20u, size = 72 * mb;
    for (const double rate : {1.0, 2.0}) {
        MovieWatch::Pass pass(size, 12.0);
        Require(!pass.Read(0.0, 0, 6 * mb), "a preload does not measure");
        int measured = 0;
        double seconds = 0.0;
        for (std::uint64_t at = 6 * mb; at < size; at += mb) {
            seconds += 1.0 / (6.0 * rate);
            if (pass.Read(seconds, at, at + mb)) ++measured;
        }
        Require(measured == 1 && pass.Measured(), "the stream crosses three quarters once");
        Require(Near(pass.Speed(), rate, 0.03 * rate), "speed from the middle half");
        Require(pass.Bytes() == size && pass.Reached() == size, "bytes of the pass");
        Require(pass.Loops(0) && !pass.Loops(size / 2), "a read at the start after the end half is a loop");
        pass.Restart();
        Require(!pass.Measured() && pass.Bytes() == 0 && !pass.Loops(0), "a restarted pass starts clean");
    }
    // A whole file read at once: measured, but the speed is not measurable.
    MovieWatch::Pass whole(size, 12.0);
    Require(whole.Read(1.0, 0, size) && whole.Speed() == 0.0, "a whole-file read has no speed");
    // An earlier range again does not move the crossings back.
    MovieWatch::Pass repeat(size, 12.0);
    repeat.Read(0.0, 0, size / 2);
    Require(!repeat.Read(1.0, 0, mb) && repeat.Reached() == size / 2, "an earlier range does not move the furthest end");
}

int main() {
    TestAddonContent();
    TestLauncher();
    TestGameFolder();
    TestHostText();
    TestReviewFixes();
    TestResolution4kUi();
    TestNativeAspect();
    TestParse();
    TestEnvironment();
    TestRenderTier();
    TestFrameRate();
    TestPacer();
    TestTitleStep();
    TestRefreshField();
    TestJobBudget();
    TestTableCompat();
    TestMovieWatch();
    TestPresentMode();
    TestDisplayFpsLimit();
    TestWindowSize();
    TestOverlay();
    std::printf("port_settings_tests: ok\n");
    return 0;
}
