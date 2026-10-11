#include "prx/libc/include/UiOverlay.hpp"
#include "prx/libc/include/General.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

std::optional<std::string> ReadWhole(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream content;
    content << file.rdbuf();
    if (file.bad()) throw std::runtime_error("cannot read " + PortSettings::PathText(path));
    return content.str();
}

// Writes `content` as anyps5-overrides/ui/<folder>/<name> unless a file there already holds it (the
// overlays are the same on every start: a second instance or a read-only install finds them in place).
// Empty on failure.
std::filesystem::path WriteOverlay(const char* folder, const char* name, const std::string& content, std::string& error) {
    std::vector<std::filesystem::path> directories;
    for (const auto& directory : PortOverrideDirectories()) {
        directories.push_back(directory / UiOverlay::DirectoryName / folder);
        const auto target = directories.back() / name;
        std::error_code ignored;
        if (std::filesystem::file_size(target, ignored) == content.size() && ReadWhole(target) == content) return target;
    }
    return PortSettings::WriteOverrideFile(directories, name, content, error);
}

void InstallButtonIcons(const std::function<std::filesystem::path(const char*)>& resolve) {
    if (GetPortSettings_nid_no_patch().buttonIcons != PortSettings::ButtonIcons::SteamDeck) return;
    std::size_t replaced = 0;
    std::string skipped;
    std::string probed;
    const char* probe = std::getenv(UiOverlay::ButtonIconsProbeSwitch);
    std::filesystem::path written;
    for (const auto& entry : UiOverlay::SteamDeckGlyphs) {
        const std::string guest = std::string(UiOverlay::GlyphGuestDirectory) + entry.name;
        const auto original = ReadWhole(resolve(guest.c_str()));
        if (!original || !UiOverlay::IsGlyphTexture(*original)) {
            skipped += std::string(skipped.empty() ? "" : ", ") + entry.name + (original ? " (unexpected format)" : " (missing)");
            continue;
        }
        std::string error;
        std::string stem(entry.name);
        stem = stem.substr(0, stem.find('.'));
        for (const char* prefix : {"ps4_", "button_"}) {
            if (stem.rfind(prefix, 0) == 0) stem.erase(0, std::string_view(prefix).size());
        }
        const bool unchanged = probe != nullptr && ("," + std::string(probe) + ",").find("," + stem + ",") != std::string::npos;
        if (unchanged) probed += std::string(probed.empty() ? "" : ", ") + stem;
        written = WriteOverlay("buttonicons-steamdeck", entry.name, unchanged ? *original : UiOverlay::GlyphTexture(*original, entry.glyph), error);
        if (written.empty()) throw std::runtime_error(error.empty() ? "no folder to write the icons to" : error);
        AddPathAliasHost_nid_no_patch(guest.c_str(), written);
        ++replaced;
    }
    std::fprintf(stderr, "[settings] button icons: Steam Deck, %zu of %zu textures replaced (%s)%s%s%s%s\n", replaced, UiOverlay::SteamDeckGlyphs.size(),
        PortSettings::PathText(written.parent_path()).c_str(), skipped.empty() ? "" : "; kept the title's ", skipped.c_str(),
        probed.empty() ? "" : "; probe, written unchanged: ", probed.c_str());
}

void InstallVisualModeRowRemoval(const std::function<std::filesystem::path(const char*)>& resolve) {
    const char* value = std::getenv(UiOverlay::HideVisualModeRowSwitch);
    if (value == nullptr || value[0] == '\0' || std::string_view(value) == "0") return;
    const auto original = ReadWhole(resolve(UiOverlay::StartMenuGuest));
    if (!original) {
        std::fprintf(stderr, "[settings] Visual Mode row kept: %s not found\n", UiOverlay::StartMenuGuest);
        return;
    }
    std::string error;
    const auto edited = UiOverlay::RemovePrefabEntity(*original, "MenuContents", "VisualMode", error);
    if (!edited) {
        std::fprintf(stderr, "[settings] Visual Mode row kept: uistartmenu.cgpr not as expected (%s)\n", error.c_str());
        return;
    }
    const auto written = WriteOverlay("scripts", "uistartmenu.cgpr", *edited, error);
    if (written.empty()) throw std::runtime_error(error.empty() ? "no folder to write uistartmenu.cgpr to" : error);
    AddPathAliasHost_nid_no_patch(UiOverlay::StartMenuGuest, written);
    std::fprintf(stderr, "[settings] Visual Mode row removed from Options > Display (%s, %zu -> %zu bytes; %s=0 keeps it)\n",
        PortSettings::PathText(written).c_str(), original->size(), edited->size(), UiOverlay::HideVisualModeRowSwitch);
}

}

void InstallUiOverlay(const std::function<std::filesystem::path(const char*)>& resolve) {
    try {
        InstallButtonIcons(resolve);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] button icons: not replaced (%s): the title's PlayStation icons stay\n", error.what());
    }
    try {
        InstallVisualModeRowRemoval(resolve);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] Visual Mode row kept (%s)\n", error.what());
    }
    std::fflush(stderr);
}
