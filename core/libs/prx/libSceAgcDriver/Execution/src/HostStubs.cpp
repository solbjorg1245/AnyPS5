// Linux (and any non-MinGW) stand-ins for the MinGW-only host replacements: FastClock.cpp's read
// counter and OperatorNew.cpp's heap report. glibc's clock_gettime and malloc need no replacement,
// so the [packets] and [heap] profile lines read zero clock reads and the plain heap.

#include "prx/libSceAgcDriver/Execution/include/HostHeap.hpp"

#include <cstdint>

std::uint64_t DriverClockReads() {
    return 0;
}

namespace AgcDriver::HostHeap {

Counters ThreadCounters() {
    return {};
}

const char* ModeName() {
    return "heap";
}

ArenaStatus Arena() {
    return {};
}

}
