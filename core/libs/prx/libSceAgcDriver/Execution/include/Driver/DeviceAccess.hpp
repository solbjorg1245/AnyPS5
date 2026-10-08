#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP

#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace AgcDriver::DriverDetail {

class DevicePointer {
public:
    std::shared_ptr<VulkanDevice> Load() const;
    operator std::shared_ptr<VulkanDevice>() const;
    DevicePointer& operator=(std::shared_ptr<VulkanDevice> value);
    void Reset();
    VulkanDevice* operator->() const;
    explicit operator bool() const;
    bool operator==(std::nullptr_t) const;

private:
    std::atomic<std::shared_ptr<VulkanDevice>> pointer;
};

// Packet execution holds the gate shared around every packet; a device replacement takes it
// exclusively. It is one SRW lock (HostSharedMutex): a pending exclusive acquire holds back new
// shared ones, as `replacing` does on the old path. The old path took a winpthreads mutex twice per
// packet and signalled the condition variable whenever the user count fell to zero, i.e. after
// every packet of the one queue-0 thread (APS5_NO_SLIM_DEVICE_GATE=1 keeps it).
class DeviceUseGate {
public:
    void lock_shared();
    void unlock_shared();
    void lock();
    void unlock();

private:
    HostSharedMutex slim;
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t users = 0;
    bool replacing = false;
};

}

#endif
