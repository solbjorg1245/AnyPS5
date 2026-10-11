#include "prx/libSceVideoOut/include/TimestepPatch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sstream>

#include "prx/libc/include/General.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#endif

namespace {

// 59.94f, 59.94f, 29.97f, 19.98f: the refresh table (PE VA 0x142507b90 in 2025-10-15.877562); the four
// floats occur once in the image.
constexpr std::array<unsigned char, 16> RefreshTableSignature{0x8f, 0xc2, 0x6f, 0x42, 0x8f, 0xc2, 0x6f, 0x42, 0x8f, 0xc2, 0xef, 0x41, 0x0a, 0xd7, 0x9f, 0x41};
constexpr std::size_t Entry60 = 1;
constexpr std::size_t Entry30 = 2;

std::uint64_t nowNanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Search {
    unsigned char* hit = nullptr;
    unsigned char* regionBegin = nullptr;
    unsigned char* regionEnd = nullptr;
    std::size_t hits = 0;

    void scan(unsigned char* begin, std::size_t size) {
        auto* end = begin + size;
        for (auto* at = begin; (at = std::search(at, end, RefreshTableSignature.begin(), RefreshTableSignature.end())) != end; ++at) {
            hit = at;
            regionBegin = begin;
            regionEnd = end;
            ++hits;
        }
    }
};

struct Located {
    float* table = nullptr;
    void* pageBase = nullptr;
    std::size_t pageBytes = 0;
    std::uint64_t neighbourhood = 0;
};

// FNV-1a of the matched bytes and up to 32 bytes either side (inside the section); logged so a build
// can be told apart from its neighbours' in a report.
std::uint64_t hashNeighbourhood(const Search& search) {
    const auto* begin = std::max(search.regionBegin, search.hit - 32);
    const auto* end = std::min(search.regionEnd, search.hit + RefreshTableSignature.size() + 32);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto* at = begin; at < end; ++at) hash = (hash ^ *at) * 1099511628211ULL;
    return hash;
}

// The build named by the title's gameversion.txt, "" when it cannot be read.
std::string guestBuildVersion() {
    try {
        std::ifstream file(ResolvePath_nid_no_patch("/app0/gameversion.txt"), std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    } catch (...) {
        return {};
    }
}

// The table in the main image's read-only data; nullptr unless it occurs exactly once and is 4-aligned.
// The page stays read-only here (store() opens it for each write).
Located locateRefreshTable() {
    Search search;
#ifdef _WIN32
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const auto flags = section.Characteristics;
        if ((flags & IMAGE_SCN_MEM_READ) == 0 || (flags & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != 0) continue;
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage) continue;
        const auto size = std::min<std::size_t>(std::min(section.Misc.VirtualSize, section.SizeOfRawData), nt->OptionalHeader.SizeOfImage - section.VirtualAddress);
        search.scan(image + section.VirtualAddress, size);
    }
    if (search.hits != 1) return {};
#elif defined(__linux__)
    dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
        auto& result = *static_cast<Search*>(data);
        if (image->dlpi_name != nullptr && image->dlpi_name[0] != '\0') return 0;
        for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
            const auto& header = image->dlpi_phdr[index];
            if (header.p_type != PT_LOAD || (header.p_flags & (PF_X | PF_W)) != 0) continue;
            result.scan(reinterpret_cast<unsigned char*>(image->dlpi_addr + header.p_vaddr), static_cast<std::size_t>(header.p_filesz));
        }
        return 1;
    }, &search);
    if (search.hits != 1) return {};
#else
    return {};
#endif
    if (reinterpret_cast<std::uintptr_t>(search.hit) % alignof(std::uint32_t) != 0) return {};
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::uintptr_t>(info.dwPageSize);
#else
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
#endif
    const auto first = reinterpret_cast<std::uintptr_t>(search.hit) & ~(page - 1);
    const auto last = (reinterpret_cast<std::uintptr_t>(search.hit) + RefreshTableSignature.size() + page - 1) & ~(page - 1);
    Located located;
    located.table = reinterpret_cast<float*>(search.hit);
    located.pageBase = reinterpret_cast<void*>(first);
    located.pageBytes = static_cast<std::size_t>(last - first);
    located.neighbourhood = hashNeighbourhood(search);
    return located;
}

// One aligned 32-bit store (never torn on the platforms we run on).
void storeFloat(float* at, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(at)).store(bits, std::memory_order_release);
}

}

TimestepPatch& TimestepPatch::Get() {
    static TimestepPatch instance;
    return instance;
}

TimestepPatch::TimestepPatch() {
    const auto& settings = GetPortSettings_nid_no_patch();
    enabled = settings.variableTimestep;
    profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    // Without FpsLimit the render config picks the mode; the digest then assumes the 60 FPS one.
    mode30 = PortSettings::GameFramesPerSecond(settings) == 30;
}

