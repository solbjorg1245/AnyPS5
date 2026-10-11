#include "prx/libc/include/PortSettings.hpp"
#include "prx/libc/include/General.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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

// An environment variable as a path (wide on Windows, so non-ASCII folders survive); empty when unset.
std::filesystem::path EnvironmentPath(const char* name) {
#ifdef _WIN32
    std::wstring wide;
    for (const char* at = name; *at != '\0'; ++at) wide.push_back(static_cast<wchar_t>(*at));
    const wchar_t* value = _wgetenv(wide.c_str());
    return value != nullptr && value[0] != L'\0' ? std::filesystem::path(value) : std::filesystem::path{};
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

std::filesystem::path UserSettingsDirectory() {
#ifdef _WIN32
    return PortSettings::UserDirectory(true, EnvironmentPath);
#else
    return PortSettings::UserDirectory(false, EnvironmentPath);
#endif
}

unsigned long ProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string ReadFile(const std::filesystem::path& path, bool& found) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) throw std::runtime_error("'" + PortSettings::PathText(path) + "' is a directory");
    std::ifstream file(path, std::ios::binary);
    found = static_cast<bool>(file);
    if (!found) return {};
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) throw std::runtime_error("cannot read " + PortSettings::PathText(path));
    return text.str();
}

// The host command line, argv[0] first (the guest sees only argv[0] on Windows).
std::vector<std::string> HostArguments() {
    std::vector<std::string> args;
#ifdef _WIN32
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) return args;
    for (int index = 0; index < count; ++index) args.push_back(PortSettings::PathText(std::filesystem::path(wide[index])));
    LocalFree(wide);
#else
    std::ifstream stream("/proc/self/cmdline", std::ios::binary);
    for (std::string argument; std::getline(stream, argument, '\0');) args.push_back(argument);
#endif
    return args;
}

