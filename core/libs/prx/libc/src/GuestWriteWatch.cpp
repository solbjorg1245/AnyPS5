#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "prx/libc/include/WindowsMappings.hpp"
#endif

#if defined(__linux__)
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace GuestWriteWatch {
namespace {

#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
constexpr std::uintptr_t PageBytes = 4096;

class Watch {
public:
    static Watch& Get() {
        static Watch watch;
        return watch;
    }

    bool Available() const {
        return _pagemap >= 0;
    }

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return;
        std::unique_lock lock(_lock);
        remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        uffdio_register registration{};
        registration.range.start = begin;
        registration.range.len = end - begin;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(_uffd, UFFDIO_REGISTER, &registration) != 0) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[memory] write watch: cannot register 0x%llx+0x%llx (%s); the range stays unwatched\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), std::strerror(errno));
            }
            return;
        }
        insert(_ranges, begin, end);
        insert(_fresh, begin, end);
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return false;
        std::unique_lock lock(_lock);
        const bool watched = remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        return watched;
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available()) return false;
        std::shared_lock lock(_lock);
        return covers(begin, end);
    }

    bool Collect(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> fresh;
        {
            std::unique_lock lock(_lock);
            if (!covers(begin, end)) return false;
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            for (; it != _fresh.end() && it->first < end; ++it) {
                const auto from = std::max(it->first, begin);
                const auto to = std::min(it->second, end);
                if (from < to) fresh.emplace_back(from, to);
            }
            if (!fresh.empty()) remove(_fresh, begin, end);
        }
        for (const auto& [from, to] : fresh) {
            written(context, from, to);
            if (protect(from, to)) continue;
            std::unique_lock lock(_lock);
            for (const auto& [left, right] : fresh) {
                if (!covers(left, right)) continue;
                remove(_fresh, left, right);
                insert(_fresh, left, right);
            }
            written(context, begin, end);
            return false;
        }
        std::array<page_region, 256> regions;
        auto cursor = begin;
        while (cursor < end) {
            pm_scan_arg scan{};
            scan.size = sizeof(scan);
            scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            scan.start = cursor;
            scan.end = end;
            scan.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            scan.vec_len = regions.size();
            scan.category_mask = PAGE_IS_WRITTEN;
            scan.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(_pagemap, PAGEMAP_SCAN, &scan);
            if (count < 0) {
                written(context, cursor, end);
                return false;
            }
            for (long i = 0; i < count; ++i) written(context, regions[i].start, regions[i].end);
            if (scan.walk_end <= cursor || scan.walk_end >= end) break;
            cursor = scan.walk_end;
        }
        return true;
    }

