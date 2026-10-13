// AnyPS5 settings launcher (s61): the player's entry point to Demon's Souls. It edits anyps5-settings.ini,
// chooses the player's own dump (GameDir), runs the release's one-time setup when needed and starts the
// game. Dear ImGui on SDL2's renderer (Direct3D on Windows, the software renderer on Linux), with mouse,
// keyboard and gamepad navigation; sized for a Steam Deck (1280x800 fullscreen) as well as a desktop window.
// Three ways it runs:
//  - From the release folder (anyps5-launcher[.exe] next to DemonsSouls.ps1 / .sh and tools/relinker, or
//    in its libs/): Play saves, runs the setup (DemonsSouls.ps1 -Dump <GameDir> -SetupOnly / DemonsSouls.sh
//    --dump <GameDir> --setup-only, hidden, its output shown here) when anyps5-setup.txt says it is due,
//    then starts the game with --launch and waits for it (its output in anyps5-game.log, Host.hpp). --launch:
//    straight to the game when the folder is valid and set up (no window), else the window (setting up at
//    once when only the setup is missing). Other arguments go to the game.
//  - From the game (libc's settings load passes --settings <file>, prx/libc/src/PortSettings.cpp): exit
//    code 0 = Play (the game goes on and reads the file), PortSettings::LauncherExitQuit = the window was
//    closed (the game exits), any other code = it failed (the game starts with the file as it is). The game
//    folder is shown, not changed (the game already runs from it).
//  - Elsewhere (a build tree): edits anyps5-settings.ini in the working directory; the button says Save.
//   anyps5-launcher [--settings <file>] [--launch] [--window WIDTHxHEIGHT] [--deck] [--dry-run]
//                   [--keys <k,k,...>] [--snapshot <file.bmp>] [game arguments]
// --window: the window's size instead of the automatic one. --deck: the built-in folder browser as on a
// Steam Deck. --dry-run (tests): runs the setup but prints the game's command instead of starting it.
// --keys (tests): presses up, down, left, right, enter, space, escape, tab, start, a, b, y or back
// (gamepad) one after another into ImGui, a key every 4 frames from frame 5. --snapshot (tests): writes the
// frame after the keys (frame 5 without them) as a BMP and exits with code 4 without saving.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "prx/libc/include/AddonContent.hpp"
#include "prx/libc/include/PortSettings.hpp"

#include "FolderBrowser.hpp"
#include "GameFolder.hpp"
#include "Host.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr int ExitSnapshot = 4;
constexpr int ExitFailure = 1;
constexpr int ExitGameStarted = 5;  // internal: the window closed, the game runs

const ImVec4 Good(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 Warn(1.0f, 0.78f, 0.35f, 1.0f);
const ImVec4 Bad(1.0f, 0.45f, 0.4f, 1.0f);

struct Options {
    std::filesystem::path settings;
    bool fromGame = false;
    bool launch = false;
    bool deck = false;
    bool dryRun = false;
    std::uint32_t windowWidth = 0;
    std::uint32_t windowHeight = 0;
    std::filesystem::path snapshot;
    std::vector<ImGuiKey> keys;
    std::vector<std::string> gameArgs;
};

bool ParseKeys(const std::string& text, std::vector<ImGuiKey>& keys) {
    static constexpr std::pair<const char*, ImGuiKey> names[] = {{"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
        {"right", ImGuiKey_RightArrow}, {"enter", ImGuiKey_Enter}, {"space", ImGuiKey_Space}, {"escape", ImGuiKey_Escape}, {"tab", ImGuiKey_Tab},
        {"start", ImGuiKey_GamepadStart}, {"a", ImGuiKey_GamepadFaceDown}, {"b", ImGuiKey_GamepadFaceRight}, {"y", ImGuiKey_GamepadFaceUp},
        {"back", ImGuiKey_GamepadBack}};
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(',', start), text.size());
        const auto name = PortSettings::Lower(PortSettings::Trim(std::string_view(text).substr(start, end - start)));
        const auto found = std::find_if(std::begin(names), std::end(names), [&](const auto& entry) { return name == entry.first; });
        if (found == std::end(names)) return false;
        keys.push_back(found->second);
        start = end + 1;
    }
    return true;
}

std::vector<std::filesystem::path> Arguments(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#ifdef _WIN32
    // The wide command line, so a settings path outside the ANSI code page survives.
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide != nullptr) {
        for (int index = 0; index < count; ++index) args.emplace_back(wide[index]);
        LocalFree(wide);
    }
#else
    for (int index = 0; index < argc; ++index) args.emplace_back(argv[index]);
#endif
    return args;
}

std::optional<Options> ParseOptions(int argc, char** argv) {
    Options options;
    const auto args = Arguments(argc, argv);
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto name = PortSettings::PathText(args[index]);
        const bool hasValue = index + 1 < args.size();
        if (name == "--settings" && hasValue) {
            options.settings = args[++index];
            options.fromGame = true;
        } else if (name == PortSettings::LaunchArgument) {
            options.launch = true;
        } else if (name == "--deck") {
            options.deck = true;
        } else if (name == "--dry-run") {
            options.dryRun = true;
        } else if (name == "--window" && hasValue) {
            if (!PortSettings::ParseResolution(PortSettings::PathText(args[++index]), options.windowWidth, options.windowHeight)) return std::nullopt;
        } else if (name == "--keys" && hasValue) {
            if (!ParseKeys(PortSettings::PathText(args[++index]), options.keys)) return std::nullopt;
        } else if (name == "--snapshot" && hasValue) {
            options.snapshot = args[++index];
        } else {
            options.gameArgs.push_back(name);  // for the game (e.g. Steam launch options)
        }
    }
    return options;
}

std::string ReadText(const std::filesystem::path& path, bool& found, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    found = static_cast<bool>(file);
    if (!found) return {};
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) error = "cannot read " + PortSettings::PathText(path);
    return text.str();
}

struct ResolutionChoice {
    std::uint32_t width;
    std::uint32_t height;
    std::string label;
};

