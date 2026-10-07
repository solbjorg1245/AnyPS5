#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"

#include <windows.h>
#include <psapi.h>

// The driver's operator new. The libraries are built without asynchronous unwind tables, so
// their exceptions unwind through libc's DWARF unwinder (__cxa_throw and the personality come
// from libc.prx, the frames' CFI from .ehfram). libstdc++'s operator new throws std::bad_alloc
// with its own __cxa_throw instead, through libgcc's SEH unwinder, which finds no unwind entry
// for the driver's frames: an allocation failure ended the process (WER 0x20474343) however the
// callers caught it, as one inside a texture lookup did (t127-t136). These throw from the driver
// itself, so the draw or dispatch that asked fails like any other, and name the size asked for.
// Blocks the arena below does not serve come from the UCRT heap that libstdc++'s new and delete
// use, so such memory may be freed by either. An arena block can reach libstdc++-6.dll's delete
// too: in C++20 basic_string<char> is no longer an extern template, so a string the driver grew
// holds a driver block, and the DLL's getline and operator>> (still extern) free it with the
// DLL's delete; so does every module without its own operator delete (libSceAgc.prx,
// libSceVideoOut.prx) and the DLL's thread routine for a std::thread's state (HostThread.hpp
// keeps that one inside the driver). The DLL's delete frees through its one `free` import, which
// hookFree() points at freeForeign(): arena blocks go back to the arena, the rest to the UCRT's
// free. Without that import the arena stays off (STATUS_HEAP_CORRUPTION at boot, t226).

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

// Small blocks. The draw thread allocates and frees a few hundred thousand short-lived objects per
// second (vectors, shared_ptr control blocks, map nodes), and the UCRT heap's RtlAllocateHeap and
// RtlFreeHeap were ~15% of its time. A request of up to MaxCached bytes takes a block of its class
// (powers of two, 16 to 4096 bytes) from the calling thread's free list, filled one of two ways:
// - the block arena (default): address space reserved once (ArenaBytes, top-down, away from the
//   guest's ranges) and committed one ChunkBytes chunk at a time. A chunk serves one class, its
//   blocks are size-aligned, and chunkClass maps a chunk to its class, so a free inside the arena
//   is a table lookup; any other pointer (the heap's: larger requests, libstdc++'s own allocations)
//   goes to free(). A thread holding more than ClassBytesLimit of a class hands a batch (one
//   chunk's worth of blocks) to the class's shared stack, and refills from it before carving a new
//   chunk, so blocks freed on another thread come back. Nothing is decommitted.
// - the block cache (APS5_NO_BLOCK_ARENA=1): every block an ordinary heap block, classified by its
//   heap size at free (_msize: RtlSizeHeap was 4.2% of the draw thread, t212) and kept on the
//   freeing thread's list up to ClassBytesLimit per class.
// APS5_NO_BLOCK_CACHE=1 disables both: plain malloc and free.
namespace {

constexpr unsigned MinShift = 4;    // 16 bytes
constexpr unsigned MaxShift = 12;   // 4096 bytes
constexpr std::size_t MaxCached = std::size_t{1} << MaxShift;
constexpr std::size_t ClassBytesLimit = 256 * 1024;

constexpr unsigned ChunkShift = 16;   // 64 KiB chunks
constexpr std::size_t ChunkBytes = std::size_t{1} << ChunkShift;
constexpr std::size_t ArenaBytes = std::size_t{32} << 30;   // reserved, never committed as a whole
constexpr std::size_t ArenaChunkCount = ArenaBytes >> ChunkShift;

enum class Mode : std::uint8_t { Heap, Cache, Arena };

struct BlockCache {
    void* heads[MaxShift + 1] = {};
    std::uint32_t counts[MaxShift + 1] = {};
    AgcDriver::HostHeap::Counters counters;
};

// A free block's first word links the next block of its list; the first block of a batch on a
// shared stack links the next batch in its second word (blocks are at least 16 bytes).
void*& nextBlock(void* block) { return *static_cast<void**>(block); }
void*& nextBatch(void* block) { return static_cast<void**>(block)[1]; }

// Blocks per chunk, and the batch a thread hands over or takes back at once.
constexpr std::uint32_t batchBlocks(unsigned shift) { return static_cast<std::uint32_t>(ChunkBytes >> shift); }
constexpr std::uint32_t localLimit(unsigned shift) { return static_cast<std::uint32_t>(ClassBytesLimit >> shift); }

struct SharedClass {
    SRWLOCK lock = SRWLOCK_INIT;
    void* batches = nullptr;        // batches of exactly batchBlocks(shift) blocks
    void* loose = nullptr;          // blocks given back singly: thread-exit remainders, threads without a cache
    std::uint32_t looseCount = 0;
    std::uint32_t batchCount = 0;
};

struct ArenaState {
    SRWLOCK lock = SRWLOCK_INIT;            // the reservation and chunk commits
    std::atomic<std::uintptr_t> base{0};    // 0 until reserved; a block's existence orders the store before its free
    std::size_t chunksUsed = 0;
    bool failed = false;
    std::atomic<std::uint64_t> orphans{0};  // frees of arena addresses in no carved chunk (a wild pointer)
    SharedClass classes[MaxShift + 1];
    std::uint8_t chunkClass[ArenaChunkCount] = {};   // a chunk's class shift; 0 while uncarved
};

constinit ArenaState arena;

struct Config {
    Mode mode = Mode::Heap;
    DWORD tls = TLS_OUT_OF_INDEXES;
    DWORD fls = FLS_OUT_OF_INDEXES;
};

bool envSet(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void releaseCache(void* data);
bool releaseArenaBlock(void* memory);

// libstdc++-6.dll's `free` import, redirected: every operator delete outside the driver ends in
// it (the DLL's own, and the one libSceAgc.prx and libSceVideoOut.prx import), so an arena block
// freed anywhere in the process comes back here.
using FreeFunction = void(__cdecl*)(void*);
FreeFunction foreignFree = nullptr;

void __cdecl freeForeign(void* memory) {
    if (!releaseArenaBlock(memory)) foreignFree(memory);
}

// Points every `free` slot in `moduleName`'s import table at freeForeign(). All slots must hold
// the same function (one CRT), or nothing is changed. The slots stay patched for the life of the
// process: the driver is never unloaded before it.
bool hookFree(const char* moduleName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (module == nullptr) return false;
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* headers = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return false;
    FreeFunction original = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor) {
            if (descriptor->OriginalFirstThunk == 0) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; names->u1.AddressOfData != 0; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(entry->Name, "free") != 0) continue;
                auto current = reinterpret_cast<FreeFunction>(slots->u1.Function);
                if (pass == 0) {
                    if (original == nullptr) original = current;
                    else if (original != current) return false;
                    continue;
                }
                DWORD protection = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &protection)) return false;
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&freeForeign);
                VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection, &protection);
            }
        }
        if (pass == 0) {
            if (original == nullptr) return false;
            foreignFree = original;
        }
    }
    return true;
}

