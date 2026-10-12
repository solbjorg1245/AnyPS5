#include "Host.hpp"

#include "GameFolder.hpp"
#include "HostText.hpp"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Launcher {

namespace {

constexpr int OutcomeBusy = 1;
constexpr std::size_t MaxLines = 400;
constexpr const char* PickerTitle = "Choose the Demon's Souls game folder (app0)";

#ifdef _WIN32
std::wstring Quote(const std::wstring& text) {
    return QuoteArgument(text);
}

std::wstring Wide(const std::string& text) {
    return PathFromText(text).wstring();
}

HANDLE OpenNul(SECURITY_ATTRIBUTES* inherit) {
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, inherit, OPEN_EXISTING, 0, nullptr);
}

// CreateProcessW with standard handles and only those inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the
// child gets the pipe or the log file and none of the launcher's other inheritable handles (a log file
// or pipe of whoever started the launcher). The handles must be inheritable. `error`: GetLastError.
bool CreateWithHandles(const std::wstring& application, std::wstring& command, DWORD flags, const wchar_t* directory, HANDLE input, HANDLE output, PROCESS_INFORMATION& process,
    DWORD& error) {
    std::vector<HANDLE> handles{input, output};
    handles.erase(std::remove_if(handles.begin(), handles.end(), [](HANDLE handle) { return handle == nullptr || handle == INVALID_HANDLE_VALUE; }), handles.end());
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> buffer(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        error = GetLastError();
        return false;
    }
    const bool listed = handles.empty() || UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.lpAttributeList = attributes;
    const BOOL created = listed && CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, !handles.empty(), flags | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory,
                                                  &startup.StartupInfo, &process);
    error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    return created;
}

std::wstring Variable(const wchar_t* name) {
    wchar_t value[64] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, 64);
    return length > 0 && length < 64 ? std::wstring(value, length) : std::wstring();
}
#else
// Runs argv[0] from PATH with stdout captured (stderr dropped), in a process group of its own (`child`
// gets its id, so kill(-child) ends it with what it started): its exit code, -1 when it did not start.
int RunCapture(const std::vector<std::string>& args, std::string& out, std::atomic<int>* child = nullptr) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        return -1;
    }
    if (child != nullptr) *child = pid;
    char buffer[4096];
    for (ssize_t count; (count = read(fds[0], buffer, sizeof(buffer))) != 0;) {
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) break;
        out.append(buffer, static_cast<std::size_t>(count));
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (child != nullptr) *child = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// xdg-desktop-portal's FileChooser through gdbus: prints the request's Response signal line, exit 3 when
// the portal (or gdbus) is not there.
constexpr const char* PortalScript = R"(command -v gdbus >/dev/null 2>&1 || exit 3
tmp=$(mktemp) || exit 3
gdbus monitor --session --dest org.freedesktop.portal.Desktop >"$tmp" 2>/dev/null &
monitor=$!
trap 'kill $monitor 2>/dev/null; rm -f "$tmp"' EXIT
sleep 0.3
reply=$(gdbus call --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop --method org.freedesktop.portal.FileChooser.OpenFile "" "$1" "{'directory': <true>, 'modal': <true>}" 2>/dev/null) || exit 3
handle=${reply#*\'}
handle=${handle%%\'*}
[ -n "$handle" ] || exit 3
while :; do
    line=$(grep -F "$handle: org.freedesktop.portal.Request.Response" "$tmp") && { printf '%s\n' "$line"; exit 0; }
    kill -0 $monitor 2>/dev/null || exit 3
    sleep 0.2
done
)";

std::atomic<int> pickerChild{-1};
#endif

}

std::filesystem::path ExecutablePath() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