void TimestepPatch::Flip(double vblankHz) {
    if (!enabled && !profile) return;
    std::lock_guard lock(mutex);
    const auto now = nowNanos();
    // The 60 FPS mode flips once per vblank, the 30 FPS mode every second one.
    const double cap = vblankHz != 0.0 ? vblankHz : PortSettings::GameRefresh60;
    double step = 1.0 / (mode30 ? cap / 2.0 : cap);
    if (enabled && !searched) {
        searched = true;
        const auto version = guestBuildVersion();
        const bool supported = PortSettings::BuildVersionSupported(version);
        const auto located = supported ? locateRefreshTable() : Located{};
        table = located.table;
        pageBase = located.pageBase;
        pageBytes = located.pageBytes;
        floorHz = std::min(GetPortSettings_nid_no_patch().timestepMinHz, cap);
        pacer = std::make_unique<PortSettings::TimestepPacer>(floorHz, cap);
        if (!supported) std::fprintf(stderr, "[timestep] game build is not %s (gameversion.txt %s): the frame step stays fixed\n", PortSettings::SupportedBuildVersion, version.empty() ? "unreadable" : "differs");
        else if (table == nullptr) std::fprintf(stderr, "[timestep] refresh table not found in the executable: the frame step stays fixed\n");
        else std::fprintf(stderr, "[timestep] variable frame step %.2f to %.2f Hz (build %s, refresh table at %p, neighbourhood hash %016llx)\n", floorHz, cap, PortSettings::SupportedBuildVersion, static_cast<void*>(table), static_cast<unsigned long long>(located.neighbourhood));
        std::fflush(stderr);
    }
    if (table != nullptr) {
        const double hz = pacer->Flip(now);
        const auto rate60 = static_cast<float>(std::min(hz, cap));
        const auto rate30 = static_cast<float>(std::min(hz, cap / 2.0));
        if (!store(rate60, rate30)) {
            std::fprintf(stderr, "[timestep] cannot make the refresh table writable: the frame step stays fixed\n");
            std::fflush(stderr);
            table = nullptr;
        }
        // The digest reports the entry the title reads: entry 2 in its 30 FPS mode, entry 1 in the 60 FPS one.
        const auto rate = mode30 ? rate30 : rate60;
        step = 1.0 / rate;
        windowFloored += pacer->Floored() ? 1u : 0u;
        windowMinHz = windowFlips == 0 ? rate : std::min<double>(windowMinHz, rate);
        windowMaxHz = windowFlips == 0 ? rate : std::max<double>(windowMaxHz, rate);
    }
    ++windowFlips;
    windowGameSeconds += step;
    if (windowStart == 0) windowStart = now;
    if (profile && now - windowStart >= 10'000'000'000ULL) report(now);
}

bool TimestepPatch::store(float rate60, float rate30) {
    // Changes below 0.01 Hz are not worth opening the page for.
    if (pageProtection != 0 && std::abs(rate60 - stored60) < 0.01f && std::abs(rate30 - stored30) < 0.01f) return true;
#ifdef _WIN32
    DWORD previous = 0;
    if (VirtualProtect(pageBase, pageBytes, PAGE_READWRITE, &previous) == FALSE) return false;
#elif defined(__linux__)
    // The table sits in a read-only (never executable, never writable) segment: read-only again afterwards.
    const unsigned long previous = PROT_READ;
    if (mprotect(pageBase, pageBytes, PROT_READ | PROT_WRITE) != 0) return false;
#else
    return false;
#endif
    // The 30 FPS mode's entry first, the 60 FPS mode's last.
    storeFloat(table + Entry30, rate30);
    storeFloat(table + Entry60, rate60);
    stored60 = rate60;
    stored30 = rate30;
    pageProtection = 1;
#ifdef _WIN32
    DWORD ignored = 0;
    VirtualProtect(pageBase, pageBytes, previous, &ignored);
#elif defined(__linux__)
    mprotect(pageBase, pageBytes, static_cast<int>(previous));
#endif
    return true;
}

void TimestepPatch::report(std::uint64_t now) {
    const double seconds = static_cast<double>(now - windowStart) / 1e9;
    if (table != nullptr) {
        std::fprintf(stderr, "[timestep] 10 s: %llu flips (%.2f FPS), game speed %.3fx, variable step %.2f-%.2f Hz (%d FPS mode entry), %llu flips slower than the %.2f Hz floor\n",
            static_cast<unsigned long long>(windowFlips), windowFlips / seconds, windowGameSeconds / seconds, windowMinHz, windowMaxHz, mode30 ? 30 : 60,
            static_cast<unsigned long long>(windowFloored), floorHz);
    } else {
        std::fprintf(stderr, "[timestep] 10 s: %llu flips (%.2f FPS), game speed %.3fx, fixed step (%d FPS mode)%s\n",
            static_cast<unsigned long long>(windowFlips), windowFlips / seconds, windowGameSeconds / seconds, mode30 ? 30 : 60,
            enabled ? ", refresh table not found" : "");
    }
    std::fflush(stderr);
    windowStart = now;
    windowFlips = 0;
    windowFloored = 0;
    windowGameSeconds = 0.0;
}
