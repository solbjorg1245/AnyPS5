#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VRAMBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VRAMBUDGET_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

// Video-memory budget (port/reports/s53-gpu3-vram.md). The driver's device-local memory (the heap
// its DEVICE_LOCAL allocations land in) is accounted by class at every vkAllocateMemory and
// vkFreeMemory of the driver, compared with a target, and kept under it by reclaiming what no
// work needs: released buffers the pool retains, cold cache entries (sampled textures, resource
// builds, staged shadows, resident read copies; "cold" = held by the cache alone), least recently
// used first. No object a recorded or submitted batch keeps is destroyed before that batch
// completed: an evicted cache entry whose memory a batch still keeps (a resident copy's buffer)
// only leaves the cache. Textures and builds are evicted only once they were not used for
// APS5_VRAM_MIN_AGE epochs (default 3; an epoch ends at a present at least 100 ms after the last,
// or after 500 ms without one), so the reclaimers never take a frame's working set and make it
// again the next frame. Allocations that have a fallback (staging shadows, resident read copies,
// pool retention, a new image-pool block) are refused while it stays over; the others are always
// made (the OOM reclaim of AllocateDeviceMemory still backs them).
// Before this, t421-t424 (16 GiB RTX 5080) collapsed at the Boletaria load at 15.3 GiB: textures
// and device buffers refused for minutes while the pool held ~2 GiB of released memory and ~12.8k
// device buffers stayed held by cached builds.
//
// Target: APS5_VRAM_BUDGET_MIB when set; else the heap budget VK_EXT_memory_budget reports (the
// heap size without the extension) minus a headroom (APS5_VRAM_HEADROOM_MIB, default 1/16 of the
// heap, or of APS5_VRAM_BUDGET_MIB when set, clamped to 256 MiB-1 GiB), never below
// min(heap / 4, 1 GiB). Used: the driver's reported heap usage (refreshed every 100 ms) plus what
// the accounting saw allocated and freed since, or the accounting alone without the extension.
// Free space inside the image pool's blocks ("slack") serves the next pooled texture and does not
// count as pressure up to half the headroom (so the heap stays at least half the headroom below
// the driver's budget); bytes of evicted objects that are certain to be freed once their batch
// completed ("pending") do not count either.
// APS5_NO_VRAM_BUDGET=1: the accounting and the [vram] line only (no extension, no reclaim, no
// refusal), the old behaviour. On an integrated GPU the budget only observes unless
// APS5_VRAM_UNIFIED is set (see ConfigureVram).
enum class VramClass : std::uint8_t {
    // Released device-local buffers the BufferPool's device tier retains for reuse.
    Pool,
    // Device-local buffers in use: staging shadows, detiler scratch, fill patterns.
    Buffers,
    // Resident read-only copies (APS5_RESIDENT_READS).
    Resident,
    // Sampled textures and storage images (dedicated allocations, the used part of image blocks).
    Textures,
    // Free space inside the image pool's blocks.
    Slack,
    // Render targets and depth surfaces.
    Targets,
    // Everything else the driver allocates device-local (unit shadows, presentation).
    Other,
};
inline constexpr std::size_t VramClassCount = 7;
const char* VramClassName(VramClass value);

// Where a reclaimer may run: Inline ones from inside any allocation on any thread (they take only
// a leaf lock, by try_lock); SafePoint ones only where the calling thread holds no cache lock
// (ResourceCache::Insert, the present) since they take the caches' locks (by try_lock as well) and
// destroy what they evicted.
// A cache's entries, those held by the cache alone ("cold": what a reclaimer may evict) and the
// device-local bytes the cold ones hold alone; `measured` false when the cache was busy.
struct VramCacheCensus {
    bool measured = false;
    std::uint64_t entries = 0;
    std::uint64_t cold = 0;
    VkDeviceSize coldBytes = 0;
};

enum class VramReclaimLevel : std::uint8_t { Inline = 0, SafePoint = 1 };

