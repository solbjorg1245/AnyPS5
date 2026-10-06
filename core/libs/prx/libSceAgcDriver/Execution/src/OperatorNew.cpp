#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
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

// Per-thread caches of freed small blocks: the draw thread allocates and frees a few hundred
// thousand short-lived objects per second (vectors, shared_ptr control blocks, maps), and the UCRT
// heap's RtlAllocateHeap/RtlFreeHeap were ~15% of its time. A freed block goes to the freeing
// thread's cache by its heap size (_msize: blocks from libstdc++'s own new are classified the same
// way, and every cached block stays an ordinary heap block that free() may release); a request takes
// a block of a class whose blocks are at least as large. Requests above MaxCached bytes and blocks
// beyond a class's limit go to the heap directly. APS5_NO_BLOCK_CACHE=1 disables it.
namespace {

constexpr unsigned MinShift = 4;    // 16 bytes
constexpr unsigned MaxShift = 12;   // 4096 bytes
constexpr std::size_t MaxCached = std::size_t{1} << MaxShift;
constexpr std::size_t ClassBytesLimit = 256 * 1024;

struct BlockCache {
    void* heads[MaxShift + 1] = {};
    std::uint32_t counts[MaxShift + 1] = {};
};

void releaseCache(void* data) {
    auto* cache = static_cast<BlockCache*>(data);
    if (cache == nullptr) return;
    for (auto* head : cache->heads) {
        while (head != nullptr) {
            void* next = *static_cast<void**>(head);
            std::free(head);
            head = next;
        }
    }
    std::free(cache);
}

// The cache is found through a TLS slot (TlsGetValue reads the TEB); an FLS slot holding the same
// pointer only frees it when the thread exits.
struct CacheSlots {
    DWORD tls = TLS_OUT_OF_INDEXES;
    DWORD fls = FLS_OUT_OF_INDEXES;
};

const CacheSlots& cacheSlots() {
    static const CacheSlots slots = [] {
        CacheSlots made;
        if (const char* value = std::getenv("APS5_NO_BLOCK_CACHE"); value != nullptr && value[0] != '\0' && value[0] != '0') return made;
        made.fls = FlsAlloc(&releaseCache);
        if (made.fls == FLS_OUT_OF_INDEXES) return made;
        made.tls = TlsAlloc();
        if (made.tls == TLS_OUT_OF_INDEXES) {
            FlsFree(made.fls);
            made.fls = FLS_OUT_OF_INDEXES;
        }
        return made;
    }();
    return slots;
}

bool cacheEnabled() {
    return cacheSlots().tls != TLS_OUT_OF_INDEXES;
}

BlockCache* threadCache(bool create) {
    const auto& slots = cacheSlots();
    if (slots.tls == TLS_OUT_OF_INDEXES) return nullptr;
    auto* cache = static_cast<BlockCache*>(TlsGetValue(slots.tls));
    if (cache == nullptr && create) {
        cache = static_cast<BlockCache*>(std::calloc(1, sizeof(BlockCache)));
        if (cache != nullptr && (!FlsSetValue(slots.fls, cache) || !TlsSetValue(slots.tls, cache))) {
            FlsSetValue(slots.fls, nullptr);
            std::free(cache);
            cache = nullptr;
        }
    }
    return cache;
}

// The class whose blocks serve `bytes`: the smallest power of two not below it.
unsigned requestClass(std::size_t bytes) {
    if (bytes <= (std::size_t{1} << MinShift)) return MinShift;
    return static_cast<unsigned>(std::bit_width(bytes - 1));
}

void* allocate(std::size_t bytes) {
    if (bytes != 0 && bytes <= MaxCached) {
        const auto shift = requestClass(bytes);
        if (auto* cache = threadCache(false); cache != nullptr && cache->heads[shift] != nullptr) {
            void* block = cache->heads[shift];
            cache->heads[shift] = *static_cast<void**>(block);
            --cache->counts[shift];
            return block;
        }
        if (cacheEnabled()) return std::malloc(std::size_t{1} << shift);
    }
    return std::malloc(bytes != 0 ? bytes : 1);
}

void release(void* memory) {
    if (memory == nullptr) return;
    if (cacheEnabled()) {
        const auto size = _msize(memory);
        if (size >= (std::size_t{1} << MinShift) && size != static_cast<std::size_t>(-1)) {
            // The largest class whose blocks this one can stand in for.
            const auto shift = std::min<unsigned>(static_cast<unsigned>(std::bit_width(size)) - 1u, MaxShift);
            if (size <= 2 * MaxCached) {
                if (auto* cache = threadCache(true); cache != nullptr && cache->counts[shift] < ClassBytesLimit >> shift) {
                    *static_cast<void**>(memory) = cache->heads[shift];
                    cache->heads[shift] = memory;
                    ++cache->counts[shift];
                    return;
                }
            }
        }
    }
    std::free(memory);
}

}

void* operator new(std::size_t bytes) {
    if (void* memory = allocate(bytes)) return memory;
    allocationFailed(bytes);
}

void* operator new[](std::size_t bytes) {
    if (void* memory = allocate(bytes)) return memory;
    allocationFailed(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate(bytes);
}

void operator delete(void* memory) noexcept {
    release(memory);
}

void operator delete[](void* memory) noexcept {
    release(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    release(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    release(memory);
}
