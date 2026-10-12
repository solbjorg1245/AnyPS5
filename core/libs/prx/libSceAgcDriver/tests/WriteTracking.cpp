#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
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

// The DCC key store guard's question (DccMetadata.cpp): the CPU's stores into a range since a
// generation by 4 KiB page, without the driver's own stores, merged across a block boundary and
// clipped to the range.
void CheckCpuStoredRuns() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the CPU-stored runs block is not collected");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    Require(CpuStoredRuns(base, 2 * Block, synced, runs) == 0 && runs.empty(), "CPU stores older than the generation are listed");
    bytes[Block - 4096 + 8] = 0x22;
    bytes[Block + 8] = 0x22;
    bytes[Block + 3 * 4096 + 8] = 0x22;
    MarkWritten(base + 4096, 64);
    Write(base + 2 * 4096, std::array<std::byte, 16>{});
    CollectWritesUncached(base, 2 * Block);
    const auto stored = CpuStoredRuns(base + 100, 2 * Block - 200, synced, runs);
    // APS5_NO_EDGE_PAGE_KEEP=1 keeps no page records: a CPU-stamped block counts whole.
    if (std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr) {
        Require(stored == 2 * Block - 200 && runs.size() == 1, "CPU-stamped blocks do not count whole with APS5_NO_EDGE_PAGE_KEEP");
        return;
    }
    Require(stored == 3 * 4096 && runs.size() == 2, "the CPU's pages are not listed alone (" + std::to_string(stored) + " bytes in " + std::to_string(runs.size()) + " runs)");
    Require(runs[0].first == base + Block - 4096 && runs[0].second == base + Block + 4096, "CPU pages on both sides of a block boundary are not one run");
    Require(runs[1].first == base + Block + 3 * 4096 && runs[1].second == base + Block + 4 * 4096, "a lone CPU page is not listed");
    Require(CpuStoredRuns(base + Block - 100, 50, synced, runs) == 50 && runs.size() == 1 && runs[0].first == base + Block - 100 && runs[0].second == base + Block - 50, "a run is not clipped to the range");
    Require(CpuStoredRuns(base, 3 * 4096, synced, runs) == 0 && runs.empty(), "the driver's own stores are listed as the CPU's");
    Require(CpuStoredRuns(base, 2 * Block, TrackerGeneration(), runs) == 0 && runs.empty(), "CPU stores older than a later generation are listed");
    Require(CpuStoredRuns(base, 2 * Block, 0, runs) == 0 && runs.empty(), "a generation of 0 lists stores");
}

// The late store re-check (APS5_KEEP_CPU_STORES): a store decided at a generation leaves out the
// 4 KiB pages a CPU store reached since (not the driver's own stores, not older ones) and stores
// the rest in address order; with the switch off it stores the run whole and counts the pages
// ("would keep" on the digest).
void CheckKeepCpuStores() {
    using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    constexpr std::uint64_t page = 4096;
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, Block);
    CollectWritesUncached(base, Block);
    const auto decided = TrackerGeneration();
    Require(decided != 0, "the late store block is not collected");
    bytes[2 * page + 8] = 0x22;
    MarkWritten(base + 6 * page, 64);
    CollectWritesUncached(base, Block);
    SetKeepCpuStores(true);
    Runs out;
    if (std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr) {
        Require(KeepCpuStoredRun(LateStoreKind::ShadowPublish, decided, base, base + 8 * page, out) == 8 * page && out.empty(), "a CPU-stamped block is not kept whole with APS5_NO_EDGE_PAGE_KEEP");
        return;
    }
    Require(KeepCpuStoredRun(LateStoreKind::ShadowPublish, decided, base, base + 8 * page, out) == page && out == Runs{{base, base + 2 * page}, {base + 3 * page, base + 8 * page}}, "the CPU's page since the decision is not left out alone");
    Require(KeptCpuStoreSummary().find("late stores over CPU-stored pages: kept 1 (4 KiB)") != std::string::npos && KeptCpuStoreSummary().find("shadow 1 (4 KiB)") != std::string::npos, "the digest does not count the kept page by kind");
    out.clear();
    Require(KeepCpuStoredRun(LateStoreKind::WriteBack, decided, base + 2 * page + 100, base + 2 * page + 200, out) == 100 && out.empty(), "a run inside the CPU's page was stored");
    out.clear();
    Require(KeepCpuStoredRun(LateStoreKind::WriteBack, TrackerGeneration(), base, base + 8 * page, out) == 0 && out == Runs{{base, base + 8 * page}}, "a store decided after the CPU's lost bytes");
    out.clear();
    Require(KeepCpuStoredRun(LateStoreKind::WriteBack, 0, base, base + 8 * page, out) == 0 && out == Runs{{base, base + 8 * page}}, "a generation of 0 left bytes out");
    SetKeepCpuStores(false);
    out.clear();
    Require(KeepCpuStoredRun(LateStoreKind::ShadowPublish, decided, base, base + 8 * page, out) == page && out == Runs{{base, base + 8 * page}}, "APS5_KEEP_CPU_STORES=0 did not store the run whole, or did not count it");
    Require(KeptCpuStoreSummary().find("late stores over CPU-stored pages: would keep") != std::string::npos, "the digest does not say 'would keep' with the switch off");
    SetKeepCpuStores(true);
}