// One check of the game folder text (and, the first time, of Python): on a thread of its own in the window,
// so a drive that does not answer (a network or disconnected drive) cannot freeze it.
struct CheckJob {
    std::string text;
    std::filesystem::path installDir;
    std::filesystem::path settings;
    bool release = false;
    bool fromGame = false;
    std::string addonDir;
    std::optional<bool> python;
    Launcher::FolderCheck folder;
    std::string setupReason;
    std::vector<AddonContent::Found> addons;
    std::atomic<bool> done{false};
};

void RunCheck(CheckJob& job) {
    job.folder = Launcher::CheckGameFolder(job.text, job.installDir);
    if (job.folder.state == Launcher::FolderState::Valid && Launcher::IsInside(job.settings, job.folder.folder)) {
        job.folder.errors.push_back("The settings file " + PortSettings::PathText(job.settings) + " is inside this dump (APS5_SETTINGS). Nothing is written into the dump: choose another settings file.");
        job.folder.state = Launcher::FolderState::Invalid;
    }
    if (!job.python) job.python = Launcher::PythonUsable();
    job.setupReason = job.release && job.folder.state == Launcher::FolderState::Valid ? Launcher::SetupReason(job.installDir, job.folder.folder, *job.python) : std::string();
    // The add-ons near the dump (the game's own search): the checked folder, or from the game without a
    // saved folder, the app0 next to the settings file whose folders link into the dump.
    std::vector<std::filesystem::path> dumps;
    if (job.folder.state == Launcher::FolderState::Valid) dumps = AddonContent::DumpFolders(job.folder.folder);
    else if (job.fromGame) dumps = AddonContent::DumpFolders(std::filesystem::absolute(job.settings).parent_path() / "app0");
    job.addons = dumps.empty() && job.addonDir.empty() ? std::vector<AddonContent::Found>() : AddonContent::Find(dumps, AddonContent::PathFromText(job.addonDir));
}

// APS5_PRESENT_AUTO (frame rate S3, experimental until the window's changes are approved): the FPS limit
// gains 120 and the display's refresh, and a Present row writes PresentMode. Without it the window shows and
// writes what it did before.
bool PresentAuto() {
    static const bool on = std::getenv("APS5_PRESENT_AUTO") != nullptr;
    return on;
}

// Model::fpsChoice past the first four (auto, 30, 60, custom), only with PresentAuto().
constexpr int FpsChoiceCustom = 3;
constexpr int FpsChoice120 = 4;
constexpr int FpsChoiceDisplay = 5;

// What the window edits; `settings` holds the choices as the parser's values.
struct Model {
    Options options;
    std::filesystem::path installDir;  // the release folder (Play sets up and starts the game); empty elsewhere
    std::string text;
    bool existed = false;
    PortSettings::Settings settings;
    std::vector<ResolutionChoice> resolutions;
    int resolution = 0;
    int fpsChoice = 0;  // auto, 30, 60, custom; FpsChoice120 and FpsChoiceDisplay with APS5_PRESENT_AUTO
    double customFps = 45.0;
    int refreshHz = 0;  // the desktop's refresh, 0 when unknown
    std::string status;
    bool statusError = false;

    bool saveFailed = false;

    std::array<char, 1024> gameDir{};  // the text field (UTF-8)
    std::string checkedText;           // the text `folder` was checked for
    Launcher::FolderCheck folder;
    std::string setupReason;     // why Play runs the setup first; empty when it is done
    std::optional<bool> python;  // a Python 3.8+ for the setup's patch; unknown until the first check
    std::shared_ptr<CheckJob> check;  // the check running on its thread
    std::vector<AddonContent::Found> addons;  // the title's add-ons the checked folder's dump holds
    std::string editedText;           // the text as last edited, checked once it stays for a moment
    double editTime = 0.0;
    double checkTime = 0.0;  // when `check` started
    std::string memoryWarning;
    std::unique_ptr<Launcher::SetupProcess> setup;
    bool setupRunning = false;
    bool autoPlay = false;
    bool confirmQuit = false;  // closing while the setup runs asks first
    bool focusPending = true;  // the first focus (Play, or Browse while Play is disabled) once the folder is checked
    Launcher::FolderBrowser browser;
    Launcher::NativeFolderPicker picker;
    bool pickerBusy = false;
    Launcher::GameProcess game;
};

bool Release(const Model& model) {
    return !model.options.fromGame && !model.installDir.empty();
}

void SetGameDirText(Model& model, const std::string& text) {
    const auto size = std::min(text.size(), model.gameDir.size() - 1);
    std::memcpy(model.gameDir.data(), text.data(), size);
    model.gameDir[size] = '\0';
}

std::shared_ptr<CheckJob> NewCheck(const Model& model, const std::string& text) {
    auto job = std::make_shared<CheckJob>();
    job->text = text;
    job->installDir = model.installDir;
    job->settings = model.options.settings;
    job->release = Release(model);
    job->fromGame = model.options.fromGame;
    job->addonDir = model.settings.addonDir;
    job->python = model.python;
    return job;
}

void ApplyCheck(Model& model, const CheckJob& job) {
    model.checkedText = job.text;
    model.folder = job.folder;
    model.python = job.python;
    model.setupReason = job.setupReason;
    model.addons = job.addons;
}

// Checks the game folder (and whether the setup is due) on this thread: at the start, for Play, after the setup.
void CheckNow(Model& model) {
    const auto job = NewCheck(model, model.gameDir.data());
    RunCheck(*job);
    ApplyCheck(model, *job);
}

// The window's check: an edited text is checked 0.3 s after the last change, on a thread.
void CheckLater(Model& model) {
    const std::string text(model.gameDir.data());
    const double now = ImGui::GetTime();
    if (model.check != nullptr && model.check->done) {
        if (model.check->text == text) ApplyCheck(model, *model.check);
        model.check.reset();
    }
    if (text == model.checkedText) return;
    if (text != model.editedText) {
        model.editedText = text;
        model.editTime = now;
    }
    if (model.check != nullptr || now - model.editTime < 0.3) return;
    model.check = NewCheck(model, text);
    model.checkTime = now;
    std::thread([job = model.check] {
        RunCheck(*job);
        job->done = true;
    }).detach();
}