bool IsSteamDeck() {
#ifdef _WIN32
    return false;
#else
    // Game Mode (gamescope) sets SteamDeck=1 and runs a gamescope session; desktop mode is told by the board.
    if (const char* deck = std::getenv("SteamDeck"); deck != nullptr && std::strcmp(deck, "1") == 0) return true;
    if (const char* gamescope = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); gamescope != nullptr && gamescope[0] != '\0') return true;
    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP"); desktop != nullptr && PortSettings::Lower(desktop) == "gamescope") return true;
    std::ifstream vendor("/sys/devices/virtual/dmi/id/board_vendor");
    std::string text;
    std::getline(vendor, text);
    return PortSettings::Trim(text) == "Valve";
#endif
}

SetupProcess::~SetupProcess() {
#ifdef _WIN32
    if (Running()) {
        if (job_ != nullptr) TerminateJobObject(job_, 1);
        else TerminateProcess(process_, 1);
    }
    if (reader_.joinable()) reader_.join();
    for (void* handle : {process_, job_, pipe_}) {
        if (handle != nullptr) CloseHandle(handle);
    }
#else
    if (pid_ > 0 && Running()) kill(-pid_, SIGTERM);
    if (reader_.joinable()) reader_.join();
    if (pipe_ >= 0) close(pipe_);
#endif
}

bool SetupProcess::Start(const std::filesystem::path& installDir, const std::filesystem::path& gameDir, std::string& error) {
#ifdef _WIN32
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    // -ExecutionPolicy Bypass for this one process: an unsigned (or downloaded) script runs unless a group
    // policy forces another policy.
    std::wstring command = Quote(powershell) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote((installDir / SetupScript).wstring()) + L" -Dump " +
        Quote(gameDir.wstring()) + L" -SetupOnly";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) {
        error = "CreatePipe error " + std::to_string(GetLastError());
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = OpenNul(&inherit);
    // The setup and everything it starts (relinker, Python) end with the launcher.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    PROCESS_INFORMATION process{};
    DWORD createError = 0;
    const bool created = CreateWithHandles(powershell, command, CREATE_NO_WINDOW | CREATE_SUSPENDED, installDir.c_str(), input, writeEnd, process, createError);
    CloseHandle(writeEnd);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!created) {
        CloseHandle(readEnd);
        if (job != nullptr) CloseHandle(job);
        error = "cannot start PowerShell (CreateProcess error " + std::to_string(createError) + ")";
        return false;
    }
    if (job != nullptr) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    job_ = job;
    pipe_ = readEnd;
    AddText("== " + PortSettings::PathText(installDir / SetupScript) + " -Dump \"" + PortSettings::PathText(gameDir) + "\" -SetupOnly\n");
#else
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // its own group: closing ends the whole setup
    posix_spawnattr_setpgroup(&attributes, 0);
    const std::string script = PortSettings::PathText(installDir / SetupScript);
    const std::string dump = PortSettings::PathText(gameDir);
    std::vector<std::string> args{"bash", script, "--dump", dump, "--setup-only"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, "bash", &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        error = std::string("cannot start bash: ") + std::strerror(spawned);
        return false;
    }
    pid_ = pid;
    pipe_ = fds[0];
    AddText("== " + script + " --dump \"" + dump + "\" --setup-only\n");
#endif
    started_ = true;
    reader_ = std::thread(&SetupProcess::Read, this);
    return true;
}

void SetupProcess::AddText(const std::string& text) {
    std::lock_guard lock(mutex_);
    partial_ += text;
    for (std::size_t end; (end = partial_.find('\n')) != std::string::npos;) {
        auto line = partial_.substr(0, end);
        partial_.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(std::move(line));
    }
    if (lines_.size() > MaxLines) lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - MaxLines));
}

std::vector<std::string> SetupProcess::Lines() const {
    std::lock_guard lock(mutex_);
    auto lines = lines_;
    if (!partial_.empty()) lines.push_back(partial_);
    return lines;
}

