#include "prx/libSceVideoOut/include/TimestepPatch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    std::size_t hits = 0;

    void scan(unsigned char* begin, std::size_t size) {
        auto* end = begin + size;
        for (auto* at = begin; (at = std::search(at, end, RefreshTableSignature.begin(), RefreshTableSignature.end())) != end; ++at) {
            hit = at;
            ++hits;
        }
    }
};

// The table in the main image's read-only data, made writable; nullptr unless it occurs exactly once.
float* locateRefreshTable() {
    Search search;
#ifdef _WIN32
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const auto flags = section.Characteristics;
        if ((flags & IMAGE_SCN_MEM_READ) == 0 || (flags & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != 0) continue;
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage) continue;
        const auto size = std::min<std::size_t>(std::min(section.Misc.VirtualSize, section.SizeOfRawData), nt->OptionalHeader.SizeOfImage - section.VirtualAddress);
        search.scan(image + section.VirtualAddress, size);
    }
    if (search.hits != 1) return nullptr;
    DWORD previous = 0;
    if (VirtualProtect(search.hit, RefreshTableSignature.size(), PAGE_READWRITE, &previous) == FALSE) return nullptr;
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
    if (search.hits != 1) return nullptr;
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto first = reinterpret_cast<std::uintptr_t>(search.hit) & ~(page - 1);
    const auto last = (reinterpret_cast<std::uintptr_t>(search.hit) + RefreshTableSignature.size() + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(first), last - first, PROT_READ | PROT_WRITE) != 0) return nullptr;
#else
    return nullptr;
#endif
    return reinterpret_cast<float*>(search.hit);
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
        table = locateRefreshTable();
        floorHz = std::min(GetPortSettings_nid_no_patch().timestepMinHz, cap);
        pacer = std::make_unique<PortSettings::TimestepPacer>(floorHz, cap);
        if (table == nullptr) std::fprintf(stderr, "[timestep] refresh table not found in the executable: the frame step stays fixed\n");
        else std::fprintf(stderr, "[timestep] variable frame step %.2f to %.2f Hz (refresh table at %p)\n", floorHz, cap, static_cast<void*>(table));
        std::fflush(stderr);
    }
    if (table != nullptr) {
        const double hz = pacer->Flip(now);
        const auto rate60 = static_cast<float>(std::min(hz, cap));
        const auto rate30 = static_cast<float>(std::min(hz, cap / 2.0));
        std::memcpy(table + Entry60, &rate60, sizeof(rate60));
        std::memcpy(table + Entry30, &rate30, sizeof(rate30));
        step = 1.0 / (mode30 ? rate30 : rate60);
        windowFloored += pacer->Floored() ? 1u : 0u;
        windowMinHz = windowFlips == 0 ? rate60 : std::min<double>(windowMinHz, rate60);
        windowMaxHz = windowFlips == 0 ? rate60 : std::max<double>(windowMaxHz, rate60);
    }
    ++windowFlips;
    windowGameSeconds += step;
    if (windowStart == 0) windowStart = now;
    if (profile && now - windowStart >= 10'000'000'000ULL) report(now);
}

void TimestepPatch::report(std::uint64_t now) {
    const double seconds = static_cast<double>(now - windowStart) / 1e9;
    if (table != nullptr) {
        std::fprintf(stderr, "[timestep] 10 s: %llu flips (%.2f FPS), game speed %.3fx, variable step %.2f-%.2f Hz (60 FPS mode entry), %llu flips slower than the %.2f Hz floor\n",
            static_cast<unsigned long long>(windowFlips), windowFlips / seconds, windowGameSeconds / seconds, windowMinHz, windowMaxHz,
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
