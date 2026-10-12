// The settings launcher's game folder checks and setup stamp (s61), header-only so port_settings_tests
// covers them. The game folder is the player's own decrypted dump of Demon's Souls (PPSA01341), the app0
// folder with eboot.bin, sce_sys and sce_module. The release's one-time setup (DemonsSouls.ps1 -Dump /
// DemonsSouls.sh --dump) relinks its eboot.bin into DemonsSouls.exe / .elf, patches it with
// tools/ds_patch.py and links the dump's folders into app0 next to it; the stamp file records what it
// was run with, so the launcher runs it again only when the folder, its eboot.bin, the tools or Python
// changed.
#pragma once

#include "prx/libc/include/PortSettings.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
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

namespace Launcher {

inline constexpr const char* TitleId = "PPSA01341";
inline constexpr const char* SupportedContentVersion = "01.007.000";
// tools/ds_patch.py's version check: the bytes its job-worker-spin patch replaces at this eboot vaddr.
inline constexpr std::uint64_t PatchSiteVaddr = 0x83B6B3;
inline constexpr std::array<std::uint8_t, 7> PatchSiteBytes{0x48, 0x01, 0xd2, 0x48, 0x89, 0x55, 0xb0};
inline constexpr const char* StampName = "anyps5-setup.txt";

#ifdef _WIN32
inline constexpr const char* GameExecutable = "DemonsSouls.exe";
inline constexpr const char* SetupScript = "DemonsSouls.ps1";
inline constexpr const char* RelinkerName = "relinker.exe";
#else
inline constexpr const char* GameExecutable = "DemonsSouls.elf";
inline constexpr const char* SetupScript = "DemonsSouls.sh";
inline constexpr const char* RelinkerName = "relinker";
#endif

// A path from UTF-8 text (std::filesystem::path(std::string) would use the ANSI code page on Windows).
inline std::filesystem::path PathFromText(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// A typed or pasted folder: trimmed, without control characters (a pasted line break), surrounding quotes
// (Explorer's "Copy as path") and trailing separators (a drive root keeps its own).
inline std::string CleanFolderText(std::string_view text) {
    std::string printable;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) printable += c;
    }
    text = PortSettings::Trim(printable);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) text = PortSettings::Trim(text.substr(1, text.size() - 2));
    std::string result(text);
    while (result.size() > 1 && (result.back() == '/' || result.back() == '\\')) {
        if (result.size() == 3 && result[1] == ':') break;  // C:\ stays
        result.pop_back();
    }
    return result;
}

enum class ExecutableKind { Missing, Elf, Encrypted, Unknown };

// What an executable's first bytes say (eboot.bin, sce_module/*.prx): a decrypted ELF, an encrypted SELF
// (PS4 4F 15 3D 1D, PS5 54 14 F5 EE).
inline ExecutableKind ReadExecutableKind(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return ExecutableKind::Missing;
    unsigned char magic[4] = {};
    if (!file.read(reinterpret_cast<char*>(magic), sizeof(magic))) return ExecutableKind::Unknown;
    if (std::memcmp(magic, "\x7f" "ELF", 4) == 0) return ExecutableKind::Elf;
    static constexpr unsigned char ps4Self[4] = {0x4f, 0x15, 0x3d, 0x1d};
    static constexpr unsigned char ps5Self[4] = {0x54, 0x14, 0xf5, 0xee};
    if (std::memcmp(magic, ps4Self, 4) == 0 || std::memcmp(magic, ps5Self, 4) == 0) return ExecutableKind::Encrypted;
    return ExecutableKind::Unknown;
}

// Whether `path` is `folder` or inside it, after following links (junctions, symlinks) as far as they exist.
inline bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const auto resolve = [](const std::filesystem::path& value) {
        std::error_code error;
        auto resolved = std::filesystem::weakly_canonical(value, error);
        if (error) resolved = value.lexically_normal();
#ifdef _WIN32
        return PathFromText(PortSettings::Lower(PortSettings::PathText(resolved)));  // NTFS paths ignore case
#else
        return resolved;
#endif
    };
    const auto relative = resolve(path).lexically_relative(resolve(folder));
    return !relative.empty() && *relative.begin() != "..";
}

