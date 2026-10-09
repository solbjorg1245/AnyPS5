#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// Narrow copy-backs end to end (APS5_NARROW_COPY_BACKS=1, with APS5_COALESCE_COPY_BACKS=1 and every
// written element staged, APS5_WRITTEN_SHADOW_MIN_KIB=0) on a real device: old-path dispatches
// whose output V# is staged in a device-local shadow copy back only the dwords they changed, and
// the results land as a whole copy-back would have landed them. The kernel stores
// out[tid * 4] = in[tid * 4] + 1, leaving three of every four dwords of the written range alone:
// - X writes B from A, then Y (another build over the same range: it takes X's shadow with its
//   baseline and X's queued copy-back) puts B's original words back, in one batch: the import
//   must end with the original words (X's copy was taken over, never landed: the baseline Y
//   inherited still holds them, so Y stores nothing there);
// - the same with a Submit between X and Y (X's copy-back lands, its baseline with it; Y's
//   restore differs from the inherited baseline and is stored);
// - Z reads and writes B three times in one batch (the same build's shadow in place);
// - X writes B, the batch completes, the CPU stores into B, X writes B again: the CPU store
//   refuses the staging chain, so the second use copies in from the import and refreshes its
//   baseline (an inherited one would equal what X stores again, and its copy-back would leave the
//   CPU's words in the import);
// - first of all, F reads and writes a fresh range three times in one batch: its first use copies
//   in from the import (a trusted baseline), so the second use, chained in place, claims a narrow
//   copy-back that is still queued when its dispatch is recorded (under APS5_PROFILE_GPU, run with
//   --profile-gpu, the program's timing range records it: that must happen ahead of the program's
//   binds, or the guest dispatch runs the compare pass instead);
// - W writes the first quarter of a V# larger than APS5_NARROW_COPY_BACK_MAX_KIB (4 here): its
//   shadow has no baseline, so its copy-back falls back to the whole written range, as before.
// With APS5_PROFILE_DRAW=1 the passes count the dwords they stored: at most one in four of those
// compared (the kernel changes one dword of every four), and some. Arguments: --profile-gpu sets
// APS5_PROFILE_GPU=1 (timing ranges flush queued copies at every range, which also breaks claims:
// fewer copies narrow), --verify sets APS5_NARROW_VERIFY=1 (no dword may be found stale),
// --resident sets APS5_RESIDENT_BUFFERS=1 (copies kept guarded past a Submit land when the CPU
// reads the results: the landing keeps the baseline equal to the import).

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Stride = 4;
constexpr std::size_t Words = Threads * Stride;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Untouched = 0xdeadbeefu;