private:
    Watch() {
        if (std::getenv("APS5_NO_WRITE_WATCH") == nullptr) open();
    }

    void open() {
        _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (_uffd < 0 && errno == EPERM) _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (_uffd < 0) return unavailable("userfaultfd");
        uffdio_api api{};
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(_uffd, UFFDIO_API, &api) != 0 || (api.features & UFFD_FEATURE_WP_ASYNC) == 0 || (api.features & UFFD_FEATURE_WP_UNPOPULATED) == 0) return unavailable("asynchronous userfaultfd write protection");
        const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pagemap < 0) return unavailable("/proc/self/pagemap");
        if (!probe(pagemap)) {
            close(pagemap);
            return unavailable("PAGEMAP_SCAN");
        }
        _pagemap = pagemap;
    }

    void unavailable(const char* what) {
        const int error = errno;
        if (_uffd >= 0) close(_uffd);
        _uffd = -1;
        std::fprintf(stderr, "[memory] write watch unavailable: %s failed (%s); guest memory is compared instead\n", what, std::strerror(error));
    }

    bool protect(std::uintptr_t begin, std::uintptr_t end) const {
        uffdio_writeprotect protection{};
        protection.range.start = begin;
        protection.range.len = end - begin;
        protection.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        return ioctl(_uffd, UFFDIO_WRITEPROTECT, &protection) == 0;
    }

    bool probe(int pagemap) {
        constexpr std::uintptr_t probeBytes = 4 * PageBytes;
        void* pages = mmap(nullptr, probeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages == MAP_FAILED) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(pages);
        auto* bytes = static_cast<volatile char*>(pages);
        bytes[0] = 1;
        uffdio_register registration{};
        registration.range.start = address;
        registration.range.len = probeBytes;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        bool working = ioctl(_uffd, UFFDIO_REGISTER, &registration) == 0 && protect(address, address + probeBytes);
        std::array<page_region, 4> regions{};
        const auto scan = [&]() -> long {
            pm_scan_arg arguments{};
            arguments.size = sizeof(arguments);
            arguments.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            arguments.start = address;
            arguments.end = address + probeBytes;
            arguments.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            arguments.vec_len = regions.size();
            arguments.category_mask = PAGE_IS_WRITTEN;
            arguments.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(pagemap, PAGEMAP_SCAN, &arguments);
            if (count < 0) return -1;
            long pagesWritten = 0;
            for (long i = 0; i < count; ++i) pagesWritten += static_cast<long>((regions[i].end - regions[i].start) / PageBytes);
            return pagesWritten;
        };
        working = working && scan() == 0;
        if (working) {
            bytes[0] = 2;
            bytes[2 * PageBytes] = 1;
            working = scan() == 2 && scan() == 0;
        }
        munmap(pages, probeBytes);
        return working;
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto next = _ranges.upper_bound(begin);
        if (next == _ranges.begin()) return false;
        return std::prev(next)->second >= end;
    }

    static void insert(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        auto next = ranges.upper_bound(begin);
        if (next != ranges.begin() && std::prev(next)->second == begin) {
            begin = std::prev(next)->first;
            ranges.erase(std::prev(next));
        }
        if (next != ranges.end() && next->first == end) {
            end = next->second;
            ranges.erase(next);
        }
        ranges.emplace(begin, end);
    }

    static bool remove(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        bool removed = false;
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) --it;
        while (it != ranges.end() && it->first < end) {
            const auto rangeBegin = it->first;
            const auto rangeEnd = it->second;
            if (rangeEnd <= begin) {
                ++it;
                continue;
            }
            removed = true;
            it = ranges.erase(it);
            if (rangeBegin < begin) ranges.emplace(rangeBegin, begin);
            if (rangeEnd > end) ranges.emplace(end, rangeEnd);
        }
        return removed;
    }

    int _uffd = -1;
    int _pagemap = -1;
    std::shared_mutex _lock;
    std::map<std::uintptr_t, std::uintptr_t> _ranges;
    std::map<std::uintptr_t, std::uintptr_t> _fresh;
};

const bool g_opened = (Watch::Get(), true);
#endif

// Guarded pages (GuestPageGuard*, see GuestWriteWatch.hpp).
class PageGuard {
public:
    static constexpr std::uintptr_t PageBytes = 4096;

    static PageGuard& Get() {
        // Never destroyed: a guard may be released while the process exits.
        static auto* guard = new PageGuard;
        return *guard;
    }

    void Install(bool (*resolve)(std::uintptr_t)) {
        _resolve.store(resolve, std::memory_order_release);
        std::call_once(_installed, &PageGuard::installHandler);
        _holding.store(true, std::memory_order_release);
    }

    // A hold over [begin, end) (GuestPageGuardHold): Protect refuses ranges over it from here on,
    // and the guards already over it are resolved. Taken under the lock Protect holds, so a guard
    // being made now is either published before (and resolved here) or refused.
    std::uint64_t Hold(std::uintptr_t begin, std::uintptr_t end) {
        if (!_holding.load(std::memory_order_acquire) || end <= begin) return 0;
        std::uint64_t id = 0;
        {
            std::unique_lock lock(_lock);
            id = ++_nextHold;
            _holds.emplace(id, std::make_pair(begin, end));
        }
        Touch(begin, end);
        return id;
    }

    void Unhold(std::uint64_t id) {
        std::unique_lock lock(_lock);
        _holds.erase(id);
    }

    bool Holding() const { return _holding.load(std::memory_order_acquire); }

