#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <map>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>

namespace GuestArena {

class WindowsMappings {
public:
    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    // Bumped by every change that puts bytes no write made at a guest address (a commit over a
    // placeholder, a shared map, a release): GuestMemory's collect memo is valid under one value.
    std::uint64_t MappingSerial() const {
        return mappingSerial.load(std::memory_order_acquire);
    }

    void Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        std::lock_guard lock(mutex);
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                rememberRange(freshRanges, cursor, cursor + size);
                mappingSerial.fetch_add(1, std::memory_order_release);
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                const auto mapped = views.find(cursor & ~(pageBytes - 1));
                if (mapped != views.end()) {
                    mapped->second.protection = protection;
                    mapped->second.armed = false;
                    invalidate(*mapped->second.page);
                }
                // A page guard's parts keep their no-access (Unguard gives them this protection).
                if (mapped != views.end() && mapped->second.guarded != 0) {
                    protectUnguarded(mapped->first, mapped->second.guarded, protection, "protect guest memory", cursor, stop);
                } else {
                    DWORD previous;
                    if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                }
                cursor = stop;
            }
        }
    }

    void Reset(void* address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
        mappingSerial.fetch_add(1, std::memory_order_release);
    }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        std::lock_guard lock(mutex);
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        reset(cursor, bytes);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        const auto owned = std::make_shared<Section>(duplicate);
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            split(cursor + done, pageBytes);
            void* page = reinterpret_cast<void*>(cursor + done);
            if (!map(section, GetCurrentProcess(), page, offset + done, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
            DWORD previous;
            if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            auto shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, 0, false, owned, offset + done, 0});
            if (const auto pinned = pinnedPages.find(base); pinned != pinnedPages.end()) shared->pins += pinned->second;
            invalidate(*shared);
        }
        mappingSerial.fetch_add(1, std::memory_order_release);
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = false;
            invalidate(*it->second.page);
        }
    }

    // Pins follow the guest address (pinnedPages): a view mapped there later, such as a fixed remap of
    // the range, takes the pin over, and a view unmapped from it gives its page's pin back (Map/reset).
    void Pin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto base = address & ~(pageBytes - 1); base < address + bytes; base += pageBytes) {
            ++pinnedPages[base];
            const auto found = views.find(base);
            if (found == views.end()) continue;
            auto& page = *found->second.page;
            ++page.pins;
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (!view.armed) continue;
                protectUnguarded(alias, view.guarded, view.protection, "pin shared guest page writable");
                view.armed = false;
            }
            invalidate(page);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto base = address & ~(pageBytes - 1); base < address + bytes; base += pageBytes) {
            const auto pinned = pinnedPages.find(base);
            if (pinned == pinnedPages.end()) continue;
            if (--pinned->second == 0) pinnedPages.erase(pinned);
            const auto found = views.find(base);
            if (found == views.end()) continue;
            auto& page = *found->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        std::lock_guard lock(mutex);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        // A part a page guard holds faults for the guard, which resolves it first (its vectored
        // handler runs ahead of this one): not a tracked write.
        if ((view.guarded & guardPartBit(address - base)) != 0) return false;
        invalidate(*view.page);
        protectUnguarded(base, view.guarded, view.protection, "resume shared memory write");
        view.armed = false;
        return true;
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        const auto end = address + bytes;
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!writable(it->second.protection)) return false;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            ++view.hostWrites;
            invalidate(*view.page);
            if (!view.armed) continue;
            protectUnguarded(it->first, view.guarded, view.protection, "open shared memory to a host write");
            view.armed = false;
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page);
        }
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        for (std::size_t done = 0; done < bytes; done += pageBytes, ++view) {
            if (view == views.end() || view->first != address + done || view->second.offset != offset + done) throw refuse("the range is not one contiguous run of views of a section");
            if (view->second.section != section && !sameSection(view->second.section->handle, section->handle)) throw refuse("the range spans several sections");
        }
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        return static_cast<char*>(alias) + lead;
    }

    // A read-write alias of shared guest memory made of several runs of views (adjacent guest
    // allocations backed by unrelated section offsets, which MapAlias refuses): one placeholder
    // reservation, split per run, each run's view mapped into its piece (page-granular, as the guest
    // views themselves). The pieces are remembered for UnmapAlias.
    void* MapSpanAlias(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write span alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        struct Run {
            HANDLE section;
            std::uint64_t offset;
            std::size_t bytes;
        };
        std::vector<Run> runs;
        auto view = views.find(address);
        for (std::size_t done = 0; done < bytes; done += pageBytes, ++view) {
            if (view == views.end() || view->first != address + done) throw refuse("a page is not a shared view");
            const auto& page = view->second;
            if (!runs.empty() && runs.back().offset + runs.back().bytes == page.offset && (runs.back().section == page.section->handle || sameSection(runs.back().section, page.section->handle))) {
                runs.back().bytes += pageBytes;
                continue;
            }
            runs.push_back({page.section->handle, page.offset, pageBytes});
        }
        auto* base = static_cast<char*>(allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (base == nullptr) throw refuse("no placeholder could be reserved");
        std::vector<void*> pieces;
        std::size_t cursor = 0;
        const auto undo = [&](const char* reason) {
            for (auto* piece : pieces) unmap(GetCurrentProcess(), piece, 0);
            if (cursor < bytes) VirtualFree(base + cursor, 0, MEM_RELEASE);
            return refuse(reason);
        };
        for (const auto& run : runs) {
            if (cursor + run.bytes < bytes && !VirtualFree(base + cursor, run.bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) throw undo("splitting the placeholder failed");
            if (map(run.section, GetCurrentProcess(), base + cursor, run.offset, run.bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr) throw undo("mapping a run failed");
            pieces.push_back(base + cursor);
            cursor += run.bytes;
        }
        spanAliases.emplace(base, std::move(pieces));
        return base;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        {
            std::lock_guard lock(mutex);
            if (const auto span = spanAliases.find(alias); span != spanAliases.end()) {
                for (auto* piece : span->second) {
                    if (!unmap(GetCurrentProcess(), piece, 0)) fail("unmap shared guest span alias");
                }
                spanAliases.erase(span);
                return;
            }
        }
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
    }

    // Debug aid: the write-tracking state of the page holding `address`, as one line.
    void Describe(std::uintptr_t address, char* text, std::size_t size) {
        std::lock_guard lock(mutex);
        const auto base = address & ~(pageBytes - 1);
        const bool clean = rangeAt(cleanRanges, address) != cleanRanges.end();
        const bool fresh = rangeAt(freshRanges, address) != freshRanges.end();
        const auto found = views.find(base);
        if (found == views.end()) {
            std::snprintf(text, size, "no view (clean %d fresh %d)", clean ? 1 : 0, fresh ? 1 : 0);
            return;
        }
        const auto& view = found->second;
        std::snprintf(text, size, "view protection 0x%lx seen %llu generation %llu armed %d host writes %u aliases %zu pins %u guarded parts 0x%x section offset 0x%llx (clean %d fresh %d)", view.protection, static_cast<unsigned long long>(view.seen), static_cast<unsigned long long>(view.page->generation), view.armed ? 1 : 0, view.hostWrites, view.page->aliases.size(), view.page->pins, static_cast<unsigned>(view.guarded), static_cast<unsigned long long>(view.offset), clean ? 1 : 0, fresh ? 1 : 0);
    }

    // Page guards over shared views (GuestWriteWatch's PageGuard, APS5_GUARD_SHARED_VIEWS=1). The
    // write tracking re-protects a view's 16 KiB page as a whole (Collect arms it read-only,
    // HandleWrite, Pin and BeginHostWrite open it again), which would lift a guard's no-access:
    // instead each view keeps the 4 KiB parts guards hold (`guarded`), every such re-protection
    // leaves them alone, and Unguard gives a released part what the tracking wants by then (read-only
    // while armed, else the guest's protection), so a write after the release still faults into
    // HandleWrite and is tracked. Refused: a page mapped at several guest addresses (a read through
    // another alias would not fault), one a host write holds open, a pinned one (a fiber stack) and
    // one the guest made inaccessible.
    enum class GuardFit { Private, Views, Mixed, Aliased, HostWrite, Pinned, Inaccessible };
    // How [begin, end) (whole 4 KiB parts) lies against the views: none (Private), only views that
    // may be guarded (Views), both (Mixed), or the first reason a view may not be.
    GuardFit FitGuard(std::uintptr_t begin, std::uintptr_t end) {
        std::lock_guard lock(mutex);
        return fitGuard(begin, end);
    }

    // Makes the parts of [begin, end) no-access and remembers them as guarded; false (nothing
    // changed) unless every page is a view FitGuard admits.
    bool Guard(std::uintptr_t begin, std::uintptr_t end) {
        std::lock_guard lock(mutex);
        if (fitGuard(begin, end) != GuardFit::Views) return false;
        std::vector<std::pair<std::uintptr_t, std::uint8_t>> done;
        for (auto base = begin & ~(pageBytes - 1); base < end; base += pageBytes) {
            auto& view = views.at(base);
            const auto parts = partsWithin(base, begin, end) & static_cast<std::uint8_t>(~view.guarded);
            if (parts != 0 && !protectParts(base, parts, PAGE_NOACCESS)) {
                // This page's runs that did turn no-access, and the pages before it.
                protectParts(base, parts, view.armed ? armedProtection(view.protection) : view.protection);
                for (const auto& [undone, mask] : done) {
                    auto& other = views.at(undone);
                    other.guarded = static_cast<std::uint8_t>(other.guarded & ~mask);
                    protectParts(undone, mask, other.armed ? armedProtection(other.protection) : other.protection);
                }
                return false;
            }
            view.guarded = static_cast<std::uint8_t>(view.guarded | parts);
            done.emplace_back(base, parts);
        }
        return true;
    }

    // Releases the guarded parts of [begin, end): each gets the protection the write tracking wants
    // now. Pages that are no views any more (unmapped or mapped again since) are left alone.
    void Unguard(std::uintptr_t begin, std::uintptr_t end) {
        std::lock_guard lock(mutex);
        for (auto base = begin & ~(pageBytes - 1); base < end; base += pageBytes) {
            const auto found = views.find(base);
            if (found == views.end()) continue;
            auto& view = found->second;
            const auto parts = static_cast<std::uint8_t>(partsWithin(base, begin, end) & view.guarded);
            if (parts == 0) continue;
            view.guarded = static_cast<std::uint8_t>(view.guarded & ~parts);
            protectParts(base, parts, view.armed ? armedProtection(view.protection) : view.protection);
        }
    }

    // Whether a shared view lies in [address, address + bytes).
    bool HasView(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto found = views.lower_bound(address & ~(pageBytes - 1));
        return found != views.end() && found->first < address + bytes;
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        std::lock_guard lock(mutex);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
        std::lock_guard lock(mutex);
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        for (auto cursor = address; cursor < end;) {
            const auto nextClean = cleanRanges.upper_bound(cursor);
            if (nextClean != cleanRanges.begin()) {
                const auto clean = std::prev(nextClean);
                if (cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found != views.end()) {
                auto& view = found->second;
                const auto stop = std::min(end, base + pageBytes);
                if (view.protection == PAGE_NOACCESS) return false;
                const bool pinned = view.page->pins != 0;
                if (pinned || view.seen != view.page->generation) {
                    const auto needed = (stop - cursor + 4095) / 4096;
                    if (needed > capacity - *count) {
                        for (auto at = cursor; *count < capacity; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                        return true;
                    }
                    for (auto at = cursor; at < stop; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                }
                if (clear && !pinned) {
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (!writable(other.protection) || other.armed || other.hostWrites != 0) continue;
                        // A page guard's parts stay no-access; Unguard arms them once released.
                        protectUnguarded(alias, other.guarded, armedProtection(other.protection), "arm shared memory write tracking");
                        other.armed = true;
                    }
                    view.seen = view.page->generation;
                    rememberClean(base, base + pageBytes);
                }
                cursor = stop;
            } else {
                const auto memory = queryRegion(cursor);
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                // A placeholder holds no memory, so nothing stores there: unwritten (a later commit over it
                // is reported through freshRanges, a shared mapping through its unseen views).
                // APS5_STRICT_COLLECT=1 fails such ranges as before (they go to the byte compare).
                static const bool strict = std::getenv("APS5_STRICT_COLLECT") != nullptr;
                if (memory.State == MEM_RESERVE && !strict) {
                    cursor = stop;
                    continue;
                }
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                auto watchStop = stop;
                if (const auto next = freshRanges.lower_bound(cursor); next != freshRanges.end()) watchStop = std::min(watchStop, std::max(cursor, next->first));
                if (const auto fresh = rangeAt(freshRanges, cursor); fresh != freshRanges.end()) {
                    // Every page of the fresh part is reported written, once.
                    const auto freshStop = std::min(stop, fresh->second);
                    auto at = cursor & ~static_cast<std::uintptr_t>(4095);
                    for (; at < freshStop && *count < capacity; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                    if (clear) {
                        ResetWriteWatch(reinterpret_cast<void*>(cursor), at - cursor);
                        forgetRange(freshRanges, cursor, at);
                    }
                    if (*count == capacity) return true;
                    cursor = freshStop;
                    continue;
                }
                ULONG_PTR available = capacity - *count;
                if (available == 0) return true;
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), watchStop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                *count += available;
                if (*count == capacity) return true;
                cursor = watchStop;
            }
        }
        return true;
    }

private:
    static constexpr std::size_t pageBytes = 0x4000;
    struct SharedPage {
        std::uint64_t generation = 1;
        std::vector<std::uintptr_t> aliases;
        std::uint32_t pins = 0;
    };
    struct Section {
        HANDLE handle;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::uint64_t seen;
        bool armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::uint32_t hostWrites;
        // The 4 KiB parts (bit i: [i * 4 KiB, (i + 1) * 4 KiB) of the page) a page guard holds
        // no-access (Guard/Unguard).
        std::uint8_t guarded = 0;
    };

    static constexpr std::size_t guardPartBytes = 0x1000;
    static constexpr std::size_t guardParts = pageBytes / guardPartBytes;
    static_assert(guardParts <= 8, "a view's guarded parts must fit its mask");

    static std::uint8_t guardPartBit(std::uintptr_t offset) {
        return static_cast<std::uint8_t>(1u << (offset / guardPartBytes));
    }

    // The parts of the view page at `base` that [begin, end) covers.
    static std::uint8_t partsWithin(std::uintptr_t base, std::uintptr_t begin, std::uintptr_t end) {
        std::uint8_t parts = 0;
        for (std::size_t part = 0; part < guardParts; ++part) {
            const auto from = base + part * guardPartBytes;
            if (from >= begin && from + guardPartBytes <= end) parts = static_cast<std::uint8_t>(parts | (1u << part));
        }
        return parts;
    }

    static DWORD armedProtection(DWORD protection) {
        return protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
    }

    // VirtualProtect over the parts of the view page at `base` set in `parts`, run by run.
    static bool protectParts(std::uintptr_t base, std::uint8_t parts, DWORD protection) {
        bool changed = true;
        for (std::size_t part = 0; part < guardParts;) {
            if ((parts & (1u << part)) == 0) {
                ++part;
                continue;
            }
            auto last = part;
            while (last < guardParts && (parts & (1u << last)) != 0) ++last;
            DWORD previous;
            changed = VirtualProtect(reinterpret_cast<void*>(base + part * guardPartBytes), (last - part) * guardPartBytes, protection, &previous) != 0 && changed;
            part = last;
        }
        return changed;
    }

    // VirtualProtect over [from, to) of the view page at `base` (the whole page by default) less the
    // parts a page guard holds; throws as the plain call it replaces did.
    static void protectUnguarded(std::uintptr_t base, std::uint8_t guarded, DWORD protection, const char* what, std::uintptr_t from = 0, std::uintptr_t to = ~std::uintptr_t{0}) {
        from = std::max(from, base);
        to = std::min(to, base + pageBytes);
        if (guarded == 0) {
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(from), to - from, protection, &previous)) fail(what);
            return;
        }
        for (auto cursor = from; cursor < to;) {
            const auto part = (cursor - base) / guardPartBytes;
            const auto partEnd = std::min(to, base + (part + 1) * guardPartBytes);
            if ((guarded & (1u << part)) == 0) {
                // Up to the next guarded part (or the end), in one call.
                auto stop = partEnd;
                while (stop < to && (guarded & (1u << ((stop - base) / guardPartBytes))) == 0) stop = std::min(to, stop + guardPartBytes);
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail(what);
                cursor = stop;
            } else {
                cursor = partEnd;
            }
        }
    }

    GuardFit fitGuard(std::uintptr_t begin, std::uintptr_t end) const {
        bool view = false;
        bool other = false;
        for (auto base = begin & ~(pageBytes - 1); base < end; base += pageBytes) {
            const auto found = views.find(base);
            if (found == views.end()) {
                other = true;
                continue;
            }
            view = true;
            const auto& entry = found->second;
            if (entry.page->aliases.size() != 1) return GuardFit::Aliased;
            if (entry.hostWrites != 0) return GuardFit::HostWrite;
            if (entry.page->pins != 0) return GuardFit::Pinned;
            if (entry.protection == PAGE_NOACCESS) return GuardFit::Inaccessible;
        }
        return view && other ? GuardFit::Mixed : view ? GuardFit::Views : GuardFit::Private;
    }
    static void forgetRange(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second > start) --it;
        while (it != ranges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = ranges.erase(it);
            if (first < start) ranges.emplace(first, start);
            if (last > end) it = ranges.emplace(end, last).first;
        }
    }

    static void rememberRange(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second >= start) --it;
        while (it != ranges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = ranges.erase(it);
        }
        ranges.emplace(start, end);
    }

    // The range of `ranges` holding `cursor`, or end().
    static std::map<std::uintptr_t, std::uintptr_t>::const_iterator rangeAt(const std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t cursor) {
        const auto next = ranges.upper_bound(cursor);
        if (next == ranges.begin()) return ranges.end();
        const auto found = std::prev(next);
        return cursor < found->second ? found : ranges.end();
    }

    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        forgetRange(cleanRanges, start, end);
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        rememberRange(cleanRanges, start, end);
    }

    void invalidate(SharedPage& page) {
        ++page.generation;
        for (const auto alias : page.aliases) forgetClean(alias, alias + pageBytes);
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
    }

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    // Collect's region query, answered from the regions it queried before while the mapping serial
    // is the one they were queried under (caller holds the mutex; every commit over a placeholder,
    // reset and shared map bumps the serial under it). Collect reads only the state, the type and
    // the region's end; what changes them inside the arena goes through those bumps. A protection
    // change (VirtualProtect, the kernel's mprotect) can split a private region into several, which
    // a cached answer then spans: the span stays one MEM_WRITE_WATCH allocation in one state, so its
    // write watch is read in one call instead of one per protection run (a VirtualQuery walk of the
    // process's VAD tree was ~3% of the queue-0 thread, t339). APS5_NO_REGION_CACHE=1 queries always.
    MEMORY_BASIC_INFORMATION queryRegion(std::uintptr_t address) {
        static const bool disabled = std::getenv("APS5_NO_REGION_CACHE") != nullptr;
        if (disabled) return query(address);
        const auto serial = mappingSerial.load(std::memory_order_relaxed);
        if (serial != regionSerial) {
            regions.clear();
            regionSerial = serial;
        }
        if (const auto next = regions.upper_bound(address); next != regions.begin()) {
            const auto found = std::prev(next);
            if (address < found->second.end) {
                MEMORY_BASIC_INFORMATION memory{};
                memory.BaseAddress = reinterpret_cast<void*>(found->first);
                memory.RegionSize = found->second.end - found->first;
                memory.State = found->second.state;
                memory.Type = found->second.type;
                return memory;
            }
        }
        const auto memory = query(address);
        if (regions.size() >= 16384) regions.clear();
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        regions[base] = CachedRegion{base + memory.RegionSize, memory.State, memory.Type};
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        const auto end = address + bytes;
        forgetClean(address, end);
        forgetRange(freshRanges, address, end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                const auto found = views.find(cursor);
                if (found != views.end()) {
                    auto& page = *found->second.page;
                    if (const auto pinned = pinnedPages.find(cursor); pinned != pinnedPages.end()) page.pins -= std::min(page.pins, pinned->second);
                    std::erase(page.aliases, cursor);
                    views.erase(found);
                }
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    std::map<std::uintptr_t, std::uintptr_t> cleanRanges;
    // Private memory committed over a placeholder since the last collect: Collect reports a placeholder
    // as unwritten (nothing can store there), so the commit that makes it memory (zeros, unseen by the
    // write watch) is reported as a write once.
    std::map<std::uintptr_t, std::uintptr_t> freshRanges;
    // Pin counts by shared page address (Pin/Unpin), carried onto the view mapped there.
    std::map<std::uintptr_t, std::uint32_t> pinnedPages;
    std::map<std::uintptr_t, View> views;
    // MapSpanAlias's reservations: base -> the pieces mapped into it.
    std::map<void*, std::vector<void*>> spanAliases;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::mutex mutex;
    std::atomic<std::uint64_t> mappingSerial{0};
    // queryRegion's answers (base -> end, state, type), valid while mappingSerial is regionSerial.
    struct CachedRegion {
        std::uintptr_t end;
        DWORD state;
        DWORD type;
    };
    std::map<std::uintptr_t, CachedRegion> regions;
    std::uint64_t regionSerial = ~std::uint64_t{0};
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