// The cache is found through a TLS slot (TlsGetValue reads the TEB); an FLS slot holding the same
// pointer releases it when the thread exits.
const Config& config() {
    static const Config made = [] {
        Config result;
        if (envSet("APS5_NO_BLOCK_CACHE")) return result;
        result.fls = FlsAlloc(&releaseCache);
        if (result.fls == FLS_OUT_OF_INDEXES) return result;
        result.tls = TlsAlloc();
        if (result.tls == TLS_OUT_OF_INDEXES) {
            FlsFree(result.fls);
            result.fls = FLS_OUT_OF_INDEXES;
            return result;
        }
        result.mode = Mode::Cache;
        if (!envSet("APS5_NO_BLOCK_ARENA")) {
            if (hookFree("libstdc++-6.dll")) result.mode = Mode::Arena;
            else std::fprintf(stderr, "[gpu] block arena: no single `free` import in libstdc++-6.dll to redirect; using the block cache\n");
        }
        return result;
    }();
    return made;
}

BlockCache* threadCache(bool create) {
    const auto& slots = config();
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

// Commits the next chunk for `shift`, reserving the arena first; nullptr when the address space
// or the commit ran out (the request then takes a heap block, which the range check sends back
// to free()).
void* carveChunk(unsigned shift) {
    AcquireSRWLockExclusive(&arena.lock);
    if (arena.base.load(std::memory_order_relaxed) == 0 && !arena.failed) {
        void* reserved = VirtualAlloc(nullptr, ArenaBytes, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
        if (reserved == nullptr) {
            arena.failed = true;
            std::fprintf(stderr, "[gpu] block arena: reserving %zu MiB of address space failed (%lu); small blocks stay on the heap\n", ArenaBytes >> 20, static_cast<unsigned long>(GetLastError()));
        } else {
            arena.base.store(reinterpret_cast<std::uintptr_t>(reserved), std::memory_order_release);
        }
    }
    void* chunk = nullptr;
    if (!arena.failed && arena.chunksUsed < ArenaChunkCount) {
        const auto index = arena.chunksUsed;
        auto* address = reinterpret_cast<void*>(arena.base.load(std::memory_order_relaxed) + index * ChunkBytes);
        if (VirtualAlloc(address, ChunkBytes, MEM_COMMIT, PAGE_READWRITE) != nullptr) {
            arena.chunkClass[index] = static_cast<std::uint8_t>(shift);
            arena.chunksUsed = index + 1;
            chunk = address;
        }
    }
    ReleaseSRWLockExclusive(&arena.lock);
    return chunk;
}

// Links a fresh chunk's blocks into one list.
void* linkChunk(void* chunk, unsigned shift) {
    const auto size = std::size_t{1} << shift;
    const auto count = batchBlocks(shift);
    auto* bytes = static_cast<std::byte*>(chunk);
    for (std::uint32_t i = 0; i + 1 < count; ++i) nextBlock(bytes + i * size) = bytes + (i + 1) * size;
    nextBlock(bytes + (count - 1) * size) = nullptr;
    return chunk;
}

// Fills the thread's empty list for `shift`: a shared batch, loose blocks, or a new chunk.
bool refill(BlockCache& cache, unsigned shift) {
    auto& shared = arena.classes[shift];
    AcquireSRWLockExclusive(&shared.lock);
    if (shared.batches != nullptr) {
        void* batch = shared.batches;
        shared.batches = nextBatch(batch);
        --shared.batchCount;
        ReleaseSRWLockExclusive(&shared.lock);
        cache.heads[shift] = batch;
        cache.counts[shift] = batchBlocks(shift);
        return true;
    }
    if (shared.loose != nullptr) {
        void* head = shared.loose;
        void* last = head;
        std::uint32_t taken = 1;
        while (taken < batchBlocks(shift) && nextBlock(last) != nullptr) {
            last = nextBlock(last);
            ++taken;
        }
        shared.loose = nextBlock(last);
        shared.looseCount -= taken;
        nextBlock(last) = nullptr;
        ReleaseSRWLockExclusive(&shared.lock);
        cache.heads[shift] = head;
        cache.counts[shift] = taken;
        return true;
    }
    ReleaseSRWLockExclusive(&shared.lock);
    void* chunk = carveChunk(shift);
    if (chunk == nullptr) return false;
    cache.heads[shift] = linkChunk(chunk, shift);
    cache.counts[shift] = batchBlocks(shift);
    return true;
}

// Moves the first batchBlocks(shift) blocks of the thread's list (it holds at least that many) to
// the class's shared stack.
void handBatch(BlockCache& cache, unsigned shift) {
    const auto count = batchBlocks(shift);
    void* head = cache.heads[shift];
    void* last = head;
    for (std::uint32_t i = 1; i < count; ++i) last = nextBlock(last);
    cache.heads[shift] = nextBlock(last);
    cache.counts[shift] -= count;
    nextBlock(last) = nullptr;
    auto& shared = arena.classes[shift];
    AcquireSRWLockExclusive(&shared.lock);
    nextBatch(head) = shared.batches;
    shared.batches = head;
    ++shared.batchCount;
    ReleaseSRWLockExclusive(&shared.lock);
}

// Splices a list of `count` blocks onto the class's loose list.
void giveLoose(unsigned shift, void* head, std::uint32_t count) {
    void* last = head;
    while (nextBlock(last) != nullptr) last = nextBlock(last);
    auto& shared = arena.classes[shift];
    AcquireSRWLockExclusive(&shared.lock);
    nextBlock(last) = shared.loose;
    shared.loose = head;
    shared.looseCount += count;
    ReleaseSRWLockExclusive(&shared.lock);
}

// The thread's lists at its exit: arena blocks go back to the shared lists, heap blocks to the heap.
void releaseCache(void* data) {
    auto* cache = static_cast<BlockCache*>(data);
    if (cache == nullptr) return;
    const auto& slots = config();
    if (slots.tls != TLS_OUT_OF_INDEXES) TlsSetValue(slots.tls, nullptr);
    for (unsigned shift = 0; shift <= MaxShift; ++shift) {
        if (cache->heads[shift] == nullptr) continue;
        if (slots.mode == Mode::Arena) {
            while (cache->counts[shift] >= batchBlocks(shift)) handBatch(*cache, shift);
            if (cache->heads[shift] != nullptr) giveLoose(shift, cache->heads[shift], cache->counts[shift]);
        } else {
            for (void* head = cache->heads[shift]; head != nullptr;) {
                void* next = nextBlock(head);
                std::free(head);
                head = next;
            }
        }
        cache->heads[shift] = nullptr;
        cache->counts[shift] = 0;
    }
    std::free(cache);
}

void* allocate(std::size_t bytes) {
    const auto mode = config().mode;
    auto* cache = mode != Mode::Heap ? threadCache(true) : nullptr;
    if (cache != nullptr) ++cache->counters.allocations;
    if (bytes != 0 && bytes <= MaxCached && cache != nullptr) {
        const auto shift = requestClass(bytes);
        if (cache->heads[shift] == nullptr && mode == Mode::Arena && !refill(*cache, shift)) return std::malloc(bytes);
        if (cache->heads[shift] != nullptr) {
            ++cache->counters.cacheHits;
            void* block = cache->heads[shift];
            cache->heads[shift] = nextBlock(block);
            --cache->counts[shift];
            return block;
        }
        return std::malloc(std::size_t{1} << shift);
    }
    return std::malloc(bytes != 0 ? bytes : 1);
}

// An arena block goes back to the freeing thread's list of its class; any other pointer (the
// heap's, or null) is left to the caller.
bool releaseArenaBlock(void* memory) {
    const auto base = arena.base.load(std::memory_order_relaxed);
    const auto offset = reinterpret_cast<std::uintptr_t>(memory) - base;
    if (base == 0 || offset >= ArenaBytes) return false;
    const unsigned shift = arena.chunkClass[offset >> ChunkShift];
    if (shift < MinShift) {
        if (arena.orphans.fetch_add(1, std::memory_order_relaxed) == 0) std::fprintf(stderr, "[gpu] block arena: free of %p, inside the arena but in no chunk; kept\n", memory);
        return true;
    }
    auto* cache = threadCache(true);
    if (cache == nullptr) {
        nextBlock(memory) = nullptr;
        giveLoose(shift, memory, 1);
        return true;
    }
    nextBlock(memory) = cache->heads[shift];
    cache->heads[shift] = memory;
    ++cache->counts[shift];
    ++cache->counters.cacheStores;
    if (cache->counts[shift] > localLimit(shift)) handBatch(*cache, shift);
    return true;
}

void release(void* memory) {
    if (memory == nullptr) return;
    const auto mode = config().mode;
    if (mode == Mode::Arena) {
        if (auto* cache = threadCache(true); cache != nullptr) ++cache->counters.releases;
        if (!releaseArenaBlock(memory)) std::free(memory);
        return;
    }
    if (mode == Mode::Cache) {
        auto* cache = threadCache(true);
        if (cache != nullptr) ++cache->counters.releases;
        const auto size = _msize(memory);
        if (size >= (std::size_t{1} << MinShift) && size != static_cast<std::size_t>(-1)) {
            // The largest class whose blocks this one can stand in for.
            const auto shift = std::min<unsigned>(static_cast<unsigned>(std::bit_width(size)) - 1u, MaxShift);
            if (size <= 2 * MaxCached) {
                if (cache != nullptr && cache->counts[shift] < ClassBytesLimit >> shift) {
                    nextBlock(memory) = cache->heads[shift];
                    cache->heads[shift] = memory;
                    ++cache->counts[shift];
                    ++cache->counters.cacheStores;
                    return;
                }
            }
        }
    }
    std::free(memory);
}

}

namespace AgcDriver::HostHeap {

Counters ThreadCounters() {
    if (auto* cache = threadCache(false); cache != nullptr) return cache->counters;
    return {};
}

const char* ModeName() {
    switch (config().mode) {
        case Mode::Arena: return "arena";
        case Mode::Cache: return "block cache";
        default: return "heap";
    }
}

ArenaStatus Arena() {
    ArenaStatus status;
    if (config().mode != Mode::Arena) return status;
    // Unlocked reads: a report, not an accounting.
    status.committedBytes = static_cast<std::uint64_t>(arena.chunksUsed) * ChunkBytes;
    for (unsigned shift = MinShift; shift <= MaxShift; ++shift) {
        const auto& shared = arena.classes[shift];
        status.sharedBytes += static_cast<std::uint64_t>(shared.batchCount) * ChunkBytes + (static_cast<std::uint64_t>(shared.looseCount) << shift);
    }
    return status;
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
