#include "prx/libSceVideoOut/include/NativeAspectPatch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "prx/libc/include/General.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"

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

constexpr std::size_t SignatureBytes = PortSettings::TierTableSignatureEntries * sizeof(std::uint32_t);

std::array<unsigned char, SignatureBytes> signature() {
    std::array<unsigned char, SignatureBytes> bytes{};
    std::memcpy(bytes.data(), PortSettings::TierTable.data(), bytes.size());
    return bytes;
}

struct Range {
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct ImageSearch {
    std::array<unsigned char, SignatureBytes> pattern = signature();
    unsigned char* hit = nullptr;
    std::size_t hits = 0;
    std::uintptr_t hitRegionEnd = 0;
    std::vector<Range> writable;

    void scan(unsigned char* begin, std::size_t size) {
        auto* end = begin + size;
        for (auto* at = begin; (at = std::search(at, end, pattern.begin(), pattern.end())) != end; ++at) {
            hit = at;
            hitRegionEnd = reinterpret_cast<std::uintptr_t>(end);
            ++hits;
        }
    }

    bool inWritable(std::uintptr_t address, std::size_t bytes) const {
        return std::any_of(writable.begin(), writable.end(), [&](const Range& range) { return address >= range.begin && address + bytes <= range.end; });
    }
};

// Read-only sections are searched for the table, writable ones bound the singleton slot.
void scanMainImage(ImageSearch& search) {
#ifdef _WIN32
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const auto flags = section.Characteristics;
        if ((flags & IMAGE_SCN_MEM_READ) == 0 || section.VirtualAddress > nt->OptionalHeader.SizeOfImage) continue;
        const auto room = nt->OptionalHeader.SizeOfImage - section.VirtualAddress;
        if ((flags & IMAGE_SCN_MEM_WRITE) != 0) {
            const auto begin = reinterpret_cast<std::uintptr_t>(image + section.VirtualAddress);
            search.writable.push_back({begin, begin + std::min<std::size_t>(section.Misc.VirtualSize, room)});
            continue;
        }
        if ((flags & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
        search.scan(image + section.VirtualAddress, std::min<std::size_t>(std::min(section.Misc.VirtualSize, section.SizeOfRawData), room));
    }
#elif defined(__linux__)
    dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
        auto& result = *static_cast<ImageSearch*>(data);
        if (image->dlpi_name != nullptr && image->dlpi_name[0] != '\0') return 0;
        for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
            const auto& header = image->dlpi_phdr[index];
            if (header.p_type != PT_LOAD) continue;
            const auto begin = static_cast<std::uintptr_t>(image->dlpi_addr + header.p_vaddr);
            if ((header.p_flags & PF_W) != 0) result.writable.push_back({begin, begin + static_cast<std::uintptr_t>(header.p_memsz)});
            else if ((header.p_flags & PF_X) == 0) result.scan(reinterpret_cast<unsigned char*>(begin), static_cast<std::size_t>(header.p_filesz));
        }
        return 1;
    }, &search);
#else
    static_cast<void>(search);
#endif
}

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