// `size` bytes at `vaddr` of a 64-bit little-endian ELF's text segment (the first loadable segment with
// flags, as ds_patch.py reads it); empty when the file is not such an ELF or the range is outside it.
inline std::vector<std::uint8_t> ReadTextBytes(const std::filesystem::path& elf, std::uint64_t vaddr, std::size_t size) {
    std::ifstream file(elf, std::ios::binary);
    unsigned char header[64] = {};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "\x7f" "ELF", 4) != 0 || header[4] != 2 || header[5] != 1) return {};
    const auto read = [](const unsigned char* bytes, int count) {
        std::uint64_t value = 0;
        for (int index = count - 1; index >= 0; --index) value = (value << 8) | bytes[index];
        return value;
    };
    const std::uint64_t phoff = read(header + 0x20, 8);
    const std::uint64_t phentsize = read(header + 0x36, 2);
    const std::uint64_t phnum = read(header + 0x38, 2);
    if (phentsize < 56) return {};
    for (std::uint64_t index = 0; index < phnum; ++index) {
        unsigned char entry[56] = {};
        file.seekg(static_cast<std::streamoff>(phoff + index * phentsize));
        if (!file.read(reinterpret_cast<char*>(entry), sizeof(entry))) return {};
        const std::uint64_t type = read(entry, 4);
        const std::uint64_t flags = read(entry + 4, 4);
        if (type != 1 || flags == 0) continue;  // PT_LOAD with flags: the text segment comes first
        const std::uint64_t offset = read(entry + 0x08, 8);
        const std::uint64_t start = read(entry + 0x10, 8);
        const std::uint64_t fileSize = read(entry + 0x20, 8);
        if ((flags & 1) == 0 || vaddr < start || vaddr + size > start + fileSize) return {};
        std::vector<std::uint8_t> bytes(size);
        file.seekg(static_cast<std::streamoff>(offset + (vaddr - start)));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return {};
        return bytes;
    }
    return {};
}

inline std::string ReadSmallFile(const std::filesystem::path& path, std::size_t limit = 1 << 20) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string text(limit, '\0');
    file.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

// A string member of param.json ("titleId", "contentVersion"), without a JSON parser; empty when absent.
inline std::string JsonString(std::string_view json, std::string_view key) {
    const auto name = json.find("\"" + std::string(key) + "\"");
    if (name == std::string_view::npos) return {};
    const auto colon = json.find(':', name + key.size() + 2);
    const auto open = colon == std::string_view::npos ? colon : json.find('"', colon);
    const auto close = open == std::string_view::npos ? open : json.find('"', open + 1);
    if (close == std::string_view::npos || PortSettings::Trim(json.substr(colon + 1, open - colon - 1)) != "") return {};
    return std::string(json.substr(open + 1, close - open - 1));
}

enum class FolderState { Empty, Invalid, Valid };

struct FolderCheck {
    FolderState state = FolderState::Empty;
    std::filesystem::path folder;  // the app0 folder to use (the picked folder's app0 when it has one)
    std::string summary;           // what was found, or what to do
    std::vector<std::string> errors;    // each one keeps Play disabled
    std::vector<std::string> warnings;  // shown, Play stays possible
};

#ifdef _WIN32
// The volume of a path: network drive, file system name (NTFS, ReFS, exFAT, FAT32...); false when unknown.
inline bool QueryVolume(const std::filesystem::path& path, bool& remote, std::string& fileSystem) {
    wchar_t root[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(path.c_str(), root, MAX_PATH)) return false;
    remote = GetDriveTypeW(root) == DRIVE_REMOTE;
    wchar_t name[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH)) {
        fileSystem.clear();
        return true;
    }
    fileSystem = PortSettings::PathText(std::filesystem::path(name));
    return true;
}
#endif

