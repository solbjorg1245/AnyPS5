#include "prx/libc/include/PortSettings.hpp"
#include "prx/libc/include/General.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return std::filesystem::current_path();
    path.resize(size);
    return std::filesystem::path(path).parent_path();
#else
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::current_path() : self.parent_path();
#endif
}

std::string ReadFile(const std::filesystem::path& path, bool& found) {
    std::ifstream file(path, std::ios::binary);
    found = static_cast<bool>(file);
    if (!found) return {};
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) throw std::runtime_error("cannot read " + path.string());
    return text.str();
}

PortSettings::Settings Load() {
    PortSettings::Settings settings;
    const char* configured = std::getenv("APS5_SETTINGS");
    const bool explicitPath = configured != nullptr && configured[0] != '\0';
    if (!explicitPath || PortSettings::Lower(configured) != "none") {
        const auto path = explicitPath ? std::filesystem::path(configured) : ExecutableDirectory() / PortSettings::FileName;
        try {
            bool found = false;
            const auto text = ReadFile(path, found);
            if (found) {
                settings = PortSettings::Parse(text, path.filename().string());
                settings.source = path.string();
            } else if (explicitPath) {
                settings.warnings.push_back("APS5_SETTINGS: cannot open '" + path.string() + "'; using the defaults");
            }
        } catch (const std::exception& error) {
            settings = PortSettings::Settings{};
            settings.warnings.push_back(std::string(error.what()) + "; using the defaults");
        }
    }
    PortSettings::ApplyEnvironment(settings, [](const char* name) { return static_cast<const char*>(std::getenv(name)); });
    PortSettings::Normalize(settings);
    for (const auto& warning : settings.warnings) std::fprintf(stderr, "[settings] %s\n", warning.c_str());
    if (settings.Any()) std::fprintf(stderr, "[settings] %s\n", PortSettings::Describe(settings).c_str());
    std::fflush(stderr);
    return settings;
}

}

extern "C" const PortSettings::Settings& GetPortSettings_nid_no_patch() {
    static const PortSettings::Settings settings = Load();
    return settings;
}

void InstallPortSettingsOverlay(const std::function<std::filesystem::path(const char*)>& resolve) {
    const auto args = PortSettings::GameArgs(GetPortSettings_nid_no_patch());
    if (args.empty()) return;
    std::string joined;
    for (const auto& arg : args) joined += (joined.empty() ? "" : " ") + arg;
    try {
        const auto directory = ExecutableDirectory() / PortSettings::OverrideDirectoryName;
        std::filesystem::create_directories(directory);
        std::size_t files = 0;
        const auto overlay = [&](const std::string& guest, const std::filesystem::path& host, bool lines) {
            bool found = false;
            const auto original = ReadFile(host, found);
            if (!found) return;
            const auto merged = lines ? PortSettings::AppendLines(original, args) : PortSettings::AppendWords(original, args);
            const auto target = directory / host.filename();
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            file.write(merged.data(), static_cast<std::streamsize>(merged.size()));
            file.close();
            if (!file) throw std::runtime_error("cannot write " + target.string());
            AddPathAlias_nid_no_patch(guest.c_str(), target.string().c_str());
            ++files;
        };
        // The render configs load after the command line and set r_Wants4K and TargetFramesPerSecond
        // themselves, so both carry the arguments (the command line alone covers the boot menus).
        overlay("app0/packagecmdlineargs.txt", resolve("/app0/packagecmdlineargs.txt"), false);
        std::error_code error;
        for (std::filesystem::directory_iterator entry(resolve("/app0/misc"), error), end; !error && entry != end; entry.increment(error)) {
            const auto name = entry->path().filename().string();
            const auto lower = PortSettings::Lower(name);
            if (!lower.starts_with("renderconfig_ps5_") || !lower.ends_with(".txt")) continue;
            overlay("app0/misc/" + name, entry->path(), true);
        }
        std::fprintf(stderr, "[settings] game args %s in %zu files (%s)\n", joined.c_str(), files, directory.string().c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] game args %s not applied: %s\n", joined.c_str(), error.what());
    }
    std::fflush(stderr);
}