    std::uint64_t Protect(std::uintptr_t begin, std::uintptr_t end) {
        if (begin % PageBytes != 0 || end % PageBytes != 0 || end <= begin) return 0;
        std::unique_lock lock(_lock);
        for (const auto& [id, range] : _holds) {
            if (range.first < end && begin < range.second) return refuse(begin, end, Refusal::Held);
        }
        // A range of shared views (Windows) is guarded through the mappings' own bookkeeping
        // (APS5_GUARD_SHARED_VIEWS=1; refused without it, as before), plain memory through its
        // protection; a range of both is refused.
        auto refusal = Refusal::Count;
        const bool view = viewRange(begin, end, refusal);
        if (refusal != Refusal::Count) return refuse(begin, end, refusal);
        // Only the pages no guard holds yet change, and only plain read-write ones may.
        const auto gaps = uncovered(begin, end);
        if (!view) {
            for (const auto& [from, to] : gaps) {
                if (!readWrite(from, to)) return refuse(begin, end, Refusal::PrivateState);
            }
        }
        for (std::size_t i = 0; i < gaps.size(); ++i) {
            if (access(gaps[i].first, gaps[i].second, false, view)) continue;
            // The rollback gives pages back their protection, as a release does (see Serial).
            _serial.fetch_add(1, std::memory_order_acq_rel);
            for (std::size_t j = 0; j <= i; ++j) access(gaps[j].first, gaps[j].second, true, view);
            _serial.fetch_add(1, std::memory_order_acq_rel);
            return refuse(begin, end, Refusal::ProtectFailed);
        }
        const auto id = ++_next;
        _entries.emplace(id, std::make_pair(begin, end));
        if (view) _viewEntries.push_back(id);
        _count.store(_entries.size(), std::memory_order_release);
        return id;
    }

    void Release(std::uint64_t id) {
        std::unique_lock lock(_lock);
        const auto found = _entries.find(id);
        if (found == _entries.end()) return;
        const auto [begin, end] = found->second;
        // Odd from the guard's unpublishing until its pages have their protection back (Serial).
        _serial.fetch_add(1, std::memory_order_acq_rel);
        _entries.erase(found);
        _count.store(_entries.size(), std::memory_order_release);
        const auto viewEntry = std::find(_viewEntries.begin(), _viewEntries.end(), id);
        const bool view = viewEntry != _viewEntries.end();
        if (view) _viewEntries.erase(viewEntry);
        for (const auto& [from, to] : uncovered(begin, end)) access(from, to, true, view);
        _serial.fetch_add(1, std::memory_order_acq_rel);
        // Remembered for a fault that raced this release (see raced).
        auto& slot = _released[_releasedNext++ % _released.size()];
        slot.begin = begin;
        slot.end = end;
        slot.view = view;
        slot.retries.store(0, std::memory_order_relaxed);
    }

    bool Covers(std::uintptr_t address) {
        if (_count.load(std::memory_order_acquire) == 0) return false;
        std::shared_lock lock(_lock);
        return covering(address) != 0;
    }

    // GuestPageGuardHeldRun: under the shared lock whatever `_count` says (a guard being made has
    // its pages no-access before it is counted; Protect holds the lock meanwhile).
    // False: `*end` is the first guarded address in [address, limit), or `limit`.
    bool HeldRun(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* end) {
        if (end != nullptr) *end = limit;
        if (!_holding.load(std::memory_order_acquire)) return false;
        std::shared_lock lock(_lock);
        if (covering(address) == 0) {
            if (end != nullptr) {
                for (const auto& [id, range] : _entries) {
                    if (range.first < *end && address < range.second) *end = std::max(address, range.first);
                }
            }
            return false;
        }
        // Every guard over the cursor moves it to its end, until none holds the cursor.
        auto cursor = address;
        for (bool moved = true; moved && cursor < limit;) {
            moved = false;
            for (const auto& [id, range] : _entries) {
                if (range.first <= cursor && cursor < range.second) {
                    cursor = range.second;
                    moved = true;
                }
            }
        }
        if (end != nullptr) *end = std::min(cursor, limit);
        return true;
    }

