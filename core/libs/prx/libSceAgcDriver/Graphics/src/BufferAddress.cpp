#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver::Graphics {

VkDeviceAddress Buffer::DeviceAddress() const {
    Require(deviceAddress != 0, "buffer has no device address");
    return deviceAddress;
}

void Buffer::initializeAddress(VkBufferUsageFlags usage) {
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0) return;
    const VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
    deviceAddress = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &info);
    Require(deviceAddress != 0, "vkGetBufferDeviceAddressKHR returned a null address");
    NoteDeviceAddress(deviceAddress, size, (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? "host buffer" : "device buffer");
}

namespace {

struct AddressEntry {
    VkDeviceAddress address = 0;
    std::uint64_t bytes = 0;
    const char* kind = "";
    std::uint64_t guestBase = 0;
    double created = 0;
    double destroyed = -1;
};

struct AddressRegistry {
    static constexpr std::size_t RetiredKept = 4096;
    std::mutex mutex;
    std::map<VkDeviceAddress, AddressEntry> live;
    std::deque<AddressEntry> retired;
};

AddressRegistry& Registry() {
    static auto* registry = new AddressRegistry();
    return *registry;
}

double SecondsNow() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}

void NoteDeviceAddress(VkDeviceAddress address, std::uint64_t bytes, const char* kind, std::uint64_t guestBase) {
    if (address == 0) return;
    auto& registry = Registry();
    const auto now = SecondsNow();
    std::lock_guard lock(registry.mutex);
    registry.live[address] = {address, bytes, kind, guestBase, now, -1};
}

void ForgetDeviceAddress(VkDeviceAddress address) {
    if (address == 0) return;
    auto& registry = Registry();
    const auto now = SecondsNow();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.live.find(address);
    if (found == registry.live.end()) return;
    found->second.destroyed = now;
    registry.retired.push_back(found->second);
    registry.live.erase(found);
    if (registry.retired.size() > AddressRegistry::RetiredKept) registry.retired.pop_front();
}

namespace {

struct DestroyedImport {
    std::uint64_t guestBase = 0;
    double destroyed = 0;
};

struct ImportHandles {
    std::mutex mutex;
    std::map<VkBuffer, DestroyedImport> destroyed;
};

ImportHandles& Handles() {
    static auto* handles = new ImportHandles();
    return *handles;
}

}

namespace {

struct CheckpointEntry {
    std::atomic<std::uint64_t> serial{0};
    char kind = 0;
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t detail = 0;
};

struct Checkpoints {
    static constexpr std::size_t Size = std::size_t{1} << 16;
    PFN_vkCmdSetCheckpointNV set = nullptr;
    PFN_vkGetQueueCheckpointDataNV get = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    std::atomic<std::uint64_t> next{0};
    std::vector<CheckpointEntry> ring = std::vector<CheckpointEntry>(Size);
};

Checkpoints& Marks() {
    static auto* marks = new Checkpoints();
    return *marks;
}

thread_local std::uint64_t checkpointFirst = 0;
thread_local std::uint64_t checkpointSecond = 0;

}

bool CheckpointsRequested() {
    static const bool requested = std::getenv("APS5_GPU_CHECKPOINTS") != nullptr;
    return requested;
}

void InstallCheckpoints(PFN_vkCmdSetCheckpointNV set, PFN_vkGetQueueCheckpointDataNV get, VkQueue queue) {
    auto& marks = Marks();
    marks.set = set;
    marks.get = get;
    marks.queue = queue;
    std::fprintf(stderr, "[checkpoints] %s%s", set != nullptr && get != nullptr ? "recording a checkpoint before every draw and dispatch" : "unavailable", "\n");
}

void SetCheckpointWork(std::uint64_t first, std::uint64_t second) {
    checkpointFirst = first;
    checkpointSecond = second;
}

void RecordCheckpoint(VkCommandBuffer commands, char kind, std::uint64_t first, std::uint64_t second, std::uint64_t detail) {
    auto& marks = Marks();
    if (marks.set == nullptr) return;
    const auto serial = marks.next.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& entry = marks.ring[serial % Checkpoints::Size];
    entry.kind = kind;
    entry.first = first;
    entry.second = second;
    entry.detail = detail;
    entry.serial.store(serial, std::memory_order_release);
    marks.set(commands, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(serial)));
}

void RecordDrawCheckpoint(VkCommandBuffer commands, std::uint64_t detail) {
    if (Marks().set == nullptr) return;
    RecordCheckpoint(commands, 'D', checkpointFirst, checkpointSecond, detail);
}