void SetupProcess::Read() {
    char buffer[4096];
#ifdef _WIN32
    // PowerShell writes redirected output in the console's OEM code page; converted a whole line at a time,
    // so a multibyte character split between two reads stays whole.
    const auto convert = [this](const std::string& bytes) {
        if (bytes.empty()) return;
        const int wideLength = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideLength);
        AddText(PortSettings::PathText(std::filesystem::path(wide)));
    };
    std::string pending;
    DWORD count = 0;
    while (ReadFile(pipe_, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
        pending.append(buffer, count);
        const auto end = pending.rfind('\n');
        if (end == std::string::npos) continue;
        convert(pending.substr(0, end + 1));
        pending.erase(0, end + 1);
    }
    convert(pending);
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    exitCode_ = static_cast<int>(code);
#else
    for (ssize_t count; (count = read(pipe_, buffer, sizeof(buffer))) != 0;) {
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) break;
        AddText(std::string(buffer, static_cast<std::size_t>(count)));
    }
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    finished_ = true;
}

std::string SetGameEnvironment() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    const auto ramGiB = std::llround(static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
    const auto set = [](const wchar_t* name, const wchar_t* value) {
        if (Variable(name).empty()) SetEnvironmentVariableW(name, value);
    };
    set(L"APS5_HOST_IMPORT_MIB", ramGiB >= 48 ? L"16384" : ramGiB >= 24 ? L"8192" : L"4096");
    set(L"APS5_BINDLESS_SLOTS", L"48");
    return "APS5_HOST_IMPORT_MIB=" + PortSettings::PathText(std::filesystem::path(Variable(L"APS5_HOST_IMPORT_MIB"))) + ", APS5_BINDLESS_SLOTS=" +
        PortSettings::PathText(std::filesystem::path(Variable(L"APS5_BINDLESS_SLOTS")));
#else
    struct sysinfo info{};
    sysinfo(&info);
    const unsigned long long ramKiB = static_cast<unsigned long long>(info.totalram) * info.mem_unit / 1024;
    const auto ramGiB = (ramKiB + 524288) / 1048576;  // rounded like DemonsSouls.sh
    setenv("APS5_HOST_IMPORT_MIB", ramGiB >= 48 ? "16384" : ramGiB >= 24 ? "8192" : "4096", 0);
    setenv("APS5_BINDLESS_SLOTS", "48", 0);
    return std::string("APS5_HOST_IMPORT_MIB=") + std::getenv("APS5_HOST_IMPORT_MIB") + ", APS5_BINDLESS_SLOTS=" + std::getenv("APS5_BINDLESS_SLOTS");
#endif
}

std::string MemoryWarning() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return {};
    const auto commitGiB = std::llround(static_cast<double>(memory.ullTotalPageFile) / (1024.0 * 1024.0 * 1024.0));
    if (commitGiB >= 64) return {};
    return "RAM + page file is " + std::to_string(commitGiB) +
        " GiB; the game commits about 60 GiB. Enlarge the page file (System > About > Advanced system settings > Performance > Virtual memory) or the game may crash with out-of-memory errors.";
#else
    struct sysinfo info{};
    if (sysinfo(&info) != 0) return {};
    const unsigned long long totalKiB = (static_cast<unsigned long long>(info.totalram) + info.totalswap) * info.mem_unit / 1024;
    if (totalKiB / 1048576 >= 64) return {};
    return "RAM + swap is below 64 GiB; the game reserves and touches about 60 GiB. Add swap if it dies with out-of-memory errors.";
#endif
}