// What evicting a cache entry gives back (a build, VramBudget's "builds" reclaimer): `alone`, the
// device-local memory only it holds, freed when it is destroyed (pending until then); `shared`,
// the memory it holds with one other holder (a texture the texture cache also holds, a shadow the
// staging registry or a batch also holds), which its going leaves to that holder: another
// reclaimer's to free next, or freed when the batch completed.
struct VramHeld {
    VkDeviceSize alone = 0;
    VkDeviceSize shared = 0;
    VkDeviceSize Total() const { return alone + shared; }
    VramHeld& operator+=(const VramHeld& other) {
        alone += other.alone;
        shared += other.shared;
        return *this;
    }
};

class VramBudget {
public:
    struct Settings {
        bool enabled = true;
        VkDeviceSize target = 0;
        VkDeviceSize headroom = 0;
        std::uint32_t backoffMs = 50;
        // Epochs a texture or build must have gone unused before a reclaimer takes it
        // (APS5_VRAM_MIN_AGE; 0: any cold entry, as on f86d6b88).
        std::uint32_t minAge = 3;
        static Settings FromEnvironment();
    };
    explicit VramBudget(const Settings& settings);
    VramBudget(const VramBudget&) = delete;
    VramBudget& operator=(const VramBudget&) = delete;

    bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
    // What the settings said (APS5_NO_VRAM_BUDGET unset): ConfigureVram enforces only then.
    bool EnabledBySettings() const { return enabledBySettings; }
    // Tests: switch the policy, or the target override (0: derived again).
    void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
    void SetTargetOverride(VkDeviceSize bytes) { targetOverride.store(bytes, std::memory_order_relaxed); }
    void SetHeadroom(VkDeviceSize bytes) { headroomSetting.store(bytes, std::memory_order_relaxed); }
    void SetBackoffMs(std::uint32_t value) { backoffMs.store(value, std::memory_order_relaxed); }
    void SetMinAge(std::uint32_t value) { minAge.store(value, std::memory_order_relaxed); }
    std::uint32_t MinAge() const { return minAge.load(std::memory_order_relaxed); }
    // Milliseconds source for the backoff, the epochs and the safe-point spacing (tests);
    // steady_clock otherwise.
    void SetClock(std::function<std::uint64_t()> clock);

    // Epochs: the age the texture and build reclaimers judge by. The caches stamp an entry with
    // Epoch() at each use; an entry is old enough to evict once MinAge() epochs passed since
    // (Aged). AdvanceEpoch ends the epoch at a present at least 100 ms after the last end, or at
    // any safe point 500 ms after it (presents stalled); true when it ended one.
    std::uint64_t Epoch() const { return epoch.load(std::memory_order_relaxed); }
    bool AdvanceEpoch(bool present);
    bool Aged(std::uint64_t usedAt) const { return usedAt + MinAge() <= Epoch(); }

    // The budgeted heap's size (0: unknown) and the driver's report on it (VK_EXT_memory_budget).
    void SetHeap(VkDeviceSize bytes) { heap.store(bytes, std::memory_order_relaxed); }
    VkDeviceSize Heap() const { return heap.load(std::memory_order_relaxed); }
    void Report(VkDeviceSize budget, VkDeviceSize usage);
    void ClearReport();
    bool Reported() const { return reported.load(std::memory_order_acquire); }
    VkDeviceSize ReportedBudget() const { return reportedBudget.load(std::memory_order_relaxed); }
    VkDeviceSize ReportedUsage() const { return reportedUsage.load(std::memory_order_relaxed); }

    // Accounting by class.
    void Add(VramClass type, VkDeviceSize bytes);
    void Remove(VramClass type, VkDeviceSize bytes);
    void Move(VramClass from, VramClass to, VkDeviceSize bytes);
    VkDeviceSize Tracked(VramClass type) const;
    VkDeviceSize Tracked() const;

    // Per allocation (the driver's VkDeviceMemory handles): Note at the allocation, Reclass when
    // the pool takes or returns it, Forget before it is freed. Unknown handles are ignored.
    void NoteAllocation(VkDevice device, VkDeviceMemory memory, VramClass type, VkDeviceSize bytes);
    void Reclass(VkDevice device, VkDeviceMemory memory, VramClass type);
    void Forget(VkDevice device, VkDeviceMemory memory);
    std::size_t Allocations() const;

