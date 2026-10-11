#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::size_t Block = 65536;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

void CheckSharedBlock() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block - 0xb800;
    const auto firstBegin = base;
    const auto secondEnd = base + 3 * Block;
    const auto sharedFirst = boundary - (base + Block);
    const auto sharedSecond = base + 2 * Block - boundary;
    std::memset(memory, 0x11, 3 * Block);

    const auto synced = CollectWrites(base, 3 * Block);
    Require(synced != 0, "the test block is not collected");

    Require(MarkWritten(firstBegin, static_cast<std::size_t>(boundary - firstBegin)) != 0, "a driver store into watched memory is not stamped");
    std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    Require(ChangedBlocks(base + Block, 2 * Block, generations, changed) && changed[0] == BlockWritten && changed[1] == BlockUnchanged, "the shared block does not read as written");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store of the neighbour's bytes counts for the second surface's part of the shared block");
    Require(StoredOver(base + Block, static_cast<std::size_t>(sharedFirst), synced), "the first surface's own store is not seen over its part of the shared block");
    Require(!StoredOver(boundary, static_cast<std::size_t>(secondEnd - boundary), synced), "the second surface reads as stored over");

    const auto touched = MarkWritten(boundary + 0x100, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store over the second surface's bytes is not seen");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), touched), "a store older than the generation is seen");

    const auto whole = TrackerGeneration();
    MarkWritten(base + Block, Block);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), whole), "a store over the whole block is not seen");
    const auto afterWhole = TrackerGeneration();
    MarkWritten(base + Block, static_cast<std::size_t>(sharedFirst));
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), afterWhole), "a partial store after a whole-block store is taken for the whole block");

    const auto many = TrackerGeneration();
    for (int store = 0; store < 6; ++store) MarkWritten(base + Block + static_cast<std::uint64_t>(store) * 64, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), many), "forgotten driver stores are taken as missing the range");

    const auto beforeCpu = CollectWrites(base, 3 * Block);
    static_cast<volatile std::uint8_t*>(memory)[Block + 8] = 0x22;
    CollectWritesUncached(base, 3 * Block);
    // APS5_NO_EDGE_PAGE_KEEP=1 restores the whole-block answer: the store anywhere in the block counts.
    if (std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr) {
        Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), beforeCpu), "a CPU store in the shared block is not seen with APS5_NO_EDGE_PAGE_KEEP");
    } else {
        Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), beforeCpu), "a CPU store outside the shared range counts for it");
    }

    std::array<std::uint8_t, 64> unwatched{};
    const auto outside = reinterpret_cast<std::uint64_t>(unwatched.data());
    if (!Watched(outside, unwatched.size())) Require(StoredOver(outside, unwatched.size(), TrackerGeneration()), "an unwatched range reads as not stored over");
}

// APS5_FIX_BOOT_STRIP: a store into one page of a block leaves the block's other pages to a
// surface sharing it (the Bink chroma plane's last 14 pages and the CPU's table after them).
void CheckUnstoredPages() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the unstored-pages block is not collected");
    const char* fix = std::getenv("APS5_FIX_BOOT_STRIP");
    const bool off = (fix != nullptr && std::string(fix) == "0") || std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr;
    constexpr std::uint32_t all = 0xffff;
    constexpr std::uint32_t surface = 0x3fff;
    constexpr std::size_t surfaceBytes = 14 * 4096;
    Require(PagesUnstoredSince(base, surfaceBytes, synced) == (off ? 0u : surface), "pages of an unwritten block read as stored");

    bytes[14 * 4096 + 8] = 0x22;
    CollectWritesUncached(base, 2 * Block);
    std::array<std::uint64_t, 1> generations{synced};
    std::array<std::uint8_t, 1> changed{};
    Require(ChangedBlocks(base, Block, generations, changed) && changed[0] == BlockWritten, "the table store does not mark its block written");
    if (off) {
        Require(PagesUnstoredSince(base, surfaceBytes, synced) == 0, "pages are kept with APS5_FIX_BOOT_STRIP=0 or APS5_NO_EDGE_PAGE_KEEP");
        return;
    }
    Require(PagesUnstoredSince(base, surfaceBytes, synced) == surface, "a CPU store into the table's page drops the surface's pages");
    Require(PagesUnstoredSince(base, Block, synced) == (all & ~(1u << 14)), "the table's stored page reads as unstored");

    MarkWritten(base + 2 * 4096 + 16, 32);
    Require(PagesUnstoredSince(base, surfaceBytes, synced) == (surface & ~(1u << 2)), "a driver piece in the surface is not seen");
    Require(PagesUnstoredSince(base + 4096, 4096, synced) == (1u << 1), "a one-page range answers for other pages");
    Require(PagesUnstoredSince(base, Block, TrackerGeneration()) == all, "stores older than the generation are seen");

    // A whole-block driver store into a noted block, a piece after it, and a ring overflow.
    const auto beforeNotedWhole = TrackerGeneration();
    MarkWritten(base, Block);
    Require(PagesUnstoredSince(base, surfaceBytes, beforeNotedWhole) == 0, "a whole-block driver store into a noted block is missed");
    const auto afterNotedWhole = TrackerGeneration();
    MarkWritten(base + 3 * 4096 + 8, 8);
    Require(PagesUnstoredSince(base, surfaceBytes, afterNotedWhole) == (surface & ~(1u << 3)), "a piece after a whole-block store is taken for the whole block");
    // More small driver stores than the pieces ring holds are still told apart by page.
    const auto beforeRing = TrackerGeneration();
    for (int store = 0; store < 4; ++store) MarkWritten(base + 4 * 4096 + static_cast<std::uint64_t>(store) * 64, 4);
    Require(PagesUnstoredSince(base, surfaceBytes, beforeRing) == (surface & ~(1u << 4)), "four pieces in one page are not told apart");
    for (int store = 0; store < 8; ++store) MarkWritten(base + 14 * 4096 + static_cast<std::uint64_t>(store) * 512, 8);
    PageStoreCause cause = PageStoreCause::None;
    Require(PagesUnstoredSince(base, surfaceBytes, beforeRing, &cause) == (surface & ~(1u << 4)) && cause == PageStoreCause::DriverPages, "driver stores pushed out of the ring are taken for the surface's pages");

    const auto beforeWhole = TrackerGeneration();
    MarkWritten(base + Block, Block);
    bytes[Block + 15 * 4096] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(PagesUnstoredSince(base + Block, surfaceBytes, beforeWhole) == 0, "a whole-block driver store into an unnoted block is missed");
    Require(PagesUnstoredSince(base + Block - 4096, 2 * 4096, synced) == 0, "a range across two blocks is answered");
}

