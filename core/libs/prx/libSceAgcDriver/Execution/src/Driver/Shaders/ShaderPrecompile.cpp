#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderPrecompile.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderKeyList.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/HostThread.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

namespace KeyList = ShaderRecompiler::ShaderKeyList;

// Files above this size are not scanned (the code lies in shader bundles and executables).
constexpr std::uintmax_t MaxScanBytes = 256ull << 20u;

struct Code {
    KeyList::CodeIdentity identity;
    std::vector<std::uint32_t> words;
    std::string path;
    std::uint64_t offset = 0;
    bool located = false;
    bool scanned = false;
};

// An APS5_* switch that is on only for a value starting with 1.
bool switchOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

unsigned processorCount() {
#ifdef _WIN32
    return static_cast<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
    const auto count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<unsigned>(count) : 1u;
#endif
}

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool endsWithNoCase(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) return false;
    const auto tail = text.substr(text.size() - suffix.size());
    return std::equal(tail.begin(), tail.end(), suffix.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// The running program's own file (KeyList::ExecutableLocator).
std::filesystem::path executablePath() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return std::filesystem::path(path);
#else
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : self;
#endif
}

// A path for the log, as UTF-8 (string() can throw on Windows for names outside the code page).
std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path locatorFile(const std::filesystem::path& app0, std::string_view locator) {
    return locator == KeyList::ExecutableLocator ? executablePath() : app0 / std::filesystem::path(std::string(locator));
}

// One APS5_PRECOMPILE_SCAN pattern: a file below app0 ("eboot.bin"), the files below a folder whose
// names end in a suffix ("materials/*.csdr", or "*.csdr" for all of app0), or "@exe" for the program.
void collectFiles(const std::filesystem::path& root, std::string_view pattern, std::set<std::filesystem::path>& files) {
    std::error_code error;
    if (pattern == KeyList::ExecutableLocator) {
        const auto path = executablePath();
        if (!path.empty() && std::filesystem::is_regular_file(path, error)) files.insert(path);
        return;
    }
    const auto star = pattern.find('*');
    if (star == std::string_view::npos) {
        const auto path = root / std::filesystem::path(std::string(pattern));
        if (std::filesystem::is_regular_file(path, error)) files.insert(path);
        return;
    }
    const auto slash = pattern.substr(0, star).rfind('/');
    const auto folder = slash == std::string_view::npos ? root : root / std::filesystem::path(std::string(pattern.substr(0, slash)));
    const auto suffix = pattern.substr(star + 1);
    const auto options = std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(folder, options, error), end; !error && it != end; it.increment(error)) {
        std::error_code typeError;
        if (!it->is_regular_file(typeError)) continue;
        if (endsWithNoCase(it->path().filename().string(), suffix)) files.insert(it->path());
    }
}

