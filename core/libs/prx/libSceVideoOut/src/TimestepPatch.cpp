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
#include "prx/libSceVideoOut/include/NativeAspectPatch.hpp"

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
// FUN_140b380e0 stores the title's nominal frame step (1/refresh, seconds) every frame in a variable at PE VA
// 0x142f8b4a0, this far from the table (APS5_FRAME_PROBE reads it).
constexpr std::ptrdiff_t TitleStepFromRefreshTable = 0x142f8b4a0 - 0x142507b90;
// APS5_TIMESTEP_FIELD: the CRenderDisplay singleton pointer (DAT_142f1a988, NativeAspectPatch's display pointer
// slot) this far from the table, and its object's refresh (float, FUN_140b380e0 divides by it) and
// TargetFramesPerSecond enum (FUN_14091b3a0 copies an entry into the refresh only when the enum changes).
constexpr std::ptrdiff_t DisplaySlotFromRefreshTable = 0x142f1a988 - 0x142507b90;
constexpr std::size_t DisplayRefreshOffset = 0x28;
constexpr std::size_t DisplayFramesPerSecondOffset = 0x30;
// APS5_JOB_BUDGET: FUN_141481b10's float pair (Unlocked 60.0, the 60 FPS mode 59.94; DAT_142509a28) this far from
// the table, in the same read-only data.
constexpr std::ptrdiff_t JobBudgetFromRefreshTable = 0x142509a28 - 0x142507b90;

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

// The title's step is a 4-aligned variable: committed, writable memory (Windows; Linux trusts the distance).
bool titleVariable(std::uintptr_t at) {
    if (at % alignof(std::uint32_t) != 0) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info)) return false;
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & writable) != 0 && (info.Protect & PAGE_GUARD) == 0;
#else
    return true;
#endif
}

// APS5_FRAME_PROBE: the title's step by its distance from the refresh table; one line either way, nullptr
// when it cannot be read.
float* locateTitleStep(float* refreshTable, bool supported) {
    const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(refreshTable)) + TitleStepFromRefreshTable);
    const char* reason = !supported ? "unsupported game build" : refreshTable == nullptr ? "refresh table not found" : !titleVariable(at) ? "no writable variable at its address" : nullptr;
    if (reason != nullptr) {
        std::fprintf(stderr, "[timestep] frame probe: title step not readable (%s)\n", reason);
        return nullptr;
    }
    std::fprintf(stderr, "[timestep] frame probe: title step at %p (refresh table at %p)\n", reinterpret_cast<void*>(at), static_cast<void*>(refreshTable));
    return reinterpret_cast<float*>(at);
}

// One aligned 32-bit load of the step the title's logic thread stores each frame (this frame's or the last).
double loadStep(float* at) {
    const auto bits = std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(at)).load(std::memory_order_relaxed);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// APS5_FRAME_PROBE's part of the 10 s line: the title's step range, the game speed its steps add up to and
// the window's vblank ticks.
std::string titleStepDigest(const TitleStepWindow& steps, double seconds, std::uint64_t ticks) {
    char text[160];
    std::snprintf(text, sizeof(text), ", title step %.2f-%.2f Hz, title speed %.3fx, vblank %llu ticks", steps.MinHz(), steps.MaxHz(), steps.Speed(seconds), static_cast<unsigned long long>(ticks));
    std::string digest = text;
    if (steps.Unreadable() != 0) {
        std::snprintf(text, sizeof(text), ", %llu flips without a plausible title step", static_cast<unsigned long long>(steps.Unreadable()));
        digest += text;
    }
    return digest;
}

// The main image's writable data (writable) or read-only data (neither writable nor executable) holds
// [address, address + bytes) (NativeAspectPatch's check of the display slot).
bool inImageData(std::uintptr_t address, std::size_t bytes, bool writable) {
#ifdef _WIN32
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const auto flags = section.Characteristics;
        const bool kind = writable ? (flags & IMAGE_SCN_MEM_WRITE) != 0 : (flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0;
        if ((flags & IMAGE_SCN_MEM_READ) == 0 || !kind || section.VirtualAddress > nt->OptionalHeader.SizeOfImage) continue;
        const auto begin = reinterpret_cast<std::uintptr_t>(image + section.VirtualAddress);
        if (address >= begin && address + bytes <= begin + std::min<std::size_t>(section.Misc.VirtualSize, nt->OptionalHeader.SizeOfImage - section.VirtualAddress)) return true;
    }
    return false;
#elif defined(__linux__)
    struct Query {
        std::uintptr_t begin;
        std::uintptr_t end;
        bool writable;
        bool inside;
    } query{address, address + bytes, writable, false};
    dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
        auto& result = *static_cast<Query*>(data);
        if (image->dlpi_name != nullptr && image->dlpi_name[0] != '\0') return 0;
        for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
            const auto& header = image->dlpi_phdr[index];
            const auto begin = static_cast<std::uintptr_t>(image->dlpi_addr + header.p_vaddr);
            const bool kind = result.writable ? (header.p_flags & PF_W) != 0 : (header.p_flags & (PF_W | PF_X)) == 0;
            if (header.p_type == PT_LOAD && kind && result.begin >= begin && result.end <= begin + header.p_memsz) result.inside = true;
        }
        return 1;
    }, &query);
    return query.inside;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(writable);
    return false;
