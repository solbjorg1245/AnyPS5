// The settings launcher's host side (s61): the one-time setup as a child process with its output
// collected for the window, starting the game, the game's environment defaults, the native folder
// pickers and Steam Deck detection. Windows and Linux.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_Window;

namespace Launcher {

std::filesystem::path ExecutablePath();
bool IsSteamDeck();

// Runs DemonsSouls.ps1 -Dump <dir> -SetupOnly (PowerShell, hidden, execution policy bypassed for this
// process) or DemonsSouls.sh --dump <dir> --setup-only; stdout and stderr become lines for the window.
// Destroying it (or closing the launcher) ends the setup and everything it started.
class SetupProcess {
public:
    SetupProcess() = default;
    SetupProcess(const SetupProcess&) = delete;
    SetupProcess& operator=(const SetupProcess&) = delete;
    ~SetupProcess();

    bool Start(const std::filesystem::path& installDir, const std::filesystem::path& gameDir, std::string& error);
    bool Running() const { return started_ && !finished_; }
    bool Finished() const { return finished_; }
    int ExitCode() const { return exitCode_; }
    std::vector<std::string> Lines() const;

private:
    void Read();
    void AddText(const std::string& text);

    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::string partial_;
    std::thread reader_;
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> exitCode_{-1};
#ifdef _WIN32
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* pipe_ = nullptr;
#else
    int pid_ = -1;
    int pipe_ = -1;
#endif
};

// APS5_HOST_IMPORT_MIB and APS5_BINDLESS_SLOTS as DemonsSouls.ps1 / .sh choose them (an already set
// variable wins); a warning when RAM plus page file / swap is below what the game commits.
std::string SetGameEnvironment();
std::string MemoryWarning();

// The started game; WaitGame waits for it (the launcher stays its parent, so Steam sees one process tree).
struct GameProcess {
#ifdef _WIN32
    void* handle = nullptr;
#else
    int pid = -1;
#endif
};

// Starts the game in the install folder with --launch (so it does not open the launcher again) and the
// environment defaults; false and `error` when it did not start. `dryRun` (tests) only prints the command.
bool LaunchGame(const std::filesystem::path& installDir, const std::vector<std::string>& args, bool dryRun, GameProcess& game, std::string& error);
// The game's exit code (0 after a dry run).
int WaitGame(GameProcess& game);

// The system's folder picker: IFileOpenDialog (FOS_PICKFOLDERS) on Windows, on Linux the desktop portal
// (gdbus), zenity or kdialog, whichever works first, on a thread so the window keeps drawing.
class NativeFolderPicker {
public:
    enum class Outcome { None, Busy, Picked, Cancelled, Unavailable };
    NativeFolderPicker() = default;
    NativeFolderPicker(const NativeFolderPicker&) = delete;
    NativeFolderPicker& operator=(const NativeFolderPicker&) = delete;
    ~NativeFolderPicker();

    void Start(SDL_Window* window, const std::filesystem::path& initial);
    // Busy while open; then once Picked (with `folder`), Cancelled or Unavailable; None otherwise.
    Outcome Poll(std::filesystem::path& folder);

private:
    std::thread worker_;
    std::atomic<int> outcome_{0};
    std::filesystem::path folder_;
};

}
