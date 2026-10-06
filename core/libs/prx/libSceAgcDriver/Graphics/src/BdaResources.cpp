#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver::Graphics {

namespace {

// The page tables built per device, reused by a build whose ranges (guest ranges, device
// addresses, permissions) equal one of them: consecutive address-based builds map the same registered
// set through the same imports and mirrors, so the ~1200-entry table repeats. The shader only reads
// the table (its lookup keeps its state in function variables), and a batch in flight keeps its
// BdaResources, so sharing the buffer is safe. The cache holds the tables weakly: they never outlive
// the builds that own them (a static owner would destroy the buffer after its device, at exit or on
// device replacement, and would fail the tests' leak check), and with deferred lease release the
// previous builds are still in flight when the next one asks, which is what makes the hit rate.
// One table was enough while every build repeated the last one exactly; now the misaligned
// descriptor sub-ranges the GPU copies out of imports sit in pool staging buffers whose device
// addresses cycle through the few buffers the batches in flight leave free, and the three
// address-based programs of one queue interleave with the lease draws, so a table differs from the
// previous one and comes back a few builds later (69% misses in the wave-8 run, 4% before the GPU
// copies). Hence several recent tables, most recently used first, matched by a word hash before
// the byte compare. APS5_BDA_TABLE_CACHE_ENTRIES sets how many (default 8; 1 is the previous
// behaviour); APS5_NO_BDA_TABLE_CACHE=1 builds every table.
// A table built for (or found equal to) the cached address space's ranges carries the space's
// serial: a build served by that space alone binds it by the serial, with no hash and no compare.
struct TableEntry {
    std::uint64_t hash;
    std::vector<ShaderRecompiler::BdaAbi::Range> ranges;
    std::weak_ptr<Buffer> buffer;
    std::uint64_t spaceSerial = 0;
};

struct TableCache {
    HostMutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::list<TableEntry> entries;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    BdaResources::TableCacheStats classes;
};

bool sameRanges(const std::vector<ShaderRecompiler::BdaAbi::Range>& left, const std::vector<ShaderRecompiler::BdaAbi::Range>& right);

// APS5_PROFILE_DRAW: why the most recently used entry did not serve this build (see
// TableCacheStats). Under the cache mutex.
void classifyFirstEntry(TableCache& cache, std::uint64_t hash, const std::vector<ShaderRecompiler::BdaAbi::Range>& ranges) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& classes = cache.classes;
    if (cache.entries.empty()) {
        ++classes.firstEmpty;
        return;
    }
    const auto& first = cache.entries.front();
    if (first.hash == hash) {
        if (first.buffer.expired()) ++classes.firstExpired;
        else if (!sameRanges(first.ranges, ranges)) ++classes.firstSameHash;
        return;
    }
    if (first.buffer.expired()) {
        ++classes.firstExpired;
        return;
    }
    const auto common = std::min(first.ranges.size(), ranges.size());
    std::size_t at = 0;
    while (at < common && std::memcmp(&first.ranges[at], &ranges[at], sizeof(ranges[at])) == 0) ++at;
    const auto begin = at < ranges.size() ? ranges[at].begin : at < first.ranges.size() ? first.ranges[at].begin : 0;
    ++(begin < 0x10000000ull ? classes.firstDiffersLow : classes.firstDiffersHeap);
}

TableCache& Tables() {
    static TableCache cache;
    return cache;
}

bool tableCacheEnabled() {
    static const bool disabled = std::getenv("APS5_NO_BDA_TABLE_CACHE") != nullptr;
    return !disabled;
}

std::size_t tableCacheEntries() {
    static const std::size_t entries = [] {
        const char* value = std::getenv("APS5_BDA_TABLE_CACHE_ENTRIES");
        const auto parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 8ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, 64));
    }();
    return entries;
}

bool sameRanges(const std::vector<ShaderRecompiler::BdaAbi::Range>& left, const std::vector<ShaderRecompiler::BdaAbi::Range>& right) {
    return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(left.front())) == 0);
}

// A 64-bit mix of the table's words: a table is ~1200 ranges of four words, and comparing the bytes
// against every held table would cost as much as building it.
std::uint64_t hashRanges(const std::vector<ShaderRecompiler::BdaAbi::Range>& ranges) {
    static_assert(sizeof(ShaderRecompiler::BdaAbi::Range) % sizeof(std::uint64_t) == 0);
    std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ ranges.size();
    for (const auto& range : ranges) {
        std::uint64_t words[sizeof(range) / sizeof(std::uint64_t)];
        std::memcpy(words, &range, sizeof(range));
        for (const auto word : words) {
            hash ^= word;
            hash *= 0xff51afd7ed558ccdull;
            hash ^= hash >> 33u;
        }
    }
    return hash;
}