// Checks a game folder typed, pasted or picked by the player. `installDir`: the AnyPS5 release folder
// the setup links the dump into (empty: not checked).
inline FolderCheck CheckGameFolder(std::string_view text, const std::filesystem::path& installDir) {
    FolderCheck check;
    const auto cleaned = CleanFolderText(text);
    if (cleaned.empty()) {
        check.summary = "Choose the folder of your own decrypted Demon's Souls dump: the app0 folder with eboot.bin, sce_sys and sce_module.";
        return check;
    }
    check.state = FolderState::Invalid;
    const auto fail = [&](std::string message) {
        check.errors.push_back(std::move(message));
        return check;
    };
    // The settings file stores the path in double quotes.
    if (cleaned.find('"') != std::string::npos) return fail("A folder path with a double quote (\") in it cannot be used. Rename the folder.");
    std::string full = cleaned;
#ifdef _WIN32
    // A network share fails below anyway; said at once, before a slow (or hanging) network lookup.
    if ((full.starts_with("\\\\") || full.starts_with("//")) && !full.starts_with("\\\\?\\") && !full.starts_with("\\\\.\\"))
        return fail("This is a network folder. The setup links the dump with NTFS junctions, which must point to a local drive: copy the dump to a local NTFS drive.");
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && (full == "~" || full.starts_with("~/"))) full = home + full.substr(1);
#endif
    std::error_code error;
    auto folder = PathFromText(full);
    // A relative path would name one folder here and another one for the setup (another working directory).
    if (!folder.is_absolute()) {
#ifdef _WIN32
        return fail("Enter the full path of the folder, with its drive (for example D:\\Games\\PPSA01341-app0), or use Browse.");
#else
        return fail("Enter the full path of the folder (starting with / or ~/), or use Browse.");
#endif
    }
    folder = folder.lexically_normal();
    bool usingApp0 = false;
    if (std::filesystem::is_regular_file(folder, error) && PortSettings::Lower(PortSettings::PathText(folder.filename())) == "eboot.bin") folder = folder.parent_path();
    if (!std::filesystem::is_directory(folder, error)) return fail("This folder does not exist (or cannot be opened).");
    if (!std::filesystem::exists(folder / "eboot.bin", error) && std::filesystem::is_regular_file(folder / "app0" / "eboot.bin", error)) {
        folder /= "app0";
        usingApp0 = true;
    }
    check.folder = folder;
    std::string missing;
    for (const char* name : {"eboot.bin", "sce_sys", "sce_module"}) {
        if (!std::filesystem::exists(folder / name, error)) missing += std::string(missing.empty() ? "" : ", ") + name;
    }
    if (!missing.empty()) return fail("This is not a PS5 game dump: " + missing + (missing.find(',') != std::string::npos ? " are" : " is") + " missing. Choose the app0 folder (the one with eboot.bin, sce_sys and sce_module).");
    if (!installDir.empty()) {
        for (const auto& own : {installDir, installDir / "app0"}) {
            if (std::filesystem::equivalent(folder, own, error)) return fail("This is AnyPS5's own folder. Choose the folder of your dump instead.");
        }
        // The setup writes the game executable, its app0 folder and the stamp into AnyPS5's folder.
        if (IsInside(installDir, folder)) return fail("AnyPS5 is unpacked inside this dump. Move the AnyPS5 folder out of the dump (the setup writes into AnyPS5's folder), then choose the dump again.");
    }
    switch (ReadExecutableKind(folder / "eboot.bin")) {
    case ExecutableKind::Elf:
        break;
    case ExecutableKind::Encrypted:
        return fail("eboot.bin is still encrypted (a SELF file, not an ELF). AnyPS5 needs a decrypted dump: decrypt the game's executables (eboot.bin and sce_module) with your dumping tool, then choose that folder.");
    default:
        return fail("eboot.bin is not a decrypted PS5 executable (it does not start like an ELF file). AnyPS5 needs a decrypted dump of the game.");
    }
    // The relinker converts only the decrypted (ELF) modules of sce_module and skips the others without an
    // error, so an encrypted one would end the game when it loads it.
    int modules = 0;
    int encryptedModules = 0;
    for (std::filesystem::directory_iterator entry(folder / "sce_module", error), end; !error && entry != end; entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_regular_file(typeError)) continue;
        const auto kind = ReadExecutableKind(entry->path());
        modules += kind == ExecutableKind::Elf;
        encryptedModules += kind == ExecutableKind::Encrypted;
    }
    if (encryptedModules > 0) {
        return fail("sce_module holds " + std::to_string(encryptedModules) + " encrypted module" + (encryptedModules == 1 ? "" : "s") +
            " (SELF files). AnyPS5 needs a decrypted dump: decrypt sce_module too with your dumping tool, then choose that folder.");
    }
    if (modules == 0) return fail("sce_module holds no decrypted game module (.prx ELF files). AnyPS5 needs a complete decrypted dump of the game.");

    const auto param = ReadSmallFile(folder / "sce_sys" / "param.json");
    const auto title = JsonString(param, "titleId");
    const auto content = JsonString(param, "contentVersion");
    const auto build = std::string(PortSettings::BuildVersionOf(ReadSmallFile(folder / "gameversion.txt")));
    const auto site = ReadTextBytes(folder / "eboot.bin", PatchSiteVaddr, PatchSiteBytes.size());
    const bool knownCode = site.size() == PatchSiteBytes.size() && std::equal(site.begin(), site.end(), PatchSiteBytes.begin());
    if (title.empty()) check.warnings.push_back("sce_sys/param.json has no title ID, so the game cannot be identified.");
    else if (title != TitleId) return fail("This is " + title + ", not Demon's Souls " + TitleId + ". This AnyPS5 release runs only that game.");
    const std::string found = (content.empty() ? std::string() : "version " + content) + (build.empty() ? std::string() : std::string(content.empty() ? "" : ", ") + "build " + build);
    // Refused before any setup: the setup would relink it, then ds_patch would refuse it.
    if (!knownCode) {
        return fail("Unsupported game version" + (found.empty() ? std::string() : " (" + found + ")") + ": this release supports version " + SupportedContentVersion + ", build " +
            PortSettings::SupportedBuildVersion + ". Choose a dump of that version.");
    } else if (!build.empty() && build != PortSettings::SupportedBuildVersion) {
        check.warnings.push_back("gameversion.txt names build " + build + "; this release supports " + PortSettings::SupportedBuildVersion + " (the executable matches it, so it should work).");
    }
    check.summary = std::string("Demon's Souls") + (title.empty() ? "" : " " + title) + (found.empty() ? "" : ", " + found) + (knownCode ? " (supported)" : "") +
        (usingApp0 ? ", in the app0 folder inside the folder you chose" : "");

