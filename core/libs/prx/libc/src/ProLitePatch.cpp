#include "prx/libc/include/ProLitePatch.hpp"
#include "prx/libc/include/PortSettings.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> active{false};

#ifdef _WIN32
// The running address of a PE VA in the main image's code: the image has a dynamic base, so the VA is
// taken relative to the header's ImageBase; null when it falls outside an executable section.
unsigned char* mapCode(std::uintptr_t va, std::size_t bytes) {
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    if (va < nt->OptionalHeader.ImageBase) return nullptr;
    const auto rva = va - nt->OptionalHeader.ImageBase;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        if (rva >= section.VirtualAddress && rva + bytes <= std::uintptr_t{section.VirtualAddress} + section.Misc.VirtualSize) return image + rva;
    }
    return nullptr;
}
#endif

// Empty when the site holds the patch afterwards, else why nothing was written.
std::string apply(const std::function<std::filesystem::path(const char*)>& resolve) {
    std::ifstream file(resolve("/app0/gameversion.txt"), std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    if (!PortSettings::BuildVersionSupported(text.str())) {
        return "build '" + std::string(PortSettings::BuildVersionOf(text.str())) + "' is not " + PortSettings::SupportedBuildVersion;
    }
#ifdef _WIN32
    auto* site = mapCode(ProLitePatch::SiteVa, ProLitePatch::Patched.size());
    if (site == nullptr) return "0x1409f4b33 is not in the image's code";
    switch (ProLitePatch::Classify(site)) {
        case ProLitePatch::Site::Patched: return {};
        case ProLitePatch::Site::Unknown: {
            char bytes[32]{};
            std::snprintf(bytes, sizeof(bytes), "%02X %02X %02X %02X %02X", site[0], site[1], site[2], site[3], site[4]);
            return std::string("0x1409f4b33 holds ") + bytes + ", not the IsPro call E8 F8 1D E6 FF";
        }
        case ProLitePatch::Site::Original: break;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, ProLitePatch::Patched.size(), PAGE_EXECUTE_READWRITE, &previous) == FALSE) return "VirtualProtect refused the code page";
    std::memcpy(site, ProLitePatch::Patched.data(), ProLitePatch::Patched.size());
    DWORD ignored = 0;
    VirtualProtect(site, ProLitePatch::Patched.size(), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, ProLitePatch::Patched.size());
    return {};
#else
    return "not implemented on this platform";
#endif
}

}

void InstallProLitePatch(const std::function<std::filesystem::path(const char*)>& resolve) {
    if (!ProLitePatch::Enabled(std::getenv(ProLitePatch::SwitchName))) return;
    std::string reason;
    try {
        reason = apply(resolve);
    } catch (const std::exception& error) {
        reason = error.what();
    }
    if (reason.empty()) {
        active.store(true, std::memory_order_release);
        std::fprintf(stderr, "[mfsr] pro-lite on (APS5_MFSR_PROBE=1): the IsPro call at 0x1409f4b33 answers true, Psml HLE on, %s\n", ProLitePatch::TrinityArg);
    } else {
        std::fprintf(stderr, "[mfsr] pro-lite off: %s; nothing written, the Psml stubs stay unimplemented\n", reason.c_str());
    }
    std::fflush(stderr);
}

extern "C" bool MfsrProbeActive_nid_no_patch() {
    return active.load(std::memory_order_acquire);
}