    VkDeviceSize Headroom() const;
    // The image pool's free space that counts as room: half the headroom (the other half stays
    // free below the driver's budget).
    VkDeviceSize SlackRoom() const { return Headroom() / 2; }
    // ~0 when nothing bounds it (no heap, no override).
    VkDeviceSize Target() const;
    VkDeviceSize Used() const;
    void AddPending(VkDeviceSize bytes);
    void RemovePending(VkDeviceSize bytes);
    VkDeviceSize Pending() const { return pending.load(std::memory_order_relaxed); }
    // Bytes over the target with `extra` more allocated (slack up to SlackRoom and pending not
    // counted); 0 within it or with the policy off.
    VkDeviceSize Excess(VkDeviceSize extra = 0) const;
    bool Over(VkDeviceSize extra = 0) const { return Excess(extra) != 0; }

    // A reclaimer frees up to about `want` bytes of its class (it may free less, or more by one
    // object), sets `evicted` to the objects it evicted and returns the bytes freed or about to be
    // (an estimate for objects whose destruction waits for a batch: what is certain to be freed
    // then it adds to Pending; what only becomes another reclaimer's to free it does not).
    using ReclaimFunction = std::function<VkDeviceSize(VkDeviceSize want, std::uint64_t& evicted)>;
    // Lower `order` runs first; `intervalMs`: the least time between two of its calls (a reclaimer
    // whose call walks a whole cache). Returns an id for RemoveReclaimer.
    std::uint64_t AddReclaimer(const char* name, VramReclaimLevel level, int order, ReclaimFunction reclaim, std::uint32_t intervalMs = 0);
    // Waits for a Relieve in progress.
    void RemoveReclaimer(std::uint64_t id);
    // While over the target (`extra` more bytes wanted), runs the reclaimers allowed at `level`
    // in order until they freed the excess. One Relieve at a time (another thread's call returns
    // 0 at once); a reclaimer that freed nothing is skipped for the backoff. Returns the bytes freed.
    VkDeviceSize Relieve(VramReclaimLevel level, VkDeviceSize extra = 0);
    // Whether a SafePoint Relieve should run now: always at a present; elsewhere (a resource cache
    // insert) at most every 16 ms, and not again in the epoch a SafePoint Relieve fell short in
    // (what it could not free stays unreclaimable until entries age or batches complete).
    bool SafePointDue(bool present) const;
    // An allocation with a fallback: true when `bytes` fit under the target (after an Inline
    // Relieve); false counts a refusal of the class.
    bool Admit(VramClass type, VkDeviceSize bytes);
    void CountRefusal(VramClass type, VkDeviceSize bytes);
    // A released pool slot destroyed instead of retained, over the target.
    void CountNotRetained(VkDeviceSize bytes);
    // AllocateDeviceMemory's driver refusals (VK_ERROR_OUT_OF_DEVICE_MEMORY) of the budgeted heap.
    void CountDriverRefusal();
    // Cold textures the texture cache evicted itself when a new image took it over the target.
    void CountInlineTextureEvictions(std::uint64_t count, VkDeviceSize bytes);
    std::uint64_t InlineTextureEvictions() const { return inlineTextures.load(std::memory_order_relaxed); }

