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
bool writeTable(std::uint32_t* table, const std::array<PortSettings::TableWrite, 2>& writes) {
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
    plan = PortSettings::PlanNativeAspect(GetPortSettings_nid_no_patch());
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
    if (!writeTable(entries, PortSettings::TierTableWrites(plan))) {
        std::fprintf(stderr, "[aspect] cannot make the tier table writable: rendering the 16:9 tier (letterboxed)\n");
        std::fflush(stderr);
        return;
    }
    table = entries;
    singleton = reinterpret_cast<unsigned char*>(slot);
    std::fprintf(stderr, "[aspect] render %ux%u in the %s slot (tier table at %p, display half %s), aspect %.4f (%ux%u), display pointer slot %p\n",
        plan.renderWidth, plan.renderHeight, plan.slot->convar, static_cast<void*>(table), displayHalf ? "as expected" : "differs (not written)", plan.Aspect(),
        plan.aspectWidth, plan.aspectHeight, static_cast<void*>(singleton));
    std::fflush(stderr);
}

void NativeAspectPatch::follow() {
    if (singleton == nullptr || fieldRefused) return;
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
        std::snprintf(text, sizeof(text), ", native aspect not applied (%s)", searched ? "see the [aspect] line" : "video out not opened");
    } else {
        std::snprintf(text, sizeof(text), ", native aspect %.4f (%ux%u in %s): display mode %d, %llu/%llu flips native, %llu field writes%s", plan.Aspect(),
            plan.renderWidth, plan.renderHeight, plan.slot->convar, mode, static_cast<unsigned long long>(windowWideFlips), static_cast<unsigned long long>(windowFlips),
            static_cast<unsigned long long>(fieldWrites), fieldRefused ? ", field refused" : "");
    }
    windowFlips = 0;
    windowWideFlips = 0;
    return text;
}
