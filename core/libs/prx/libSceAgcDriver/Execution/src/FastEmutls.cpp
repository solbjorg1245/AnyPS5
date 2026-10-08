#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <windows.h>
#include <intrin.h>

// The driver's __emutls_get_address. MinGW's GCC (15) has no native TLS: every thread_local access
// calls __emutls_get_address, and libgcc's finds the thread's object array through winpthreads'
// pthread_getspecific, which takes a per-thread spinlock and saves and restores the last error
// around TlsGetValue. The queue-0 thread spent 12.7% of its samples inside libwinpthread-1.dll
// (t342 profile; GpuLockThreadTag's one thread_local alone 1.8%). This one keeps the same layout
// (the compiler's control objects, one array of object pointers per thread) but holds the array in
// a TLS slot of its own and reads it straight from the TEB (gs:[0x1480 + 8 * slot]): a hit is an
// image-range check, two loads and a bounds check. Objects outside the driver's image (libstdc++'s
// __once_call, whose offsets libgcc assigns) go to libgcc's function. A thread's array and objects
// are never freed (the thread_local destructors still run, on storage that stays valid): the
// driver's threads are long-lived. APS5_NO_FAST_EMUTLS=1 sends every object to libgcc's, as before.

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

// libgcc's __emutls_object and __emutls_array (emutls.c).
struct EmutlsObject {
    std::uintptr_t size;
    std::uintptr_t align;
    union {
        std::uintptr_t offset;
        void* ptr;
    } loc;
    void* templ;
};

struct EmutlsArray {
    std::uintptr_t size;
    void* data[1];
};

using GetAddress = void* (*)(EmutlsObject*);

enum Mode : int { Unset, Fast, Libgcc };

std::atomic<int> mode{Unset};
GetAddress libgccGetAddress = nullptr;
DWORD slot = TLS_OUT_OF_INDEXES;
std::uintptr_t slotOffset = 0;
std::uintptr_t imageBegin = 0;
std::uintptr_t imageSize = 0;
SRWLOCK lock = SRWLOCK_INIT;
std::uintptr_t assigned = 0;
std::atomic<std::uint32_t> threads{0};

void initialize() {
    AcquireSRWLockExclusive(&lock);
    if (mode.load(std::memory_order_relaxed) == Unset) {
        if (HMODULE libgcc = GetModuleHandleA("libgcc_s_seh-1.dll")) libgccGetAddress = reinterpret_cast<GetAddress>(reinterpret_cast<void*>(GetProcAddress(libgcc, "__emutls_get_address")));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(&__ImageBase) + __ImageBase.e_lfanew);
        imageBegin = reinterpret_cast<std::uintptr_t>(&__ImageBase);
        imageSize = nt->OptionalHeader.SizeOfImage;
        const bool disabled = std::getenv("APS5_NO_FAST_EMUTLS") != nullptr;
        if (!disabled) slot = TlsAlloc();
        // Only the TEB's 64 inline slots are read directly; past them it would be TlsGetValue again.
        const bool fast = !disabled && slot < 64;
        if (fast) slotOffset = 0x1480 + slot * sizeof(void*);
        else if (slot != TLS_OUT_OF_INDEXES) TlsFree(slot);
        std::fprintf(stderr, "[tls] driver thread_locals: %s (slot %lu)\n", fast ? "fast emutls" : disabled ? "libgcc's emutls (APS5_NO_FAST_EMUTLS)" : "libgcc's emutls (no inline TLS slot)", fast ? static_cast<unsigned long>(slot) : 0ul);
        mode.store(fast ? Fast : Libgcc, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&lock);
}

void* allocateObject(const EmutlsObject* object) {
    const std::size_t align = object->align > sizeof(void*) ? object->align : sizeof(void*);
    void* storage = _aligned_malloc(object->size != 0 ? object->size : 1, align);
    if (storage == nullptr) {
        std::fprintf(stderr, "[tls] thread_local storage of %llu bytes failed\n", static_cast<unsigned long long>(object->size));
        std::abort();
    }
    if (object->templ != nullptr) std::memcpy(storage, object->templ, object->size);
    else std::memset(storage, 0, object->size);
    return storage;
}

__attribute__((noinline)) void* slowAddress(EmutlsObject* object) {
    auto offset = __atomic_load_n(&object->loc.offset, __ATOMIC_ACQUIRE);
    if (offset == 0) {
        AcquireSRWLockExclusive(&lock);
        offset = object->loc.offset;
        if (offset == 0) {
            offset = ++assigned;
            __atomic_store_n(&object->loc.offset, offset, __ATOMIC_RELEASE);
        }
        ReleaseSRWLockExclusive(&lock);
    }
    const DWORD error = GetLastError();
    auto* array = static_cast<EmutlsArray*>(TlsGetValue(slot));
    if (array == nullptr || offset > array->size) {
        const std::uintptr_t old = array != nullptr ? array->size : 0;
        const std::uintptr_t size = offset + 32 > old * 2 ? offset + 32 : old * 2;
        auto* grown = static_cast<EmutlsArray*>(std::realloc(array, sizeof(EmutlsArray) + size * sizeof(void*)));
        if (grown == nullptr) {
            std::fprintf(stderr, "[tls] thread_local array of %llu entries failed\n", static_cast<unsigned long long>(size));
            std::abort();
        }
        std::memset(grown->data + old, 0, (size - old) * sizeof(void*));
        grown->size = size;
        TlsSetValue(slot, grown);
        if (array == nullptr) threads.fetch_add(1, std::memory_order_relaxed);
        array = grown;
    }
    void*& entry = array->data[offset - 1];
    if (entry == nullptr) entry = allocateObject(object);
    SetLastError(error);
    return entry;
}

__attribute__((noinline)) void* otherAddress(EmutlsObject* object) {
    if (mode.load(std::memory_order_acquire) == Unset) initialize();
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (mode.load(std::memory_order_relaxed) == Fast && address - imageBegin < imageSize) return slowAddress(object);
    if (libgccGetAddress == nullptr) {
        std::fprintf(stderr, "[tls] libgcc's __emutls_get_address not found\n");
        std::abort();
    }
    return libgccGetAddress(object);
}

} // namespace

extern "C" void* __emutls_get_address(EmutlsObject* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (mode.load(std::memory_order_acquire) == Fast && address - imageBegin < imageSize) [[likely]] {
        const auto offset = __atomic_load_n(&object->loc.offset, __ATOMIC_ACQUIRE);
        const auto* array = reinterpret_cast<const EmutlsArray*>(__readgsqword(static_cast<unsigned long>(slotOffset)));
        if (array != nullptr && offset - 1 < array->size) [[likely]] {
            if (void* entry = array->data[offset - 1]) [[likely]] return entry;
        }
        return slowAddress(object);
    }
    return otherAddress(object);
}
