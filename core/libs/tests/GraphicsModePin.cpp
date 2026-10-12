#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GraphicsModePin.hpp"
#include "SceTypes.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
}

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "graphics_mode_pin_tests: %s\n", what);
    std::abort();
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

// The switch and the plan: unset or empty = pinned, "0" = the title's own configs, a missing performance
// config leaves the option live.
static void TestPlan() {
    Require(GraphicsModePin::Enabled(nullptr) && GraphicsModePin::Enabled("") && GraphicsModePin::Enabled("1"), "default on");
    Require(!GraphicsModePin::Enabled("0"), "=0 is the old path");
    const std::filesystem::path perf = "perf.txt";
    const auto pinned = GraphicsModePin::PlanPin(nullptr, perf, true);
    Require(pinned.alias && pinned.target == perf && pinned.line.starts_with("[settings] graphics mode pinned"), "pinned plan");
    const auto off = GraphicsModePin::PlanPin("0", perf, true);
    Require(!off.alias && off.line.find("APS5_HIDE_GRAPHICS_MODE=0") != std::string::npos, "switch off");
    const auto missing = GraphicsModePin::PlanPin("1", perf, false);
    Require(!missing.alias && missing.line.find("not found") != std::string::npos, "missing performance config");
}

// End to end with player settings: the cinematic config serves the overlay's merged performance copy, in
// either spelling; other files are untouched.
static void TestPinWithOverlay() {
    const auto root = std::filesystem::canonical(std::filesystem::current_path());
    Require(!std::filesystem::exists(root / "app0"), "run from a directory without app0");
    const auto settingsPath = root / ("anyps5-settings-pin-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    { std::ofstream file(settingsPath, std::ios::binary); file << "Resolution = 1280x800\nFpsLimit = 30\n"; }
    std::filesystem::create_directories(root / "app0" / "misc");
    { std::ofstream file(root / "app0" / "PackageCmdLineArgs.txt", std::ios::binary); file << "+cp11_callHomeLockDownEnabled=false"; }
    { std::ofstream file(root / "app0" / "misc" / "renderconfig_ps5_perf.txt", std::ios::binary); file << "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60"; }
    { std::ofstream file(root / "app0" / "misc" / "renderconfig_ps5_high.txt", std::ios::binary); file << "+r_Wants4K = WantsSuperHigh\r\n+TargetFramesPerSecond = 30"; }
    { std::ofstream file(root / "app0" / "misc" / "renderconfig_ps5_lowenergy.txt", std::ios::binary); file << "+lowenergy"; }
    SetVariable("APS5_SETTINGS", settingsPath.string().c_str());

    const std::string merged = "+r_Wants4K = WantsHigh\r\n+TargetFramesPerSecond = 60\r\n+r_Wants4K=WantsHigh\r\n+TargetFramesPerSecond=30\r\n";
    Require(ReadGuest("/app0/misc/RenderConfig_PS5_Perf.txt") == merged, "performance config merged");
    Require(ReadGuest("/app0/misc/RenderConfig_PS5_High.txt") == merged, "cinematic config serves the performance one");
    Require(ReadGuest("/app0/misc/renderconfig_ps5_high.txt") == merged, "the title's spelling");
    Require(ReadGuest("/app0/misc/RenderConfig_PS5_LowEnergy.txt") == "+lowenergy\r\n+r_Wants4K=WantsHigh\r\n+TargetFramesPerSecond=30\r\n", "other configs keep their own text");

    std::filesystem::remove_all(root / "app0");
    std::filesystem::remove(settingsPath);
}

int main() {
    TestPlan();
    TestPinWithOverlay();
    std::printf("graphics_mode_pin_tests: ok\n");
    return 0;
}