// tests/execution/CacheControl.cpp's kernel: out[tid * 4] = in[tid * 4] + 1 through the V#s in
// user SGPRs 0-3 (input) and 4-7 (output), one group of 64 threads.
alignas(256) constexpr std::array<std::uint32_t, 23> IncrementCode{
    0x34020082, 0xbf930000, 0xbf940001, 0xe0302000, 0x80000401, 0xf4840000, 0x00000000, 0xf4800000,
    0x00000000, 0xf47c0000, 0x00000000, 0xe1c80000, 0x00000000, 0xe1c40000, 0x00000000, 0xbfa20000,
    0xbf8c3f70, 0xbfa80001, 0x4a080881, 0xbf950001, 0xe0702000, 0x80010401, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

ShaderRecompiler::RecompileResult Compile(const AgcDriver::VulkanDevice& device, const std::uint32_t* input, const std::uint32_t* output, std::size_t outputWords = Words) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto in = BufferDescriptor(input, static_cast<std::uint32_t>(Words));
    const auto out = BufferDescriptor(output, static_cast<std::uint32_t>(outputWords));
    std::copy(in.begin(), in.end(), userData.begin());
    std::copy(out.begin(), out.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(IncrementCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

// In the write-watched guest arena when there is one (the staging chain needs the tracking), else
// plain memory (every use then copies in from the import).
void* AllocateBlock() {
#ifdef _WIN32
    void* block = nullptr;
    if (AgcDriver::GuestMemory::WriteWatched()) {
        block = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
    }
    if (block == nullptr) block = VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(BlockBytes, BlockBytes);
#endif
    Require(block != nullptr, "cannot allocate the narrow copy-back test block");
    std::memset(block, 0, BlockBytes);
    return block;
}

std::uint32_t Original(std::uint32_t tid) {
    return tid * 0x01010101u + 9u;
}

void CheckOutput(const std::uint32_t* output, std::uint32_t by, const char* what) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Require(output[tid * Stride] == Original(tid) + by, std::string(what) + ": thread " + std::to_string(tid) + " stored " + std::to_string(output[tid * Stride]) + ", expected " + std::to_string(Original(tid) + by));
        for (std::uint32_t word = 1; word < Stride; ++word) {
            Require(output[tid * Stride + word] == Untouched, std::string(what) + ": a word no thread stores changed at thread " + std::to_string(tid));
        }
    }
}

// The wide V# (W): 2048 dwords (8 KiB), of which the kernel writes the first 256's every fourth.
constexpr std::size_t WideWords = 2048;

bool Run(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver;
    void* block = AllocateBlock();
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, BlockBytes, true, true);
    }
    auto* words = static_cast<std::uint32_t*>(block);
    auto* source = words;
    auto* output = words + 0x1000 / 4;
    auto* restore = words + 0x2000 / 4;
    auto* fresh = words + 0x3000 / 4;
    auto* wide = words + 0x4000 / 4;
    std::fill(output, output + Words, Untouched);
    std::fill(wide, wide + WideWords, Untouched);
    std::fill(fresh, fresh + Words, Untouched);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        source[tid * Stride] = tid * 3u + 5u;
        output[tid * Stride] = Original(tid);
        restore[tid * Stride] = Original(tid) - 1u;
        fresh[tid * Stride] = Original(tid);
    }
    const auto x = Compile(device, source, output);
    const auto y = Compile(device, restore, output);
    const auto z = Compile(device, output, output);
    const auto w = Compile(device, source, wide, WideWords);
    const auto f = Compile(device, fresh, fresh);
    const auto program = reinterpret_cast<std::uintptr_t>(IncrementCode.data());

    std::lock_guard gpu(GuestMemory::GpuMutex());
    // A fresh range in place, three times in one batch (the second and third uses claim the
    // previous use's queued narrow copy-back).
    for (int run = 0; run < 3; ++run) device.Dispatch(f, 1, 1, 1, {}, program);
    device.WaitIdle();
    CheckOutput(fresh, 3, "three uses of a fresh range in place");
    const auto before = Graphics::Recorder::CopyBackCounts();
    // X's copy-back taken over by Y in the same batch.
    device.Dispatch(x, 1, 1, 1, {}, program);
    device.Dispatch(y, 1, 1, 1, {}, program);
    device.WaitIdle();
    CheckOutput(output, 0, "a restore taking over the earlier copy-back");
    // X's copy-back recorded first (a Submit between).
    device.Dispatch(x, 1, 1, 1, {}, program);
    auto* recorder = Graphics::Recorder::Active();
    Require(recorder != nullptr, "no active recorder");
    recorder->Submit();
    device.Dispatch(y, 1, 1, 1, {}, program);
    device.WaitIdle();
    CheckOutput(output, 0, "a restore after the earlier copy-back landed");
    // The same build in place, three times in one batch.
    for (int run = 0; run < 3; ++run) device.Dispatch(z, 1, 1, 1, {}, program);
    device.WaitIdle();
    CheckOutput(output, 3, "three read-modify-write uses of one shadow");
    // A CPU store between two uses breaks the chain: the second copies in from the import.
    device.Dispatch(x, 1, 1, 1, {}, program);
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) output[tid * Stride] = 0x77777777u ^ tid;
    device.Dispatch(x, 1, 1, 1, {}, program);
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Require(output[tid * Stride] == source[tid * Stride] + 1u, "a use after a CPU store: thread " + std::to_string(tid) + " left " + std::to_string(output[tid * Stride]) + ", expected " + std::to_string(source[tid * Stride] + 1u));
    }
    const auto beforeWide = Graphics::Recorder::CopyBackCounts();
    // Over the baseline cap: whole copy-backs, twice in one batch (the second takes the first over).
    device.Dispatch(w, 1, 1, 1, {}, program);
    device.Dispatch(w, 1, 1, 1, {}, program);
    device.WaitIdle();
    const auto after = Graphics::Recorder::CopyBackCounts();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Require(wide[tid * Stride] == tid * 3u + 6u, "a region over the baseline cap: thread " + std::to_string(tid) + " stored " + std::to_string(wide[tid * Stride]));
        for (std::uint32_t word = 1; word < Stride; ++word) Require(wide[tid * Stride + word] == Untouched, "a region over the baseline cap: a word no thread stores changed");
    }
    for (std::size_t word = Words; word < WideWords; ++word) Require(wide[word] == Untouched, "a region over the baseline cap: a word past the written quarter changed");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    if (after.deferred == before.deferred) {
        // Nothing staged (no host imports): the results above came through the other paths.
        std::printf("skipped, nothing was staged (no host imports?)\n");
        return false;
    }
    Require(beforeWide.narrow > before.narrow, "staged copy-backs were not recorded narrow");
    // Resident buffers (--resident) keep the copy guarded past the Submit; the CPU's read of the
    // results above landed it through a fault instead of a recorded pass.
    const bool resident = std::getenv("APS5_RESIDENT_BUFFERS") != nullptr;
    Require(after.narrow == beforeWide.narrow && (resident || after.recorded > beforeWide.recorded), "a region over the baseline cap was not copied back whole (narrow " + std::to_string(beforeWide.narrow) + " -> " + std::to_string(after.narrow) + ", recorded " + std::to_string(beforeWide.recorded) + " -> " + std::to_string(after.recorded) + ")");
    // Only changed dwords crossed: at most one in four of those compared, and some.
    const auto compared = beforeWide.narrowBytes - before.narrowBytes;
    const auto stored = beforeWide.narrowStoredBytes - before.narrowStoredBytes;
    Require(stored != 0 && stored * 4 <= compared, "narrow copy-backs stored " + std::to_string(stored) + " of " + std::to_string(compared) + " bytes compared");
    // APS5_NARROW_VERIFY (--verify): no baseline was stale.
    Require(after.narrowStale == 0, "APS5_NARROW_VERIFY found " + std::to_string(after.narrowStale) + " stale dwords");
    // Under APS5_PROFILE_GPU (--profile-gpu) every timing range records the queued copies, claimed
    // ones too: a baseline inherited from a shadow whose claimed copy was recorded since loses its
    // trust (Recorder::ClaimBreaks), and the results above are still exact.
    const auto trust = Graphics::GuestBufferMemory::NarrowTrustCounts();
    if (std::getenv("APS5_PROFILE_GPU") != nullptr) Require(trust.claimBroken != 0, "no inherited baseline lost its trust to a claim break under APS5_PROFILE_GPU");
    std::printf("baselines: %llu refreshed, %llu inherited (%llu untrusted), %llu copy-backs narrow, %llu whole, %llu claims broken\n", static_cast<unsigned long long>(trust.refreshed), static_cast<unsigned long long>(trust.inherited), static_cast<unsigned long long>(trust.inheritedUntrusted), static_cast<unsigned long long>(trust.narrow), static_cast<unsigned long long>(trust.whole), static_cast<unsigned long long>(trust.claimBroken));
    std::printf("%llu copy-backs deferred, %llu recorded (%llu narrow in %llu spans, %llu with a baseline copied whole) in %llu passes; %llu bytes compared, %llu stored\n", static_cast<unsigned long long>(after.deferred - before.deferred), static_cast<unsigned long long>(after.recorded - before.recorded), static_cast<unsigned long long>(after.narrow - before.narrow), static_cast<unsigned long long>(after.narrowSpans - before.narrowSpans), static_cast<unsigned long long>(after.narrowWhole - before.narrowWhole), static_cast<unsigned long long>(after.passes - before.passes), static_cast<unsigned long long>(compared), static_cast<unsigned long long>(stored));
    return true;
}

}

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const char* name = argument == "--profile-gpu" ? "APS5_PROFILE_GPU" : argument == "--verify" ? "APS5_NARROW_VERIFY" : argument == "--resident" ? "APS5_RESIDENT_BUFFERS" : nullptr;
            Require(name != nullptr, "unknown argument " + argument + " (--profile-gpu, --verify, --resident)");
#ifdef _WIN32
            _putenv_s(name, "1");
#else
            setenv(name, "1", 1);
#endif
        }
#ifdef _WIN32
        _putenv("APS5_COALESCE_COPY_BACKS=1");
        _putenv("APS5_NARROW_COPY_BACKS=1");
        _putenv("APS5_WRITTEN_SHADOW_MIN_KIB=0");
        _putenv("APS5_NARROW_COPY_BACK_MAX_KIB=4");
        _putenv("APS5_PROFILE_DRAW=1");
#else
        setenv("APS5_COALESCE_COPY_BACKS", "1", 1);
        setenv("APS5_NARROW_COPY_BACKS", "1", 1);
        setenv("APS5_WRITTEN_SHADOW_MIN_KIB", "0", 1);
        setenv("APS5_NARROW_COPY_BACK_MAX_KIB", "4", 1);
        setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!Run(*device)) return 0;
        std::puts("narrow copy-back tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