bool checkTables() {
    static const bool check = std::getenv("APS5_CHECK_STALE_IMPORTS") != nullptr;
    return check;
}

struct LiveTable {
    std::weak_ptr<Buffer> buffer;
    std::vector<ShaderRecompiler::BdaAbi::Range> ranges;
    std::chrono::steady_clock::time_point made;
};

struct LiveTables {
    HostMutex mutex;
    std::list<LiveTable> tables;
};

LiveTables& Live() {
    static auto* live = new LiveTables();
    return *live;
}

void noteLiveTable(const std::shared_ptr<Buffer>& table, const std::vector<ShaderRecompiler::BdaAbi::Range>& ranges) {
    if (!checkTables()) return;
    auto& live = Live();
    std::lock_guard lock(live.mutex);
    live.tables.push_back({table, ranges, std::chrono::steady_clock::now()});
}

// The stack slots that point into the driver's code, as offsets (addr2line -f -C -e
// libSceAgcDriver.prx with the image base added), from the caller outwards.
void reportDriverFrames() {
#ifdef _WIN32
    HMODULE driver = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&reportDriverFrames), &driver)) return;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), driver, &info, sizeof(info))) return;
    const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
    const auto end = base + info.SizeOfImage;
    std::fprintf(stderr, "[bda-check]   driver addresses on the destroying thread's stack:");
    const auto* slot = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    int printed = 0;
    for (int i = 0; i < 4096 && printed < 32; ++i) {
        const auto value = slot[i];
        if (value <= base || value >= end) continue;
        std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(value - base));
        ++printed;
    }
    std::fprintf(stderr, "%s", "\n");
#endif
}

}

void CheckDestroyedImport(VkDeviceAddress address, std::uint64_t bytes, std::uint64_t guestBase) {
    if (!checkTables() || address == 0) return;
    static std::atomic<int> reports{0};
    auto& live = Live();
    std::lock_guard lock(live.mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = live.tables.begin(); it != live.tables.end();) {
        const auto owners = it->buffer.use_count();
        if (owners == 0) {
            it = live.tables.erase(it);
            continue;
        }
        const auto mapped = std::find_if(it->ranges.begin(), it->ranges.end(), [&](const ShaderRecompiler::BdaAbi::Range& range) { return range.deviceAddress < address + bytes && address < range.deviceAddress + (range.end - range.begin); });
        if (mapped != it->ranges.end() && reports.fetch_add(1) < 12) {
            std::fprintf(stderr, "[bda-check] import guest 0x%llx+0x%llx (device 0x%llx) destroyed while a live page table (%ld owners, made %.3f s ago, %zu ranges) maps guest 0x%llx+0x%llx to device 0x%llx%s", static_cast<unsigned long long>(guestBase), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(address), owners, std::chrono::duration<double>(now - it->made).count(), it->ranges.size(), static_cast<unsigned long long>(mapped->begin), static_cast<unsigned long long>(mapped->end - mapped->begin), static_cast<unsigned long long>(mapped->deviceAddress), "\n");
            reportDriverFrames();
            std::fflush(stderr);
        }
        ++it;
    }
}

namespace {
}