// The late-land check (APS5_LATE_LAND_CHECK): a GPU land decided at a generation finds the CPU's
// stores under its range since then: before the submit from the collected stamps alone, after it
// (in flight, waited) by a walk of the range and only since the submit's generation; never the
// driver's own stores or older ones. Level 0 checks nothing, level 1 not the title's fills/copies.
void CheckLateLands() {
    constexpr std::uint64_t page = 4096;
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x33, Block);
    CollectWritesUncached(base, Block);
    const auto decided = TrackerGeneration();
    Require(decided != 0, "the late land block is not collected");
    SetLateLandLevel(1);
    const auto land = MakeLateLand(LateLandKind::ShadowPublish, base, base + 8 * page, decided, "test");
    Require(CheckLateLand(land, LateLandPhase::Submit, 0, 1) == 0, "a land with no CPU store since its decision was reported");
    bytes[3 * page + 4] = 0x44;
    MarkWritten(base + 5 * page, 64);
    CollectWritesUncached(base, Block);
    if (std::getenv("APS5_NO_EDGE_PAGE_KEEP") != nullptr) {
        Require(CheckLateLand(land, LateLandPhase::Submit, 0, 1) == 8 * page, "a CPU-stamped block is not reported whole with APS5_NO_EDGE_PAGE_KEEP");
        return;
    }
    Require(CheckLateLand(land, LateLandPhase::Submit, 0, 1) == page, "the CPU's page since the decision was not found before the submit");
    const auto submitted = TrackerGeneration();
    Require(CheckLateLand(land, LateLandPhase::Finish, submitted, 1) == 0, "a store before the submit was counted in flight");
    bytes[6 * page + 8] = 0x55;
    Require(CheckLateLand(land, LateLandPhase::Finish, submitted, 1) == page, "the finish check did not walk to the store in flight");
    Require(CheckLandedNow(LateLandKind::TextureStore, base, base + 8 * page, TrackerGeneration(), "test") == 0, "a land decided after the stores was reported");
    Require(CheckLandedNow(LateLandKind::TextureStore, base, base + 8 * page, 0, "test") == 0, "an untracked land (generation 0) was reported");
    const auto summary = LateLandSummary();
    Require(summary.find("before submit 1 (4 KiB)") != std::string::npos && summary.find("in flight 1 (4 KiB)") != std::string::npos && summary.find("shadow 2 (8 KiB)") != std::string::npos, "the digest does not count the hits by phase and kind: " + summary);
    Require(LateLandChecked(LateLandKind::DccKeys) && !LateLandChecked(LateLandKind::Fill) && !LateLandChecked(LateLandKind::Copy) && !LateLandChecked(LateLandKind::CopyBack) && !LateLandChecked(LateLandKind::GpuWrite), "level 1 does not check the driver's lands alone");
    SetLateLandLevel(2);
    Require(LateLandChecked(LateLandKind::Fill) && LateLandChecked(LateLandKind::Copy) && LateLandChecked(LateLandKind::CopyBack) && LateLandChecked(LateLandKind::GpuWrite), "level 2 does not check the title's fills and copies, the copy-backs and the shaders' writes");
    SetLateLandLevel(0);
    Require(CheckLateLand(land, LateLandPhase::Submit, 0, 1) == 0 && LateLandSummary() == "late lands over CPU stores: off", "APS5_LATE_LAND_CHECK=0 still checks");
    SetLateLandLevel(1);
}