// Stores the entries with the table's page writable only for the stores.
bool writeTable(std::uint32_t* table, const std::vector<PortSettings::TableWrite>& writes) {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::uintptr_t>(info.dwPageSize);
#elif defined(__linux__)
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
#else
    return false;
#endif
    const auto first = reinterpret_cast<std::uintptr_t>(table) & ~(page - 1);
    const auto last = (reinterpret_cast<std::uintptr_t>(table + PortSettings::TierTable.size()) + page - 1) & ~(page - 1);
    auto* base = reinterpret_cast<void*>(first);
    const auto bytes = static_cast<std::size_t>(last - first);
#ifdef _WIN32
    DWORD previous = 0;
    if (VirtualProtect(base, bytes, PAGE_READWRITE, &previous) == FALSE) return false;
#elif defined(__linux__)
    if (mprotect(base, bytes, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    for (const auto& write : writes) std::atomic_ref<std::uint32_t>(table[write.entry]).store(write.value, std::memory_order_release);
#ifdef _WIN32
    DWORD ignored = 0;
    VirtualProtect(base, bytes, previous, &ignored);
#elif defined(__linux__)
    mprotect(base, bytes, PROT_READ);
#endif
    return true;
}

// Native aspect part 2: the code of the supported build (PE VAs, found at the same distance from the
// tier table as in the image). CRenderDisplay::SetMode FUN_14091a600 loads a mode's render and display
// size pairs with `vmovsd xmm0/xmm1, [rip+disp32]` (C5 FB 10 05/0D) from constants other code shares;
// CRenderDisplay::Init FUN_14091ad80 creates its two 3840x2160 display buffers (mov edi/esi) and their
// 1920x1080 views (mov edx/ecx); the display-size getters FUN_1409956d0/FUN_140995700 (mov ecx/eax).
constexpr std::uintptr_t TierTableVa = 0x14251eb48;

struct LoadSite {
    std::uintptr_t va;
    unsigned char modrm;
    std::uintptr_t constant;
    std::uint32_t width;
    std::uint32_t height;
};

struct ImmediateSite {
    std::uintptr_t va;
    unsigned char opcode;
    std::uint32_t value;
    bool height;
};

struct TargetSites {
    LoadSite shared;
    LoadSite display;
    std::array<ImmediateSite, 6> immediates;
};

// WantsPerf (1) and WantsHigh (3); WantsSuperHigh (4) loads one pair for both (its `shared` site, the
// display one unused) and has WantsHigh's 4K buffers.
constexpr std::array<ImmediateSite, 6> Display4kSites{{{0x14091af74, 0xbf, 3840, false}, {0x14091af79, 0xbe, 2160, true}, {0x14091b061, 0xbf, 3840, false},
    {0x14091b066, 0xbe, 2160, true}, {0x1409956d7, 0xb9, 3840, false}, {0x140995707, 0xb9, 2160, true}}};
constexpr TargetSites PerfSites{{0x14091a663, 0x05, 0x142509230, 1600, 900}, {0x14091a65b, 0x0d, 0x142507810, 1920, 1080},
    {{{0x14091b00e, 0xba, 1920, false}, {0x14091b013, 0xb9, 1080, true}, {0x14091b0b4, 0xba, 1920, false}, {0x14091b0b9, 0xb9, 1080, true},
      {0x1409956e8, 0xb8, 1920, false}, {0x140995718, 0xb8, 1080, true}}}};
constexpr TargetSites HighSites{{0x14091a67f, 0x05, 0x142509550, 2560, 1440}, {0x14091a677, 0x0d, 0x142504830, 3840, 2160}, Display4kSites};
constexpr TargetSites SuperHighSites{{0x14091a66d, 0x05, 0x142504830, 3840, 2160}, {}, Display4kSites};

std::uint32_t readWord(std::uintptr_t address) {
    std::uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

// The load's opcode, its target the expected constant, and the constant the expected pair.
bool loadMatches(std::intptr_t delta, const LoadSite& site) {
    const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site.va) + delta);
    const auto* bytes = reinterpret_cast<const unsigned char*>(at);
    if (bytes[0] != 0xc5 || bytes[1] != 0xfb || bytes[2] != 0x10 || bytes[3] != site.modrm) return false;
    const auto disp = static_cast<std::int32_t>(readWord(at + 4));
    const auto target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(at + 8) + disp);
    if (target != static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site.constant) + delta)) return false;
    return readWord(target) == site.width && readWord(target + 4) == site.height;
}

bool immediateMatches(std::intptr_t delta, const ImmediateSite& site) {
    const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site.va) + delta);
    return *reinterpret_cast<const unsigned char*>(at) == site.opcode && readWord(at + 1) == site.value;
}

// A committed page for the size pairs within a rel32 of `anchor` (below the image, else above it).
std::uint32_t* allocateNear(std::uintptr_t anchor) {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = info.dwAllocationGranularity;
    constexpr std::uintptr_t reach = 0x70000000;
    const auto tryAt = [&](std::uintptr_t candidate) -> std::uint32_t* {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(candidate), &region, sizeof(region)) != sizeof(region) || region.State != MEM_FREE) return nullptr;
        return static_cast<std::uint32_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    };
    for (auto candidate = (anchor & ~(granularity - 1)) - granularity; candidate > granularity && anchor - candidate < reach; candidate -= granularity) {
        if (auto* page = tryAt(candidate)) return page;
    }
    for (auto candidate = (anchor & ~(granularity - 1)) + granularity; candidate - anchor < reach; candidate += granularity) {
        if (auto* page = tryAt(candidate)) return page;
    }