BdaResources::BdaResources(const Context& context) {
    static_assert(std::endian::native == std::endian::little);
    Require(ShaderRecompiler::BdaAbi::FaultBufferBytes <= context.limits.maxStorageBufferRange, "BDA fault buffer exceeds storage buffer range limit");
    fault = std::make_unique<Buffer>(context, ShaderRecompiler::BdaAbi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(fault->Bytes().data(), 0, fault->Bytes().size());
}

BdaResources::BdaResources(const Context& context, const GuestBufferMemory& memory) : BdaResources(context) {
    const auto cached = memory.CachedAddressTable();
    std::vector<ShaderRecompiler::BdaAbi::Range> built;
    if (!cached.has_value()) built = memory.AddressRanges();
    const auto& ranges = cached.has_value() ? *cached->ranges : built;
    const auto serial = cached.has_value() ? cached->serial : 0;
    // APS5_CHECK_STALE_IMPORTS: every mapped device range must be a live buffer's.
    if (checkTables()) {
        static std::atomic<int> reports{0};
        for (const auto& range : ranges) {
            if (range.end <= range.begin || DeviceAddressLive(range.deviceAddress, range.end - range.begin)) continue;
            if (reports.fetch_add(1) < 16) {
                std::fprintf(stderr, "[bda-check] page table (%s, space serial %llu, %zu ranges) maps guest 0x%llx+0x%llx to device 0x%llx, which is no live buffer:%s%s", cached.has_value() ? "the cached space's" : "built", static_cast<unsigned long long>(serial), ranges.size(), static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end - range.begin), static_cast<unsigned long long>(range.deviceAddress), DescribeDeviceAddress(range.deviceAddress).c_str(), "\n");
                reportDriverFrames();
                std::fflush(stderr);
            }
            break;
        }
    }
    Require(ranges.size() <= std::numeric_limits<std::uint32_t>::max(), "BDA table range count overflow");
    Require(ranges.size() <= (std::numeric_limits<std::size_t>::max() - sizeof(ShaderRecompiler::BdaAbi::Header)) / sizeof(ShaderRecompiler::BdaAbi::Range), "BDA table size overflow");
    tableBytes = sizeof(ShaderRecompiler::BdaAbi::Header) + ranges.size() * sizeof(ShaderRecompiler::BdaAbi::Range);
    Require(tableBytes <= context.limits.maxStorageBufferRange && ShaderRecompiler::BdaAbi::FaultBufferBytes <= context.limits.maxStorageBufferRange, "BDA descriptors exceed storage buffer range limit");
    auto& cache = Tables();
    std::uint64_t hash = 0;
    if (tableCacheEnabled()) {
        std::lock_guard lock(cache.mutex);
        // Another device's tables are not this one's (their buffers are dead or foreign).
        if (cache.device != context.device) {
            cache.entries.clear();
            cache.device = context.device;
        }
        if (serial != 0) {
            for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
                if (it->spaceSerial != serial) continue;
                if (auto shared = it->buffer.lock(); shared != nullptr) {
                    ++cache.hits;
                    ++cache.classes.spaceTables;
                    table = std::move(shared);
                    cache.entries.splice(cache.entries.begin(), cache.entries, it);
                    return;
                }
                cache.entries.erase(it);
                break;
            }
        }
        hash = hashRanges(ranges);
        classifyFirstEntry(cache, hash, ranges);
        for (auto it = cache.entries.begin(); it != cache.entries.end();) {
            // Compare the hash before taking a strong reference: locking a non-matching entry's
            // buffer could make this thread its last owner (a concurrent reap releasing it) and
            // run the pool release under the cache mutex; expired() takes no reference.
            if (it->hash != hash) {
                if (it->buffer.expired()) {
                    it = cache.entries.erase(it);
                } else {
                    ++it;
                }
                continue;
            }
            auto shared = it->buffer.lock();
            if (shared == nullptr) {
                // Every build that owned the table is gone, and its buffer with it.
                it = cache.entries.erase(it);
                continue;
            }
            if (sameRanges(it->ranges, ranges)) {
                ++cache.hits;
                // An equal table of an earlier space (a rebuild that mapped the same ranges the
                // same way): the next build of this space binds it by the serial.
                if (serial != 0) it->spaceSerial = serial;
                table = std::move(shared);
                cache.entries.splice(cache.entries.begin(), cache.entries, it);
                return;
            }
            ++it;
        }
    }
    table = std::make_shared<Buffer>(context, tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const ShaderRecompiler::BdaAbi::Header header{ShaderRecompiler::BdaAbi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(ShaderRecompiler::BdaAbi::Range), 0};
    std::memcpy(table->Bytes().data(), &header, sizeof(header));
    if (!ranges.empty()) std::memcpy(table->Bytes().data() + sizeof(header), ranges.data(), ranges.size() * sizeof(ranges.front()));
    noteLiveTable(table, ranges);
    if (tableCacheEnabled()) {
        std::lock_guard lock(cache.mutex);
        ++cache.misses;
        if (cache.device != context.device) {
            cache.entries.clear();
            cache.device = context.device;
        }
        cache.entries.push_front({hash, cached.has_value() ? ranges : std::move(built), table, serial});
        while (cache.entries.size() > tableCacheEntries()) cache.entries.pop_back();
    }
}