#ifdef _WIN32
    // The setup links the dump's folders into AnyPS5's app0 with NTFS directory junctions: they live on
    // AnyPS5's drive (NTFS needed) and must point to a local drive.
    bool remote = false;
    std::string fileSystem;
    if (QueryVolume(folder, remote, fileSystem)) {
        if (remote) fail("This folder is on a network drive. The setup links the dump with NTFS junctions, which must point to a local drive: copy the dump to a local NTFS drive.");
        else if (!fileSystem.empty() && fileSystem != "NTFS" && fileSystem != "ReFS")
            check.warnings.push_back("The dump is on a " + fileSystem + " drive, not NTFS. If the setup cannot link it, copy the dump to an NTFS drive.");
    }
    if (!installDir.empty() && QueryVolume(installDir, remote, fileSystem)) {
        if (remote) fail("AnyPS5's own folder is on a network drive; the setup needs it on a local NTFS drive. Move the AnyPS5 folder.");
        else if (!fileSystem.empty() && fileSystem != "NTFS" && fileSystem != "ReFS")
            fail("AnyPS5's own folder is on a " + fileSystem + " drive. The setup creates NTFS junctions in its app0 folder: move the AnyPS5 folder to an NTFS drive.");
    }
#endif
    check.state = check.errors.empty() ? FolderState::Valid : FolderState::Invalid;
    return check;
}

// The AnyPS5 release folder: the launcher's own folder, or its parent when the launcher sits in libs/;
// recognised by the setup script and tools/relinker. Empty in a build tree.
inline std::filesystem::path FindInstallDir(const std::filesystem::path& launcherDir) {
    for (const auto& candidate : {launcherDir, launcherDir.parent_path()}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / SetupScript, error) && std::filesystem::is_regular_file(candidate / "tools" / RelinkerName, error)) return candidate;
    }
    return {};
}

// Size and modification time of the files the setup depends on: an AnyPS5 update changes them.
inline std::string ToolsFingerprint(const std::filesystem::path& installDir) {
    std::string result;
    for (const auto& file : {installDir / "tools" / RelinkerName, installDir / "tools" / "ds_patch.py", installDir / SetupScript}) {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, error);
        result += (result.empty() ? "" : ";") + PortSettings::PathText(file.filename()) + ":" + (error ? std::string("missing") : std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count()));
    }
    return result;
}

