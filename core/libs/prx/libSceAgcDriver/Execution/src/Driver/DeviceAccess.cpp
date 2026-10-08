#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

bool slimGate() {
    static const bool enabled = std::getenv("APS5_NO_SLIM_DEVICE_GATE") == nullptr;
    return enabled;
}

}

std::shared_ptr<VulkanDevice> DevicePointer::Load() const { return pointer.load(std::memory_order_acquire); }

DevicePointer::operator std::shared_ptr<VulkanDevice>() const { return Load(); }

DevicePointer& DevicePointer::operator=(std::shared_ptr<VulkanDevice> value) {
    pointer.store(std::move(value), std::memory_order_release);
    return *this;
}

void DevicePointer::Reset() { *this = nullptr; }

VulkanDevice* DevicePointer::operator->() const { return Load().get(); }

DevicePointer::operator bool() const { return Load() != nullptr; }

bool DevicePointer::operator==(std::nullptr_t) const { return Load() == nullptr; }

void DeviceUseGate::lock_shared() {
    if (slimGate()) return slim.lock_shared();
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    ++users;
}

void DeviceUseGate::unlock_shared() {
    if (slimGate()) return slim.unlock_shared();
    std::lock_guard lock(mutex);
    if (--users == 0) changed.notify_all();
}

void DeviceUseGate::lock() {
    if (slimGate()) return slim.lock();
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    replacing = true;
    changed.wait(lock, [&] { return users == 0; });
}

void DeviceUseGate::unlock() {
    if (slimGate()) return slim.unlock();
    std::lock_guard lock(mutex);
    replacing = false;
    changed.notify_all();
}

}