#else
    static_cast<void>(anchor);
#endif
    return nullptr;
}

// Writes `bytes` over code at `address` with its page writable only for the store.
bool writeCode(std::uintptr_t address, const void* bytes, std::size_t size) {
#ifdef _WIN32
    DWORD previous = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &previous) == FALSE) return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), size);
    return true;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(size);
    return false;
#endif
}

bool pointLoad(std::intptr_t delta, const LoadSite& site, const std::uint32_t* pair) {
    const auto at = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site.va) + delta);
    const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(pair) - static_cast<std::intptr_t>(at + 8));
    return writeCode(at + 4, &disp, sizeof(disp));
}

bool setImmediate(std::intptr_t delta, const ImmediateSite& site, std::uint32_t value) {
    return writeCode(static_cast<std::uintptr_t>(static_cast<std::intptr_t>(site.va) + delta) + 1, &value, sizeof(value));
}

// The display object is 8-aligned and its first 0x40 bytes are committed, writable memory (Windows; Linux
// trusts the slot).
bool objectWritable(std::uintptr_t object) {
    if (object == 0 || object % alignof(std::uint64_t) != 0) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(object), &info, sizeof(info)) != sizeof(info)) return false;
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (info.State != MEM_COMMIT || (info.Protect & writable) == 0 || (info.Protect & PAGE_GUARD) != 0) return false;
    return object + PortSettings::DisplayModeOffset + sizeof(std::int32_t) <= reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
#else
    return object != 0;
#endif
}

}

NativeAspectPatch& NativeAspectPatch::Get() {
    static NativeAspectPatch instance;
    return instance;
}

NativeAspectPatch::NativeAspectPatch() {
    plan = PortSettings::PlanTierRewrite(GetPortSettings_nid_no_patch());
    if (plan.Active()) wideBits = PortSettings::AspectBits(plan.Aspect());
}

void NativeAspectPatch::Open() {
    if (!plan.Active()) return;
    std::lock_guard lock(mutex);
    if (!searched) locate();
    follow();
}

void NativeAspectPatch::Flip() {
    if (!plan.Active()) return;
    std::lock_guard lock(mutex);
    if (!searched) locate();
    follow();
    ++windowFlips;
    if (presenting) ++windowWideFlips;
}

void NativeAspectPatch::locate() {
    searched = true;
    const auto version = guestBuildVersion();
    if (!PortSettings::BuildVersionSupported(version)) {
        std::fprintf(stderr, "[aspect] game build is not %s (gameversion.txt %s): rendering the 16:9 tier (letterboxed)\n", PortSettings::SupportedBuildVersion, version.empty() ? "unreadable" : "differs");
        std::fflush(stderr);
        return;
    }
    ImageSearch search;
    scanMainImage(search);
    if (search.hits != 1 || reinterpret_cast<std::uintptr_t>(search.hit) % alignof(std::uint32_t) != 0) {
        std::fprintf(stderr, "[aspect] tier table found %zu times in the executable (need one, 4-aligned): rendering the 16:9 tier (letterboxed)\n", search.hits);
        std::fflush(stderr);
        return;
    }
    const auto hit = reinterpret_cast<std::uintptr_t>(search.hit);
    const bool displayHalf = hit + PortSettings::TierTable.size() * sizeof(std::uint32_t) <= search.hitRegionEnd
        && std::memcmp(search.hit + SignatureBytes, PortSettings::TierTable.data() + PortSettings::TierTableSignatureEntries, SignatureBytes) == 0;
    const auto slot = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(hit) + PortSettings::DisplaySingletonFromTierTable);
    if (slot % alignof(std::uint64_t) != 0 || !search.inWritable(slot, sizeof(std::uint64_t))) {
        std::fprintf(stderr, "[aspect] display pointer slot %p is not in the executable's writable data: rendering the 16:9 tier (letterboxed)\n", reinterpret_cast<void*>(slot));
        std::fflush(stderr);
        return;
    }
    auto* entries = reinterpret_cast<std::uint32_t*>(search.hit);
    const auto delta = static_cast<std::intptr_t>(hit) - static_cast<std::intptr_t>(TierTableVa);
    bool display = false;
    if (plan.SharedNative() || plan.DisplayNative()) patchTargets(delta, display);
    if (!writeTable(entries, PortSettings::TierTableWrites(plan, display))) {
        std::fprintf(stderr, "[aspect] cannot make the tier table writable: rendering the 16:9 tier (letterboxed)\n");
        std::fflush(stderr);
        return;
    }
    table = entries;
    singleton = reinterpret_cast<unsigned char*>(slot);
    std::fprintf(stderr, "[aspect] render %ux%u in the %s slot (tier table at %p, display half %s), aspect %.4f (%ux%u), display pointer slot %p%s\n",
        plan.renderWidth, plan.renderHeight, plan.slot->convar, static_cast<void*>(table), displayHalf ? "as expected" : "differs (not written)", plan.Aspect(),
        plan.aspectWidth, plan.aspectHeight, static_cast<void*>(singleton), plan.uiOnly ? " (Resolution's 4K UI: aspect field and presentation left alone)" : "");
    std::fflush(stderr);
}

