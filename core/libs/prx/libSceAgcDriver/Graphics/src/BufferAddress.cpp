#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

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
