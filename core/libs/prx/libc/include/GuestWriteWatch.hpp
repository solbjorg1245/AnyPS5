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
// Resolves every guard over [address, address + bytes) as a fault would: host I/O into or out of
// guest memory (GuestArena::HostWrite, the kernel's writes) fails on a guarded page instead of
// faulting. One atomic load while no guard is live.
void GuestPageGuardTouch_nid_postfix(std::uintptr_t address, std::size_t bytes);
// Touch with a hold: until GuestPageGuardUnhold(the id returned; 0 for nothing held), Protect
// refuses ranges over [address, address + bytes) (counted as 'held'), so no guard is live there
// while the holder changes the range's mapping or protection, or host I/O reads or writes it (a
// Touch alone leaves a window in which the GPU thread may take a guard again). Nothing is held
// (0) before the first Install.
std::uint64_t GuestPageGuardHold_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestPageGuardUnhold_nid_postfix(std::uint64_t hold);
// Whether holds are taken (an Install happened): callers may skip gathering ranges to hold.
bool GuestPageGuardHolding_nid_postfix();
// A page query (VirtualQuery, /proc/self/maps) that finds no access at `address` asks here whether a
// guard holds the page: the guest still sees it mapped with its own protection (an access faults
// into the resolver, which lands the bytes first), so it must not be taken for a hole. True when a
// guard holds the page at `address`, with `*end` the end of the run of guarded pages from it (at
// most `limit`). Taken under the guards' lock: a guard being made (pages no-access, not yet
// published) or released (published no longer, protection being restored) is seen whole, so false
// after a no-access answer means that answer may be stale (a release restored the page since): the
// caller queries once more. Always false before the first Install.
bool GuestPageGuardHeldRun_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* end);
// Whether a guard holds any page of [begin, end) (false before the first Install).
bool GuestPageGuardHeldWithin_nid_postfix(std::uintptr_t begin, std::uintptr_t end);
// Totals since start: faults on guarded pages and those that left a guard to release by force;
// the live guards.
void GuestPageGuardCounts_nid_postfix(std::uint64_t* faults, std::uint64_t* forced, std::uint64_t* guards);
// Guards Protect refused since start, by reason (the first `count` reasons; GuestPageGuardRefusalName
// names reason `index`, null past the last). The first refusal of each reason is described once on
// stderr ('[page-guard] first guard refused as ...': the host's and the shared mappings' view of
// the range's first page). Windows: a range of shared views (WindowsMappings) is guarded through the
// mappings' own bookkeeping only with APS5_GUARD_SHARED_VIEWS=1 (refused as 'shared view' without
// it); one whose page is mapped at several guest addresses is refused as 'aliased view'.
void GuestPageGuardRefusals_nid_postfix(std::uint64_t* counts, std::size_t count);
const char* GuestPageGuardRefusalName_nid_postfix(std::size_t index);

}

// A GuestPageGuardHold for the object's lifetime.
class PageGuardHold {
public:
    PageGuardHold(const void* address, std::size_t bytes) : hold(address != nullptr && bytes != 0 ? GuestPageGuardHold_nid_postfix(reinterpret_cast<std::uintptr_t>(address), bytes) : 0) {}
    PageGuardHold(PageGuardHold&& other) noexcept : hold(other.hold) { other.hold = 0; }
    PageGuardHold(const PageGuardHold&) = delete;
    PageGuardHold& operator=(const PageGuardHold&) = delete;
    PageGuardHold& operator=(PageGuardHold&&) = delete;
    ~PageGuardHold() {
        if (hold != 0) GuestPageGuardUnhold_nid_postfix(hold);
    }

private:
    std::uint64_t hold;
};

}

#endif