// The write-back range trace's switch (APS5_TRACE_WRITEBACK_RANGE=<lo>-<hi>, hex): the range is
// [lo, hi), and a store is traced only when one of its bytes lies inside it.
void CheckRangeTraceFilter() {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    Require(ParseTraceRange("0x3c0000000-0x3e0000000", begin, end) && begin == 0x3c0000000ull && end == 0x3e0000000ull, "the range switch is not parsed");
    Require(ParseTraceRange("1000-2000", begin, end) && begin == 0x1000 && end == 0x2000, "a range without 0x is not parsed as hex");
    for (const char* bad : {"", "0x1000", "0x1000-", "-0x1000", "0x2000-0x1000", "0x1000-0x1000", "0x1000--0x2000", "0x1000-0x2000x", " 0x1000-0x2000", "0x1000-0x2000 ", "0x-0x10"}) {
        Require(!ParseTraceRange(bad, begin, end), std::string("the range switch '") + bad + "' is accepted");
    }
    Require(!ParseTraceRange(nullptr, begin, end), "an unset range switch is accepted");
    constexpr std::uint64_t low = 0x1000;
    constexpr std::uint64_t high = 0x2000;
    Require(TraceRangeHits(low, high, low, 1) && TraceRangeHits(low, high, high - 1, 1), "the range's first or last byte is not traced");
    Require(!TraceRangeHits(low, high, high, 16), "a store at the range's end is traced (exclusive)");
    Require(!TraceRangeHits(low, high, low - 16, 16), "a store ending at the range's begin is traced");
    Require(TraceRangeHits(low, high, low - 16, 17) && TraceRangeHits(low, high, 0, ~0ull), "a store over the range's begin is not traced");
    Require(!TraceRangeHits(low, high, low, 0), "an empty store is traced");
    Require(!TraceRangeHits(low, high, ~0ull - 4, 4), "a store at the top of the address space is traced");
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

// The free log (APS5_DROP_FREED_STORES): a store whose source clock predates a release of its pages
// leaves those pages out, one whose source followed the release keeps them, and with the switch off
// every store is kept whole (and counted). A registry Remove releases, less what the same mutation
// registers again; a new registration or a protection change does not.
void CheckFreedStores() {
    using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    constexpr std::uint64_t page = 4096;
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    SetDropFreedStores(true);
    const auto before = FreeClock();
    Runs runs{{base, base + 4 * page}};
    Require(DropFreedRuns(FreedStoreKind::WriteBack, before, runs) == 0 && runs == Runs{{base, base + 4 * page}}, "a store with no release since its source lost bytes");
    NoteReleased(base + page + 100, 50);
    const auto released = FreeClock();
    if (released == before) {
        std::cout << "free log: the test block is outside the guest arena, skipped\n";
        return;
    }
    Require(DropFreedRuns(FreedStoreKind::WriteBack, before, runs) == page && runs == Runs{{base, base + page}, {base + 2 * page, base + 4 * page}}, "the page released after the store's source was stored");
    runs = {{base + page - 96, base + page + 32}};
    Require(DropFreedRuns(FreedStoreKind::ShadowPublish, before, runs) == 32 && runs == Runs{{base + page - 96, base + page}}, "a run is not cut at the released page's bound");
    runs = {{base, base + 4 * page}};
    Require(DropFreedRuns(FreedStoreKind::WriteBack, released, runs) == 0 && runs == Runs{{base, base + 4 * page}}, "a store whose source followed the release lost bytes");
    Require(DropFreedRuns(FreedStoreKind::WriteBack, NoFreeClock, runs) == 0 && runs.size() == 1, "a store of unknown age lost bytes");
    SetDropFreedStores(false);
    Require(DropFreedRuns(FreedStoreKind::KeyStore, before, runs) == page && runs == Runs{{base, base + 4 * page}}, "APS5_DROP_FREED_STORES=0 did not store the run whole, or did not count it");
    Require(FreedStoreSummary().find("would drop") != std::string::npos, "the digest does not say 'would drop' with the switch off");
    SetDropFreedStores(true);
    Require(FreedStoreSummary().find("stores into freed pages: dropped") != std::string::npos, "the digest does not count dropped stores");
    void* piece = reinterpret_cast<void*>(base + 8 * page);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(piece, 4 * page, true, true);
    }
    const auto added = FreeClock();
    Require(added == released, "a new registration moved the free clock");
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(piece, page, true, false, [] {});
    }
    Require(FreeClock() == added, "a protection change moved the free clock");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(piece);
        mutation.Add(piece, 2 * page, true, true);
    }
    const auto shrunk = FreeClock();
    Require(shrunk > added, "a shrinking re-registration did not move the free clock");
    runs = {{base + 8 * page, base + 12 * page}};
    Require(DropFreedRuns(FreedStoreKind::CopyBack, added, runs) == 2 * page && runs == Runs{{base + 8 * page, base + 10 * page}}, "a re-registration released the pages it kept, or kept those it gave back");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(piece);
    }
    Require(FreeClock() > shrunk, "a registry Remove did not move the free clock");
    runs = {{base + 8 * page, base + 12 * page}};
    Require(DropFreedRuns(FreedStoreKind::CopyBack, shrunk, runs) == 2 * page && runs == Runs{{base + 10 * page, base + 12 * page}}, "the removed allocation's pages were stored");
}

