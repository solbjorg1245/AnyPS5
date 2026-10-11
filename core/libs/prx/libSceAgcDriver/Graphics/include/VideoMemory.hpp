#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIDEOMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VIDEOMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace AgcDriver::Graphics {

// Video memory budget guard (APS5_VRAM_GUARD; port/reports/s53-gpu3-retile.md, -fix.md).
//
// Windows pages a process's device-local allocations out to system memory once it holds more than
// its video memory budget, and the GPU then reaches them over PCIe. t431 (APS5_RESIDENT_READS=1,
// ~1 GiB of resident copies on top of the 2 GiB the buffer pool retains) went over at the area load
// (one vkAllocateMemory refused) and the storage image write-backs, whose scratch buffers the pool
// hands out again and again, cost 25x from then on (5.2 -> 132 ms per present at the same count and
// bytes); nothing else changed on the GPU.
//
// The guard samples VK_EXT_memory_budget (the largest device-local heap: this process's usage and
// its budget) at most every APS5_VRAM_POLL_MS (250) from Recorder::Submit. The margin m is
// APS5_VRAM_MARGIN_MIB (512), at most a 16th of the budget (small heaps).
//  - An episode starts when usage + m > budget or an allocation is refused. It is "hard" when a
//    sample over the budget or a refusal was seen (only then may anything have been paged out).
//  - During an episode the guard frees what exceeds usage + 2m (the end threshold, so it can end
//    an episode it started): first the buffer pool's device-tier slots idle for APS5_VRAM_IDLE_MS
//    (1000; never the working set, and the pool's limit is left alone, so no allocate/free churn),
//    down to APS5_VRAM_POOL_FLOOR_MIB (0); then cached resident copies, least recently used, those
//    no build holds first; a held one is dropped from the cache and its builds are rebuilt (in
//    place while the episode lasts). Their buffers are destroyed, not retained by the pool. No new
//    resident copy is made.
//  - The episode ends when usage + 2m < budget, or when usage + m <= budget and the guard had
//    nothing left to free (the game itself holds the rest). Builds refused a copy for the budget
//    are rebuilt after it (Rounds), so they take copies again.
//  - Outside an episode a copy is made only while usage + the copies made since the sample + 2m
//    fit the budget (Admits).
//  - After a hard episode, at most every 10 s (doubling up to 160 s while recycles follow each
//    other within a minute), the allocations made before are recycled (the epoch advances): pooled
//    device buffers are destroyed instead of retained when they come back, resident copies are
//    made anew at their next use and the builds holding old ones are rebuilt, so nothing the OS
//    may have paged out keeps being reused. APS5_VRAM_NO_RECYCLE=1 skips it.
// Off (APS5_VRAM_GUARD=0; unset means on exactly when APS5_RESIDENT_READS is on): no action, the
// pool and the resident cache behave as before. With APS5_PROFILE_DRAW or APS5_PROFILE_GPU the
// samples and episodes are still observed (the [vram] line and the "under pressure" attribution
// counters), nothing is acted on.
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

// The resident read cache's trim: evicts cached copies, least recently used, those no build holds
// first, until the bytes of the unheld ones reach `bytes`; a held one is evicted too when the
// unheld ones do not suffice, and its builds are rebuilt (its memory goes with the last of them).
// Their buffers are destroyed instead of returned to the pool. Registered by the cache
// (GuestBufferMemory).
struct ResidentTrim {
    std::uint64_t bytes = 0, copies = 0;
    std::uint64_t heldBytes = 0, heldCopies = 0;
};
using ResidentTrimmer = ResidentTrim (*)(std::uint64_t bytes);
void SetResidentTrimmer(ResidentTrimmer trimmer);

// A pressure episode is open now. Observed with the guard off too while sampling (APS5_PROFILE_*:
// the attribution counters); only the guard acts on it (Admits refuses only with the guard on).
bool UnderPressure();
// Pressure episodes ended so far (guard on): a build refused a resident copy for the budget is
// rebuilt once this moved past the value at its refusal and no episode is open.
std::uint64_t Rounds();
// The recycle epoch: advanced after a hard pressure episode (never with the guard off). Pooled
// device allocations and resident copies remember the epoch they were made in.
std::uint64_t Epoch();
// The margin for a heap of `budget` bytes: APS5_VRAM_MARGIN_MIB, at most budget / 16.
std::uint64_t Margin(std::uint64_t budget);
// Whether a new resident copy of `bytes` fits (always true with the guard off); counts it as made
// until the next sample (a conservative estimate: a copy refused after it still counts).
bool Admits(std::uint64_t bytes);
// AllocateDeviceMemory saw VK_ERROR_OUT_OF_DEVICE_MEMORY: a hard episode starts (observed with the
// guard off while sampling).
void NoteOutOfMemory();
// A device-local allocation was made (not taken from the pool): counted when made under pressure.
void NoteDeviceAllocation(std::uint64_t bytes);

struct Counts {
    std::uint64_t polls = 0;
    std::uint64_t episodes = 0;
    // Episodes that saw usage over the budget or a refused allocation (they recycle), and those the
    // guard ended with nothing left to free.
    std::uint64_t hardEpisodes = 0;
    std::uint64_t exhaustedEnds = 0;
    std::uint64_t outOfMemory = 0;
    std::uint64_t poolTrimmedBytes = 0;
    std::uint64_t residentTrimmedBytes = 0;
    std::uint64_t residentTrimmedCopies = 0;
    // Held copies evicted (their memory goes when their builds are rebuilt).
    std::uint64_t residentHeldBytes = 0;
    std::uint64_t residentHeldCopies = 0;
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
// device's VK_EXT_memory_budget query), set the poll interval, margin, pool floor and the idle time
// a pool slot needs to be trimmed, and reset the state.
using Sampler = std::function<bool(const Context&, VideoMemorySample&)>;
void ConfigureForTests(int guard, Sampler sampler, std::uint32_t pollMs, std::uint64_t marginBytes, std::uint64_t poolFloorBytes, std::uint32_t idleMs = 0);
void ResetForTests();
// Tests: the recycle runs without its spacing.
void RecycleNowForTests(const Context& context);

}

}

#endif