// anyps5-launcher next to the executable, else in its libs folder (the release layout); empty when missing.
std::filesystem::path FindLauncher() {
#ifdef _WIN32
    const std::string name = std::string(PortSettings::LauncherName) + ".exe";
#else
    const std::string name = PortSettings::LauncherName;
#endif
    const auto directory = ExecutableDirectory();
    for (const auto& candidate : {directory / name, directory / "libs" / name}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

// Runs the launcher on the settings file and waits for it (its lines go to this process's stderr): its
// exit code, -1 when it did not start or ended on a signal.
int SpawnLauncher(const std::filesystem::path& launcher, const std::filesystem::path& file, std::string& error) {
#ifdef _WIN32
    std::wstring command = L"\"" + launcher.wstring() + L"\" --settings \"" + file.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        error = "CreateProcess error " + std::to_string(GetLastError());
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(process.hProcess, &code)) code = static_cast<DWORD>(-1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::string program = launcher.string();
    std::string option = "--settings";
    std::string path = file.string();
    char* argv[] = {program.data(), option.data(), path.data(), nullptr};
    pid_t child = 0;
    if (const int result = posix_spawn(&child, program.c_str(), nullptr, nullptr, argv, environ); result != 0) {
        error = std::string("posix_spawn: ") + std::strerror(result);
        return -1;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            error = std::string("waitpid: ") + std::strerror(errno);
            return -1;
        }
    }
    if (!WIFEXITED(status)) error = "ended on signal " + std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// The settings launcher (s61, default on; --launch, APS5_NO_LAUNCHER=1 or APS5_LAUNCHER=0 skip it): runs
// before the file is read, so its choices apply to this start. Closing its window ends the process here,
// before the window, Vulkan or the path overlay exist; a launcher that fails or is missing leaves the file
// as it is and the game starts.
void RunLauncher(const std::filesystem::path& file) {
    if (const char* reason = PortSettings::LauncherSkipReason(HostArguments(), [](const char* name) { return static_cast<const char*>(std::getenv(name)); })) {
        std::fprintf(stderr, "[launcher] not shown (%s)\n", reason);
        return;
    }
    const auto launcher = FindLauncher();
    if (launcher.empty()) {
        std::fprintf(stderr, "[launcher] not shown: %s not found next to the executable or in its libs folder\n", PortSettings::LauncherName);
        return;
    }
    std::fflush(stderr);
    std::string error;
    const int code = SpawnLauncher(launcher, file, error);
    if (code == 0) {
        std::fprintf(stderr, "[launcher] Play\n");
    } else if (code == PortSettings::LauncherExitQuit) {
        std::fprintf(stderr, "[launcher] closed without Play: exiting\n");
        std::fflush(stderr);
        std::fflush(stdout);
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0);
#endif
        std::_Exit(0);
    } else {
        std::fprintf(stderr, "[launcher] %s failed (%s): starting with %s as it is\n", PortSettings::PathText(launcher).c_str(),
            error.empty() ? ("exit code " + std::to_string(code)).c_str() : error.c_str(), PortSettings::PathText(file).c_str());
    }
}

PortSettings::Settings Load() {
    PortSettings::Settings settings;
    const char* configured = std::getenv("APS5_SETTINGS");
    const bool explicitPath = configured != nullptr && configured[0] != '\0';
    if (!explicitPath || PortSettings::Lower(configured) != "none") {
        const auto path = explicitPath ? EnvironmentPath("APS5_SETTINGS") : ExecutableDirectory() / PortSettings::FileName;
        const std::string display = PortSettings::PathText(path);
        RunLauncher(path);
        try {
            bool found = false;
            const auto text = ReadFile(path, found);
            if (found) {
                settings = PortSettings::Parse(text, PortSettings::PathText(path.filename()));
                settings.source = display;
            } else if (explicitPath) {
                settings.warnings.push_back("APS5_SETTINGS: cannot open '" + display + "'; using the defaults");
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

// The merged copies this process wrote; removed at exit (best effort: a copy the guest still holds open
// stays until a later start's sweep).
std::vector<std::filesystem::path>& WrittenCopies() {
    static std::vector<std::filesystem::path> copies;
    return copies;
}

void RemoveWrittenCopies() {
    for (const auto& copy : WrittenCopies()) {
        std::error_code ignored;
        std::filesystem::remove(copy, ignored);
    }
}

// Copies of processes that died without cleaning up (named <pid>-<file>, a week old or more).
void SweepStaleCopies(const std::filesystem::path& directory) {
    std::error_code error;
    const auto limit = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
    for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
        std::error_code stale;
        const bool regular = entry->is_regular_file(stale);
        if (stale || !regular) continue;
        const auto written = entry->last_write_time(stale);
        if (!stale && written < limit) std::filesystem::remove(entry->path(), stale);
    }
}

}

extern "C" const PortSettings::Settings& GetPortSettings_nid_no_patch() {
    static const PortSettings::Settings settings = Load();
    return settings;
}

std::vector<std::filesystem::path> PortOverrideDirectories() {
    return PortSettings::OverrideDirectories(ExecutableDirectory(), UserSettingsDirectory());
}

void InstallPortSettingsOverlay(const std::function<std::filesystem::path(const char*)>& resolve) {
    const auto args = PortSettings::GameArgs(GetPortSettings_nid_no_patch());
    if (args.empty()) return;
    std::string joined;
    for (const auto& arg : args) joined += (joined.empty() ? "" : " ") + arg;
    try {
        // Next to the executable when that folder takes writes (an install under Program Files or a
        // read-only image does not), else the per-user folder. Names carry the process id and every
        // write is a temporary file renamed into place, so two instances never share or half-write a file.
        const auto directories = PortSettings::OverrideDirectories(ExecutableDirectory(), UserSettingsDirectory());
        for (const auto& directory : directories) SweepStaleCopies(directory);
        const std::string prefix = std::to_string(ProcessId()) + "-";
        std::size_t files = 0;
        std::filesystem::path lastDirectory;
        std::string failure;
        const auto overlay = [&](const std::string& guest, const std::filesystem::path& host, bool lines) {
            bool found = false;
            const auto original = ReadFile(host, found);
            if (!found) return;
            const auto merged = lines ? PortSettings::AppendLines(original, args) : PortSettings::AppendWords(original, args);
            auto name = std::filesystem::path(prefix);
            name += host.filename();
            const auto target = PortSettings::WriteOverrideFile(directories, name, merged, failure);
            if (target.empty()) throw std::runtime_error(failure.empty() ? "no folder to write the merged files to" : failure);
            if (WrittenCopies().empty()) std::atexit(RemoveWrittenCopies);
            WrittenCopies().push_back(target);
            lastDirectory = target.parent_path();
            AddPathAliasHost_nid_no_patch(guest.c_str(), target);
            ++files;
        };
        // The render configs load after the command line and set r_Wants4K and TargetFramesPerSecond
        // themselves, so both carry the arguments (the command line alone covers the boot menus).
        overlay("app0/packagecmdlineargs.txt", resolve("/app0/packagecmdlineargs.txt"), false);
        std::error_code error;
        for (std::filesystem::directory_iterator entry(resolve("/app0/misc"), error), end; !error && entry != end; entry.increment(error)) {
            const auto name = PortSettings::PathText(entry->path().filename());
            const auto lower = PortSettings::Lower(name);
            if (!lower.starts_with("renderconfig_ps5_") || !lower.ends_with(".txt")) continue;
            overlay("app0/misc/" + name, entry->path(), true);
        }
        std::fprintf(stderr, "[settings] game args %s in %zu files (%s)\n", joined.c_str(), files, PortSettings::PathText(lastDirectory).c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] game args %s not applied: %s\n", joined.c_str(), error.what());
    }
    std::fflush(stderr);
}
