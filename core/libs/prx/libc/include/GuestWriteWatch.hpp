#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEWATCH_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEWATCH_HPP

#include <cstddef>
#include <cstdint>

namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix();
void GuestWriteWatchRegister_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestWriteWatchUnregister_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes);
bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context);

// Guarded pages (APS5_RESIDENT_BUFFERS, see the driver's Recorder::KeepsResidentBuffers): host
// pages whose bytes are not current yet are made inaccessible, and a CPU access to one faults into
// `resolve` (Install; a later call replaces it), which lands the bytes and releases the guards over
// the page; a guard still over it afterwards is released anyway (counted as forced), so the access
// always proceeds. Protect takes whole 4 KiB pages that are plain read-write (or guarded already)
// and returns the guard's id (0: refused); several guards may hold a page, which turns accessible
// again once none does. Windows: a vectored exception handler; Linux: a SIGSEGV handler that passes
// the faults it does not own on. Elsewhere Protect refuses.
void GuestPageGuardInstall_nid_postfix(bool (*resolve)(std::uintptr_t address));
std::uint64_t GuestPageGuardProtect_nid_postfix(std::uintptr_t begin, std::uintptr_t end);
void GuestPageGuardRelease_nid_postfix(std::uint64_t id);
bool GuestPageGuardCovers_nid_postfix(std::uintptr_t address);
// Totals since start: faults on guarded pages and those that left a guard to release by force;
// the live guards.
void GuestPageGuardCounts_nid_postfix(std::uint64_t* faults, std::uint64_t* forced, std::uint64_t* guards);

}

}

#endif