    struct ReclaimerCounts {
        std::string name;
        std::uint64_t calls = 0;
        std::uint64_t evicted = 0;
        VkDeviceSize bytes = 0;
    };
    // Since the start (tests), in order.
    std::vector<ReclaimerCounts> Reclaimers() const;
    std::uint64_t Refusals(VramClass type) const;
    std::uint64_t NotRetained() const { return notRetained.load(std::memory_order_relaxed); }
    std::uint64_t Relieves() const { return relieves.load(std::memory_order_relaxed); }
    std::uint64_t Shortfalls() const { return shortfalls.load(std::memory_order_relaxed); }
    // The [vram] line; `window` restarts the per-window counts it prints (evictions, refusals).
    // `extra`: appended before the newline (the driver's census).
    std::string Digest(bool window, const std::string& extra = {});

private:
    std::uint64_t now() const;
    struct Reclaimer {
        std::uint64_t id = 0;
        std::string name;
        VramReclaimLevel level = VramReclaimLevel::SafePoint;
        int order = 0;
        ReclaimFunction reclaim;
        std::uint64_t retryAt = 0;
        std::uint32_t intervalMs = 0;
        std::uint64_t calls = 0;
        std::uint64_t evicted = 0;
        VkDeviceSize bytes = 0;
        std::uint64_t windowEvicted = 0;
        VkDeviceSize windowBytes = 0;
    };
    struct Allocation {
        VramClass type;
        VkDeviceSize bytes;
    };
    struct KeyHash {
        std::size_t operator()(const std::pair<std::uintptr_t, std::uintptr_t>& key) const noexcept {
            return static_cast<std::size_t>((static_cast<std::uint64_t>(key.first) * 0x9E3779B97F4A7C15ull) ^ static_cast<std::uint64_t>(key.second));
        }
    };
    const bool enabledBySettings;
    std::atomic<bool> enabled;
    std::atomic<VkDeviceSize> targetOverride;
    std::atomic<VkDeviceSize> headroomSetting;
    std::atomic<std::uint32_t> backoffMs;
    std::atomic<std::uint32_t> minAge;
    std::function<std::uint64_t()> clock;
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<std::uint64_t> epochEndedAt{0};
    std::atomic<std::uint64_t> lastSafePointAt{0};
    std::atomic<bool> safePointRan{false};
    std::atomic<std::uint64_t> shortEpoch{0};
    std::atomic<VkDeviceSize> heap{0};
    std::atomic<bool> reported{false};
    std::atomic<VkDeviceSize> reportedBudget{0};
    std::atomic<VkDeviceSize> reportedUsage{0};
    // The report's usage minus the accounting's total when it was taken, in one word, so a reader
    // never pairs a new usage with an old total (Used = total + offset).
    std::atomic<std::int64_t> usageOffset{0};
    std::array<std::atomic<VkDeviceSize>, VramClassCount> tracked{};
    // The sum of `tracked`, kept apart: a Move between classes never changes it, so a report
    // taken during one cannot count its bytes twice or not at all.
    std::atomic<VkDeviceSize> trackedTotal{0};
    std::atomic<VkDeviceSize> pending{0};
    mutable HostMutex ledgerMutex;
    std::unordered_map<std::pair<std::uintptr_t, std::uintptr_t>, Allocation, KeyHash> ledger;
    mutable HostMutex reclaimersMutex;
    std::vector<Reclaimer> reclaimers;
    std::uint64_t nextReclaimerId = 1;
    std::atomic<bool> relieving{false};
    std::array<std::atomic<std::uint64_t>, VramClassCount> refusals{};
    std::array<std::atomic<VkDeviceSize>, VramClassCount> refusedBytes{};
    std::array<std::atomic<std::uint64_t>, VramClassCount> windowRefusals{};
    std::array<std::atomic<VkDeviceSize>, VramClassCount> windowRefusedBytes{};
    std::atomic<std::uint64_t> notRetained{0};
    std::atomic<std::uint64_t> windowNotRetained{0};
    std::atomic<VkDeviceSize> windowNotRetainedBytes{0};
    std::atomic<std::uint64_t> relieves{0};
    std::atomic<std::uint64_t> windowRelieves{0};
    std::atomic<std::uint64_t> shortfalls{0};
    std::atomic<std::uint64_t> windowShortfalls{0};
    std::atomic<std::uint64_t> driverRefusals{0};
    std::atomic<std::uint64_t> windowDriverRefusals{0};
    std::atomic<std::uint64_t> inlineTextures{0};
    std::atomic<std::uint64_t> windowInlineTextures{0};
    std::atomic<VkDeviceSize> windowInlineTextureBytes{0};
    std::atomic<VkDeviceSize> windowPeak{0};
};

// The process's budget (settings from the environment, the driver's reclaimers registered).
VramBudget& Vram();

// The class Buffer allocations and pool takes of device-local memory are accounted under on this
// thread (Buffers outside any scope).
class VramClassScope {
public:
    explicit VramClassScope(VramClass type);
    ~VramClassScope();
    VramClassScope(const VramClassScope&) = delete;
    VramClassScope& operator=(const VramClassScope&) = delete;

private:
    VramClass previous;
};
VramClass CurrentVramClass();