// APS5_FIX_BOOT_STRIP: a mixed edge block, the surface's tail and a table the CPU and the driver
// both write, some of the CPU's stores landing in the surface's pages too: the write-back's
// per-page merge stores exactly the surface pages no store reached; a whole-block driver store
// since leaves nothing to merge.
void CheckMixedEdge() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the mixed edge block is not collected");
    const char* fix = std::getenv("APS5_FIX_BOOT_STRIP");
    const bool off = (fix != nullptr && std::string(fix) == "0") || std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr;
    constexpr std::size_t surfaceBytes = 14 * 4096;
    bytes[14 * 4096 + 8] = 0x22;
    bytes[13 * 4096 + 100] = 0x22;
    for (int store = 0; store < 6; ++store) MarkWritten(base + 15 * 4096 + static_cast<std::uint64_t>(store) * 256, 16);
    MarkWritten(base + 6 * 4096 + 32, 16);
    CollectWritesUncached(base, 2 * Block);
    Require(StoredOver(base, surfaceBytes, synced), "the mixed block's surface part does not read as stored over");
    PageStoreCause cause = PageStoreCause::None;
    const auto gpu = PagesUnstoredSince(base, surfaceBytes, synced, &cause);
    if (off) {
        Require(gpu == 0 && cause == PageStoreCause::None, "a mixed block merges with APS5_FIX_BOOT_STRIP=0 or APS5_NO_EDGE_PAGE_KEEP");
        return;
    }
    Require(gpu == (0x3fffu & ~(1u << 13) & ~(1u << 6)) && cause == PageStoreCause::CpuPages, "the mixed block's unstored surface pages are wrong");
    const auto beforeWhole = TrackerGeneration();
    MarkWritten(base, Block);
    bytes[15 * 4096] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(PagesUnstoredSince(base, surfaceBytes, beforeWhole, &cause) == 0 && cause == PageStoreCause::DriverWhole, "a whole-block driver store leaves pages to merge");
    const auto later = TrackerGeneration();
    Require(PagesUnstoredSince(base + Block, surfaceBytes, later, &cause) == 0x3fffu && cause == PageStoreCause::None, "an unwritten block has stored pages");
}

void CheckOwnStore() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto second = base + Block;
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the own-store block is not collected");

    std::array<std::byte, 8192> bytes{};
    bytes.fill(std::byte{0x44});
    Write(second, bytes);
    const auto stored = TrackerGeneration();
    Require(!UnchangedSince(second, bytes.size(), synced), "the driver store is not stamped");
    CollectWritesUncached(base, 2 * Block);
    Require(UnchangedSince(second, Block, stored), "a walk after a driver store reports the store again as a newer write");
    Require(UnchangedSinceCollected(second, bytes.size(), synced), "the driver store's own page faults are stamped as a CPU write");

    static_cast<volatile std::uint8_t*>(memory)[Block + 3 * 4096 + 8] = 0x22;
    const std::array<std::byte, 64> label{};
    Write(second + 3 * 4096 + 64, label);
    Require(!UnchangedSinceCollected(second + 3 * 4096, 4096, stored), "a CPU write before a driver store in its page is not stamped as one");

    const auto beforeCpu = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[16] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(!UnchangedSince(base, 64, beforeCpu), "a CPU write after the driver store is not seen");
}

