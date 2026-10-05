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
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
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

    bool HandleWrite(std::uintptr_t address) {
        std::lock_guard lock(mutex);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        invalidate(*view.page);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(base), pageBytes, view.protection, &previous)) fail("resume shared memory write");
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
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(it->first), pageBytes, view.protection, &previous)) fail("open shared memory to a host write");
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

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
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
        std::snprintf(text, size, "view protection 0x%lx seen %llu generation %llu armed %d host writes %u aliases %zu section offset 0x%llx (clean %d fresh %d)", view.protection, static_cast<unsigned long long>(view.seen), static_cast<unsigned long long>(view.page->generation), view.armed ? 1 : 0, view.hostWrites, view.page->aliases.size(), static_cast<unsigned long long>(view.offset), clean ? 1 : 0, fresh ? 1 : 0);
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
                if (view.seen != view.page->generation) {
                    const auto needed = (stop - cursor + 4095) / 4096;
                    if (needed > capacity - *count) {
                        for (auto at = cursor; *count < capacity; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                        return true;
                    }
                    for (auto at = cursor; at < stop; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                }
                if (clear) {
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (!writable(other.protection) || other.armed || other.hostWrites != 0) continue;
                        DWORD previous;
                        const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        if (!VirtualProtect(reinterpret_cast<void*>(alias), pageBytes, protection, &previous)) fail("arm shared memory write tracking");
                        other.armed = true;
                    }
                    view.seen = view.page->generation;
                    rememberClean(base, base + pageBytes);
                }
                cursor = stop;
            } else {
                const auto memory = query(cursor);
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
    };
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
                    std::erase(found->second.page->aliases, cursor);
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
    std::map<std::uintptr_t, View> views;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::mutex mutex;
    std::atomic<std::uint64_t> mappingSerial{0};
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