// Whether the shown check is not the text's (Play waits).
bool Checking(const Model& model) {
    return !model.options.fromGame && std::string(model.gameDir.data()) != model.checkedText;
}

void BuildResolutions(Model& model, int desktopWidth, int desktopHeight) {
    model.resolutions.push_back({0, 0, "Auto (a window at 60% of the screen)"});
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const auto desktop = std::to_string(desktopWidth) + "x" + std::to_string(desktopHeight);
    if (desktopWidth > 0 && PortSettings::ParseResolution(desktop, width, height)) model.resolutions.push_back({width, height, "Desktop (" + desktop + ")"});
    for (const auto& preset : PortSettings::ResolutionPresets) model.resolutions.push_back({preset.width, preset.height, preset.label});
    const auto& settings = model.settings;
    const auto found = std::find_if(model.resolutions.begin(), model.resolutions.end(), [&](const ResolutionChoice& choice) { return choice.width == settings.width && choice.height == settings.height; });
    if (found != model.resolutions.end()) {
        model.resolution = static_cast<int>(found - model.resolutions.begin());
    } else {
        model.resolutions.push_back({settings.width, settings.height, std::to_string(settings.width) + "x" + std::to_string(settings.height) + " (from the file)"});
        model.resolution = static_cast<int>(model.resolutions.size() - 1);
    }
}

void LoadModel(Model& model) {
    model.installDir = model.options.fromGame ? std::filesystem::path() : Launcher::FindInstallDir(Launcher::ExecutablePath().parent_path());
    if (!model.options.fromGame) {
        // The file the game reads: APS5_SETTINGS, else next to the game in the release folder.
        const char* configured = std::getenv("APS5_SETTINGS");
        if (configured != nullptr && configured[0] != '\0' && PortSettings::Lower(configured) != "none") model.options.settings = Launcher::PathFromText(configured);
        else if (!model.installDir.empty()) model.options.settings = model.installDir / PortSettings::FileName;
        else model.options.settings = PortSettings::FileName;
    }
    std::string error;
    model.text = ReadText(model.options.settings, model.existed, error);
    if (!error.empty()) {
        model.status = error;
        model.statusError = true;
    }
    model.settings = PortSettings::Parse(model.text, PortSettings::PathText(model.options.settings.filename()));
    const double fps = model.settings.fpsLimit;
    if (fps == 0.0) model.fpsChoice = 0;
    else if (std::abs(fps - 30.0) < 0.1) model.fpsChoice = 1;
    else if (std::abs(fps - 60.0) < 0.1) model.fpsChoice = 2;
    else if (PresentAuto() && std::abs(fps - 120.0) < 0.1) model.fpsChoice = FpsChoice120;
    else {
        model.fpsChoice = 3;
        model.customFps = fps;
    }
    SetGameDirText(model, model.settings.gameDir);
    model.memoryWarning = Launcher::MemoryWarning();
    // --launch decides at once whether the window is needed (and a snapshot shows a checked folder); the
    // window checks on its thread, the saved folder from its first frame.
    if (model.options.fromGame || model.options.launch || !model.options.snapshot.empty()) CheckNow(model);
    model.editedText = model.gameDir.data();
    model.editTime = -1.0;
}

// The "Display (N Hz)" preset's limit, 0 when the window has no such entry: the refresh is unknown or the
// 30, 60 or 120 entry already matches it.
double DisplayPreset(const Model& model) {
    const double fps = PortSettings::DisplayFpsLimit(model.refreshHz);
    return fps == 30.0 || fps == 60.0 || fps == 120.0 ? 0.0 : fps;
}

// The desktop's refresh, known once SDL runs (after LoadModel): a file limit equal to the display preset selects it.
void SetRefresh(Model& model, int refreshHz) {
    model.refreshHz = refreshHz;
    const double display = DisplayPreset(model);
    if (PresentAuto() && model.fpsChoice == FpsChoiceCustom && display != 0.0 && std::abs(model.customFps - display) < 0.1) model.fpsChoice = FpsChoiceDisplay;
}

// The choices as settings (the launcher's rules applied).
PortSettings::Settings Chosen(const Model& model) {
    auto settings = model.settings;
    const auto& resolution = model.resolutions[static_cast<std::size_t>(model.resolution)];
    settings.width = resolution.width;
    settings.height = resolution.height;
    static constexpr double fixedRates[] = {0.0, 30.0, 60.0};
    // A custom value the slider never moved keeps the file's number (it may be fractional).
    settings.fpsLimit = model.fpsChoice < 3 ? fixedRates[model.fpsChoice]
        : model.fpsChoice == FpsChoice120   ? 120.0
        : model.fpsChoice == FpsChoiceDisplay ? DisplayPreset(model)
                                              : model.customFps;
    if (!model.options.fromGame) {
        // A valid folder as resolved (the app0 inside a picked parent); otherwise the text, kept for next time
        // (not one with a double quote, which the file's quoting cannot hold).
        const auto text = Launcher::CleanFolderText(model.gameDir.data());
        settings.gameDir = !Checking(model) && model.folder.state == Launcher::FolderState::Valid ? PortSettings::PathText(model.folder.folder)
            : text.find('"') == std::string::npos                                                ? text
                                                                                                 : std::string();
    }
    PortSettings::ApplyLauncherRules(settings);
    return settings;
}

std::string NewText(const Model& model) {
    const std::string header = model.existed ? std::string() : std::string("# AnyPS5 settings, written by the launcher (keys: anyps5-settings.ini.example).\n");
    // The window's own folder is always written, so clearing the field clears it in the file.
    const auto chosen = Chosen(model);
    auto values = PortSettings::LauncherValuesWithGameDir(chosen, !model.options.fromGame);
    // The Present row's choice with APS5_PRESENT_AUTO; without it a PresentMode line in the file stays as it is.
    if (PresentAuto()) values.emplace_back("PresentMode", PortSettings::PresentModeName(chosen.presentMode));
    return PortSettings::SetIniValues(model.existed ? model.text : header, values);
}