    bool HeldWithin(std::uintptr_t begin, std::uintptr_t end) {
        if (!_holding.load(std::memory_order_acquire) || end <= begin) return false;
        std::shared_lock lock(_lock);
        return std::any_of(_entries.begin(), _entries.end(), [&](const auto& entry) { return entry.second.first < end && begin < entry.second.second; });
    }

    std::uint64_t Serial() const { return _serial.load(std::memory_order_acquire); }

    bool Live(std::uint64_t id) {
        if (id == 0 || _count.load(std::memory_order_acquire) == 0) return false;
        std::shared_lock lock(_lock);
        return _entries.contains(id);
    }

    // A fault at `address`: false when no guard holds it (not this handler's fault). `write`: the
    // access was a store (1), a load (0), or unknown (-1).
    bool Fault(std::uintptr_t address, int write = -1) {
        if (!Covers(address)) return raced(address, write);
        _faults.fetch_add(1, std::memory_order_relaxed);
        const auto resolve = _resolve.load(std::memory_order_acquire);
        // Guards made over the page after the resolver started (another thread decided to keep a
        // newer copy resident there before this one looked again) are resolved as well, a few
        // times at most; only a guard the resolver itself left is released by force (a forced
        // release leaves its copy's bytes unlanded).
        std::uint64_t newest = 0;
        {
            std::shared_lock lock(_lock);
            newest = _next;
        }
        bool forced = resolve == nullptr || !resolve(address);
        for (int again = 0; !forced && again < 4; ++again) {
            std::shared_lock lock(_lock);
            const bool newer = std::any_of(_entries.begin(), _entries.end(), [&](const auto& entry) { return entry.first > newest && entry.second.first <= address && address < entry.second.second; });
            if (!newer) break;
            newest = _next;
            lock.unlock();
            forced = !resolve(address);
        }
        for (;;) {
            std::uint64_t id = 0;
            {
                std::shared_lock lock(_lock);
                id = covering(address);
            }
            if (id == 0) break;
            forced = true;
            Release(id);
        }
        if (forced) _forced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Host I/O into or out of guarded pages fails where a CPU access would fault (ReadFile and
    // WriteFile report ERROR_NOACCESS, read and write EFAULT): every guard over [begin, end) is
    // resolved first, as a fault on it would be.
    void Touch(std::uintptr_t begin, std::uintptr_t end) {
        while (_count.load(std::memory_order_acquire) != 0) {
            std::uintptr_t at = 0;
            bool found = false;
            {
                std::shared_lock lock(_lock);
                for (const auto& [id, range] : _entries) {
                    if (range.first < end && begin < range.second) {
                        at = std::max(begin, range.first);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) return;
            Fault(at);
        }
    }

    void Counts(std::uint64_t* faults, std::uint64_t* forced, std::uint64_t* guards) const {
        if (faults != nullptr) *faults = _faults.load(std::memory_order_relaxed);
        if (forced != nullptr) *forced = _forced.load(std::memory_order_relaxed);
        if (guards != nullptr) *guards = _count.load(std::memory_order_relaxed);
    }

    // Why Protect refused, by reason (GuestPageGuardRefusalName's order).
    enum class Refusal : std::size_t { SharedView, Mixed, Aliased, HostWrite, Pinned, ViewProtection, PrivateState, ProtectFailed, Held, Count };

    void Refusals(std::uint64_t* counts, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i) counts[i] = i < _refusals.size() ? _refusals[i].load(std::memory_order_relaxed) : 0;
    }

    static const char* RefusalName(std::size_t index) {
        static constexpr std::array<const char*, static_cast<std::size_t>(Refusal::Count)> names{"shared view", "view and plain memory", "aliased view", "host write", "pinned", "view protection", "not plain read-write", "protect failed", "held"};
        return index < names.size() ? names[index] : nullptr;
    }

private:
    PageGuard() = default;

    // A fault no guard holds that raced a release: the page was guarded when the access faulted and
    // another thread's resolve released it before this handler looked. The access runs again, a
    // bounded number of times per release (a later genuine fault there is passed on).
    // Also a fault on a guard being made: Protect makes the pages no-access before it publishes the
    // guard (Covers saw none yet); the lock waited for it, so the access runs again and faults on
    // the published guard.
    bool raced(std::uintptr_t address, int write) {
        std::shared_lock lock(_lock);
        if (covering(address) != 0) return true;
        for (auto& slot : _released) {
            if (slot.begin <= address && address < slot.end) return slot.retries.fetch_add(1, std::memory_order_relaxed) < 64 && (!slot.view || allowedNow(address, write));
        }
        return false;
    }

    // A fault refused for one reason is described once (the first of each reason), with what the
    // host and the shared mappings know of the range's first page.
    std::uint64_t refuse(std::uintptr_t begin, std::uintptr_t end, Refusal refusal) {
        const auto index = static_cast<std::size_t>(refusal);
        if (_refusals[index].fetch_add(1, std::memory_order_relaxed) == 0) describe(begin, end, RefusalName(index));
        return 0;
    }

    std::uint64_t covering(std::uintptr_t address) const {
        for (const auto& [id, range] : _entries) {
            if (range.first <= address && address < range.second) return id;
        }
        return 0;
    }

    // [begin, end) less every guarded range.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> uncovered(std::uintptr_t begin, std::uintptr_t end) const {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> held, gaps;
        for (const auto& [id, range] : _entries) {
            if (range.first < end && begin < range.second) held.push_back(range);
        }
        std::sort(held.begin(), held.end());
        auto cursor = begin;
        for (const auto& [from, to] : held) {
            if (from > cursor) gaps.emplace_back(cursor, from);
            cursor = std::max(cursor, to);
        }
        if (cursor < end) gaps.emplace_back(cursor, end);
        return gaps;
    }

#if defined(_WIN32)
    // APS5_GUARD_SHARED_VIEWS=1: guards over shared views (WindowsMappings), whose write tracking
    // re-protects whole pages (Collect arms them, HandleWrite opens them), go through the mappings'
    // guard bookkeeping (WindowsMappings::Guard/Unguard), which keeps the guarded parts no-access
    // through those re-protections. Without it such ranges are refused, as before (the title's
    // GPU memory lives in shared views: t388 made no resident copy, ~2k refused per 10 s). Read at
    // each guard over a view (a test sets it between guards).
    static bool sharedViewsEnabled() {
        const char* value = std::getenv("APS5_GUARD_SHARED_VIEWS");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }

    // Whether [begin, end) is guarded as shared views (true) or as plain memory (false); `refusal`
    // is set when it may be neither.
    static bool viewRange(std::uintptr_t begin, std::uintptr_t end, Refusal& refusal) {
        using Fit = GuestArena::WindowsMappings::GuardFit;
        const auto fit = GuestArena::WindowsMappings::Get().FitGuard(begin, end);
        if (fit == Fit::Private) return false;
        if (!sharedViewsEnabled()) {
            refusal = Refusal::SharedView;
            return false;
        }
        switch (fit) {
        case Fit::Views: return true;
        case Fit::Mixed: refusal = Refusal::Mixed; break;
        case Fit::Aliased: refusal = Refusal::Aliased; break;
        case Fit::HostWrite: refusal = Refusal::HostWrite; break;
        case Fit::Pinned: refusal = Refusal::Pinned; break;
        default: refusal = Refusal::ViewProtection; break;
        }
        return false;
    }

    // A fault no guard holds within a range a shared-view guard released recently runs again only
    // if the access is allowed now: a store to a view page the release left armed read-only (write
    // tracking) faults into WindowsMappings::HandleWrite instead of retrying here. (Plain-memory
    // guards retry as before.)
    static bool allowedNow(std::uintptr_t address, int write) {
        if (write < 0) return true;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
        const auto protection = info.Protect & 0xffu;
        if (protection == PAGE_NOACCESS) return false;
        return write == 0 || protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY;
    }

    static void describe(std::uintptr_t begin, std::uintptr_t end, const char* reason) {
        MEMORY_BASIC_INFORMATION info{};
        const bool queried = VirtualQuery(reinterpret_cast<void*>(begin), &info, sizeof(info)) != 0;
        char view[256];
        GuestArena::WindowsMappings::Get().Describe(begin, view, sizeof(view));
        std::fprintf(stderr, "[page-guard] first guard refused as '%s': 0x%llx+0x%llx, VirtualQuery %s state 0x%lx protect 0x%lx type 0x%lx region 0x%llx; shared view %d: %s\n", reason, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), queried ? "ok" : "failed", queried ? info.State : 0ul, queried ? info.Protect : 0ul, queried ? info.Type : 0ul, queried ? static_cast<unsigned long long>(info.RegionSize) : 0ull, GuestArena::WindowsMappings::Get().HasView(begin, end - begin) ? 1 : 0, view);
        std::fflush(stderr);
    }

    static bool readWrite(std::uintptr_t begin, std::uintptr_t end) {
        for (auto cursor = begin; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) return false;
            cursor = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        return true;
    }

    // Shared views (`view`): through the mappings' guard bookkeeping. Plain memory region by region:
    // back to read-write only where the guard's no-access still stands (a range mapped again since
    // keeps the protection it was given).
    static bool access(std::uintptr_t begin, std::uintptr_t end, bool accessible, bool view) {
        if (view) {
            auto& mappings = GuestArena::WindowsMappings::Get();
            if (!accessible) return mappings.Guard(begin, end);
            mappings.Unguard(begin, end);
            return true;
        }
        bool changed = true;
        for (auto cursor = begin; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0) return false;
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
            if (!accessible || (info.State == MEM_COMMIT && info.Protect == PAGE_NOACCESS)) {
                DWORD previous = 0;
                changed = VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, accessible ? PAGE_READWRITE : PAGE_NOACCESS, &previous) != 0 && changed;
            }
            cursor = stop;
        }
        return changed;
    }

    static LONG CALLBACK handler(EXCEPTION_POINTERS* exception) {
        const auto* record = exception->ExceptionRecord;
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
        const auto kind = record->ExceptionInformation[0];
        return Get().Fault(static_cast<std::uintptr_t>(record->ExceptionInformation[1]), kind == 0 ? 0 : kind == 1 ? 1 : -1) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }

    // First in line, ahead of the crash reporter's handler.
    static void installHandler() { AddVectoredExceptionHandler(1, &PageGuard::handler); }