// Size and modification time of the dump's eboot.bin: a dump updated in place at the same path changes it.
inline std::string DumpFingerprint(const std::filesystem::path& gameDir) {
    std::error_code error;
    const auto eboot = gameDir / "eboot.bin";
    const auto size = std::filesystem::file_size(eboot, error);
    const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(eboot, error);
    return error ? std::string("missing") : std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count());
}

// The stamp's text: written after a setup that succeeded; a different text means the setup runs again.
// `patched`: ds_patch ran (false: Python 3.8+ was missing, the game runs unpatched).
inline std::string SetupStamp(const std::filesystem::path& gameDir, const std::string& tools, const std::string& dump, bool patched) {
    return "# Written by the AnyPS5 launcher after the one-time setup. Delete this file to run the setup again.\n"
           "GameDir=" + PortSettings::PathText(gameDir) + "\nEboot=" + dump + "\nTools=" + tools + "\nPatched=" + (patched ? "yes" : "no (Python 3.8+ was missing)") + "\n";
}

// Why the setup must run before the game can start; empty when the last setup still holds. `python`: a
// usable Python 3.8+ is there now (an unpatched setup then runs again, patched this time).
inline std::string SetupReason(const std::filesystem::path& installDir, const std::filesystem::path& gameDir, bool python) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(installDir / GameExecutable, error)) return "first start: the game executable is not made yet";
    if (!std::filesystem::exists(installDir / "app0" / "sce_sys", error)) return "the game's app0 folder is not linked yet";
    const auto stamp = ReadSmallFile(installDir / StampName);
    if (stamp.empty()) return "no record of an earlier setup by this launcher";
    const auto tools = ToolsFingerprint(installDir);
    const auto dump = DumpFingerprint(gameDir);
    if (stamp == SetupStamp(gameDir, tools, dump, true)) return {};
    if (stamp.find("\nGameDir=" + PortSettings::PathText(gameDir) + "\n") == std::string::npos) return "the game folder changed";
    if (stamp.find("\nEboot=" + dump + "\n") == std::string::npos) return "the game folder's eboot.bin changed";
    if (stamp.find("\nTools=" + tools + "\n") == std::string::npos) return "AnyPS5 was updated";
    if (stamp == SetupStamp(gameDir, tools, dump, false)) return python ? "Python 3.8 or newer is installed now: the speed patch can be applied" : std::string();
    return "the setup record is not readable";
}

// Python as DemonsSouls.ps1 / .sh look for it (ds_patch.py needs 3.8 or newer): on Windows the first py
// (with -3), python3 and python on PATH that is not the Microsoft Store alias in WindowsApps, on Linux
// python3. Commands to try in turn; the caller runs PythonVersionProbe with each.
inline std::vector<std::vector<std::string>> PythonCandidates(const std::function<const char*(const char*)>& lookup) {
    std::vector<std::vector<std::string>> candidates;
    const char* path = lookup("PATH");
    if (path == nullptr) return candidates;
#ifdef _WIN32
    constexpr char separator = ';';
    static constexpr std::pair<const char*, const char*> names[] = {{"py.exe", "-3"}, {"python3.exe", nullptr}, {"python.exe", nullptr}};
#else
    constexpr char separator = ':';
    static constexpr std::pair<const char*, const char*> names[] = {{"python3", nullptr}};
#endif
    for (const auto& [name, argument] : names) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const auto end = rest.find(separator);
            const auto entry = PortSettings::Unquote(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            if (entry.empty() || PortSettings::Lower(entry).find("windowsapps") != std::string::npos) continue;
            std::error_code error;
            if (!std::filesystem::is_regular_file(PathFromText(entry) / name, error)) continue;
            candidates.push_back({PortSettings::PathText(PathFromText(entry) / name)});
            if (argument != nullptr) candidates.back().push_back(argument);
            break;
        }
    }
    return candidates;
}

// What DemonsSouls.ps1 / .sh run with a Python candidate: exit code 0 for 3.8 or newer.
inline constexpr const char* PythonVersionProbe = "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)";

// Whether a finished setup's output says the executable was left unpatched (no Python 3.8+).
inline bool SetupLeftUnpatched(const std::vector<std::string>& lines) {
    return std::any_of(lines.begin(), lines.end(), [](const std::string& line) { return line.find("NOT patched") != std::string::npos; });
}

}
