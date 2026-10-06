#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP

// Mutexes on the host's own primitives. MinGW's std::mutex, std::recursive_mutex and
// std::shared_mutex go through libwinpthread, whose uncontended lock and unlock cost a tenth of
// the AGC driver's draw thread in Demon's Souls (drvprof: libwinpthread-1.dll 11.5% exclusive);
// an SRW lock acquires and releases in a few instructions, a critical section with a spin count
// nearly so. Usable with std::lock_guard, std::unique_lock and std::shared_lock. A condition
// variable needs std::mutex, so a mutex it waits on stays a std::mutex.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h macros that collide with identifiers in code including this header.
#undef interface
#undef near
#undef far
#undef small
#undef IN
#undef OUT
#undef OPTIONAL

class HostMutex {
public:
    HostMutex() = default;
    HostMutex(const HostMutex&) = delete;
    HostMutex& operator=(const HostMutex&) = delete;
    void lock() { AcquireSRWLockExclusive(&handle); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&handle) != 0; }
    void unlock() { ReleaseSRWLockExclusive(&handle); }

private:
    SRWLOCK handle = SRWLOCK_INIT;
};

class HostSharedMutex {
public:
    HostSharedMutex() = default;
    HostSharedMutex(const HostSharedMutex&) = delete;
    HostSharedMutex& operator=(const HostSharedMutex&) = delete;
    void lock() { AcquireSRWLockExclusive(&handle); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&handle) != 0; }
    void unlock() { ReleaseSRWLockExclusive(&handle); }
    void lock_shared() { AcquireSRWLockShared(&handle); }
    bool try_lock_shared() { return TryAcquireSRWLockShared(&handle) != 0; }
    void unlock_shared() { ReleaseSRWLockShared(&handle); }

private:
    SRWLOCK handle = SRWLOCK_INIT;
};

class HostRecursiveMutex {
public:
    HostRecursiveMutex() { InitializeCriticalSectionAndSpinCount(&section, 4000); }
    ~HostRecursiveMutex() { DeleteCriticalSection(&section); }
    HostRecursiveMutex(const HostRecursiveMutex&) = delete;
    HostRecursiveMutex& operator=(const HostRecursiveMutex&) = delete;
    void lock() { EnterCriticalSection(&section); }
    bool try_lock() { return TryEnterCriticalSection(&section) != 0; }
    void unlock() { LeaveCriticalSection(&section); }

private:
    CRITICAL_SECTION section;
};
#else
#include <mutex>
#include <shared_mutex>
using HostMutex = std::mutex;
using HostSharedMutex = std::shared_mutex;
using HostRecursiveMutex = std::recursive_mutex;
#endif

#endif