#endif
}

// The display object is 8-aligned and its refresh and enum (display+0x28..+0x37) lie in committed, writable
// memory (NativeAspectPatch's objectWritable; the first and the last word cover both pages they can span).
bool displayFieldsWritable(std::uintptr_t object) {
    return object != 0 && object % alignof(std::uint64_t) == 0 && titleVariable(object + DisplayRefreshOffset) && titleVariable(object + DisplayFramesPerSecondOffset + sizeof(std::int32_t));
}

// APS5_TIMESTEP_FIELD: the display pointer slot by its distance from the refresh table; nullptr and one line
// when the field cannot be followed.
std::uint64_t* locateDisplaySlot(float* refreshTable, bool supported) {
    const auto slot = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(refreshTable)) + DisplaySlotFromRefreshTable);
    const char* reason = !supported ? "unsupported game build" : refreshTable == nullptr ? "refresh table not found"
        : (slot % alignof(std::uint64_t) != 0 || !inImageData(slot, sizeof(std::uint64_t), true)) ? "the display pointer slot is not in the executable's writable data" : nullptr;
    if (reason == nullptr) return reinterpret_cast<std::uint64_t*>(slot);
    std::fprintf(stderr, "[timestep] refresh field: off, %s (APS5_TIMESTEP_FIELD=1)\n", reason);
    return nullptr;
}

// APS5_JOB_BUDGET: the job runner's float pair by its distance from the refresh table, 4-aligned and committed in
// the image's read-only data like the table; nullptr when it is not there.
float* locateJobBudget(float* refreshTable) {
    const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(refreshTable)) + JobBudgetFromRefreshTable);
    constexpr std::size_t bytes = 2 * sizeof(float);
    if (at % alignof(std::uint32_t) != 0 || !inImageData(at, bytes, false)) return nullptr;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0
        || at + bytes > reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) return nullptr;
#endif
    return reinterpret_cast<float*>(at);
}

// Both floats of the pair, one aligned 32-bit store each, the page open for writing only meanwhile (store()'s idiom).
bool storeJobBudget(float* at, float value) {
#ifdef _WIN32
    DWORD previous = 0;
    if (VirtualProtect(at, 2 * sizeof(float), PAGE_READWRITE, &previous) == FALSE) return false;
#elif defined(__linux__)
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    auto* base = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(at) & ~(page - 1));
    const auto bytes = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(at + 2) - reinterpret_cast<std::uintptr_t>(base));
    if (mprotect(base, bytes, PROT_READ | PROT_WRITE) != 0) return false;
#else
    return false;
#endif
    storeFloat(at, value);
    storeFloat(at + 1, value);
#ifdef _WIN32
    DWORD ignored = 0;
    VirtualProtect(at, 2 * sizeof(float), previous, &ignored);
#elif defined(__linux__)
    mprotect(base, bytes, PROT_READ);
#endif
    return true;
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
    probe = std::getenv("APS5_FRAME_PROBE") != nullptr;
    // APS5_TIMESTEP_FIELD writes the variable step's entries into the title's refresh field: only with that step.
    refreshField = std::getenv("APS5_TIMESTEP_FIELD") != nullptr;
    if (refreshField && !enabled) {
        std::fprintf(stderr, "[timestep] refresh field: off, Timestep is fixed (APS5_TIMESTEP_FIELD=1)\n");
        std::fflush(stderr);
        refreshField = false;
    }
    // APS5_JOB_BUDGET follows the vblank the variable step is capped at: only with that step.
    jobBudget = std::getenv("APS5_JOB_BUDGET") != nullptr;
    if (jobBudget && !enabled) {
        std::fprintf(stderr, "[timestep] job budget: left alone (Timestep is fixed) (APS5_JOB_BUDGET=1)\n");
        std::fflush(stderr);
        jobBudget = false;
    }
    // Without FpsLimit the render config picks the mode; the digest then assumes the 60 FPS one.
    mode30 = PortSettings::GameFramesPerSecond(settings) == 30;
}