bool Save(Model& model) {
    const auto text = NewText(model);
    if (model.existed && text == model.text) {
        std::fprintf(stderr, "[launcher] %s unchanged\n", PortSettings::PathText(model.options.settings).c_str());
        return true;
    }
    std::string error;
    if (!PortSettings::WriteFileAtomic(model.options.settings, text, error)) {
        model.status = "Cannot save " + PortSettings::PathText(model.options.settings) + ": " + error;
        model.statusError = true;
        model.saveFailed = true;
        std::fprintf(stderr, "[launcher] %s\n", model.status.c_str());
        return false;
    }
    model.saveFailed = false;
    std::fprintf(stderr, "[launcher] saved %s\n", PortSettings::PathText(model.options.settings).c_str());
    model.text = text;
    model.existed = true;
    return true;
}

// Starts the game; ExitGameStarted, or -1 with the error shown.
int StartPlaying(Model& model) {
    std::string error;
    if (Launcher::LaunchGame(model.installDir, model.options.gameArgs, model.options.dryRun, model.game, error)) return ExitGameStarted;
    model.status = error;
    model.statusError = true;
    std::fprintf(stderr, "[launcher] %s\n", error.c_str());
    return -1;
}

void StartSetup(Model& model) {
    model.setup = std::make_unique<Launcher::SetupProcess>();
    std::string error;
    std::fprintf(stderr, "[launcher] setup: %s\n", model.setupReason.c_str());
    if (!model.setup->Start(model.installDir, model.folder.folder, error)) {
        model.status = "The setup did not start: " + error;
        model.statusError = true;
        std::fprintf(stderr, "[launcher] %s\n", model.status.c_str());
        return;
    }
    model.setupRunning = true;
    if (!model.saveFailed) {
        model.status.clear();
        model.statusError = false;
    }
}

// Play: save, then (release folder) the setup when due, then the game. The run's result, or -1 to stay.
int Play(Model& model) {
    // A settings file that cannot be written does not keep the player from the game: it starts with the
    // file as it is (only the Save button of a build tree stops at the error).
    if (!Save(model)) {
        if (!model.options.fromGame && !Release(model)) return -1;
        model.status += ". The game starts with the settings file as it is.";
        std::fprintf(stderr, "[launcher] going on without saving\n");
    }
    if (!Release(model)) return 0;
    CheckNow(model);
    if (model.folder.state != Launcher::FolderState::Valid) return -1;
    if (!model.setupReason.empty()) {
        StartSetup(model);
        return -1;
    }
    return StartPlaying(model);
}

// A finished setup: the stamp, then the game; or its failure explained.
int SetupFinished(Model& model) {
    model.setupRunning = false;
    const int code = model.setup->ExitCode();
    std::fprintf(stderr, "[launcher] setup exited with code %d\n", code);
    if (code == 0) {
        std::string error;
        // Unpatched when the script found no Python 3.8+ (its own check decides, whatever this launcher found).
        const bool patched = model.python.value_or(false) && !Launcher::SetupLeftUnpatched(model.setup->Lines());
        const auto stamp = Launcher::SetupStamp(model.folder.folder, Launcher::ToolsFingerprint(model.installDir), Launcher::DumpFingerprint(model.folder.folder), patched);
        if (!PortSettings::WriteFileAtomic(model.installDir / Launcher::StampName, stamp, error)) std::fprintf(stderr, "[launcher] cannot write %s: %s\n", Launcher::StampName, error.c_str());
        CheckNow(model);
        if (model.folder.state == Launcher::FolderState::Valid) model.setupReason = Launcher::SetupReason(model.installDir, model.folder.folder, patched);
        if (model.folder.state == Launcher::FolderState::Valid && model.setupReason.empty()) return StartPlaying(model);
        model.status = "The setup ended without errors but is not complete (" + model.setupReason + "). Its output is above.";
        model.statusError = true;
        return -1;
    }
    bool policy = false;
    for (const auto& line : model.setup->Lines()) {
        const auto lower = PortSettings::Lower(line);
        if (lower.find("execution polic") != std::string::npos || lower.find("not digitally signed") != std::string::npos || lower.find("cannot be loaded") != std::string::npos) policy = true;
    }
    model.status = policy ? "PowerShell refused to run DemonsSouls.ps1 (an execution policy set by a group policy). Right-click DemonsSouls.ps1 > Properties > Unblock, or ask the PC's administrator to allow it."
                          : "The setup failed (exit code " + std::to_string(code) + "). Its output is above; fix the cause and press Play to try again.";
    model.statusError = true;
    return -1;
}

// A readable sans-serif font of the system, else ImGui's built-in one.
void LoadFont(ImGuiIO& io) {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    if (const char* windows = std::getenv("WINDIR")) {
        candidates.push_back(std::filesystem::path(windows) / "Fonts" / "segoeui.ttf");
        candidates.push_back(std::filesystem::path(windows) / "Fonts" / "arial.ttf");
    }
#else
    for (const char* path : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})
        candidates.emplace_back(path);
#endif
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error)) continue;
        if (io.Fonts->AddFontFromFileTTF(PortSettings::PathText(candidate).c_str(), 20.0f) != nullptr) return;
    }
    io.Fonts->AddFontDefault();
}