BdaResources::TableCacheStats BdaResources::TableCacheCounters() {
    auto& cache = Tables();
    std::lock_guard lock(cache.mutex);
    auto stats = cache.classes;
    stats.hits = cache.hits;
    stats.misses = cache.misses;
    stats.held = cache.entries.size();
    return stats;
}

VkDescriptorBufferInfo BdaResources::Table() const {
    Require(table != nullptr, "BDA page table was not requested");
    return {table->Handle(), 0, tableBytes};
}

VkDescriptorBufferInfo BdaResources::Fault() const {
    return {fault->Handle(), 0, ShaderRecompiler::BdaAbi::FaultBufferBytes};
}

namespace {
std::atomic<bool> loopGuardTripped{false};
}

bool LoopGuardTripped() {
    return loopGuardTripped.load(std::memory_order_relaxed);
}

void BdaResources::CheckFault() const {
    markWrittenPages();
    ShaderRecompiler::BdaAbi::Fault report{};
    std::memcpy(&report, fault->Bytes().data(), sizeof(report));
    if (report.state == ShaderRecompiler::BdaAbi::FaultState::Empty) {
        Require(static_cast<std::uint32_t>(report.reason) == 0 && report.address == 0 && report.bytes == 0 && report.stage == 0 && report.instruction == 0 && report.reserved == 0, "BDA fault record has data without publication");
        return;
    }
    Require(report.state == ShaderRecompiler::BdaAbi::FaultState::Ready && report.reserved == 0, "incomplete or invalid BDA fault record");
    Require(report.reason != ShaderRecompiler::BdaAbi::FaultReason::InvalidRectangle, "rect-list requires finite nondegenerate axis-aligned positions with equal positive W");
    if (report.reason == ShaderRecompiler::BdaAbi::FaultReason::LoopLimit) {
        // APS5_LOOP_GUARD: the shader left a loop that ran past the guard; the dispatch result is kept.
        loopGuardTripped.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[gpu] loop guard: the loop exit at pc 0x%x of shader 0x%llx ran past %u evaluations\n", report.instruction, static_cast<unsigned long long>(report.address), report.bytes);
        std::memset(fault->Bytes().data(), 0, fault->Bytes().size());
        return;
    }
    std::ostringstream message;
    message << "BDA access failed: address=0x" << std::hex << report.address << " instruction=0x" << report.instruction << std::dec << " bytes=" << report.bytes << " stage=" << report.stage << " reason=" << static_cast<std::uint32_t>(report.reason);
    if (report.reason == ShaderRecompiler::BdaAbi::FaultReason::Permission && table != nullptr) {
        const auto bytes = table->Bytes();
        ShaderRecompiler::BdaAbi::Header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        for (std::uint32_t index = 0; index < header.count; ++index) {
            ShaderRecompiler::BdaAbi::Range range{};
            std::memcpy(&range, bytes.data() + sizeof(header) + index * sizeof(range), sizeof(range));
            if (report.address < range.begin || report.address >= range.end) continue;
            message << std::hex << "; the store hit 0x" << range.begin << "+0x" << range.end - range.begin << ", read-only in the BDA table: stores through GPU-selected descriptors reach only writable ranges imported in place, not ones served by a mirror or a copy (past APS5_HOST_IMPORT_MIB, or refused by the driver)";
            break;
        }
    }
    throw std::runtime_error(message.str());
}

}

namespace AgcDriver::Graphics {

void BdaResources::markWrittenPages() const {
    namespace Abi = ShaderRecompiler::BdaAbi;
    auto* words = reinterpret_cast<std::uint32_t*>(fault->Bytes().data());
    Require(words[Abi::WrittenOverflowWord] == 0, "more than " + std::to_string(Abi::WrittenPageSlots) + " pages stored to through GPU-selected buffer descriptors in one use are not implemented");
    bool any = false;
    for (std::uint32_t slot = 0; slot < Abi::WrittenPageSlots; ++slot) {
        const auto page = words[Abi::WrittenSlotsWord + slot];
        if (page == 0) continue;
        GuestMemory::MarkWritten(static_cast<std::uint64_t>(page - 1u) << Abi::WrittenPageShift, std::size_t{1} << Abi::WrittenPageShift);
        any = true;
    }
    if (any) std::memset(words + Abi::WrittenSlotsWord, 0, Abi::WrittenPageSlots * sizeof(std::uint32_t));
}

}