#elif defined(__linux__)
    static bool viewRange(std::uintptr_t, std::uintptr_t, Refusal&) { return false; }
    static bool allowedNow(std::uintptr_t, int) { return true; }
    static void describe(std::uintptr_t begin, std::uintptr_t end, const char* reason) {
        std::fprintf(stderr, "[page-guard] first guard refused as '%s': 0x%llx+0x%llx\n", reason, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
        std::fflush(stderr);
    }

    // Not queried: the guarded ranges are the driver's writable imports.
    static bool readWrite(std::uintptr_t, std::uintptr_t) { return true; }

    static bool access(std::uintptr_t begin, std::uintptr_t end, bool accessible, bool) {
        return mprotect(reinterpret_cast<void*>(begin), end - begin, accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
    }

    static struct sigaction& previousAction() {
        static struct sigaction previous{};
        return previous;
    }

    static void handler(int signal, siginfo_t* info, void* context) {
        if (Get().Fault(reinterpret_cast<std::uintptr_t>(info->si_addr))) return;
        const auto& previous = previousAction();
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) return previous.sa_sigaction(signal, info, context);
        if ((previous.sa_flags & SA_SIGINFO) == 0 && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) return previous.sa_handler(signal);
        // Nobody else's either: the access runs again under the default action.
        struct sigaction fallback{};
        fallback.sa_handler = SIG_DFL;
        sigemptyset(&fallback.sa_mask);
        sigaction(SIGSEGV, &fallback, nullptr);
    }

    static void installHandler() {
        struct sigaction action{};
        action.sa_sigaction = &PageGuard::handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previousAction());
    }