void CheckEqualsSince() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 4 * Block);
    std::vector<std::byte> snapshot(4 * Block);
    std::memcpy(snapshot.data(), memory, snapshot.size());
    const auto synced = CollectWrites(base, 4 * Block);
    Require(EqualsCommittedSince(base, snapshot, synced), "unchanged memory differs from its snapshot");

    bytes[2 * Block + 100] = 0x22;
    CollectWritesUncached(base, 4 * Block);
    Require(!EqualsCommittedSince(base, snapshot, synced), "a CPU write into a stamped block is not seen");
    Require(!EqualsCommittedSince(base, snapshot, 0), "a whole compare (generation 0) misses the write");

    bytes[2 * Block + 100] = 0x11;
    CollectWritesUncached(base, 4 * Block);
    Require(EqualsCommittedSince(base, snapshot, synced), "a stamped block written back to its old bytes differs");

    const auto partial = std::span<const std::byte>(snapshot).subspan(Block / 2, 2 * Block);
    Write(base + 3 * Block, std::array<std::byte, 16>{});
    Require(EqualsCommittedSince(base + Block / 2, partial, synced), "a store outside the compared range is compared");
    Require(!EqualsCommittedSince(base, snapshot, synced), "a driver store into a stamped block is not seen");
}

// The crash report's store ring (APS5_CRASH_RESOURCES=1, set by main): the driver's stores of 16
// bytes or more over an address, newest first, with the thread's StoreTag and the stored span.
void CheckStoreRing() {
    Require(CrashResourcesEnabled(), "the store ring is off with APS5_CRASH_RESOURCES=1");
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0, Block);
    CollectWrites(base, Block);
    std::array<std::byte, 256> bytes{};
    bytes.fill(std::byte{0x5a});
    {
        const StoreTag tag("test image", "test reason", base);
        Write(base + 512, bytes);
    }
    MarkWritten(base + 4096, 64);
    Write(base + 8192, std::array<std::byte, 8>{});
    std::vector<std::byte> original(1024, std::byte{0});
    std::vector<std::byte> current(original);
    std::fill(current.begin() + 100, current.begin() + 300, std::byte{1});
    WriteChanged(base + 16384, current, original);

    std::array<GuestStoreRecord, 8> found{};
    auto count = RecentGuestStores(base + 600, 16, found);
    Require(count == 1 && found[0].begin == base + 512 && found[0].end == base + 768, "the ring does not hold the store over the address");
    Require(found[0].tag != nullptr && std::strcmp(found[0].tag, "test image") == 0 && found[0].reason != nullptr && std::strcmp(found[0].reason, "test reason") == 0 && found[0].tagAddress == base, "the store's tag is not recorded");
    Require(found[0].kind != nullptr && std::strcmp(found[0].kind, "driver memcpy") == 0, "a Write is not recorded as a driver memcpy");
    count = RecentGuestStores(base, Block, found);
    Require(count == 3, "the ring holds " + std::to_string(count) + " stores over the block, not 3 (an 8-byte store is below the recorded size)");
    Require(found[0].begin == base + 16384 + 100 && found[0].end == base + 16384 + 300, "WriteChanged is not recorded as its changed span");
    Require(found[1].begin == base + 4096 && found[1].end == base + 4096 + 64 && found[1].kind != nullptr && std::strcmp(found[1].kind, "gpu (MarkWritten)") == 0 && found[1].tag == nullptr, "MarkWritten is not recorded untagged after the tag's scope");
    Require(found[2].begin == base + 512, "the stores are not listed newest first");
    Require(RecentGuestStores(base + 768, 4096 - 768, found) == 0, "a store is listed outside its range");
}
}

int main() {
    // Before the first store: the ring is made then (CheckStoreRing).
#ifdef _WIN32
    _putenv_s("APS5_CRASH_RESOURCES", "1");
#else
    setenv("APS5_CRASH_RESOURCES", "1", 1);
#endif
    try {
        if (!WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return 77;
        }
        CheckStoreRing();
        CheckSharedBlock();
        CheckUnstoredPages();
        CheckMixedEdge();
        CheckOwnStore();
        CheckEqualsSince();
    } catch (const std::exception& error) {
        std::cerr << "write tracking test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "write tracking tests passed\n";
    return 0;
}
