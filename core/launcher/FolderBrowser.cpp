#include "FolderBrowser.hpp"

#include "GameFolder.hpp"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Launcher {

namespace {

constexpr const char* PopupName = "Choose the game folder";
constexpr std::size_t MaxEntries = 5000;
constexpr std::size_t MaxMarked = 400;  // folders checked for eboot.bin (a huge folder stays quick)

bool IsGameFolder(const std::filesystem::path& folder) {
    std::error_code error;
    return std::filesystem::is_regular_file(folder / "eboot.bin", error) || std::filesystem::is_regular_file(folder / "app0" / "eboot.bin", error);
}

std::filesystem::path Home() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home != nullptr ? std::filesystem::path(home) : std::filesystem::path();
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

}

void FolderBrowser::Open(const std::filesystem::path& start) {
    places_.clear();
#ifdef _WIN32
    places_.emplace_back("Drives", std::filesystem::path());
    const DWORD drives = GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if ((drives & (1u << letter)) == 0) continue;
        const std::string root = std::string(1, static_cast<char>('A' + letter)) + ":\\";
        places_.emplace_back(root, std::filesystem::path(root));
    }
    if (const auto home = Home(); !home.empty()) places_.emplace_back("Home", home);
#else
    // SteamOS mounts SD cards and USB drives under /run/media (/run/media/deck/<label>, /run/media/mmcblk0p1).
    for (const auto& [name, path] : std::initializer_list<std::pair<const char*, std::filesystem::path>>{
             {"Home", Home()}, {"SD card / USB (/run/media)", "/run/media"}, {"/media", "/media"}, {"/mnt", "/mnt"}, {"/", "/"}}) {
        std::error_code error;
        if (!path.empty() && std::filesystem::is_directory(path, error)) places_.emplace_back(name, path);
    }
#endif
    // The nearest existing folder of the start (the typed text may be half a path), else home.
    std::filesystem::path folder = start;
    std::error_code error;
    while (!folder.empty() && !std::filesystem::is_directory(folder, error)) {
        const auto parent = folder.parent_path();
        if (parent == folder) {
            folder.clear();
            break;
        }
        folder = parent;
    }
    if (folder.empty()) folder = Home();
    Enter(folder);
    open_ = true;
    openPopup_ = true;
}

void FolderBrowser::Enter(const std::filesystem::path& folder) {
    current_ = folder;
    entries_.clear();
    error_.clear();
    refocus_ = true;
    currentIsGame_ = false;
    if (folder.empty()) {
        for (const auto& [name, path] : places_) {
            if (!path.empty() && path.has_root_name() && path == path.root_path()) entries_.push_back({name, path, false});
        }
        return;
    }
    std::error_code error;
    currentIsGame_ = std::filesystem::is_regular_file(folder / "eboot.bin", error);
    std::filesystem::directory_iterator iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        error_ = "Cannot open this folder: " + error.message();
        return;
    }
    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        if (error) break;
        std::error_code typeError;
        if (!iterator->is_directory(typeError)) continue;
        const auto name = PortSettings::PathText(iterator->path().filename());
        if (name.empty() || name.front() == '.' || name.front() == '$' || name == "System Volume Information") continue;
        entries_.push_back({name, iterator->path(), false});
        if (entries_.size() >= MaxEntries) break;
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& left, const Entry& right) { return PortSettings::Lower(left.name) < PortSettings::Lower(right.name); });
    if (entries_.size() <= MaxMarked) {
        for (auto& entry : entries_) entry.game = IsGameFolder(entry.path);
    }
}

std::filesystem::path FolderBrowser::Parent() const {
    const auto parent = current_.parent_path();
#ifdef _WIN32
    if (parent == current_) return {};  // above a drive root: the drive list
#endif
    return parent;
}

bool FolderBrowser::Draw(std::filesystem::path& chosen) {
    if (!open_) return false;
    if (openPopup_) {
        ImGui::OpenPopup(PopupName);
        openPopup_ = false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.94f, viewport->WorkSize.y * 0.92f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(PopupName, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        open_ = false;
        return false;
    }
    bool picked = false;
    bool close = false;
    std::filesystem::path next;
    bool go = false;

    Hint("A / Enter opens a folder, B / Backspace goes up, Y takes the folder shown, Back / Escape cancels. Choose the app0 folder of your dump (marked [game]).");
    ImGui::TextWrapped("%s", current_.empty() ? "Drives" : PortSettings::PathText(current_).c_str());
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (std::size_t index = 0; index < places_.size(); ++index) {
        if (index > 0) {
            const float width = ImGui::CalcTextSize(places_[index].first.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right) ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::SmallButton(places_[index].first.c_str())) {
            next = places_[index].second;
            go = true;
        }
        ImGui::PopID();
    }
    if (currentIsGame_) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "This folder has eboot.bin: take it with \"Use this folder\" (Y).");
    if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("entries", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (refocus_) ImGui::SetScrollY(0.0f);
    if (!current_.empty() && ImGui::Selectable(".. (up one folder)")) {
        next = Parent();
        go = true;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (refocus_ && index == 0) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        const auto& entry = entries_[index];
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(entry.game ? (entry.name + "   [game]").c_str() : entry.name.c_str())) {
            next = entry.path;
            go = true;
        }
        ImGui::PopID();
    }
    if (entries_.empty() && error_.empty()) Hint("(no folders here)");
    refocus_ = false;
    ImGui::EndChild();

    ImGui::BeginDisabled(current_.empty());
    if (ImGui::Button("Use this folder")) picked = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) close = true;

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (!current_.empty()) {
                next = Parent();
                go = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && !current_.empty()) picked = true;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    if (picked) {
        chosen = current_;
        close = true;
    } else if (go) {
        Enter(next);
    }
    if (close) {
        ImGui::CloseCurrentPopup();
        open_ = false;
    }
    ImGui::EndPopup();
    return picked;
}

}
