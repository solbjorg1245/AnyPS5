#include "prx/libSceAgcDriver/Graphics/include/VramBudget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace AgcDriver::Graphics {

namespace {

constexpr VkDeviceSize Mib = VkDeviceSize{1} << 20u;

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

VkDeviceSize mibSetting(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<VkDeviceSize>(std::strtoull(value, nullptr, 10)) * Mib : 0;
}

void subtractClamped(std::atomic<VkDeviceSize>& counter, VkDeviceSize bytes) {
    auto current = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(current, current > bytes ? current - bytes : 0, std::memory_order_relaxed)) {
    }
}

void raiseTo(std::atomic<VkDeviceSize>& counter, VkDeviceSize value) {
    auto current = counter.load(std::memory_order_relaxed);
    while (current < value && !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

unsigned long long asMib(VkDeviceSize bytes) {
    return static_cast<unsigned long long>((bytes + Mib / 2) / Mib);
}

thread_local VramClass currentClass = VramClass::Buffers;

}

const char* VramClassName(VramClass value) {
    switch (value) {
        case VramClass::Pool: return "pool";
        case VramClass::Buffers: return "buffers";
        case VramClass::Resident: return "resident";
        case VramClass::Textures: return "textures";
        case VramClass::Slack: return "slack";
        case VramClass::Targets: return "targets";
        case VramClass::Other: return "other";
    }
    return "?";
}

VramBudget::Settings VramBudget::Settings::FromEnvironment() {
    Settings settings;
    settings.enabled = std::getenv("APS5_NO_VRAM_BUDGET") == nullptr;
    settings.target = mibSetting("APS5_VRAM_BUDGET_MIB");
    settings.headroom = mibSetting("APS5_VRAM_HEADROOM_MIB");
    return settings;
}

VramBudget::VramBudget(const Settings& settings) : enabled(settings.enabled), targetOverride(settings.target), headroomSetting(settings.headroom), backoffMs(settings.backoffMs) {}

void VramBudget::SetClock(std::function<std::uint64_t()> value) {
    std::lock_guard lock(reclaimersMutex);
    clock = std::move(value);
}

std::uint64_t VramBudget::now() const {
    return clock ? clock() : steadyMs();
}

void VramBudget::Report(VkDeviceSize budget, VkDeviceSize usage) {
    reportedBudget.store(budget, std::memory_order_relaxed);
    reportedUsage.store(usage, std::memory_order_relaxed);
    trackedAtReport.store(Tracked(), std::memory_order_relaxed);
    reported.store(true, std::memory_order_release);
}

void VramBudget::ClearReport() {
    reported.store(false, std::memory_order_release);
    reportedBudget.store(0, std::memory_order_relaxed);
    reportedUsage.store(0, std::memory_order_relaxed);
}

void VramBudget::Add(VramClass type, VkDeviceSize bytes) {
    tracked[static_cast<std::size_t>(type)].fetch_add(bytes, std::memory_order_relaxed);
}

void VramBudget::Remove(VramClass type, VkDeviceSize bytes) {
    subtractClamped(tracked[static_cast<std::size_t>(type)], bytes);
}

void VramBudget::Move(VramClass from, VramClass to, VkDeviceSize bytes) {
    if (from == to || bytes == 0) return;
    // Added first: a concurrent reader may see the bytes twice for an instant, never none.
    Add(to, bytes);
    Remove(from, bytes);
}

VkDeviceSize VramBudget::Tracked(VramClass type) const {
    return tracked[static_cast<std::size_t>(type)].load(std::memory_order_relaxed);
}

VkDeviceSize VramBudget::Tracked() const {
    VkDeviceSize total = 0;
    for (const auto& value : tracked) total += value.load(std::memory_order_relaxed);
    return total;
}

void VramBudget::NoteAllocation(VkDevice device, VkDeviceMemory memory, VramClass type, VkDeviceSize bytes) {
    if (memory == VK_NULL_HANDLE) return;
    const std::pair<std::uintptr_t, std::uintptr_t> key{reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(memory)};
    {
        std::lock_guard lock(ledgerMutex);
        auto [it, inserted] = ledger.try_emplace(key, Allocation{type, bytes});
        if (!inserted) {
            // A handle freed without Forget and handed out again: its old bytes are gone.
            Remove(it->second.type, it->second.bytes);
            it->second = Allocation{type, bytes};
        }
    }
    Add(type, bytes);
}

void VramBudget::Reclass(VkDevice device, VkDeviceMemory memory, VramClass type) {
    if (memory == VK_NULL_HANDLE) return;
    const std::pair<std::uintptr_t, std::uintptr_t> key{reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(memory)};
    std::lock_guard lock(ledgerMutex);
    const auto found = ledger.find(key);
    if (found == ledger.end() || found->second.type == type) return;
    Move(found->second.type, type, found->second.bytes);
    found->second.type = type;
}

void VramBudget::Forget(VkDevice device, VkDeviceMemory memory) {
    if (memory == VK_NULL_HANDLE) return;
    const std::pair<std::uintptr_t, std::uintptr_t> key{reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(memory)};
    Allocation gone{};
    {
        std::lock_guard lock(ledgerMutex);
        const auto found = ledger.find(key);
        if (found == ledger.end()) return;
        gone = found->second;
        ledger.erase(found);
    }
    Remove(gone.type, gone.bytes);
}

std::size_t VramBudget::Allocations() const {
    std::lock_guard lock(ledgerMutex);
    return ledger.size();
}

VkDeviceSize VramBudget::Headroom() const {
    if (const auto value = headroomSetting.load(std::memory_order_relaxed); value != 0) return value;
    const auto size = Heap();
    return std::clamp<VkDeviceSize>(size / 16, 256 * Mib, 1024 * Mib);
}

VkDeviceSize VramBudget::Target() const {
    if (const auto value = targetOverride.load(std::memory_order_relaxed); value != 0) return value;
    const auto size = Heap();
    VkDeviceSize limit = size;
    if (Reported()) {
        const auto budget = ReportedBudget();
        if (budget != 0) limit = size != 0 ? std::min(size, budget) : budget;
    }
    if (limit == 0) return ~VkDeviceSize{0};
    const auto headroom = Headroom();
    const auto floor = std::min<VkDeviceSize>(size != 0 ? size / 4 : limit / 4, 1024 * Mib);
    return std::max(limit > headroom ? limit - headroom : 0, floor);
}

VkDeviceSize VramBudget::Used() const {
    const auto own = Tracked();
    if (!Reported()) return own;
    // The driver's figure at the last report, moved by what the accounting saw since.
    const auto base = static_cast<std::int64_t>(ReportedUsage()) + static_cast<std::int64_t>(own) - static_cast<std::int64_t>(trackedAtReport.load(std::memory_order_relaxed));
    return base > 0 ? static_cast<VkDeviceSize>(base) : 0;
}

void VramBudget::AddPending(VkDeviceSize bytes) {
    pending.fetch_add(bytes, std::memory_order_relaxed);
}

void VramBudget::RemovePending(VkDeviceSize bytes) {
    subtractClamped(pending, bytes);
}

VkDeviceSize VramBudget::Excess(VkDeviceSize extra) const {
    if (!Enabled()) return 0;
    const auto target = Target();
    if (target == ~VkDeviceSize{0}) return 0;
    const auto used = Used();
    // Slack serves the next pooled textures, but not buffers or dedicated images: at most the
    // headroom of it counts as room, so used memory stays within the target plus the headroom.
    const auto relief = Pending() + std::min(Tracked(VramClass::Slack), Headroom());
    const auto pressure = (used > relief ? used - relief : 0) + extra;
    return pressure > target ? pressure - target : 0;
}

std::uint64_t VramBudget::AddReclaimer(const char* name, VramReclaimLevel level, int order, ReclaimFunction reclaim) {
    std::lock_guard lock(reclaimersMutex);
    Reclaimer entry;
    const auto id = nextReclaimerId++;
    entry.id = id;
    entry.name = name;
    entry.level = level;
    entry.order = order;
    entry.reclaim = std::move(reclaim);
    const auto at = std::upper_bound(reclaimers.begin(), reclaimers.end(), order, [](int value, const Reclaimer& other) { return value < other.order; });
    reclaimers.insert(at, std::move(entry));
    return id;
}

void VramBudget::RemoveReclaimer(std::uint64_t id) {
    std::lock_guard lock(reclaimersMutex);
    std::erase_if(reclaimers, [&](const Reclaimer& entry) { return entry.id == id; });
}

VkDeviceSize VramBudget::Relieve(VramReclaimLevel level, VkDeviceSize extra) {
    auto excess = Excess(extra);
    if (excess == 0) return 0;
    bool idle = false;
    if (!relieving.compare_exchange_strong(idle, true, std::memory_order_acquire)) return 0;
    struct Release {
        std::atomic<bool>& flag;
        ~Release() { flag.store(false, std::memory_order_release); }
    } release{relieving};
    relieves.fetch_add(1, std::memory_order_relaxed);
    windowRelieves.fetch_add(1, std::memory_order_relaxed);
    VkDeviceSize freed = 0;
    {
        std::lock_guard lock(reclaimersMutex);
        const auto at = now();
        const auto backoff = backoffMs.load(std::memory_order_relaxed);
        for (auto& entry : reclaimers) {
            if (static_cast<int>(entry.level) > static_cast<int>(level) || at < entry.retryAt) continue;
            std::uint64_t evicted = 0;
            VkDeviceSize got = 0;
            try {
                got = entry.reclaim(excess - freed, evicted);
            } catch (...) {
                got = 0;
                evicted = 0;
            }
            ++entry.calls;
            entry.evicted += evicted;
            entry.bytes += got;
            entry.windowEvicted += evicted;
            entry.windowBytes += got;
            if (got == 0) entry.retryAt = at + backoff;
            freed += got;
            if (freed >= excess) break;
        }
    }
    if (freed < excess) {
        shortfalls.fetch_add(1, std::memory_order_relaxed);
        windowShortfalls.fetch_add(1, std::memory_order_relaxed);
    }
    return freed;
}

bool VramBudget::Admit(VramClass type, VkDeviceSize bytes) {
    if (!Over(bytes)) return true;
    Relieve(VramReclaimLevel::Inline, bytes);
    if (!Over(bytes)) return true;
    CountRefusal(type, bytes);
    return false;
}

void VramBudget::CountRefusal(VramClass type, VkDeviceSize bytes) {
    const auto index = static_cast<std::size_t>(type);
    refusals[index].fetch_add(1, std::memory_order_relaxed);
    refusedBytes[index].fetch_add(bytes, std::memory_order_relaxed);
    windowRefusals[index].fetch_add(1, std::memory_order_relaxed);
    windowRefusedBytes[index].fetch_add(bytes, std::memory_order_relaxed);
}

void VramBudget::CountNotRetained(VkDeviceSize bytes) {
    notRetained.fetch_add(1, std::memory_order_relaxed);
    windowNotRetained.fetch_add(1, std::memory_order_relaxed);
    windowNotRetainedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void VramBudget::CountDriverRefusal() {
    driverRefusals.fetch_add(1, std::memory_order_relaxed);
    windowDriverRefusals.fetch_add(1, std::memory_order_relaxed);
}

void VramBudget::CountInlineTextureEvictions(std::uint64_t count, VkDeviceSize bytes) {
    if (count == 0) return;
    inlineTextures.fetch_add(count, std::memory_order_relaxed);
    windowInlineTextures.fetch_add(count, std::memory_order_relaxed);
    windowInlineTextureBytes.fetch_add(bytes, std::memory_order_relaxed);
}

std::vector<VramBudget::ReclaimerCounts> VramBudget::Reclaimers() const {
    std::lock_guard lock(reclaimersMutex);
    std::vector<ReclaimerCounts> result;
    for (const auto& entry : reclaimers) result.push_back({entry.name, entry.calls, entry.evicted, entry.bytes});
    return result;
}

std::uint64_t VramBudget::Refusals(VramClass type) const {
    return refusals[static_cast<std::size_t>(type)].load(std::memory_order_relaxed);
}

std::string VramBudget::Digest(bool window, const std::string& extra) {
    const auto used = Used();
    raiseTo(windowPeak, used);
    const auto target = Target();
    char source[160];
    if (targetOverride.load(std::memory_order_relaxed) != 0) {
        std::snprintf(source, sizeof(source), "APS5_VRAM_BUDGET_MIB");
    } else if (target == ~VkDeviceSize{0}) {
        std::snprintf(source, sizeof(source), "unbounded: heap unknown");
    } else if (Reported() && ReportedBudget() != 0) {
        std::snprintf(source, sizeof(source), "driver budget %llu - headroom %llu, heap %llu", asMib(ReportedBudget()), asMib(Headroom()), asMib(Heap()));
    } else {
        std::snprintf(source, sizeof(source), "heap %llu - headroom %llu", asMib(Heap()), asMib(Headroom()));
    }
    const auto own = Tracked();
    const auto untracked = static_cast<long long>(used) - static_cast<long long>(own);
    std::string line;
    char text[512];
    std::snprintf(text, sizeof(text), "[vram] target %llu MiB (%s%s), used %llu MiB (peak %llu%s), over %llu MiB; tracked %llu MiB:", target == ~VkDeviceSize{0} ? 0ull : asMib(target), source, Enabled() ? "" : "; off: APS5_NO_VRAM_BUDGET=1, accounting only", asMib(used), asMib(windowPeak.load(std::memory_order_relaxed)), Reported() ? ", driver-reported" : ", accounted", asMib(Excess()), asMib(own));
    line += text;
    for (std::size_t index = 0; index < VramClassCount; ++index) {
        std::snprintf(text, sizeof(text), "%s %s %llu", index == 0 ? "" : ",", VramClassName(static_cast<VramClass>(index)), asMib(tracked[index].load(std::memory_order_relaxed)));
        line += text;
    }
    std::snprintf(text, sizeof(text), ", untracked %lld, pending %llu", untracked / static_cast<long long>(Mib), asMib(Pending()));
    line += text;
    const auto take = [&](auto& counter) {
        return window ? counter.exchange(0, std::memory_order_relaxed) : counter.load(std::memory_order_relaxed);
    };
    const auto relieveCount = take(windowRelieves);
    const auto shortCount = take(windowShortfalls);
    std::snprintf(text, sizeof(text), "; 10 s: relieves %llu (%llu short); evictions", static_cast<unsigned long long>(relieveCount), static_cast<unsigned long long>(shortCount));
    line += text;
    {
        std::unique_lock lock(reclaimersMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            line += " (busy)";
        } else {
            bool first = true;
            for (auto& entry : reclaimers) {
                std::snprintf(text, sizeof(text), "%s %s %llu (%llu MiB)", first ? "" : ",", entry.name.c_str(), static_cast<unsigned long long>(entry.windowEvicted), asMib(entry.windowBytes));
                line += text;
                first = false;
                if (window) {
                    entry.windowEvicted = 0;
                    entry.windowBytes = 0;
                }
            }
            if (first) line += " none registered";
        }
    }
    {
        const auto count = take(windowInlineTextures);
        const auto bytes = take(windowInlineTextureBytes);
        std::snprintf(text, sizeof(text), ", textures at lookup %llu (%llu MiB)", static_cast<unsigned long long>(count), asMib(bytes));
        line += text;
    }
    line += "; refusals";
    bool any = false;
    for (std::size_t index = 0; index < VramClassCount; ++index) {
        const auto count = take(windowRefusals[index]);
        const auto bytes = take(windowRefusedBytes[index]);
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu (%llu MiB)", VramClassName(static_cast<VramClass>(index)), static_cast<unsigned long long>(count), asMib(bytes));
        line += text;
        any = true;
    }
    if (!any) line += " 0";
    const auto notKept = take(windowNotRetained);
    const auto notKeptBytes = take(windowNotRetainedBytes);
    const auto driver = take(windowDriverRefusals);
    std::snprintf(text, sizeof(text), ", pool slots not retained %llu (%llu MiB), driver refusals %llu", static_cast<unsigned long long>(notKept), asMib(notKeptBytes), static_cast<unsigned long long>(driver));
    line += text;
    line += extra;
    line += '\n';
    if (window) windowPeak.store(used, std::memory_order_relaxed);
    return line;
}

// The driver's side: the process budget, its reclaimers, the configured device and the reports.
namespace {

struct DriverVram {
    HostMutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceMemoryProperties2 query = nullptr;
    std::uint64_t poolReclaimer = 0;
    std::atomic<VkDevice> configuredDevice{VK_NULL_HANDLE};
    // The budgeted heaps (a bit per heap index): the heap of the device's DEVICE_LOCAL memory, or
    // every heap of a unified-memory device.
    std::atomic<std::uint32_t> heapMask{0};
    std::atomic<std::uint64_t> nextRefresh{0};
    std::atomic<std::uint64_t> nextDigest{0};
    // The driver caches' census for the [vram] line (RegisterDriverVramReclaimers).
    std::function<std::string()> census;
};

DriverVram& driverVram() {
    static auto* state = new DriverVram();
    return *state;
}

bool digestEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr || std::getenv("APS5_VRAM_DIGEST") != nullptr;
    return enabled;
}

void refreshReport(bool force) {
    auto& state = driverVram();
    const auto at = steadyMs();
    auto next = state.nextRefresh.load(std::memory_order_relaxed);
    if (!force && at < next) return;
    if (!force && !state.nextRefresh.compare_exchange_strong(next, at + 100, std::memory_order_relaxed)) return;
    if (force) state.nextRefresh.store(at + 100, std::memory_order_relaxed);
    PFN_vkGetPhysicalDeviceMemoryProperties2 query = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    {
        std::lock_guard lock(state.mutex);
        query = state.query;
        physical = state.physical;
    }
    const auto mask = state.heapMask.load(std::memory_order_relaxed);
    if (query == nullptr || physical == VK_NULL_HANDLE || mask == 0) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    query(physical, &properties);
    VkDeviceSize heapBudget = 0;
    VkDeviceSize heapUsage = 0;
    for (std::uint32_t heap = 0; heap < VK_MAX_MEMORY_HEAPS && heap < properties.memoryProperties.memoryHeapCount; ++heap) {
        if ((mask & (1u << heap)) == 0) continue;
        heapBudget += budget.heapBudget[heap];
        heapUsage += budget.heapUsage[heap];
    }
    if (heapBudget != 0) Vram().Report(heapBudget, heapUsage);
}

void maybeDigest() {
    if (!digestEnabled()) return;
    auto& state = driverVram();
    const auto at = steadyMs();
    auto next = state.nextDigest.load(std::memory_order_relaxed);
    if (next == 0) {
        state.nextDigest.compare_exchange_strong(next, at + 10000, std::memory_order_relaxed);
        return;
    }
    if (at < next || !state.nextDigest.compare_exchange_strong(next, at + 10000, std::memory_order_relaxed)) return;
    std::function<std::string()> census;
    {
        std::lock_guard lock(state.mutex);
        census = state.census;
    }
    const auto line = Vram().Digest(true, census ? census() : std::string{});
    std::fputs(line.c_str(), stderr);
}

}

VramBudget& Vram() {
    static auto* budget = new VramBudget(VramBudget::Settings::FromEnvironment());
    return *budget;
}

void SetVramCensus(std::function<std::string()> census) {
    auto& state = driverVram();
    std::lock_guard lock(state.mutex);
    state.census = std::move(census);
}

VramClassScope::VramClassScope(VramClass type) : previous(currentClass) {
    currentClass = type;
}

VramClassScope::~VramClassScope() {
    currentClass = previous;
}

VramClass CurrentVramClass() {
    return currentClass;
}

bool VramBudgeted(const Context& context, std::uint32_t memoryType) {
    if (memoryType >= context.memory.memoryTypeCount) return false;
    const auto heapIndex = context.memory.memoryTypes[memoryType].heapIndex;
    const auto& state = driverVram();
    if (context.device != VK_NULL_HANDLE && state.configuredDevice.load(std::memory_order_relaxed) == context.device) return heapIndex < 32 && (state.heapMask.load(std::memory_order_relaxed) & (1u << heapIndex)) != 0;
    return heapIndex < context.memory.memoryHeapCount && (context.memory.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
}

void NoteDeviceMemory(const Context& context, VkDeviceMemory memory, const VkMemoryAllocateInfo& allocation, VramClass type) {
    if (memory == VK_NULL_HANDLE || !VramBudgeted(context, allocation.memoryTypeIndex)) return;
    Vram().NoteAllocation(context.device, memory, type, allocation.allocationSize);
}

void ForgetDeviceMemory(VkDevice device, VkDeviceMemory memory) {
    Vram().Forget(device, memory);
}

void ReclassDeviceMemory(VkDevice device, VkDeviceMemory memory, VramClass type) {
    Vram().Reclass(device, memory, type);
}

void FreeDeviceMemory(const Context& context, VkDeviceMemory memory) {
    if (memory == VK_NULL_HANDLE) return;
    ForgetDeviceMemory(context.device, memory);
    context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

void ConfigureVram(const Context& context, PFN_vkGetPhysicalDeviceMemoryProperties2 query, const std::shared_ptr<BufferPool>& pool, bool unified) {
    auto& budget = Vram();
    auto& state = driverVram();
    std::uint32_t type = 0;
    try {
        type = context.MemoryType(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (const std::exception&) {
        std::fprintf(stderr, "[vram] no device-local memory type: no budget\n");
        return;
    }
    const auto heapIndex = context.memory.memoryTypes[type].heapIndex;
    // A unified-memory device (an APU, the Steam Deck) spills its small DEVICE_LOCAL carve-out into
    // system memory instead of refusing: every heap is budgeted, against their budgets together.
    // APS5_VRAM_UNIFIED=1/0 overrides the device type.
    if (const char* value = std::getenv("APS5_VRAM_UNIFIED"); value != nullptr) unified = std::strcmp(value, "0") != 0;
    std::uint32_t mask = 1u << heapIndex;
    VkDeviceSize heapBytes = context.memory.memoryHeaps[heapIndex].size;
    if (unified) {
        mask = 0;
        heapBytes = 0;
        for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount && heap < 32; ++heap) {
            mask |= 1u << heap;
            heapBytes += context.memory.memoryHeaps[heap].size;
        }
    }
    std::uint64_t previousPool = 0;
    {
        std::lock_guard lock(state.mutex);
        state.device = context.device;
        state.physical = context.physical;
        state.query = query;
        previousPool = state.poolReclaimer;
        state.poolReclaimer = 0;
    }
    state.heapMask.store(mask, std::memory_order_relaxed);
    state.configuredDevice.store(context.device, std::memory_order_relaxed);
    budget.SetHeap(heapBytes);
    budget.ClearReport();
    if (previousPool != 0) budget.RemoveReclaimer(previousPool);
    std::weak_ptr<BufferPool> weak = pool;
    const auto id = budget.AddReclaimer("pool", VramReclaimLevel::Inline, 0, [weak](VkDeviceSize want, std::uint64_t& evicted) -> VkDeviceSize {
        const auto held = weak.lock();
        return held != nullptr ? held->TrimDevice(want, evicted) : 0;
    });
    {
        std::lock_guard lock(state.mutex);
        state.poolReclaimer = id;
    }
    refreshReport(true);
    const auto target = budget.Target();
    std::fprintf(stderr, "[vram] heaps 0x%x%s: %llu MiB, VK_EXT_memory_budget %s (budget %llu MiB, usage %llu MiB), target %llu MiB, headroom %llu MiB%s\n", mask, unified ? " (unified memory)" : "", asMib(budget.Heap()), query != nullptr ? "on" : "off", asMib(budget.ReportedBudget()), asMib(budget.ReportedUsage()), target == ~VkDeviceSize{0} ? 0ull : asMib(target), asMib(budget.Headroom()), budget.Enabled() ? "" : "; enforcement off (APS5_NO_VRAM_BUDGET=1): accounting only");
}

void ReleaseVram(VkDevice device) {
    auto& state = driverVram();
    std::uint64_t pool = 0;
    {
        std::lock_guard lock(state.mutex);
        if (state.device != device) return;
        state.device = VK_NULL_HANDLE;
        state.physical = VK_NULL_HANDLE;
        state.query = nullptr;
        pool = state.poolReclaimer;
        state.poolReclaimer = 0;
    }
    state.configuredDevice.store(VK_NULL_HANDLE, std::memory_order_relaxed);
    Vram().ClearReport();
    if (pool != 0) Vram().RemoveReclaimer(pool);
}

void RelieveVram() {
    refreshReport(false);
    auto& budget = Vram();
    if (budget.Over()) budget.Relieve(VramReclaimLevel::SafePoint);
    maybeDigest();
}

void VramBeforeAllocation(VkDeviceSize bytes) {
    refreshReport(false);
    auto& budget = Vram();
    if (budget.Over(bytes)) budget.Relieve(VramReclaimLevel::Inline, bytes);
}

bool AdmitVram(VramClass type, VkDeviceSize bytes) {
    return Vram().Admit(type, bytes);
}

bool VramPressure() {
    return Vram().Over();
}

}