#else
    static bool viewRange(std::uintptr_t, std::uintptr_t, Refusal&) { return false; }
    static bool allowedNow(std::uintptr_t, int) { return true; }
    static void describe(std::uintptr_t, std::uintptr_t, const char*) {}
    static bool readWrite(std::uintptr_t, std::uintptr_t) { return false; }
    static bool access(std::uintptr_t, std::uintptr_t, bool, bool) { return false; }
    static void installHandler() {}
#endif

    std::shared_mutex _lock;
    std::map<std::uint64_t, std::pair<std::uintptr_t, std::uintptr_t>> _entries;
    // The live holds (Hold), and whether holds are taken at all (from the first Install on).
    std::map<std::uint64_t, std::pair<std::uintptr_t, std::uintptr_t>> _holds;
    std::uint64_t _nextHold = 0;
    std::atomic<bool> _holding{false};
    // The live guards made over shared views (released through WindowsMappings::Unguard).
    std::vector<std::uint64_t> _viewEntries;
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Refusal::Count)> _refusals{};
    std::uint64_t _next = 0;
    std::atomic<std::size_t> _count{0};
    // See GuestPageGuardSerial: bumped twice (odd in between) around every protection restore.
    std::atomic<std::uint64_t> _serial{0};
    std::atomic<std::uint64_t> _faults{0}, _forced{0};
    std::atomic<bool (*)(std::uintptr_t)> _resolve{nullptr};
    std::once_flag _installed;
    // The last releases, for raced (written under the exclusive lock).
    struct Released {
        std::uintptr_t begin = 0, end = 0;
        // Released from a shared-view guard (see allowedNow).
        bool view = false;
        std::atomic<std::uint32_t> retries{0};
    };
    std::array<Released, 64> _released{};
    std::size_t _releasedNext = 0;
};

}

