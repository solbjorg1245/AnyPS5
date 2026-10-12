// The title's add-on content (s61 T-deluxe): which of its add-ons the player's own dump holds, so the
// entitlement query (libSceNpEntitlementAccess) answers from what the dump contains and the launcher can
// show it. Header-only so the launcher and port_settings_tests share it; nothing here writes anywhere.
// Demon's Souls asks sceNpEntitlementAccessGetAddcontEntitlementInfoList (EntitlementManager's worker, eboot
// 0x87eda0) for the two labels registered at 0x147dbc1: DEMONSSOULSGIFT1 ("PreOrder") and DEMONSSOULSGIFT2
// ("DigitalDeluxe"); a label in the answer is owned (status 2, 0x88a8b0). DigitalDeluxe owned: the character
// creator offers the Deluxe package (Player_DeluxePackageAvailable) and 0x449460 puts the Deluxe items (the
// Fluted armour set) into the inventory of a character that took it; not owned: it takes them away again.
// A dump holds an add-on as a folder near the game's app0 whose sce_sys/param.json or param.sfo names the
// add-on's content id (EP9000-PPSA01341_00-DEMONSSOULSGIFT2: this title's id and the label). A folder's name
// is no evidence (a bare folder named after the label is not the add-on); it only decides where to look.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AddonContent {

inline constexpr const char* PreOrderLabel = "DEMONSSOULSGIFT1";
inline constexpr const char* DeluxeEditionLabel = "DEMONSSOULSGIFT2";
inline constexpr std::array<const char*, 2> Labels{PreOrderLabel, DeluxeEditionLabel};
// The title's ids (PPSA01341 Europe, PPSA01342 America; both are in the eboot): a folder named with one is
// looked into for add-on folders ("PPSA01341-ac", "PPSA01341-addcont\DEMONSSOULSGIFT2").
inline constexpr std::array<const char*, 2> TitleIds{"PPSA01341", "PPSA01342"};
// Folder names other dump layouts keep add-ons in ("addcont\PPSA01341\DEMONSSOULSGIFT2").
inline constexpr std::array<const char*, 4> ContainerNames{"ADDCONT", "DLC", "ADDONS", "ADD-ONS"};
// At most this many folders are looked at per search (a game folder on a big drive stays quick).
inline constexpr std::size_t MaxFolders = 2048;

struct Found {
    std::string label;
    std::filesystem::path folder;
    std::string reason;  // what named the label: "its name", "sce_sys/param.json", ...
};

inline std::string Upper(std::string_view text) {
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    return result;
}