void ReportCheckpoints() {
    auto& marks = Marks();
    if (marks.get == nullptr || marks.queue == VK_NULL_HANDLE) return;
    std::uint32_t count = 0;
    marks.get(marks.queue, &count, nullptr);
    std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
    if (count != 0) marks.get(marks.queue, &count, data.data());
    std::fprintf(stderr, "[checkpoints] %u stage checkpoint(s), %llu recorded in all%s", count, static_cast<unsigned long long>(marks.next.load()), "\n");
    const auto describe = [&](std::uint64_t serial, const char* prefix) {
        const auto& entry = marks.ring[serial % Checkpoints::Size];
        if (serial == 0 || entry.serial.load(std::memory_order_acquire) != serial) {
            std::fprintf(stderr, "[checkpoints] %s #%llu (overwritten)%s", prefix, static_cast<unsigned long long>(serial), "\n");
            return;
        }
        std::fprintf(stderr, "[checkpoints] %s #%llu %s programs 0x%llx / 0x%llx, %s 0x%llx%s", prefix, static_cast<unsigned long long>(serial), entry.kind == 'D' ? "draw" : "dispatch", static_cast<unsigned long long>(entry.first), static_cast<unsigned long long>(entry.second), entry.kind == 'D' ? "color target" : "groups", static_cast<unsigned long long>(entry.detail), "\n");
    };
    for (const auto& checkpoint : data) {
        const auto serial = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(checkpoint.pCheckpointMarker));
        std::fprintf(stderr, "[checkpoints] stage 0x%x reached:%s", static_cast<unsigned>(checkpoint.stage), "\n");
        for (std::uint64_t back = 3; back > 0; --back) {
            if (serial > back) describe(serial - back, "  before");
        }
        describe(serial, "  last  ");
        describe(serial + 1, "  next  ");
    }
}

bool CheckStaleImports() {
    static const bool check = std::getenv("APS5_CHECK_STALE_IMPORTS") != nullptr;
    return check;
}

void NoteImportHandle(VkBuffer buffer, std::uint64_t guestBase, bool live) {
    if (!CheckStaleImports() || buffer == VK_NULL_HANDLE) return;
    auto& handles = Handles();
    const auto now = SecondsNow();
    std::lock_guard lock(handles.mutex);
    if (live) handles.destroyed.erase(buffer);
    else handles.destroyed[buffer] = {guestBase, now};
}

// The stack slots that point into the driver's code, as offsets (llvm-symbolizer --relative-address
// on libSceAgcDriver.prx, tools/prxstack.py), from the caller outwards.
void ReportDriverStack(const char* tag) {
#ifdef _WIN32
    HMODULE driver = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ReportDriverStack), &driver)) return;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), driver, &info, sizeof(info))) return;
    const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
    const auto end = base + info.SizeOfImage;
    std::fprintf(stderr, "%s   driver addresses on the stack (image base 0x%llx):", tag, static_cast<unsigned long long>(base));
    const auto* slot = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    int printed = 0;
    for (int i = 0; i < 4096 && printed < 32; ++i) {
        const auto value = slot[i];
        if (value <= base || value >= end) continue;
        std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(value - base));
        ++printed;
    }
    std::fprintf(stderr, "\n");
#endif
}

bool ReportDestroyedImport(VkBuffer buffer, const char* where, std::uint64_t detail, std::uint64_t* guestBase) {
    if (!CheckStaleImports() || buffer == VK_NULL_HANDLE) return false;
    auto& handles = Handles();
    std::lock_guard lock(handles.mutex);
    const auto found = handles.destroyed.find(buffer);
    if (found == handles.destroyed.end()) return false;
    if (guestBase != nullptr) *guestBase = found->second.guestBase;
    static int reports = 0;
    if (reports++ < 16) {
        std::fprintf(stderr, "[stale-import] %s (0x%llx) uses the buffer of import guest 0x%llx destroyed %.3f s ago%s", where, static_cast<unsigned long long>(detail), static_cast<unsigned long long>(found->second.guestBase), SecondsNow() - found->second.destroyed, "\n");
        if (reports <= 6) ReportDriverStack("[stale-import]");
        std::fflush(stderr);
    }
    return true;
}

bool DeviceAddressLive(VkDeviceAddress address, std::uint64_t bytes) {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    auto above = registry.live.upper_bound(address);
    if (above == registry.live.begin()) return false;
    const auto& entry = std::prev(above)->second;
    return address >= entry.address && address + bytes <= entry.address + entry.bytes;
}

std::string DescribeDeviceAddress(VkDeviceAddress address) {
    auto& registry = Registry();
    const auto now = SecondsNow();
    std::lock_guard lock(registry.mutex);
    std::string text;
    char line[256];
    const auto describe = [&](const char* relation, const AddressEntry& entry) {
        const auto end = entry.address + entry.bytes;
        const auto distance = address < entry.address ? entry.address - address : address >= end ? address - end : 0;
        std::snprintf(line, sizeof(line), "\n[gpu]     %s %s 0x%llx+0x%llx (distance 0x%llx), guest 0x%llx, created %.2f s ago", relation, entry.kind, static_cast<unsigned long long>(entry.address), static_cast<unsigned long long>(entry.bytes), static_cast<unsigned long long>(distance), static_cast<unsigned long long>(entry.guestBase), now - entry.created);
        text += line;
        if (entry.destroyed >= 0) {
            std::snprintf(line, sizeof(line), ", destroyed %.3f s ago", now - entry.destroyed);
            text += line;
        }
    };
    auto above = registry.live.upper_bound(address);
    if (above != registry.live.begin()) {
        const auto& below = std::prev(above)->second;
        describe(address < below.address + below.bytes ? "in live" : "after live", below);
    }
    if (above != registry.live.end()) describe("before live", above->second);
    for (auto it = registry.retired.rbegin(); it != registry.retired.rend(); ++it) {
        if (address >= it->address && address < it->address + it->bytes) describe("in destroyed", *it);
    }
    std::snprintf(line, sizeof(line), "\n[gpu]     (%zu live ranges, %zu destroyed kept)", registry.live.size(), registry.retired.size());
    text += line;
    return text;
}

}
