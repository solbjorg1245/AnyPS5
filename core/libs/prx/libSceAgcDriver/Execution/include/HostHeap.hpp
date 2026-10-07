#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTHEAP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTHEAP_HPP

#include <cstdint>

namespace AgcDriver::HostHeap {

// What the driver heap (OperatorNew.cpp) did on the calling thread since it started: every
// operator new and delete, and how many of them the per-thread block cache served or kept. All
// zero when the block cache is disabled (APS5_NO_BLOCK_CACHE=1).
struct Counters {
    std::uint64_t allocations = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t releases = 0;
    std::uint64_t cacheStores = 0;
};

Counters ThreadCounters();

}

#endif