bool GuestWriteWatchAvailable_nid_postfix() {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    return Watch::Get().Available();
#else
    return false;
#endif
}

void GuestWriteWatchRegister_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().Register(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

bool GuestWriteWatchUnregister_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().Unregister(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    return bytes != 0 && address + bytes > address && Watch::Get().Covers(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

void GuestPageGuardInstall_nid_postfix(bool (*resolve)(std::uintptr_t address)) {
    PageGuard::Get().Install(resolve);
}

std::uint64_t GuestPageGuardProtect_nid_postfix(std::uintptr_t begin, std::uintptr_t end) {
    return PageGuard::Get().Protect(begin, end);
}

void GuestPageGuardRelease_nid_postfix(std::uint64_t id) {
    if (id != 0) PageGuard::Get().Release(id);
}

void GuestPageGuardTouch_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    if (bytes != 0) PageGuard::Get().Touch(address, address + bytes);
}

std::uint64_t GuestPageGuardHold_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0 || address + bytes < address) return 0;
    return PageGuard::Get().Hold(address, address + bytes);
}

void GuestPageGuardUnhold_nid_postfix(std::uint64_t hold) {
    if (hold != 0) PageGuard::Get().Unhold(hold);
}

bool GuestPageGuardHolding_nid_postfix() {
    return PageGuard::Get().Holding();
}

bool GuestPageGuardHeldRun_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* end) {
    return PageGuard::Get().HeldRun(address, limit, end);
}

bool GuestPageGuardHeldWithin_nid_postfix(std::uintptr_t begin, std::uintptr_t end) {
    return PageGuard::Get().HeldWithin(begin, end);
}

std::uint64_t GuestPageGuardSerial_nid_postfix() {
    return PageGuard::Get().Serial();
}

bool GuestPageGuardLive_nid_postfix(std::uint64_t id) {
    return PageGuard::Get().Live(id);
}

bool GuestPageGuardCovers_nid_postfix(std::uintptr_t address) {
    return PageGuard::Get().Covers(address);
}

void GuestPageGuardCounts_nid_postfix(std::uint64_t* faults, std::uint64_t* forced, std::uint64_t* guards) {
    PageGuard::Get().Counts(faults, forced, guards);
}

void GuestPageGuardRefusals_nid_postfix(std::uint64_t* counts, std::size_t count) {
    PageGuard::Get().Refusals(counts, count);
}

const char* GuestPageGuardRefusalName_nid_postfix(std::size_t index) {
    return PageGuard::RefusalName(index);
}

}