void Label(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Colored(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// The Deluxe Edition add-on (s61 T-deluxe): it can be on only when the dump holds it (AddonContent.hpp,
// the same search the game's entitlement query answers from); the checkbox turns an owned add-on off.
void DrawDeluxe(Model& model) {
    Label("Deluxe Edition content");
    const auto* found = Checking(model) ? nullptr : AddonContent::FindLabel(model.addons, AddonContent::DeluxeEditionLabel);
    // APS5_DELUXE_EDITION wins over the ini (the game reads it): the box shows what the game will do.
    const char* environment = std::getenv("APS5_DELUXE_EDITION");
    const bool overridden = environment != nullptr && environment[0] != '\0';
    const std::string value = overridden ? AddonContent::Upper(environment) : std::string();
    const auto effective = overridden ? (value == "OFF" || value == "0" || value == "FALSE" ? PortSettings::DeluxeEdition::Off : PortSettings::DeluxeEdition::Auto) : model.settings.deluxeEdition;
    ImGui::BeginDisabled(found == nullptr || overridden);
    bool enabled = found != nullptr && effective != PortSettings::DeluxeEdition::Off;
    if (ImGui::Checkbox("Deluxe Edition items (Fluted armour set)##deluxe", &enabled)) model.settings.deluxeEdition = enabled ? PortSettings::DeluxeEdition::Auto : PortSettings::DeluxeEdition::Off;
    ImGui::EndDisabled();
    if (overridden) Note((std::string("(set by APS5_DELUXE_EDITION=") + environment + ")").c_str());
    if (Checking(model)) {
        Note("Checking the folder...");
    } else if (found != nullptr) {
        Colored(Good, "Found in your dump");
        Note((PortSettings::PathText(found->folder) + " (" + found->reason + "). Offered when creating a character; turned off, the game takes the items away from a character that has them.").c_str());
    } else {
        Note("Not found in your dump");
    }
}

// The setup script's current step ("== relinking ...") in plain words.
std::string SetupStep(const std::vector<std::string>& lines) {
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        if (line->starts_with("== relinking")) return "Step 1 of 3: relinking eboot.bin into the game executable (a minute or two)";
        if (line->starts_with("== patching")) return "Step 2 of 3: patching the executable (ds_patch)";
        if (line->starts_with("== linking")) return "Step 3 of 3: linking the dump's folders into app0";
        if (line->starts_with("== setup done")) return "Setup done";
    }
    return "Starting the setup";
}

enum class Action { None, Play, Quit, QuitNow, Browse };

void DrawGameFolder(Model& model, Action& action, bool focusBrowse) {
    Label("Game folder");
    const bool fixed = model.options.fromGame;
    ImGui::BeginDisabled(fixed);
    const float browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
#ifdef _WIN32
    const char* hint = "D:\\Games\\PPSA01341-app0";
#else
    const char* hint = "/home/deck/Games/PPSA01341-app0";
#endif
    ImGui::InputTextWithHint("##gamedir", hint, model.gameDir.data(), model.gameDir.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(browseWidth, 0.0f))) action = Action::Browse;
    if (focusBrowse) {
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNavCursorVisible(true);
    }
    ImGui::EndDisabled();
    if (!fixed) CheckLater(model);

    const auto& folder = model.folder;
    if (Checking(model)) {
        if (model.check != nullptr && ImGui::GetTime() - model.checkTime > 5.0) Colored(Warn, "Still checking the folder: its drive does not answer (a network or disconnected drive?).");
        else Note("Checking the folder...");
    } else if (folder.state == Launcher::FolderState::Empty) Note(folder.summary.c_str());
    else if (folder.state == Launcher::FolderState::Valid) Colored(Good, folder.summary);
    if (!Checking(model)) {
        for (const auto& error : folder.errors) Colored(Bad, error);
        for (const auto& warning : folder.warnings) Colored(Warn, warning);
    }
    if (fixed) Note("The game runs from the folder it was set up with. To change it, quit the game and start anyps5-launcher on its own.");
    if (model.pickerBusy) Note("Waiting for the folder picker...");
}

void DrawSetup(Model& model) {
    Label("Setup");
    if (model.setupRunning) {
        const auto lines = model.setup->Lines();
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0.0f), "Setting up...");
        ImGui::TextWrapped("%s", SetupStep(lines).c_str());
    } else if (Checking(model) || model.folder.state != Launcher::FolderState::Valid) {
        Note("Needs a valid game folder.");
    } else if (model.setupReason.empty()) {
        Colored(Good, "Done. Play starts the game.");
    } else {
        Note(("Play runs the one-time setup first (" + model.setupReason +
              "): it relinks eboot.bin into the game executable, patches it and links the dump's folders into AnyPS5's app0 folder. Nothing in the dump is changed.")
                 .c_str());
    }
    if (model.python.has_value() && !*model.python && (!model.setupReason.empty() || model.setupRunning)) {
#ifdef _WIN32
        Colored(Warn, "Python 3.8 or newer was not found: the setup skips the speed patch (ds_patch) and the game runs much slower. Install it with: winget install "
                      "Python.Python.3.12 (or from python.org), then start this window again.");
#else
        Colored(Warn, "python3 3.8 or newer was not found: the setup skips the speed patch (ds_patch) and the game runs much slower. Install python3 with your package "
                      "manager, then start this window again.");
#endif
    }
    if (!model.memoryWarning.empty()) Colored(Warn, model.memoryWarning);
}