void TimestepPatch::Flip(double vblankHz, std::uint64_t vblankCount) {
    if (!enabled && !profile && !probe) return;
    std::lock_guard lock(mutex);
    const auto now = nowNanos();
    // The 60 FPS mode flips once per vblank, the 30 FPS mode every second one.
    const double cap = vblankHz != 0.0 ? vblankHz : PortSettings::GameRefresh60;
    double step = 1.0 / (mode30 ? cap / 2.0 : cap);
    if ((enabled || probe) && !searched) {
        searched = true;
        const auto version = guestBuildVersion();
        const bool supported = PortSettings::BuildVersionSupported(version);
        const auto located = supported ? locateRefreshTable() : Located{};
        if (enabled) {
            table = located.table;
            pageBase = located.pageBase;
            pageBytes = located.pageBytes;
            floorHz = std::min(GetPortSettings_nid_no_patch().timestepMinHz, cap);
            pacer = std::make_unique<PortSettings::TimestepPacer>(floorHz, cap);
            if (!supported) std::fprintf(stderr, "[timestep] game build is not %s (gameversion.txt %s): the frame step stays fixed\n", PortSettings::SupportedBuildVersion, version.empty() ? "unreadable" : "differs");
            else if (table == nullptr) std::fprintf(stderr, "[timestep] refresh table not found in the executable: the frame step stays fixed\n");
            else std::fprintf(stderr, "[timestep] variable frame step %.2f to %.2f Hz (build %s, refresh table at %p, neighbourhood hash %016llx)\n", floorHz, cap, PortSettings::SupportedBuildVersion, static_cast<void*>(table), static_cast<unsigned long long>(located.neighbourhood));
        }
        // The probe only reads, so it also runs with the fixed step.
        if (probe) titleStep = locateTitleStep(located.table, supported);
        if (refreshField) displaySlot = locateDisplaySlot(table, supported);
        if (jobBudget) applyJobBudget(cap, supported);
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
        if (displaySlot != nullptr && table != nullptr) followRefreshField();
        // The digest reports the entry the title reads: entry 2 in its 30 FPS mode, entry 1 in the 60 FPS one.
        const auto rate = mode30 ? rate30 : rate60;
        step = 1.0 / rate;
        windowFloored += pacer->Floored() ? 1u : 0u;
        windowMinHz = windowFlips == 0 ? rate : std::min<double>(windowMinHz, rate);
        windowMaxHz = windowFlips == 0 ? rate : std::max<double>(windowMaxHz, rate);
    }
    if (titleStep != nullptr) titleSteps.Add(loadStep(titleStep));
    ++windowFlips;
    windowGameSeconds += step;
    if (windowStart == 0) {
        windowStart = now;
        windowVblank = vblankCount;
    }
    if (profile && now - windowStart >= 10'000'000'000ULL) report(now, vblankCount);
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

// APS5_TIMESTEP_FIELD, each flip after the entries are stored: the display object's refresh field gets the
// entry of the mode in effect (TitleRefreshField), since FUN_14091b3a0 copies one only when the mode changes.
// The title's logic thread may change mode between our read of the enum and our store: the field then holds
// the old mode's entry until the next flip (in mode 3, which is left alone, until the next mode change).
void TimestepPatch::followRefreshField() {
    if (fieldRefused) return;
    const auto object = static_cast<std::uintptr_t>(std::atomic_ref<std::uint64_t>(*displaySlot).load(std::memory_order_acquire));
    if (!displayFieldsWritable(object)) {
        ++windowFieldLeft;
        return;
    }
    auto* refresh = reinterpret_cast<float*>(object + DisplayRefreshOffset);
    const auto mode = std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(object + DisplayFramesPerSecondOffset)).load(std::memory_order_relaxed);
    const auto current = static_cast<float>(loadStep(refresh));
    const auto step = TitleRefreshField::Decide(mode, current, fieldKnown, stored60, stored30);
    if (step.action == TitleRefreshField::Action::Refuse) {
        fieldRefused = true;
        std::fprintf(stderr, "[timestep] refresh field: display+0x28 holds %g in mode %d (not a title refresh): left alone\n", current, mode);
        std::fflush(stderr);
        return;
    }
    windowModeChanges += step.titleRewrote ? 1u : 0u;
    windowFieldLeft += step.action == TitleRefreshField::Action::OtherMode ? 1u : 0u;
    fieldKnown = current;
    if (step.action != TitleRefreshField::Action::Write) return;
    storeFloat(refresh, step.value);
    fieldKnown = step.value;
    ++windowFieldWrites;
    if (fieldStarted) return;
    fieldStarted = true;
    std::fprintf(stderr, "[timestep] refresh field: the title's step follows the variable step (display object %p, field %p, mode %d) (APS5_TIMESTEP_FIELD=1)\n",
        reinterpret_cast<void*>(object), static_cast<void*>(refresh), mode);
    std::fflush(stderr);
}

// APS5_JOB_BUDGET, once when the table is looked for: the vblank (the variable step's cap) into the job runner's
// budget for Unlocked and the 60 FPS mode (TitleJobBudget); one line either way. The 20 and 30 FPS modes' budgets
// are immediates and stay.
void TimestepPatch::applyJobBudget(double cap, bool supported) {
    char reason[96] = "";
    auto* at = supported && table != nullptr && TitleJobBudget::Above(cap) ? locateJobBudget(table) : nullptr;
    if (!supported) std::snprintf(reason, sizeof(reason), "unsupported game build");
    else if (table == nullptr) std::snprintf(reason, sizeof(reason), "refresh table not found");
    else if (!TitleJobBudget::Above(cap)) std::snprintf(reason, sizeof(reason), "vblank %.2f Hz is not above the title's %.2f Hz", cap, TitleJobBudget::TitleHz);
    else if (at == nullptr) std::snprintf(reason, sizeof(reason), "its address is not in the executable's read-only data");
    if (at != nullptr) {
        const auto unlocked = static_cast<float>(loadStep(at));
        const auto mode60 = static_cast<float>(loadStep(at + 1));
        const auto budget = TitleJobBudget::Decide(cap, unlocked, mode60);
        if (budget.action == TitleJobBudget::Action::Refuse) std::snprintf(reason, sizeof(reason), "it holds %g / %g, not the title's 60 / 59.94", unlocked, mode60);
        else if (budget.action == TitleJobBudget::Action::Write && !storeJobBudget(at, budget.value)) std::snprintf(reason, sizeof(reason), "cannot make its page writable");
        else {
            jobBudgetAt = at;
            jobBudgetHz = budget.value;
            std::fprintf(stderr, "[timestep] job budget %.2f Hz (title %.2f / %.2f%s) (APS5_JOB_BUDGET=1)\n", budget.value, unlocked, mode60,
                budget.action == TitleJobBudget::Action::AlreadySet ? ", already set" : "");
            return;
        }
    }
    std::fprintf(stderr, "[timestep] job budget: left alone (%s) (APS5_JOB_BUDGET=1)\n", reason);
}

void TimestepPatch::report(std::uint64_t now, std::uint64_t vblankCount) {
    const double seconds = static_cast<double>(now - windowStart) / 1e9;
    const auto title = titleStep != nullptr ? titleStepDigest(titleSteps, seconds, vblankCount - std::min(windowVblank, vblankCount)) : std::string();
    char field[128] = "";
    if (displaySlot != nullptr && fieldRefused) std::snprintf(field, sizeof(field), ", refresh field refused");
    else if (displaySlot != nullptr) std::snprintf(field, sizeof(field), ", refresh field writes %llu (mode changes %llu, left alone %llu)", static_cast<unsigned long long>(windowFieldWrites),
        static_cast<unsigned long long>(windowModeChanges), static_cast<unsigned long long>(windowFieldLeft));
    // APS5_JOB_BUDGET: the floats should keep the budget (the title only reads them); shown only when they do not.
    char budget[48] = "";
    if (jobBudgetAt != nullptr) {
        const int rewritten = (static_cast<float>(loadStep(jobBudgetAt)) != jobBudgetHz ? 1 : 0) + (static_cast<float>(loadStep(jobBudgetAt + 1)) != jobBudgetHz ? 1 : 0);
        if (rewritten > 0) std::snprintf(budget, sizeof(budget), ", job budget rewritten %d", rewritten);
    }
    if (table != nullptr) {
        std::fprintf(stderr, "[timestep] 10 s: %llu flips (%.2f FPS), game speed %.3fx, variable step %.2f-%.2f Hz (%d FPS mode entry), %llu flips slower than the %.2f Hz floor%s%s%s%s\n",
            static_cast<unsigned long long>(windowFlips), windowFlips / seconds, windowGameSeconds / seconds, windowMinHz, windowMaxHz, mode30 ? 30 : 60,
            static_cast<unsigned long long>(windowFloored), floorHz, field, budget, title.c_str(), NativeAspectPatch::Get().Digest().c_str());
    } else {
        std::fprintf(stderr, "[timestep] 10 s: %llu flips (%.2f FPS), game speed %.3fx, fixed step (%d FPS mode)%s%s%s%s%s\n",
            static_cast<unsigned long long>(windowFlips), windowFlips / seconds, windowGameSeconds / seconds, mode30 ? 30 : 60,
            enabled ? ", refresh table not found" : "", field, budget, title.c_str(), NativeAspectPatch::Get().Digest().c_str());
    }
    std::fflush(stderr);
    windowStart = now;
    windowVblank = vblankCount;
    windowFlips = 0;
    windowFloored = 0;
    windowGameSeconds = 0.0;
    titleSteps = TitleStepWindow{};
    windowFieldWrites = 0;
    windowModeChanges = 0;
    windowFieldLeft = 0;
}
