// AnyPS5 settings launcher (s61): edits anyps5-settings.ini before the game starts. libc's settings load
// (prx/libc/src/PortSettings.cpp) runs it as a child process and waits: exit code 0 = Play (the game starts
// and reads the file), PortSettings::LauncherExitQuit = the window was closed (the game exits), any other
// code = it failed (the game starts with the file as it is). Dear ImGui on SDL2's renderer (Direct3D on
// Windows, the software renderer on Linux), with mouse, keyboard and gamepad navigation; sized for a
// Steam Deck (1280x800 fullscreen) as well as a desktop window.
//   anyps5-launcher [--settings <file>] [--window WIDTHxHEIGHT] [--keys <k,k,...>] [--snapshot <file.bmp>]
// --settings: the file to edit (libc passes it); without it the launcher edits anyps5-settings.ini in the
// working directory and its button says Save. --window: the window's size instead of the automatic one.
// --keys (tests): presses up, down, left, right, enter, space, escape, tab or start (gamepad) one after
// another into ImGui, a key every 4 frames from frame 5. --snapshot (tests): writes the frame after the
// keys (frame 5 without them) as a BMP and exits with code 4 without saving.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "prx/libc/include/PortSettings.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
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

struct Options {
    std::filesystem::path settings;
    bool fromGame = false;
    std::uint32_t windowWidth = 0;
    std::uint32_t windowHeight = 0;
    std::filesystem::path snapshot;
    std::vector<ImGuiKey> keys;
};

bool ParseKeys(const std::string& text, std::vector<ImGuiKey>& keys) {
    static constexpr std::pair<const char*, ImGuiKey> names[] = {{"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
        {"right", ImGuiKey_RightArrow}, {"enter", ImGuiKey_Enter}, {"space", ImGuiKey_Space}, {"escape", ImGuiKey_Escape}, {"tab", ImGuiKey_Tab},
        {"start", ImGuiKey_GamepadStart}};
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
        } else if (name == "--window" && hasValue) {
            if (!PortSettings::ParseResolution(PortSettings::PathText(args[++index]), options.windowWidth, options.windowHeight)) return std::nullopt;
        } else if (name == "--keys" && hasValue) {
            if (!ParseKeys(PortSettings::PathText(args[++index]), options.keys)) return std::nullopt;
        } else if (name == "--snapshot" && hasValue) {
            options.snapshot = args[++index];
        } else {
            return std::nullopt;
        }
    }
    if (options.settings.empty()) options.settings = PortSettings::FileName;
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

// What the window edits; `settings` holds the choices as the parser's values.
struct Model {
    Options options;
    std::string text;
    bool existed = false;
    PortSettings::Settings settings;
    std::vector<ResolutionChoice> resolutions;
    int resolution = 0;
    int fpsChoice = 0;  // auto, 30, 60, custom
    double customFps = 45.0;
    std::string status;
    bool statusError = false;
};

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
    else {
        model.fpsChoice = 3;
        model.customFps = fps;
    }
}

// The choices as settings (the launcher's rules applied).
PortSettings::Settings Chosen(const Model& model) {
    auto settings = model.settings;
    const auto& resolution = model.resolutions[static_cast<std::size_t>(model.resolution)];
    settings.width = resolution.width;
    settings.height = resolution.height;
    static constexpr double fixedRates[] = {0.0, 30.0, 60.0};
    // A custom value the slider never moved keeps the file's number (it may be fractional).
    settings.fpsLimit = model.fpsChoice < 3 ? fixedRates[model.fpsChoice] : model.customFps;
    PortSettings::ApplyLauncherRules(settings);
    return settings;
}

std::string NewText(const Model& model) {
    const std::string header = model.existed ? std::string() : std::string("# AnyPS5 settings, written by the launcher (keys: anyps5-settings.ini.example).\n");
    return PortSettings::SetIniValues(model.existed ? model.text : header, PortSettings::LauncherValues(Chosen(model)));
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
        std::fprintf(stderr, "[launcher] %s\n", model.status.c_str());
        return false;
    }
    std::fprintf(stderr, "[launcher] saved %s\n", PortSettings::PathText(model.options.settings).c_str());
    model.text = text;
    model.existed = true;
    return true;
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

enum class Action { None, Play, Quit };

Action Draw(Model& model, bool firstFrame) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    Action action = Action::None;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("Demon's Souls");
    ImGui::PopFont();
    Note("AnyPS5 settings. Arrow keys / D-pad move, Enter / A changes, Start plays.");
    ImGui::Separator();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    ImGui::BeginChild("options", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
    if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.68f);

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

        Label("FPS limit");
        static const char* const fpsNames[] = {"Auto (the game's graphics mode)", "30", "60", "Custom"};
        ImGui::Combo("##fps", &model.fpsChoice, fpsNames, IM_ARRAYSIZE(fpsNames));
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
        ImGui::EndTable();
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
    if (!model.status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, model.statusError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::TextWrapped("%s", model.status.c_str());
        ImGui::PopStyleColor();
    }
    const float buttonWidth = std::max(ImGui::CalcTextSize("Quit").x, ImGui::CalcTextSize("Play").x) + ImGui::GetStyle().FramePadding.x * 8.0f;
    if (ImGui::Button(model.options.fromGame ? "Play" : "Save", ImVec2(buttonWidth, 0.0f))) action = Action::Play;
    // Play starts focused with the cursor shown, so the first Enter / A plays (a hidden cursor eats it).
    if (firstFrame) {
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNavCursorVisible(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(buttonWidth, 0.0f))) action = Action::Quit;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) action = Action::Play;
    Note(("Settings file: " + PortSettings::PathText(model.options.settings) + ". Start the game with --launch to skip this window (Steam launch options: %command% --launch).").c_str());
    ImGui::End();
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
    std::fprintf(stderr, "[launcher] window %dx%d%s, renderer %s, settings %s%s\n", width, height, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 ? " (fullscreen)" : "",
        info.name != nullptr ? info.name : "?", PortSettings::PathText(model.options.settings).c_str(), model.existed ? "" : " (new)");

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

    int result = -1;
    int frame = 0;
    while (result < 0) {
        SDL_Event event;
        // 30 frames a second at most while idle (enough for held D-pad repeats), at once on input.
        if (SDL_WaitEventTimeout(&event, 33)) {
            do {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) result = PortSettings::LauncherExitQuit;
            } while (SDL_PollEvent(&event));
        }
        if (result >= 0) break;
        SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        ImGui::GetStyle().FontScaleMain = std::clamp(static_cast<float>(outputHeight) / 720.0f, 0.75f, 4.0f);
        const auto& keys = model.options.keys;
        if (const int step = frame - 5; step >= 0 && step / 4 < static_cast<int>(keys.size()) && step % 4 < 2) {
            io.AddKeyEvent(keys[static_cast<std::size_t>(step / 4)], step % 4 == 0);
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto action = Draw(model, frame == 0);
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
        if (action == Action::Quit) result = PortSettings::LauncherExitQuit;
        if (action == Action::Play && Save(model)) result = 0;
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
        std::fprintf(stderr, "usage: %s [--settings <file>] [--window WIDTHxHEIGHT] [--keys <k,k,...>] [--snapshot <file.bmp>]\n", PortSettings::LauncherName);
        return ExitFailure;
    }
    Model model;
    model.options = *options;
    LoadModel(model);
    const int result = Run(model);
    std::fprintf(stderr, "[launcher] %s\n", result == 0 ? "Play" : result == PortSettings::LauncherExitQuit ? "Quit" : result == ExitSnapshot ? "snapshot taken" : "failed");
    std::fflush(stderr);
    return result;
}
