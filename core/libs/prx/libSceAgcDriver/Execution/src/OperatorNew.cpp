#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <windows.h>
#include <psapi.h>

// The driver's operator new. The libraries are built without asynchronous unwind tables, so
// their exceptions unwind through libc's DWARF unwinder (__cxa_throw and the personality come
// from libc.prx, the frames' CFI from .ehfram). libstdc++'s operator new throws std::bad_alloc
// with its own __cxa_throw instead, through libgcc's SEH unwinder, which finds no unwind entry
// for the driver's frames: an allocation failure ended the process (WER 0x20474343) however the
// callers caught it, as one inside a texture lookup did (t127-t136). These throw from the driver
// itself, so the draw or dispatch that asked fails like any other, and name the size asked for.
// Both allocate from the UCRT heap that libstdc++'s new and delete use, so memory allocated here
// may be freed by either.

namespace {

// The driver's frames have no unwind tables, so the report lists the stack slots that point into
// the driver's code: return addresses as driver offsets (addr2line -f -C -e libSceAgcDriver.prx
// with the image base added), the first one being operator new's caller.
void reportCallers() {
    HMODULE driver = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&reportCallers), &driver)) return;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), driver, &info, sizeof(info))) return;
    const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
    const auto end = base + info.SizeOfImage;
    std::fprintf(stderr, "[gpu]   caller 0x%llx; driver addresses on the stack:", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - base));
    const auto* slot = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    int printed = 0;
    for (int i = 0; i < 2048 && printed < 24; ++i) {
        const auto value = slot[i];
        if (value <= base || value >= end) continue;
        std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(value - base));
        ++printed;
    }
    std::fprintf(stderr, "\n");
}

[[noreturn]] void allocationFailed(std::size_t bytes) {
    std::fprintf(stderr, "[gpu] host allocation of %zu bytes failed\n", bytes);
    reportCallers();
    throw std::bad_alloc();
}

}

void* operator new(std::size_t bytes) {
    if (void* memory = std::malloc(bytes != 0 ? bytes : 1)) return memory;
    allocationFailed(bytes);
}

void* operator new[](std::size_t bytes) {
    if (void* memory = std::malloc(bytes != 0 ? bytes : 1)) return memory;
    allocationFailed(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return std::malloc(bytes != 0 ? bytes : 1);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return std::malloc(bytes != 0 ? bytes : 1);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
