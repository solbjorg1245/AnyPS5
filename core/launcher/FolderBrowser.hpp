// The settings launcher's built-in folder browser (s61): an ImGui modal that works with a gamepad, for
// the Steam Deck's Game Mode (gamescope), where a desktop dialog may not show or cannot be navigated, and
// as the fallback when no system folder picker works.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Launcher {

class FolderBrowser {
public:
    void Open(const std::filesystem::path& start);
    bool IsOpen() const { return open_; }
    // Draws it while open (call once per frame, outside other windows); true once, with `chosen`, when the
    // player takes a folder.
    bool Draw(std::filesystem::path& chosen);

private:
    struct Entry {
        std::string name;
        std::filesystem::path path;
        bool game = false;
    };
    void Enter(const std::filesystem::path& folder);
    std::filesystem::path Parent() const;

    bool open_ = false;
    bool openPopup_ = false;
    bool refocus_ = false;
    std::filesystem::path current_;  // empty: the drive list (Windows)
    bool currentIsGame_ = false;
    std::vector<Entry> entries_;
    std::vector<std::pair<std::string, std::filesystem::path>> places_;
    std::string error_;
};

}