// A path from UTF-8 text (the settings file's GameDir and AddonDir).
inline std::filesystem::path PathFromText(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string NameText(const std::filesystem::path& path) {
    const auto name = path.filename().u8string();
    return std::string(name.begin(), name.end());
}

inline std::string ReadSmall(const std::filesystem::path& path, std::size_t limit = 1 << 20) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    std::string text(std::min<std::uintmax_t>(size, limit), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

// param.json's "contentId" value, without a JSON parser; empty when absent.
inline std::string ContentId(std::string_view json) {
    const auto name = json.find("\"contentId\"");
    if (name == std::string_view::npos) return {};
    const auto colon = json.find(':', name + 11);
    const auto open = colon == std::string_view::npos ? colon : json.find('"', colon);
    const auto close = open == std::string_view::npos ? open : json.find('"', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(json.substr(open + 1, close - open - 1));
}

// Whether a content id (EP9000-PPSA01341_00-DEMONSSOULSGIFT2) is this title's add-on `label`.
inline bool ContentIdIs(std::string_view contentId, std::string_view label) {
    const auto id = Upper(contentId);
    if (id.size() < label.size() + 1 || id.compare(id.size() - label.size(), label.size(), label) != 0 || id[id.size() - label.size() - 1] != '-') return false;
    for (const char* title : TitleIds) {
        if (id.find(title) != std::string::npos) return true;
    }
    return false;
}

inline bool HasEntries(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    return !error && entries != std::filesystem::directory_iterator();
}

// The labels a folder holds: its param.json / param.sfo names the content id (this title's id and the
// label). The folder's name counts for nothing; an empty folder or one without such metadata holds nothing.
inline void LookAt(const std::filesystem::path& folder, std::vector<Found>& found) {
    if (!HasEntries(folder)) return;
    std::string json;
    std::string sfo;
    std::string sfoUpper;
    bool read = false;
    for (const char* label : Labels) {
        bool known = false;
        for (const auto& entry : found) known = known || entry.label == label;
        if (known) continue;
        if (!read) {
            read = true;
            json = ReadSmall(folder / "sce_sys" / "param.json");
            if (json.empty()) json = ReadSmall(folder / "param.json");
            sfo = ReadSmall(folder / "sce_sys" / "param.sfo");
            if (sfo.empty()) sfo = ReadSmall(folder / "param.sfo");
            sfoUpper = Upper(sfo);
        }
        if (!json.empty() && ContentIdIs(ContentId(json), label)) {
            found.push_back({label, folder, "its sce_sys/param.json"});
            continue;
        }
        // param.sfo holds CONTENT_ID as a NUL-terminated string among its values.
        for (std::size_t at = sfoUpper.find(label); at != std::string::npos; at = sfoUpper.find(label, at + 1)) {
            const auto start = sfo.rfind('\0', at);
            const auto first = start == std::string::npos ? 0 : start + 1;
            if (ContentIdIs(std::string_view(sfo).substr(first, at + std::string_view(label).size() - first), label)) {
                found.push_back({label, folder, "its param.sfo"});
                break;
            }
        }
    }
}

inline bool NameHoldsAny(const std::string& upperName, const auto& names) {
    for (const char* name : names) {
        if (upperName.find(name) != std::string::npos) return true;
    }
    return false;
}

// A game's own app0 or patch dump (another region's, a patch): not looked into.
inline bool IsGameDump(const std::filesystem::path& folder) {
    std::error_code error;
    return std::filesystem::is_regular_file(folder / "eboot.bin", error);
}

// Each folder below `root` down to `depth` levels (not `root` itself), until `budget` runs out.
inline void LookBelow(const std::filesystem::path& root, int depth, const std::filesystem::path& skip, std::size_t& budget, std::vector<Found>& found) {
    std::error_code error;
    for (std::filesystem::directory_iterator entry(root, error), end; !error && entry != end && budget > 0; entry.increment(error)) {
        std::error_code kind;
        if (!entry->is_directory(kind) || kind || entry->path() == skip) continue;
        --budget;
        LookAt(entry->path(), found);
        if (depth > 1 && !IsGameDump(entry->path())) LookBelow(entry->path(), depth - 1, skip, budget, found);
    }
}

// The add-ons near the game: for each game folder (the dump's app0), the folders next to it, two levels into
// those named with the title id or a container name (addcont, dlc), and two levels into such folders inside
// it; `addonDir` (the player's choice, may be empty): the folder itself and two levels below it.
inline std::vector<Found> Find(const std::vector<std::filesystem::path>& gameFolders, const std::filesystem::path& addonDir) {
    std::vector<Found> found;
    std::size_t budget = MaxFolders;
    std::error_code error;
    if (!addonDir.empty() && std::filesystem::is_directory(addonDir, error)) {
        LookAt(addonDir, found);
        LookBelow(addonDir, 2, {}, budget, found);
    }
    for (const auto& game : gameFolders) {
        if (game.empty() || !std::filesystem::is_directory(game, error)) continue;
        const auto parent = game.parent_path();
        for (std::filesystem::directory_iterator entry(parent, error), end; parent != game && !error && entry != end && budget > 0; entry.increment(error)) {
            std::error_code kind;
            if (!entry->is_directory(kind) || kind || entry->path() == game) continue;
            --budget;
            LookAt(entry->path(), found);
            const auto name = Upper(NameText(entry->path()));
            if ((NameHoldsAny(name, TitleIds) || NameHoldsAny(name, ContainerNames)) && !IsGameDump(entry->path())) LookBelow(entry->path(), 2, game, budget, found);
        }
        for (const char* container : {"addcont", "dlc"}) {
            if (std::filesystem::is_directory(game / container, error)) LookBelow(game / container, 2, {}, budget, found);
        }
    }
    return found;
}

inline const Found* FindLabel(const std::vector<Found>& found, std::string_view label) {
    for (const auto& entry : found) {
        if (entry.label == label) return &entry;
    }
    return nullptr;
}

// The labels the entitlement query answers: the entitlements file's own labels (anyps5-entitlements.ini,
// AnyPS5's manual list) except the Deluxe Edition's, which only the dump can enable, then the add-ons found
// in the dump (the Deluxe Edition's only while `deluxe` is on). `notes` says what was left out.
inline std::vector<std::string> AnsweredLabels(const std::vector<std::string>& fileLabels, const std::vector<Found>& found, bool deluxe, std::vector<std::string>& notes) {
    std::vector<std::string> labels;
    const auto add = [&](const std::string& label) {
        for (const auto& known : labels) {
            if (known == label) return;
        }
        labels.push_back(label);
    };
    for (const auto& label : fileLabels) {
        if (label == DeluxeEditionLabel) notes.push_back(label + " in the entitlements file ignored: the Deluxe Edition is answered from the dump only");
        else add(label);
    }
    for (const auto& entry : found) {
        if (entry.label == DeluxeEditionLabel && !deluxe) notes.push_back(entry.label + " found in the dump but turned off (DeluxeEdition = off)");
        else add(entry.label);
    }
    return labels;
}

// Where a path really is (directory junctions and symbolic links followed): the release's app0 links the
// dump's folders, so app0/sce_sys leads back to the dump. The path itself when it cannot be resolved.
inline std::filesystem::path FinalPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return path;
    std::wstring text(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, text.data(), static_cast<DWORD>(text.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= text.size()) return path;
    text.resize(length);
    if (text.starts_with(L"\\\\?\\UNC\\")) text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) text = text.substr(4);
    return std::filesystem::path(text);
#else
    std::error_code error;
    auto resolved = std::filesystem::canonical(path, error);
    return error ? path : resolved;
#endif
}

// The dump folders a game folder stands for: itself and, for the release's app0 whose folders link into
// the dump, the folder its sce_sys really is in.
inline std::vector<std::filesystem::path> DumpFolders(const std::filesystem::path& app0) {
    std::vector<std::filesystem::path> folders{app0};
    const auto dump = FinalPath(app0 / "sce_sys").parent_path();
    std::error_code error;
    if (!dump.empty() && !std::filesystem::equivalent(dump, app0, error) && !error) folders.push_back(dump);
    return folders;
}

}