// Whether memory type `memoryType` of the context's device lies in a budgeted heap (the heap of
// the configured device's DEVICE_LOCAL memory, every heap of a unified one; unconfigured: any
// DEVICE_LOCAL heap).
bool VramBudgeted(const Context& context, std::uint32_t memoryType);
// The allocation accounting of the driver's own vkAllocateMemory/vkFreeMemory calls (the ones
// through AllocateDeviceMemory are noted there).
void NoteDeviceMemory(const Context& context, VkDeviceMemory memory, const VkMemoryAllocateInfo& allocation, VramClass type);
void ForgetDeviceMemory(VkDevice device, VkDeviceMemory memory);
void ReclassDeviceMemory(VkDevice device, VkDeviceMemory memory, VramClass type);
// ForgetDeviceMemory, then vkFreeMemory.
void FreeDeviceMemory(const Context& context, VkDeviceMemory memory);
// The device setup: the budgeted heap, the budget query (null without VK_EXT_memory_budget) and
// the device's buffer pool (the "pool" reclaimer trims its device tier). `integrated`: an
// integrated GPU (an APU, the Steam Deck), whose DEVICE_LOCAL heap is a small carve-out the kernel
// spills into system memory. There every heap is accounted together, against their budgets
// together, but the budget only observes (no reclaim, no refusal): their usage includes the host
// imports of guest memory and the pool's host tiers, which no reclaimer can free, and the policy
// has not run on such a device. APS5_VRAM_UNIFIED=1 enforces it over every heap, =0 over the
// DEVICE_LOCAL heap alone (either way on any device). Prints the [vram] configuration line.
void ConfigureVram(const Context& context, PFN_vkGetPhysicalDeviceMemoryProperties2 query, const std::shared_ptr<BufferPool>& pool, bool integrated = false);
// The device teardown: drops the query, the heap and the pool reclaimer of `device`.
void ReleaseVram(VkDevice device);
// A safe point other than the present (a resource cache insert; the caller holds no cache lock):
// refreshes the driver's report (every 100 ms), ends a stalled epoch, relieves while over the
// target when SafePointDue, prints the [vram] line every 10 s (APS5_PROFILE_DRAW).
void RelieveVram();
// The present's safe point: ends the epoch (VramBudget::AdvanceEpoch), then as RelieveVram but
// always relieves while over.
void VramPresent();
// Inside an allocation of `bytes` in the budgeted heap: the report refresh, and an Inline Relieve
// while over.
void VramBeforeAllocation(VkDeviceSize bytes);
// Budget().Admit for the driver's allocations with a fallback.
bool AdmitVram(VramClass type, VkDeviceSize bytes);
// Whether a released device-local pool slot should be destroyed instead of retained.
bool VramPressure();
// The census of the driver's caches appended to the [vram] line.
void SetVramCensus(std::function<std::string()> census);

// VramReclaimers.cpp (the driver's caches, so not in the targets that build Resources.cpp alone):
// registers the process-wide reclaimers on Vram() once, in the order they run (what costs nothing
// to make again first), and the census:
//  10 shadows (Inline, at most every 10 ms): staging-chain registry entries whose shadow nothing
//     else holds.
//  20 textures: sampled texture cache entries nothing else holds (no build, no batch) and not used
//     for MinAge epochs, destroyed once the open batch completed.
//  30 builds: resource cache entries nothing else holds and not used for MinAge epochs that hold
//     device-local memory alone, least recently used first, destroyed once the open batch
//     completed (their shadows only they hold are pending until then); those shadows go back to
//     the pool (destroyed while over), and textures and shadows they shared with one other holder
//     become the other reclaimers' for the next pass.
//  40 resident: resident read copies nothing else holds (APS5_RESIDENT_READS).
// The device registers the pool's (order 0, Inline) in ConfigureVram.
void RegisterDriverVramReclaimers();
// Hands evicted objects to the open batch (destroyed once it completed, off the GPU mutex) when
// the calling thread records under the GPU mutex, else drops them here; `estimate` (the bytes
// certain to be freed) is pending until just before they are destroyed.
void DisposeVramVictims(std::vector<std::shared_ptr<void>> victims, VkDeviceSize estimate);

}

#endif
