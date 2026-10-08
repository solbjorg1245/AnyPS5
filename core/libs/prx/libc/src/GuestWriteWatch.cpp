#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    }

    std::uint64_t Protect(std::uintptr_t begin, std::uintptr_t end) {
        if (begin % PageBytes != 0 || end % PageBytes != 0 || end <= begin) return 0;
        std::unique_lock lock(_lock);
        // Only the pages no guard holds yet change, and only plain read-write ones may.
        const auto gaps = uncovered(begin, end);
        for (const auto& [from, to] : gaps) {
            if (!readWrite(from, to)) return 0;
        }
        for (std::size_t i = 0; i < gaps.size(); ++i) {
            if (access(gaps[i].first, gaps[i].second, false)) continue;
            for (std::size_t j = 0; j <= i; ++j) access(gaps[j].first, gaps[j].second, true);
            return 0;
        }
        const auto id = ++_next;
        _entries.emplace(id, std::make_pair(begin, end));
        _count.store(_entries.size(), std::memory_order_release);
        return id;
    }

    void Release(std::uint64_t id) {
        std::unique_lock lock(_lock);
        const auto found = _entries.find(id);
        if (found == _entries.end()) return;
        const auto [begin, end] = found->second;
        _entries.erase(found);
        _count.store(_entries.size(), std::memory_order_release);
        for (const auto& [from, to] : uncovered(begin, end)) access(from, to, true);
        // Remembered for a fault that raced this release (see raced).
        auto& slot = _released[_releasedNext++ % _released.size()];
        slot.begin = begin;
        slot.end = end;
        slot.retries.store(0, std::memory_order_relaxed);
    }

    bool Covers(std::uintptr_t address) {
        if (_count.load(std::memory_order_acquire) == 0) return false;
        std::shared_lock lock(_lock);
        return covering(address) != 0;
    }

    // A fault at `address`: false when no guard holds it (not this handler's fault).
    bool Fault(std::uintptr_t address) {
        if (!Covers(address)) return raced(address);
        _faults.fetch_add(1, std::memory_order_relaxed);
        const auto resolve = _resolve.load(std::memory_order_acquire);
        bool forced = resolve == nullptr || !resolve(address);
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

private:
    PageGuard() = default;

    // A fault no guard holds that raced a release: the page was guarded when the access faulted and
    // another thread's resolve released it before this handler looked. The access runs again, a
    // bounded number of times per release (a later genuine fault there is passed on).
    bool raced(std::uintptr_t address) {
        std::shared_lock lock(_lock);
        for (auto& slot : _released) {
            if (slot.begin <= address && address < slot.end) return slot.retries.fetch_add(1, std::memory_order_relaxed) < 64;
        }
        return false;
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
    static bool readWrite(std::uintptr_t begin, std::uintptr_t end) {
        // Shared views re-protect their pages for write tracking (WindowsMappings::Collect and
        // HandleWrite), which would lift a guard: refused.
        if (GuestArena::WindowsMappings::Get().HasView(begin, end - begin)) return false;
        for (auto cursor = begin; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) return false;
            cursor = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        return true;
    }

    // Region by region (a range may span several views). Back to read-write only where the guard's
    // no-access still stands: a range mapped again since keeps the protection it was given.
    static bool access(std::uintptr_t begin, std::uintptr_t end, bool accessible) {
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
        return Get().Fault(static_cast<std::uintptr_t>(record->ExceptionInformation[1])) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }

    // First in line, ahead of the crash reporter's handler.
    static void installHandler() { AddVectoredExceptionHandler(1, &PageGuard::handler); }
#elif defined(__linux__)
    // Not queried: the guarded ranges are the driver's writable imports.
    static bool readWrite(std::uintptr_t, std::uintptr_t) { return true; }

    static bool access(std::uintptr_t begin, std::uintptr_t end, bool accessible) {
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
    static bool readWrite(std::uintptr_t, std::uintptr_t) { return false; }
    static bool access(std::uintptr_t, std::uintptr_t, bool) { return false; }
    static void installHandler() {}
#endif

    std::shared_mutex _lock;
    std::map<std::uint64_t, std::pair<std::uintptr_t, std::uintptr_t>> _entries;
    std::uint64_t _next = 0;
    std::atomic<std::size_t> _count{0};
    std::atomic<std::uint64_t> _faults{0}, _forced{0};
    std::atomic<bool (*)(std::uintptr_t)> _resolve{nullptr};
    std::once_flag _installed;
    // The last releases, for raced (written under the exclusive lock).
    struct Released {
        std::uintptr_t begin = 0, end = 0;
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

bool GuestPageGuardCovers_nid_postfix(std::uintptr_t address) {
    return PageGuard::Get().Covers(address);
}

void GuestPageGuardCounts_nid_postfix(std::uint64_t* faults, std::uint64_t* forced, std::uint64_t* guards) {
    PageGuard::Get().Counts(faults, forced, guards);
}

}