// The precompile of one list (RunShaderPrecompile); throws only what it cannot handle itself. The
// shipped list (KeyList::ChooseList) runs only while the cache is cold for it and is never rewritten.
void precompileList(const ShaderRecompiler::SpirvTarget& target, const std::filesystem::path& listFile, bool shipped, std::chrono::steady_clock::time_point started) {
    const auto listText = pathText(listFile);
    const char* listPath = listText.c_str();
    const auto entryDirectory = ShaderRecompiler::ShaderDiskCache::EntryDirectory();
    std::uint64_t listHash = 0, context = 0;
    if (shipped) {
        std::vector<std::byte> bytes;
        std::error_code error;
        const auto size = std::filesystem::file_size(listFile, error);
        if (error || size > KeyList::MaxListBytes || !ShaderRecompiler::ReadWholeFile(listFile, bytes)) {
            std::fprintf(stderr, "[precompile] cannot read %s (over %llu MiB?)\n", listPath, static_cast<unsigned long long>(KeyList::MaxListBytes >> 20u));
            return;
        }
        listHash = ShaderRecompiler::HashBytes(bytes);
        context = ShaderRecompiler::ShaderDiskCache::ContextDigest(target);
        if (KeyList::PrecompileDone(entryDirectory, listHash, context)) {
            std::fprintf(stderr, "[precompile] %s: precompiled into this cache before (%s), skipped in %.3f s (APS5_PRECOMPILE=0 turns the shipped list off)\n", listPath, std::string(KeyList::MarkerName).c_str(), secondsSince(started));
            return;
        }
    }
    KeyList::ListFile list;
    if (!KeyList::ReadList(listFile, list)) {
        std::fprintf(stderr, "[precompile] cannot read %s (missing, or over %llu MiB)\n", listPath, static_cast<unsigned long long>(KeyList::MaxListBytes >> 20u));
        return;
    }
    auto& records = list.records;

    // One code per distinct identity; records with the same code share it.
    std::vector<Code> codes;
    std::vector<std::size_t> recordCode(records.size());
    {
        std::map<std::tuple<std::uint32_t, std::uint64_t, std::uint64_t>, std::size_t> index;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto& identity = records[i].code;
            const auto [it, added] = index.try_emplace({identity.dwords, identity.hash[0], identity.hash[1]}, codes.size());
            if (added) codes.push_back({identity});
            auto& code = codes[it->second];
            if (code.path.empty() && !records[i].locatorPath.empty()) {
                code.path = records[i].locatorPath;
                code.offset = records[i].locatorOffset;
            }
            recordCode[i] = it->second;
        }
    }

    const std::filesystem::path app0 = ResolvePath_nid_no_patch("/app0");
    const auto take = [](Code& code, std::span<const std::byte> bytes, std::uint64_t offset) {
        if (!KeyList::TakeCode(bytes, offset, code.identity, code.words)) return false;
        code.offset = offset;
        code.located = true;
        return true;
    };
    // A file read whole, unless it is over MaxScanBytes.
    const auto readBounded = [](const std::filesystem::path& path, std::vector<std::byte>& bytes) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        return !error && size <= MaxScanBytes && ShaderRecompiler::ReadWholeFile(path, bytes);
    };

    // The locators the list carries (ValidLocatorPath: below app0, or the program), one read per file.
    std::size_t byLocator = 0, staleLocators = 0;
    {
        std::map<std::string, std::vector<std::size_t>> byPath;
        for (std::size_t i = 0; i < codes.size(); ++i) {
            if (!codes[i].path.empty()) byPath[codes[i].path].push_back(i);
        }
        std::vector<std::byte> bytes;
        for (const auto& [path, members] : byPath) {
            const bool read = readBounded(locatorFile(app0, path), bytes);
            for (const auto i : members) {
                if (read && take(codes[i], bytes, codes[i].offset)) ++byLocator;
                else {
                    ++staleLocators;
                    codes[i].path.clear();
                }
            }
        }
    }

    // APS5_PRECOMPILE_SCAN=<pattern>;...: the codes still missing are searched for in those files.
    std::size_t byScan = 0, scannedFiles = 0;
    std::uint64_t scannedBytes = 0;
    double scanSeconds = 0;
    const char* scan = std::getenv("APS5_PRECOMPILE_SCAN");
    std::vector<std::size_t> missing;
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if (!codes[i].located) missing.push_back(i);
    }
    if (scan != nullptr && *scan != '\0' && !missing.empty()) {
        const auto scanStarted = std::chrono::steady_clock::now();
        std::set<std::filesystem::path> files;
        const std::string_view patterns(scan);
        for (std::size_t start = 0; start <= patterns.size();) {
            const auto end = std::min(patterns.find(';', start), patterns.size());
            if (end > start) collectFiles(app0, patterns.substr(start, end - start), files);
            start = end + 1;
        }
        std::vector<KeyList::CodeIdentity> identities;
        for (const auto i : missing) identities.push_back(codes[i].identity);
        const KeyList::Scanner scanner(identities);
        const auto executable = executablePath();
        std::vector<std::byte> bytes;
        for (const auto& path : files) {
            if (!readBounded(path, bytes)) continue;
            ++scannedFiles;
            scannedBytes += bytes.size();
            for (const auto& hit : scanner.Find(bytes)) {
                auto& code = codes[missing[hit.index]];
                if (code.located || !take(code, bytes, hit.offset)) continue;
                code.path = !executable.empty() && path == executable ? std::string(KeyList::ExecutableLocator) : path.lexically_relative(app0).generic_string();
                // A file the list could not name again (outside app0 through a link) is used, not recorded.
                if (!KeyList::ValidLocatorPath(code.path)) code.path.clear();
                code.scanned = true;
                ++byScan;
            }
        }
        scanSeconds = secondsSince(scanStarted);
    }

    // The list gains the locators found, and format 1 and 2 lines become format 3 (written after the
    // compile, with the inputs digests it adds); every other line stays as it was.
    bool listChanged = list.legacy != 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& code = codes[recordCode[i]];
        if (!code.located || code.path.empty() || (records[i].locatorPath == code.path && records[i].locatorOffset == code.offset)) continue;
        records[i].locatorPath = code.path;
        records[i].locatorOffset = code.offset;
        listChanged = true;
    }

    std::vector<std::size_t> work;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (codes[recordCode[i]].located) work.push_back(i);
    }
    // Two processors stay with the game's own threads (loading goes on meanwhile).
    unsigned threads = std::clamp(processorCount() > 2 ? processorCount() - 2 : 1u, 1u, 16u);
    if (const char* value = std::getenv("APS5_PRECOMPILE_THREADS")) threads = std::clamp(static_cast<unsigned>(std::strtoul(value, nullptr, 10)), 1u, 64u);
    // APS5_PRECOMPILE_UNVERIFIED=1: variants whose entry name differs from the recording's (another
    // device, sources or switch set: nothing to compare their SPIR-V with) are compiled and stored too.
    const bool storeUnverified = switchOn("APS5_PRECOMPILE_UNVERIFIED");
    // APS5_PRECOMPILE_BUDGET_S=<seconds> (default 600): the compile stops taking records after that long
    // since the precompile began; the rest is left to the on-demand compile.
    double budgetSeconds = 600.0;
    if (const char* value = std::getenv("APS5_PRECOMPILE_BUDGET_S"); value != nullptr && *value != '\0') budgetSeconds = std::clamp(std::strtod(value, nullptr), 1.0, 86400.0);
    std::atomic<std::size_t> cut{0};
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> stored{0}, storedByInputs{0}, storedUnverified{0}, unverified{0}, present{0}, diverged{0}, failed{0}, sameEntry{0}, sameSpirv{0};
    // The inputs digest of each record the compile verified by its entry name (for a record without one).
    std::vector<std::uint64_t> digests(records.size(), 0);
    std::atomic<std::uint64_t> compileNs{0};
    HostMutex failureMutex;
    std::vector<std::string> failures;
    const auto compileStarted = std::chrono::steady_clock::now();
    const auto worker = [&] {
        KeyList::RebuiltRequest rebuilt;
        while (true) {
            const auto item = next.fetch_add(1, std::memory_order_relaxed);
            if (item >= work.size()) return;
            if (secondsSince(started) > budgetSeconds) {
                cut.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto& record = records[work[item]];
            const auto itemStarted = std::chrono::steady_clock::now();
            try {
                KeyList::Rebuild(record, codes[recordCode[work[item]]].words, target, rebuilt);
                const auto outcome = KeyList::Precompile(record, rebuilt, storeUnverified);
                switch (outcome.status) {
                case KeyList::PrecompileStatus::Stored: stored.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::StoredByInputs: storedByInputs.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::StoredUnverified: storedUnverified.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::Unverified: unverified.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::Present: present.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::Diverged: diverged.fetch_add(1, std::memory_order_relaxed); break;
                case KeyList::PrecompileStatus::Disabled: break;
                }
                if (outcome.entry == record.entry && outcome.status != KeyList::PrecompileStatus::Diverged) {
                    sameEntry.fetch_add(1, std::memory_order_relaxed);
                    digests[work[item]] = outcome.inputDigest;
                }
                if ((outcome.status == KeyList::PrecompileStatus::Stored || outcome.status == KeyList::PrecompileStatus::StoredByInputs) && outcome.spirvHash == record.spirvHash) sameSpirv.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& error) {
                failed.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(failureMutex);
                if (failures.size() < 4) failures.push_back(std::string(error.what()).substr(0, 300));
            } catch (...) {
                failed.fetch_add(1, std::memory_order_relaxed);
            }
            compileNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - itemStarted).count()), std::memory_order_relaxed);
        }
    };
    {
        std::vector<HostThread> pool;
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
        for (auto& thread : pool) thread.join();
    }
    ShaderRecompiler::ShaderDiskCache::Flush();
    const auto compileSeconds = secondsSince(compileStarted);

    // Records verified by their entry name gain their inputs digest, so the list verifies on later builds.
    std::size_t digestsAdded = 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].inputDigest != 0 || digests[i] == 0) continue;
        records[i].inputDigest = digests[i];
        ++digestsAdded;
    }
    // APS5_PRECOMPILE_KEEP_LIST=1 leaves the file alone, and so do a recording into it and the shipped list.
    bool listUpdated = false;
    if (listChanged || digestsAdded != 0) {
        bool recordingInto = false;
        if (const char* keys = std::getenv("APS5_SHADER_KEYS"); keys != nullptr && *keys != '\0') {
            std::error_code error;
            recordingInto = std::filesystem::equivalent(keys, listFile, error) || std::filesystem::path(keys) == listFile;
        }
        if (!shipped && !recordingInto && !switchOn("APS5_PRECOMPILE_KEEP_LIST")) listUpdated = KeyList::WriteList(listFile, list);
    }

    std::size_t notFound = 0;
    for (const auto& code : codes) {
        if (!code.located) ++notFound;
    }
    for (const auto& failure : failures) std::fprintf(stderr, "[precompile] compile failed: %s\n", failure.c_str());
    if (cut.load() != 0) std::fprintf(stderr, "[precompile] time budget of %.0f s reached (APS5_PRECOMPILE_BUDGET_S): %zu of %zu variants left to the on-demand compile\n", budgetSeconds, cut.load(), work.size());
    if (list.overLimit != 0) std::fprintf(stderr, "[precompile] %zu records beyond the %zu-record limit left out\n", list.overLimit, KeyList::MaxRecords);
    if (diverged.load() != 0) std::fprintf(stderr, "[precompile] %zu variants compiled to other SPIR-V than the recording under the same entry name: not stored (the rebuilt request misses an input)\n", diverged.load());
    if (unverified.load() != 0) std::fprintf(stderr, "[precompile] %zu variants have neither the recording's entry name nor its inputs digest (a format 1 or 2 record from another build, device or switch set): not compiled (APS5_PRECOMPILE_UNVERIFIED=1 stores them unverified)\n", unverified.load());
    if (!work.empty() && stored.load() + storedByInputs.load() + present.load() == 0) std::fprintf(stderr, "[precompile] WARNING: no record of %s verified on this build and device (%zu unverifiable, %zu diverged, %zu failed): record the list again (APS5_SHADER_KEYS)\n", listPath, unverified.load(), diverged.load(), failed.load());
    if (!codes.empty() && notFound == codes.size()) std::fprintf(stderr, "[precompile] WARNING: none of the %zu codes of %s is in the game files at its locator (another game version? APS5_PRECOMPILE_SCAN searches for them)\n", codes.size(), listPath);
    std::fprintf(stderr, "[precompile] %s%s: %zu records (%zu unreadable or oversized lines, %zu duplicates, %zu format 1 or 2), %zu codes: %zu by locator (%zu stale), %zu by scan (%zu files, %.1f MiB, %.1f s), %zu not found%s; %zu compiled (%zu by inputs digest, %zu unverified), %zu present, %zu diverged, %zu unverifiable skipped, %zu failed in %.1f s on %u threads (%.1f s compiling); %zu entry names as recorded, %zu stored with the recorded SPIR-V; total %.1f s\n",
                 listPath, shipped ? " (shipped)" : "", records.size(), list.unreadable, list.duplicates, list.legacy, codes.size(), byLocator, staleLocators, byScan, scannedFiles, static_cast<double>(scannedBytes) / (1024.0 * 1024.0), scanSeconds, notFound, listUpdated ? ", list rewritten" : "",
                 stored.load() + storedByInputs.load() + storedUnverified.load(), storedByInputs.load(), storedUnverified.load(), present.load(), diverged.load(), unverified.load(), failed.load(), compileSeconds, threads, static_cast<double>(compileNs.load()) / 1e9, sameEntry.load(), sameSpirv.load(), secondsSince(started));
    if (listUpdated && digestsAdded != 0) std::fprintf(stderr, "[precompile] %s: %zu inputs digests added (format 3)\n", listPath, digestsAdded);
    // The shipped list is done for this cache once it ran to the end (a budget cut leaves it cold).
    if (shipped && cut.load() == 0) KeyList::MarkPrecompiled(entryDirectory, listHash, context);
}

}