// Part 2: checks every site of the slot's halves, then points SetMode's loads at the planned pairs and
// rewrites Init's display buffer sizes and the getters. `display`: the display half was written (the
// table's display entries follow it). A half with any site differing writes nothing.
void NativeAspectPatch::patchTargets(std::intptr_t delta, bool& display) {
    const auto mode = plan.slot->mode;
    const auto& sites = mode == 1 ? PerfSites : mode == 3 ? HighSites : SuperHighSites;
    const bool joint = mode == 4;
    const auto refuse = [&](const char* half, std::uintptr_t va) {
        std::fprintf(stderr, "[aspect] %s: the code at 0x%llx is not the expected build's: kept at the slot's 16:9 size (part 1)\n", half, static_cast<unsigned long long>(va));
        std::fflush(stderr);
    };
    bool shared = plan.SharedNative() && loadMatches(delta, sites.shared);
    if (plan.SharedNative() && !shared) refuse(joint ? "shared render targets and display buffer" : "shared render targets", sites.shared.va);
    bool wantDisplay = plan.DisplayNative() && (!joint || shared);
    if (wantDisplay && !joint && !loadMatches(delta, sites.display)) {
        refuse("display buffer", sites.display.va);
        wantDisplay = false;
    }
    for (const auto& site : sites.immediates) {
        if (wantDisplay && !immediateMatches(delta, site)) {
            refuse(joint ? "shared render targets and display buffer" : "display buffer", site.va);
            wantDisplay = false;
            if (joint) shared = false;
        }
    }
    if (!shared && !wantDisplay) return;
    const auto anchor = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(sites.shared.va) + delta);
    auto* pairs = allocateNear(anchor);
    if (pairs == nullptr) {
        std::fprintf(stderr, "[aspect] no page within reach of CRenderDisplay::SetMode for the size pairs: shared targets and display buffer kept at 16:9 (part 1)\n");
        std::fflush(stderr);
        return;
    }
    pairs[0] = plan.SharedNative() ? plan.sharedWidth : plan.slot->renderWidth;
    pairs[1] = plan.SharedNative() ? plan.sharedHeight : plan.slot->renderHeight;
    pairs[2] = plan.displayWidth;
    pairs[3] = plan.displayHeight;
#ifdef _WIN32
    DWORD previous = 0;
    VirtualProtect(pairs, 4 * sizeof(std::uint32_t), PAGE_READONLY, &previous);
