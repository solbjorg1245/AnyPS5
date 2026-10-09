#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIDEOMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIDEOMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

// Video memory budget guard (APS5_VRAM_GUARD; port/reports/s53-gpu3-retile.md).
//
// Windows pages a process's device-local allocations out to system memory once it holds more than
// its video memory budget, and the GPU then reaches them over PCIe. t431 (APS5_RESIDENT_READS=1,
// ~1 GiB of resident copies on top of the 2 GiB the buffer pool retains) went over at the area load
// (one vkAllocateMemory refused) and the storage image write-backs, whose scratch buffers the pool
// hands out again and again, cost 25x from then on (5.2 -> 132 ms per present at the same count and
// bytes); nothing else changed on the GPU.
//
// The guard samples VK_EXT_memory_budget (the largest device-local heap: this process's usage and
// its budget) at most every APS5_VRAM_POLL_MS (250) from Recorder::Submit and keeps the usage under
// the budget less a margin (APS5_VRAM_MARGIN_MIB, 512):
//  - under pressure (usage + margin > budget, or a refused allocation) the device tier of the buffer
//    pool gives back its least recently used retained slots (down to APS5_VRAM_POOL_FLOOR_MIB, 256)
//    and retains no more than that until the pressure ends; then the resident read-only copies are
//    evicted, least recently used first; no new resident copy is made;
//  - a resident copy is only made while the sampled usage plus the copies made since, plus twice
//    the margin, stays under the budget (Admits), so the cache never pushes the process over;
//  - a pressure episode ends once usage + 2 x margin < budget (a refusal without budget support: 2 s
//    after the last one). Then, at most every 10 s, the allocations made before are recycled (the
//    guard's epoch advances): pooled device buffers are destroyed instead of retained when they come
//    back, resident copies are made anew at their next use and the builds holding old ones are
//    rebuilt, so nothing the OS may have paged out during the episode keeps being reused.
// Off (APS5_VRAM_GUARD=0; unset means on exactly when APS5_RESIDENT_READS is on): no action, the
// pool and the resident cache behave as before; the [vram] line still reports with
// APS5_PROFILE_DRAW or APS5_PROFILE_GPU (sampling only).
struct VideoMemorySample {
    bool valid = false;
    // The largest device-local heap and the largest other heap: this process's usage and budget.
    std::uint64_t usage = 0;
    std::uint64_t budget = 0;
    std::uint64_t hostUsage = 0;
    std::uint64_t hostBudget = 0;
};

namespace VideoMemory {

// APS5_VRAM_GUARD (read once): "0" off, anything else on; unset: on when APS5_RESIDENT_READS is on.
bool GuardEnabled();
// The [vram] line every 10 s: the guard is on, or APS5_PROFILE_DRAW / APS5_PROFILE_GPU is set.
bool Reporting();
// The device asks for VK_EXT_memory_budget when either holds.
bool Wanted();

// Samples (rate-limited) and acts as described above. Under the GPU mutex (Recorder::Submit).
void Poll(const Context& context);

// The resident read cache's trim: evicts copies (least recently used) worth at least `bytes`,
// returns the bytes and copies evicted. Registered by the cache (GuestBufferMemory).
using ResidentTrimmer = std::pair<std::uint64_t, std::uint64_t> (*)(std::uint64_t bytes);
void SetResidentTrimmer(ResidentTrimmer trimmer);

// A pressure episode is open now (always false with the guard off).
bool UnderPressure();
// The recycle epoch: advanced after a pressure episode (never with the guard off). Pooled device
// allocations and resident copies remember the epoch they were made in.
std::uint64_t Epoch();
// Whether a new resident copy of `bytes` fits (always true with the guard off); counts it as made
// until the next sample.
bool Admits(std::uint64_t bytes);
// AllocateDeviceMemory saw VK_ERROR_OUT_OF_DEVICE_MEMORY: an episode starts (guard on).
void NoteOutOfMemory();
// A device-local allocation was made (not taken from the pool): counted when made under pressure.
void NoteDeviceAllocation(std::uint64_t bytes);

struct Counts {
    std::uint64_t polls = 0;
    std::uint64_t episodes = 0;
    std::uint64_t outOfMemory = 0;
    std::uint64_t poolTrimmedBytes = 0;
    std::uint64_t residentTrimmedBytes = 0;
    std::uint64_t residentTrimmedCopies = 0;
    std::uint64_t residentRefused = 0;
    std::uint64_t recycles = 0;
    std::uint64_t recycledPoolBytes = 0;
    std::uint64_t pressuredAllocations = 0;
    std::uint64_t pressuredBytes = 0;
    double pressureSeconds = 0;
    VideoMemorySample last {};
    std::uint64_t peakUsage = 0;
};
Counts Read();
// The [vram] line (the counts since the last report).
std::string Report();

// Tests: force the guard on or off (-1 back to the environment), replace the sampler (null: the
// device's VK_EXT_memory_budget query), set the poll interval and margins, and reset the state.
using Sampler = std::function<bool(const Context&, VideoMemorySample&)>;
void ConfigureForTests(int guard, Sampler sampler, std::uint32_t pollMs, std::uint64_t marginBytes, std::uint64_t poolFloorBytes);
void ResetForTests();
// Tests: the recycle runs without its 10 s spacing.
void RecycleNowForTests(const Context& context);

}

}

#endif