// The calling thread's collect epoch (ThreadCollectEpoch, ResetCollectEpoch; with
// APS5_RECORD_PIPE_EPOCH=1 the recording thread follows A's): without one every collect walks (each
// walk moves the generation); within one a repeated collect is the memo's (no walk); a bump walks again.
void CheckThreadEpoch() {
    void* memory = AllocateWatched(Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, Block);
    ResetCollectEpoch();
    Require(ThreadCollectEpoch() == 0, "a reset thread kept its collect epoch");
    const auto first = CollectWrites(base, Block);
    Require(first != 0 && CollectWrites(base, Block) != first, "a thread without a collect epoch reused a walk");
    BumpCollectEpoch();
    const auto epoch = ThreadCollectEpoch();
    Require(epoch != 0, "a bumped thread has no collect epoch");
    const auto walked = CollectWrites(base, Block);
    Require(CollectWrites(base, Block) == walked && ThreadCollectEpoch() == epoch, "a collect inside one epoch walked again");
    BumpCollectEpoch();
    Require(ThreadCollectEpoch() != epoch && CollectWrites(base, Block) != walked, "a bumped epoch reused the previous epoch's walk");
    ResetCollectEpoch();
    const auto again = CollectWrites(base, Block);
    Require(ThreadCollectEpoch() == 0 && CollectWrites(base, Block) != again, "a reset thread reused a walk");
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
        CheckRangeTraceFilter();
        CheckStoreRing();
        CheckFreedStores();
        CheckSharedBlock();
        CheckUnstoredPages();
        CheckMixedEdge();
        CheckOwnStore();
        CheckEqualsSince();
        CheckCpuStoredRuns();
        CheckKeepCpuStores();
        CheckLateLands();
        CheckThreadEpoch();
    } catch (const std::exception& error) {
        std::cerr << "write tracking test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "write tracking tests passed\n";
    return 0;
}