#endif
    if (shared) sharedApplied = pointLoad(delta, sites.shared, pairs);
    if (wantDisplay) {
        bool written = joint ? sharedApplied : pointLoad(delta, sites.display, pairs + 2);
        for (const auto& site : sites.immediates) written = written && setImmediate(delta, site, site.height ? plan.displayHeight : plan.displayWidth);
        displayApplied = written;
    }
    display = displayApplied;
    std::fprintf(stderr, "[aspect] part 2 in %s (experimental): shared render targets %ux%u (%s), display buffer %ux%u (%s); size pairs at %p\n", plan.slot->convar,
        sharedApplied ? pairs[0] : plan.slot->renderWidth, sharedApplied ? pairs[1] : plan.slot->renderHeight, sharedApplied ? "SetMode's load patched, APS5_NATIVE_ASPECT_RENDER: post effects break, t557/t558/t560" : "the slot's own",
        displayApplied ? plan.displayWidth : plan.slot->displayWidth, displayApplied ? plan.displayHeight : plan.slot->displayHeight,
        displayApplied ? "SetMode, Init's buffers, getters and table patched, APS5_NATIVE_ASPECT_UI: the bottom HUD is cut off wider than 16:9" : "the slot's own, presenter un-squeezes", static_cast<void*>(pairs));
    std::fflush(stderr);
}

void NativeAspectPatch::follow() {
    if (plan.uiOnly || singleton == nullptr || fieldRefused) return;
    const auto object = static_cast<std::uintptr_t>(std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(singleton)).load(std::memory_order_acquire));
    if (object == 0 || !objectWritable(object)) return;
    auto& aspectField = *reinterpret_cast<std::uint32_t*>(object + PortSettings::DisplayAspectOffset);
    const auto current = std::atomic_ref<std::uint32_t>(aspectField).load(std::memory_order_relaxed);
    const auto currentMode = std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(object + PortSettings::DisplayModeOffset)).load(std::memory_order_relaxed);
    const auto wanted = PortSettings::DisplayAspectBits(current, currentMode, plan);
    if (!wanted) {
        fieldRefused = true;
        if (presenting) AgcDriver::SetPresentationAspect_nid_postfix(0, 0);
        presenting = false;
        std::fprintf(stderr, "[aspect] display aspect field holds %08x in mode %d (neither 16:9 nor %08x): left alone, presenting the buffer's own aspect\n", current, currentMode, wideBits);
        std::fflush(stderr);
        return;
    }
    if (*wanted != current) {
        std::atomic_ref<std::uint32_t>(aspectField).store(*wanted, std::memory_order_relaxed);
        ++fieldWrites;
    }
    const bool wide = *wanted == wideBits;
    if (wide != presenting || currentMode != mode) {
        AgcDriver::SetPresentationAspect_nid_postfix(wide ? plan.aspectWidth : 0, wide ? plan.aspectHeight : 0);
        std::fprintf(stderr, "[aspect] display mode %d: camera aspect %s, presented at %s\n", currentMode, wide ? "native" : "16:9",
            wide ? (std::to_string(plan.aspectWidth) + "x" + std::to_string(plan.aspectHeight)).c_str() : "the buffer's own aspect");
        std::fflush(stderr);
    }
    presenting = wide;
    mode = currentMode;
}

std::string NativeAspectPatch::Digest() {
    if (!plan.Active()) return {};
    std::lock_guard lock(mutex);
    char text[256];
    if (table == nullptr) {
        std::snprintf(text, sizeof(text), ", %s not applied (%s)", plan.uiOnly ? "4K UI" : "native aspect", searched ? "see the [aspect] line" : "video out not opened");
    } else if (plan.uiOnly) {
        std::snprintf(text, sizeof(text), ", 4K UI: %ux%u in %s, display buffer %ux%u", plan.renderWidth, plan.renderHeight, plan.slot->convar, plan.slot->displayWidth, plan.slot->displayHeight);
    } else {
        std::snprintf(text, sizeof(text), ", native aspect %.4f (%ux%u in %s): display mode %d, %llu/%llu flips native, %llu field writes%s; shared targets %s, display buffer %s", plan.Aspect(),
            plan.renderWidth, plan.renderHeight, plan.slot->convar, mode, static_cast<unsigned long long>(windowWideFlips), static_cast<unsigned long long>(windowFlips),
            static_cast<unsigned long long>(fieldWrites), fieldRefused ? ", field refused" : "", sharedApplied ? "native" : "16:9", displayApplied ? "native" : "16:9");
    }
    windowFlips = 0;
    windowWideFlips = 0;
    return text;
}
