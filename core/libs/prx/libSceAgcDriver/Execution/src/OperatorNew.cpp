#include <cstdio>
#include <cstdlib>
#include <new>

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

[[noreturn]] void allocationFailed(std::size_t bytes) {
    std::fprintf(stderr, "[gpu] host allocation of %zu bytes failed\n", bytes);
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
