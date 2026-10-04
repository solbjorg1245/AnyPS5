#ifndef CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_DIRECTMEMORY_HPP
#define CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_DIRECTMEMORY_HPP

#include <cstdint>
#include <cstddef>

#include "prx/libkernel/KernelErrors.hpp"

static constexpr size_t DIRECT_MEMORY_SIZE = 13824ULL * 1024 * 1024;
static constexpr size_t PS5_PAGE_SIZE = 0x4000;

int DirectMemoryAlloc(int64_t searchStart, int64_t searchEnd, size_t len, size_t alignment, int memoryType, int64_t* physOut);
void DirectMemoryFree(int64_t start, size_t len);
void CreateDirectMemoryBacking(int64_t start, size_t len, int memoryType);
enum class GuestBacking { Direct, Reserved, Other };
// Narrows [*start, *end), a range around address, to the piece with one kind of backing. For a
// direct mapping it also returns the physical offset of the narrowed *start and the memory type.
GuestBacking QueryGuestBacking(std::uintptr_t address, std::uintptr_t* start, std::uintptr_t* end, std::uint64_t* offset, int* memoryType);
// Marks a range as reserved without backing (memory pool decommit) or backed (memory pool commit).
void MarkReserved(const void* addr, size_t len, bool reserved);
void ForgetDirectMemory(int64_t start, size_t len);
bool DirectMemoryFind(int64_t offset, bool findNext, int64_t* start, int64_t* end, int* memoryType);
size_t DirectMemoryFreeRun(uint64_t offset, uint64_t limit);
int DoMapDirect(void** addr, size_t len, int prot, int flags, int64_t physStart, size_t alignment);
int DoMapAnon(void** addr, size_t len, int prot, int flags);
int DoMprotect(const void* addr, size_t len, int prot);
int DoMunmap(void* addr, size_t len);
int DoReserveVirtual(void** addr, size_t len, int flags, size_t alignment);
bool GuestProtection(uintptr_t addr, int* prot);

#endif