// The FPS limit's entries with APS5_PRESENT_AUTO: 120 and, when no entry matches the display, its refresh.
void DrawFpsPresets(Model& model) {
    std::vector<std::pair<std::string, int>> entries{{"Auto (the game's graphics mode)", 0}, {"30", 1}, {"60", 2}, {"120", FpsChoice120}};
    if (DisplayPreset(model) != 0.0) entries.emplace_back("Display (" + std::to_string(model.refreshHz) + " Hz)", FpsChoiceDisplay);
    entries.emplace_back("Custom", FpsChoiceCustom);
    const auto current = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.second == model.fpsChoice; });
    if (ImGui::BeginCombo("##fps", current != entries.end() ? current->first.c_str() : "")) {
        for (const auto& [name, choice] : entries) {
            const bool selected = choice == model.fpsChoice;
            if (ImGui::Selectable(name.c_str(), selected)) model.fpsChoice = choice;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// The Present row with APS5_PRESENT_AUTO: how finished frames reach the display (PresentMode; the game's
// ChoosePresentMode decides auto).
void DrawPresent(Model& model, const PortSettings::Settings& chosen) {
    using PortSettings::PresentMode;
    Label("Present");
    // APS5_PRESENT_MODE wins over the ini (the game reads it): the row shows what the game will do.
    const char* environment = std::getenv("APS5_PRESENT_MODE");
    const bool overridden = environment != nullptr && environment[0] != '\0';
    PortSettings::Settings fromEnvironment;
    if (overridden) PortSettings::SetKey(fromEnvironment, "PresentMode", environment, "APS5_PRESENT_MODE");
    const PresentMode mode = overridden ? fromEnvironment.presentMode : model.settings.presentMode;
    // In PresentMode's order.
    static const char* const presentNames[] = {"Auto", "Wait for the display (fifo)", "Newest frame (mailbox)", "Immediate (tears)"};
    int present = static_cast<int>(mode);
    ImGui::BeginDisabled(overridden);
    if (ImGui::Combo("##present", &present, presentNames, IM_ARRAYSIZE(presentNames))) model.settings.presentMode = static_cast<PresentMode>(present);
    ImGui::EndDisabled();
    if (overridden) Note((std::string("(set by APS5_PRESENT_MODE=") + environment + ")").c_str());
    if (model.refreshHz <= 0 || !(chosen.fpsLimit > model.refreshHz + 1.0)) return;
    const auto refresh = std::to_string(model.refreshHz);
    if (mode == PresentMode::Auto || mode == PresentMode::Mailbox)
        Note(("Above the display's " + refresh + " Hz: the newest frame is shown at each refresh (frames dropped, no tearing).").c_str());
    else if (mode == PresentMode::Fifo)
        Note(("The display shows at most " + refresh + " frames a second and holds the game to that rate.").c_str());
}

Action Draw(Model& model) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    Action action = Action::None;
    const bool release = Release(model);
    const bool busy = model.setupRunning || model.pickerBusy;
    const bool canPlay = !busy && (!release || (!Checking(model) && model.folder.state == Launcher::FolderState::Valid));
    const bool firstFocus = model.focusPending && !Checking(model);
    if (firstFocus) model.focusPending = false;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("Demon's Souls");
    ImGui::PopFont();
    Note("AnyPS5 settings. Arrow keys / D-pad move, Enter / A changes, Start plays.");
    ImGui::Separator();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    ImGui::BeginChild("options", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.68f);

        DrawGameFolder(model, action, firstFocus && !canPlay && !model.options.fromGame);
        if (release) DrawSetup(model);

        Label("Resolution");
        const auto& current = model.resolutions[static_cast<std::size_t>(model.resolution)];
        if (ImGui::BeginCombo("##resolution", current.label.c_str())) {
            for (std::size_t index = 0; index < model.resolutions.size(); ++index) {
                const bool selected = static_cast<int>(index) == model.resolution;
                if (ImGui::Selectable(model.resolutions[index].label.c_str(), selected)) model.resolution = static_cast<int>(index);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        Label("Window mode");
        bool fullscreen = model.settings.windowMode == PortSettings::WindowMode::Fullscreen;
        if (ImGui::RadioButton("Windowed", !fullscreen)) model.settings.windowMode = PortSettings::WindowMode::Windowed;
        ImGui::SameLine();
        if (ImGui::RadioButton("Fullscreen", fullscreen)) model.settings.windowMode = PortSettings::WindowMode::Fullscreen;

        Label("Button icons");
        // APS5_BUTTON_ICONS wins over the ini (the game reads it): the row shows what the game will do.
        const char* iconsEnvironment = std::getenv("APS5_BUTTON_ICONS");
        const bool iconsOverridden = iconsEnvironment != nullptr && iconsEnvironment[0] != '\0';
        PortSettings::Settings iconsFromEnvironment;
        if (iconsOverridden) PortSettings::SetKey(iconsFromEnvironment, "ButtonIcons", iconsEnvironment, "APS5_BUTTON_ICONS");
        const bool deck = (iconsOverridden ? iconsFromEnvironment.buttonIcons : model.settings.buttonIcons) == PortSettings::ButtonIcons::SteamDeck;
        ImGui::BeginDisabled(iconsOverridden);
        if (ImGui::RadioButton("PlayStation", !deck)) model.settings.buttonIcons = PortSettings::ButtonIcons::PlayStation;
        ImGui::SameLine();
        if (ImGui::RadioButton("Steam Deck", deck)) model.settings.buttonIcons = PortSettings::ButtonIcons::SteamDeck;
        ImGui::EndDisabled();
        if (iconsOverridden) Note((std::string("(set by APS5_BUTTON_ICONS=") + iconsEnvironment + ")").c_str());
        if (deck) Note("A, B, X, Y, Menu and View in the prompts; View works as the touchpad.");

        Label("FPS limit");
        static const char* const fpsNames[] = {"Auto (the game's graphics mode)", "30", "60", "Custom"};
        if (PresentAuto()) DrawFpsPresets(model);
        else ImGui::Combo("##fps", &model.fpsChoice, fpsNames, IM_ARRAYSIZE(fpsNames));
        if (model.fpsChoice == 3) {
            int custom = static_cast<int>(std::lround(model.customFps));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##customfps", &custom, 20, 240, "%d FPS", ImGuiSliderFlags_AlwaysClamp)) model.customFps = custom;
        }

        const auto chosen = Chosen(model);
        const bool forced = chosen.fpsLimit != 0.0 && !PortSettings::IsGameRate(chosen.fpsLimit);
        Label("Timestep");
        ImGui::BeginDisabled(forced);
        if (ImGui::RadioButton("Fixed", !chosen.variableTimestep)) model.settings.variableTimestep = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Variable", chosen.variableTimestep)) model.settings.variableTimestep = true;
        ImGui::EndDisabled();
        if (forced) Note("Limits other than 30 and 60 need the variable timestep (set automatically).");
        else Note("Variable: the game speed follows the real frame rate (experimental).");
        if (PresentAuto()) DrawPresent(model, chosen);

        Label("Native aspect");
        ImGui::BeginDisabled(chosen.width == 0);
        bool native = chosen.nativeAspect;
        if (ImGui::Checkbox("Render at the resolution's shape (wider view, experimental)", &native)) model.settings.nativeAspect = native;
        ImGui::EndDisabled();
        if (chosen.width == 0) Note("Needs a resolution other than Auto.");

        Label("Upscaler");
        ImGui::BeginDisabled();
        int upscaler = 0;
        static const char* const upscalerNames[] = {"Off (FSR / DLSS coming later)"};
        ImGui::Combo("##upscaler", &upscaler, upscalerNames, IM_ARRAYSIZE(upscalerNames));
        ImGui::EndDisabled();

        Label("Frame generation");
        ImGui::BeginDisabled();
        bool frameGeneration = false;
        ImGui::Checkbox("Coming later", &frameGeneration);
        ImGui::EndDisabled();

        DrawDeluxe(model);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    // The setup's own output while it runs, and after it failed.
    if (model.setup != nullptr && (model.setupRunning || model.setup->ExitCode() != 0)) {
        ImGui::Spacing();
        ImGui::BeginChild("setuplog", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 7.0f), ImGuiChildFlags_Borders);
        for (const auto& line : model.setup->Lines()) ImGui::TextUnformatted(line.c_str());
        if (model.setupRunning) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // What the game will make of it: the parser's own summary and warnings on the text that would be saved.
    auto preview = PortSettings::Parse(NewText(model), PortSettings::PathText(model.options.settings.filename()));
    PortSettings::Normalize(preview);
    preview.source = PortSettings::PathText(model.options.settings);
    ImGui::Spacing();
    Note(PortSettings::Describe(preview).c_str());
    for (const auto& warning : preview.warnings) Note(warning.c_str());
    std::string overridden;
    for (const auto& [variable, key] : PortSettings::EnvironmentKeys) {
        const char* value = std::getenv(variable);
        if (value != nullptr && value[0] != '\0') overridden += std::string(overridden.empty() ? "" : ", ") + variable + "=" + value;
    }
    if (!overridden.empty()) Note(("Set in the environment, wins over this window: " + overridden).c_str());
    ImGui::EndChild();

    ImGui::Separator();
    if (!model.status.empty()) Colored(model.statusError ? Bad : ImGui::GetStyleColorVec4(ImGuiCol_Text), model.status);
    const char* playLabel = model.options.fromGame || release ? "Play" : "Save";
    const float buttonWidth = std::max(ImGui::CalcTextSize("Quit").x, ImGui::CalcTextSize(playLabel).x) + ImGui::GetStyle().FramePadding.x * 8.0f;
    ImGui::BeginDisabled(!canPlay);
    if (ImGui::Button(playLabel, ImVec2(buttonWidth, 0.0f))) action = Action::Play;
    // Play starts focused with the cursor shown, so the first Enter / A plays (a hidden cursor eats it).
    if (firstFocus && canPlay) {
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNavCursorVisible(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(buttonWidth, 0.0f))) action = Action::Quit;
    if (canPlay && !model.browser.IsOpen() && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) action = Action::Play;
    if (release) {
        Note(("Settings: " + PortSettings::PathText(model.options.settings) +
              ". Steam: add this launcher as a non-Steam game; the launch option --launch (%command% --launch) starts the game without this window once it is set up.")
                 .c_str());
    } else {
        Note(("Settings file: " + PortSettings::PathText(model.options.settings) + ". Start the game with --launch to skip this window (Steam launch options: %command% --launch).").c_str());
    }
    ImGui::End();

    std::filesystem::path chosenFolder;
    if (model.browser.Draw(chosenFolder)) SetGameDirText(model, PortSettings::PathText(chosenFolder));

    // Closing while the setup runs: stopping it leaves the setup to run again on the next Play.
    if (model.confirmQuit) {
        ImGui::OpenPopup("Stop the setup?");
        model.confirmQuit = false;
    }
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.6f, 0.0f));
    if (ImGui::BeginPopupModal("Stop the setup?", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("The setup is still running. Quitting stops it; it runs again from the start on the next Play. Your dump is not affected.");
        ImGui::Spacing();
        if (ImGui::Button("Stop and quit")) action = Action::QuitNow;
        ImGui::SameLine();
        if (ImGui::Button("Keep setting up") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::EndPopup();
    }
    return action;
}

bool WriteSnapshot(SDL_Renderer* renderer, const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0) return false;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) return false;
    bool written = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0
        && SDL_SaveBMP(surface, PortSettings::PathText(path).c_str()) == 0;
    SDL_FreeSurface(surface);
    return written;
}

// Waits for the started game. Without a console window of its own, a game that ended with an error says
// so in a message box that names its log (the window is gone by then).
int WaitAndReport(Model& model) {
    const int code = Launcher::WaitGame(model.game);
    std::fprintf(stderr, "[launcher] the game exited with code %d\n", code);
    if (code != 0 && !model.game.log.empty()) {
        char text[64];
        std::snprintf(text, sizeof(text), code < 0 || code > 0xffff ? "0x%08X" : "%d", static_cast<unsigned>(code));
        const auto message = std::string("Demon's Souls ended with an error (exit code ") + text + ").\n\nIts output is in " + PortSettings::PathText(model.game.log) +
            " (include it when you report the problem).";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Demon's Souls - AnyPS5", message.c_str(), nullptr);
    }
    return code;
}

int Run(Model& model) {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[launcher] SDL_Init: %s\n", SDL_GetError());
        return ExitFailure;
    }
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(0, &desktop) != 0) desktop = {};
    BuildResolutions(model, desktop.w, desktop.h);
    SetRefresh(model, desktop.refresh_rate);

    // A Steam Deck sized screen (or smaller) gets the whole screen; a desktop a window of 60% of its height.
    const bool small = desktop.w > 0 && (desktop.w <= 1280 || desktop.h <= 800);
    int width = model.options.windowWidth != 0 ? static_cast<int>(model.options.windowWidth) : 960;
    int height = model.options.windowHeight != 0 ? static_cast<int>(model.options.windowHeight) : 600;
    if (model.options.windowWidth == 0 && desktop.h > 0) {
        height = std::clamp(static_cast<int>(desktop.h * 0.6), std::min(600, desktop.h), desktop.h);
        width = std::min(static_cast<int>(height * 1.6), static_cast<int>(desktop.w * 0.9));
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (small && model.options.windowWidth == 0) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window* window = SDL_CreateWindow("Demon's Souls - AnyPS5 settings", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (window == nullptr) {
        std::fprintf(stderr, "[launcher] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return ExitFailure;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) renderer = SDL_CreateRenderer(window, -1, 0);
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return ExitFailure;
    }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer, &info);
    std::fprintf(stderr, "[launcher] window %dx%d%s, renderer %s, settings %s%s, %s\n", width, height, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 ? " (fullscreen)" : "",
        info.name != nullptr ? info.name : "?", PortSettings::PathText(model.options.settings).c_str(), model.existed ? "" : " (new)",
        model.options.fromGame ? "from the game" : model.installDir.empty() ? "no release folder (Save only)" : ("release folder " + PortSettings::PathText(model.installDir)).c_str());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    LoadFont(io);

    // Sizes follow the window: 20 px text at 720 lines, so 22 px on a Deck and larger on 1440p/4K.
    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
    const float scale = std::clamp(static_cast<float>(outputHeight) / 720.0f, 0.75f, 4.0f);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FrameRounding = 4.0f * scale;
    ImGui::GetStyle().ItemSpacing.y *= 1.6f;
    ImGui::GetStyle().CellPadding.y *= 2.0f;
    const bool deck = model.options.deck || Launcher::IsSteamDeck();

    int result = -1;
    int frame = 0;
    while (result < 0) {
        SDL_Event event;
        // 30 frames a second at most while idle (enough for held D-pad repeats), at once on input.
        if (SDL_WaitEventTimeout(&event, 33)) {
            do {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                    if (model.setupRunning) model.confirmQuit = true;
                    else result = PortSettings::LauncherExitQuit;
                }
            } while (SDL_PollEvent(&event));
        }
        if (result >= 0) break;
        if (model.pickerBusy) {
            std::filesystem::path picked;
            const auto outcome = model.picker.Poll(picked);
            if (outcome == Launcher::NativeFolderPicker::Outcome::Picked) SetGameDirText(model, PortSettings::PathText(picked));
            if (outcome == Launcher::NativeFolderPicker::Outcome::Unavailable) {
                std::fprintf(stderr, "[launcher] no system folder picker: the built-in browser\n");
                model.browser.Open(model.folder.folder.empty() ? Launcher::PathFromText(Launcher::CleanFolderText(model.gameDir.data())) : model.folder.folder);
            }
            if (outcome != Launcher::NativeFolderPicker::Outcome::Busy) model.pickerBusy = false;
        }
        SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        ImGui::GetStyle().FontScaleMain = std::clamp(static_cast<float>(outputHeight) / 720.0f, 0.75f, 4.0f);
        const auto& keys = model.options.keys;
        if (const int step = frame - 5; step >= 0 && step / 4 < static_cast<int>(keys.size()) && step % 4 < 2) {
            io.AddKeyEvent(keys[static_cast<std::size_t>(step / 4)], step % 4 == 0);
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        auto action = Draw(model);
        ImGui::Render();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 15, 15, 18, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        ++frame;
        if (!model.options.snapshot.empty() && frame == 5 + 4 * static_cast<int>(model.options.keys.size())) {
            const bool written = WriteSnapshot(renderer, model.options.snapshot);
            std::fprintf(stderr, "[launcher] snapshot %s%s\n", PortSettings::PathText(model.options.snapshot).c_str(), written ? "" : " failed");
            result = written ? ExitSnapshot : ExitFailure;
        }
        SDL_RenderPresent(renderer);
        if (result >= 0) break;
        // --launch with only the setup missing: set up (and then play) at once.
        if (model.autoPlay && frame == 2) {
            model.autoPlay = false;
            action = Action::Play;
        }
        if (action == Action::Quit && model.setupRunning) model.confirmQuit = true;
        else if (action == Action::Quit || action == Action::QuitNow) result = PortSettings::LauncherExitQuit;
        if (action == Action::Browse) {
            const auto start = model.folder.folder.empty() ? Launcher::PathFromText(Launcher::CleanFolderText(model.gameDir.data())) : model.folder.folder;
            if (deck) {
                model.browser.Open(start);
            } else {
                model.picker.Start(window, start);
                model.pickerBusy = true;
            }
        }
        if (action == Action::Play) result = Play(model);
        if (model.setupRunning && model.setup->Finished()) result = SetupFinished(model);
    }

#ifdef _WIN32
    // The game's window opens after this one closes: let it take the foreground this window had.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}

}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    const auto options = ParseOptions(argc, argv);
    if (!options) {
        std::fprintf(stderr, "usage: %s [--settings <file>] [--launch] [--window WIDTHxHEIGHT] [--deck] [--dry-run] [--keys <k,k,...>] [--snapshot <file.bmp>] [game arguments]\n",
            PortSettings::LauncherName);
        return ExitFailure;
    }
    auto model = std::make_unique<Model>();
    model->options = *options;
    LoadModel(*model);
    if (Release(*model) && model->options.launch) {
        // --launch: straight to the game when it can; the window only to set up or to fix the folder.
        if (model->folder.state == Launcher::FolderState::Valid && model->setupReason.empty() && model->options.snapshot.empty()) {
            std::fprintf(stderr, "[launcher] --launch: the game folder is set up\n");
            if (StartPlaying(*model) == ExitGameStarted) return WaitAndReport(*model);
        } else if (model->folder.state == Launcher::FolderState::Valid) {
            model->autoPlay = true;
        } else {
            model->status = "The game could not start straight away (--launch): choose a valid game folder, then press Play.";
            model->statusError = true;
        }
    }
    int result = Run(*model);
    std::fprintf(stderr, "[launcher] %s\n", result == 0 ? (Release(*model) ? "saved" : "Play") : result == PortSettings::LauncherExitQuit ? "Quit" : result == ExitSnapshot ? "snapshot taken" : result == ExitGameStarted ? "game started" : "failed");
    std::fflush(stderr);
    if (result == ExitGameStarted) result = WaitAndReport(*model);
    return result;
}
