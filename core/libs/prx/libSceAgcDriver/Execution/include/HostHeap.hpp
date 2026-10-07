#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTHEAP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTHEAP_HPP

#include <cstdint>

namespace AgcDriver::HostHeap {

// What the driver heap (OperatorNew.cpp) did on the calling thread since it started: every
// operator new and delete, and how many of them the thread's block lists served (cacheHits) or
// took (cacheStores), whether the lists are filled by the block arena or the block cache. All
// zero when both are disabled (APS5_NO_BLOCK_CACHE=1).
struct Counters {
    std::uint64_t allocations = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t releases = 0;
    std::uint64_t cacheStores = 0;
};

Counters ThreadCounters();

// "arena" (the default), "block cache" (APS5_NO_BLOCK_ARENA=1) or "heap" (APS5_NO_BLOCK_CACHE=1).
const char* ModeName();

// The block arena's footprint: chunks committed so far (never decommitted), and the part of it
// sitting on the shared per-class lists rather than on a thread's own. Zero without the arena.
struct ArenaStatus {
    std::uint64_t committedBytes = 0;
    std::uint64_t sharedBytes = 0;
};

ArenaStatus Arena();

}

#endif