bool PythonUsable() {
    const auto candidates = PythonCandidates([](const char* name) { return std::getenv(name); });
    for (const auto& candidate : candidates) {
#ifdef _WIN32
        std::wstring command = Quote(Wide(candidate[0]));
        for (std::size_t index = 1; index < candidate.size(); ++index) command += L" " + Quote(Wide(candidate[index]));
        command += L" -c " + Quote(Wide(PythonVersionProbe));
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE nul = OpenNul(&inherit);
        PROCESS_INFORMATION process{};
        DWORD error = 0;
        const bool created = CreateWithHandles(Wide(candidate[0]), command, CREATE_NO_WINDOW, nullptr, nul, nul, process, error);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!created) continue;
        CloseHandle(process.hThread);
        DWORD code = 1;
        if (WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
        else TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        if (code == 0) return true;
#else
        std::vector<std::string> args = candidate;
        args.insert(args.end(), {"-c", PythonVersionProbe});
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        for (const int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
        pid_t pid = -1;
        const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0) continue;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        pid_t done = 0;
        while ((done = waitpid(pid, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline) usleep(20000);
        if (done == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            continue;
        }
        if (done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
#endif
    }
    return false;
}

namespace {

bool UseConsole() {
    const char* value = std::getenv("APS5_LAUNCHER_CONSOLE");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// The game's log file, the previous one kept as anyps5-game.old.log.
std::filesystem::path RotateGameLog(const std::filesystem::path& installDir) {
    const auto log = installDir / GameLogName;
    std::error_code error;
    if (std::filesystem::exists(log, error)) std::filesystem::rename(log, installDir / "anyps5-game.old.log", error);
    return log;
}

}

bool LaunchGame(const std::filesystem::path& installDir, const std::vector<std::string>& args, bool dryRun, GameProcess& game, std::string& error) {
    const auto environment = SetGameEnvironment();
    const auto executable = installDir / GameExecutable;
    const bool console = UseConsole();
#ifdef _WIN32
    std::wstring command = Quote(executable.wstring()) + L" --launch";
    for (const auto& arg : args) command += L" " + Quote(Wide(arg));
    std::fprintf(stderr, "[launcher] %s %s (%s; output: %s)\n", dryRun ? "would start" : "starting", PortSettings::PathText(std::filesystem::path(command)).c_str(), environment.c_str(),
        console ? "its own console window" : PortSettings::PathText(installDir / GameLogName).c_str());
    std::fflush(stderr);
    if (dryRun) return true;
    PROCESS_INFORMATION process{};
    DWORD createError = 0;
    bool created = false;
    if (console) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, installDir.c_str(), &startup, &process);
        createError = created ? 0 : GetLastError();
    } else {
        // No console window: the game's output goes to the log file instead.
        game.log = RotateGameLog(installDir);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE log = CreateFileW(game.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            error = "Cannot write " + PortSettings::PathText(game.log) + " (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        HANDLE input = OpenNul(&inherit);
        created = CreateWithHandles(executable.wstring(), command, CREATE_NO_WINDOW, installDir.c_str(), input, log, process, createError);
        CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    }
    if (!created) {
        error = "Cannot start " + PortSettings::PathText(executable) + " (CreateProcess error " + std::to_string(createError) + ").";
        return false;
    }
    CloseHandle(process.hThread);
    game.handle = process.hProcess;
    return true;
#else
    std::vector<std::string> argv{PortSettings::PathText(executable), "--launch"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string shown;
    for (const auto& arg : argv) shown += (shown.empty() ? "" : " ") + arg;
    std::fprintf(stderr, "[launcher] %s %s (%s; output: %s)\n", dryRun ? "would start" : "starting", shown.c_str(), environment.c_str(),
        console ? "the launcher's own" : PortSettings::PathText(installDir / GameLogName).c_str());
    std::fflush(stderr);
    if (dryRun) return true;
    // The game runs in the install folder (the launcher needs no working directory of its own after this).
    if (chdir(PortSettings::PathText(installDir).c_str()) != 0) {
        error = "Cannot enter " + PortSettings::PathText(installDir) + ": " + std::strerror(errno) + ".";
        return false;
    }
    std::vector<char*> pointers;
    for (auto& arg : argv) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::string logText;
    if (!console) {
        game.log = RotateGameLog(installDir);
        logText = PortSettings::PathText(game.log);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, logText.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, pointers[0], &actions, nullptr, pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        error = "Cannot start " + argv[0] + ": " + std::strerror(spawned) + ".";
        return false;
    }
    game.pid = pid;
    return true;
#endif
}

int WaitGame(GameProcess& game) {
#ifdef _WIN32
    if (game.handle == nullptr) return 0;
    WaitForSingleObject(game.handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(game.handle, &code);
    CloseHandle(game.handle);
    game.handle = nullptr;
    return static_cast<int>(code);
#else
    if (game.pid <= 0) return 0;
    int status = 0;
    while (waitpid(game.pid, &status, 0) < 0 && errno == EINTR) {}
    game.pid = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

NativeFolderPicker::~NativeFolderPicker() {
    // An open dialog is left to the process exit (joining would wait for the player); its thread writes
    // into the shared state only.
#ifndef _WIN32
    if (const int child = pickerChild.load(); child > 0) kill(-child, SIGTERM);
#endif
    if (worker_.joinable()) worker_.detach();
}

void NativeFolderPicker::Start(SDL_Window* window, const std::filesystem::path& initial) {
    if (worker_.joinable()) worker_.join();
    auto state = state_;
    state->outcome = OutcomeBusy;
#ifdef _WIN32
    HWND owner = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &info)) owner = info.info.win.window;
    worker_ = std::thread([state, owner, initial] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto outcome = Outcome::Unavailable;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(Wide(PickerTitle).c_str());
            std::error_code error;
            if (!initial.empty() && std::filesystem::is_directory(initial, error)) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            const HRESULT shown = dialog->Show(owner);
            if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                outcome = Outcome::Cancelled;
            } else if (SUCCEEDED(shown)) {
                IShellItem* result = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->GetResult(&result)) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    state->folder = path;
                    outcome = Outcome::Picked;
                    CoTaskMemFree(path);
                }
                if (result != nullptr) result->Release();
            }
            dialog->Release();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        state->outcome = static_cast<int>(outcome);
    });
#else
    (void)window;
    worker_ = std::thread([state, initial] {
        const std::string start = initial.empty() ? std::string(std::getenv("HOME") != nullptr ? std::getenv("HOME") : "/") : PortSettings::PathText(initial);
        std::string out;
        // The desktop portal first (the desktop's own dialog, also for Flatpak), then zenity, then kdialog.
        int code = std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr ? RunCapture({"sh", "-c", PortalScript, "sh", PickerTitle}, out, &pickerChild) : -1;
        if (code == 0) {
            std::string folder;
            const auto answer = ParsePortalResponse(out, folder);
            if (answer == PortalAnswer::Picked) {
                state->folder = PathFromText(folder);
                state->outcome = static_cast<int>(Outcome::Picked);
                return;
            }
            if (answer == PortalAnswer::Cancelled) {
                state->outcome = static_cast<int>(Outcome::Cancelled);
                return;
            }
        }
        const std::vector<std::vector<std::string>> tools{
            {"zenity", "--file-selection", "--directory", std::string("--title=") + PickerTitle, "--filename=" + start + "/"},
            {"kdialog", "--title", PickerTitle, "--getexistingdirectory", start}};
        for (const auto& tool : tools) {
            out.clear();
            code = RunCapture(tool, out, &pickerChild);
            if (code == 0 && !PortSettings::Trim(out).empty()) {
                state->folder = PathFromText(PortSettings::Trim(out));
                state->outcome = static_cast<int>(Outcome::Picked);
                return;
            }
            if (code == 1) {
                state->outcome = static_cast<int>(Outcome::Cancelled);
                return;
            }
        }
        state->outcome = static_cast<int>(Outcome::Unavailable);
    });
#endif
}

NativeFolderPicker::Outcome NativeFolderPicker::Poll(std::filesystem::path& folder) {
    const auto outcome = static_cast<Outcome>(state_->outcome.load());
    if (outcome == Outcome::None || outcome == Outcome::Busy) return outcome;
    if (worker_.joinable()) worker_.join();
    state_->outcome = 0;
    folder = state_->folder;
    return outcome;
}

}