void RunShaderPrecompile(const ShaderRecompiler::SpirvTarget& target) {
    static std::atomic<bool> ran{false}, delayed{false};
    // APS5_PRECOMPILE_DELAY_MS=<ms> (diagnostic): the first device creation waits that long, with or without
    // a precompile, so a run can separate the precompile's start-up delay from its other effects.
    if (const char* delay = std::getenv("APS5_PRECOMPILE_DELAY_MS"); delay != nullptr && *delay != '\0' && !delayed.exchange(true)) {
        const auto ms = std::strtoul(delay, nullptr, 10);
        if (ms > 0 && ms <= 600000) {
            std::fprintf(stderr, "[precompile] waiting %lu ms at the first device creation (APS5_PRECOMPILE_DELAY_MS)\n", ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    // APS5_PRECOMPILE=<list> precompiles that list at every launch; unset, the list shipped next to the
    // program is precompiled while the cache is cold for it; APS5_PRECOMPILE=0 turns both off.
    if (ran.exchange(true)) return;
    const auto choice = KeyList::ChooseList(std::getenv("APS5_PRECOMPILE"), executablePath().parent_path());
    if (choice.path.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    if (!ShaderRecompiler::ShaderDiskCache::Enabled()) {
        std::fprintf(stderr, "[precompile] the shader disk cache is off: nothing to fill\n");
        return;
    }
    // This runs in the device's constructor: whatever fails, the game goes on without the precompile.
    try {
        precompileList(target, choice.path, choice.shipped, started);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[precompile] stopped: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[precompile] stopped by an unknown exception\n");
    }
    // APS5_PRECOMPILE_EXIT=1: the headless use (a one-time setup) ends the process once the cache is filled.
    if (switchOn("APS5_PRECOMPILE_EXIT")) {
        std::fflush(stderr);
        std::_Exit(0);
    }
}

}
