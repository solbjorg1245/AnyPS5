#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
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

// F5 of the draw fast path (docs/design/draw-fastpath.md 2.9) on a real device: a storage-buffer
// write recorded by VulkanDevice::FastDispatch (push descriptors, the V#s bound in place in the
// host import of their registered allocation) lands in guest memory, stamps its range as written
// and keeps its 64 KiB block pending until the batch completed; an old-path dispatch recorded
// right after it in the same batch reads what it wrote; a GPU-indirect fast dispatch takes its
// group counts from the import; a V# outside every registered allocation declines (no import)
// without recording anything.

namespace {

using AgcDriver::Graphics::Require;
using Decline = AgcDriver::Graphics::FastDispatchDecline;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Stride = 4;
constexpr std::size_t Words = Threads * Stride;
constexpr std::size_t BlockBytes = 65536;

// tests/execution/CacheControl.cpp's kernel: out[tid * 4] = in[tid * 4] + 1 through the V#s in
// user SGPRs 0-3 (input) and 4-7 (output), one group of 64 threads.
alignas(256) constexpr std::array<std::uint32_t, 23> IncrementCode{
    0x34020082, 0xbf930000, 0xbf940001, 0xe0302000, 0x80000401, 0xf4840000, 0x00000000, 0xf4800000,
    0x00000000, 0xf47c0000, 0x00000000, 0xe1c80000, 0x00000000, 0xe1c40000, 0x00000000, 0xbfa20000,
    0xbf8c3f70, 0xbfa80001, 0x4a080881, 0xbf950001, 0xe0702000, 0x80010401, 0xbf810000,
};

// Outside every registered allocation: no host import serves it.
alignas(256) std::array<std::uint32_t, Words> Unregistered{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

ShaderRecompiler::RecompileResult Compile(const AgcDriver::VulkanDevice& device, const std::uint32_t* input, const std::uint32_t* output) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto in = BufferDescriptor(input, static_cast<std::uint32_t>(Words));
    const auto out = BufferDescriptor(output, static_cast<std::uint32_t>(Words));
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

void CheckIncremented(const std::uint32_t* input, const std::uint32_t* output, std::uint32_t by, const char* what) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto expected = input[tid * Stride] + by;
        const auto actual = output[tid * Stride];
        Require(actual == expected, std::string(what) + ": thread " + std::to_string(tid) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
    }
}

// In the write-watched guest arena when there is one (the stamp check needs it), else plain memory.
void* AllocateBlock(bool& watched) {
    watched = false;
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
    Require(block != nullptr, "cannot allocate the fast dispatch test block");
    std::memset(block, 0, BlockBytes);
    watched = AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), BlockBytes);
    return block;
}

void Run(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver;
    bool watched = false;
    void* block = AllocateBlock(watched);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, BlockBytes, true, true);
    }
    auto* words = static_cast<std::uint32_t*>(block);
    auto* input = words;
    auto* fastOutput = words + 0x1000 / 4;
    auto* oldOutput = words + 0x2000 / 4;
    auto* arguments = words + 0x3000 / 4;
    auto* indirectOutput = words + 0x4000 / 4;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) input[tid * Stride] = tid * 0x01010101u + 7u;
    std::fill(fastOutput, fastOutput + Words, 0xdeadbeefu);
    std::fill(oldOutput, oldOutput + Words, 0xdeadbeefu);
    std::fill(indirectOutput, indirectOutput + Words, 0xdeadbeefu);
    arguments[0] = arguments[1] = arguments[2] = 1;
    const auto fast = Compile(device, input, fastOutput);
    const auto old = Compile(device, fastOutput, oldOutput);
    const auto indirect = Compile(device, input, indirectOutput);
    const auto unregistered = Compile(device, input, Unregistered.data());
    const auto program = reinterpret_cast<std::uintptr_t>(IncrementCode.data());
    const auto outputAddress = reinterpret_cast<std::uint64_t>(fastOutput);
    const auto outputBytes = Words * sizeof(std::uint32_t);

    std::lock_guard gpu(GuestMemory::GpuMutex());
    if (!Graphics::Recorder::PendingBlocksTracked()) throw std::runtime_error("APS5_FAST_DISPATCH did not turn the pending-block table on");
    const auto generation = watched ? GuestMemory::CollectWrites(outputAddress, outputBytes) : 0;
    Graphics::FastDispatchTiming timing;
    auto result = device.FastDispatch(fast, 1, 1, 1, 0, program, timing);
    if (result == Decline::NoPush || result == Decline::NoImport) {
        // No VK_KHR_push_descriptor, or no VK_EXT_external_memory_host: nothing to run on.
        std::printf("skipped, the device cannot take fast dispatches (%s)\n", Graphics::FastDispatchDeclineNames[static_cast<std::size_t>(*result)]);
        device.WaitIdle();
        return;
    }
    Require(!result && timing.recorded, std::string("the fast dispatch declined: ") + (result ? Graphics::FastDispatchDeclineNames[static_cast<std::size_t>(*result)] : ""));
    // Its two V#s bound in place; with no storage result or unit shadow anywhere, the one scan per
    // dispatch spared both elements their FlushPending (per element under APS5_FAST_DISPATCH_PER_ELEMENT=1).
    Require(timing.elements == 2, "the fast dispatch did not bind its two V#s in place");
    Require(timing.flushSkipped == Graphics::FastDispatchBatchedElements(), "the per-dispatch scan did not spare the elements their flush (or ran per element)");
    timing = {};
    const auto declined = device.FastDispatch(unregistered, 1, 1, 1, 0, program, timing);
    Require(declined == Decline::NoImport && !timing.recorded, std::string("a V# outside the registered allocations: ") + (declined ? Graphics::FastDispatchDeclineNames[static_cast<std::size_t>(*declined)] : "taken"));
    Require(Graphics::Recorder::BlockPending(outputAddress), "the written range's block is not pending before its batch completed");
    if (watched) Require(!GuestMemory::UnchangedSince(outputAddress, outputBytes, generation), "the written range was not stamped as written");
    // The old path in the same batch reads the fast dispatch's results.
    device.Dispatch(old, 1, 1, 1, {}, program);
    timing = {};
    result = device.FastDispatch(indirect, 0, 0, 0, reinterpret_cast<std::uint64_t>(arguments), program, timing);
    Require(!result, std::string("the indirect fast dispatch declined: ") + (result ? Graphics::FastDispatchDeclineNames[static_cast<std::size_t>(*result)] : ""));
    device.WaitIdle();
    Require(!Graphics::Recorder::BlockPending(outputAddress), "the written range's block is still pending after its batch completed");
    CheckIncremented(input, fastOutput, 1, "fast dispatch");
    CheckIncremented(input, oldOutput, 2, "old-path dispatch after the fast one");
    CheckIncremented(input, indirectOutput, 1, "indirect fast dispatch");
    Require(std::all_of(Unregistered.begin(), Unregistered.end(), [](std::uint32_t word) { return word == 0; }), "the declined dispatch wrote");
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv("APS5_FAST_DISPATCH=1");
#else
        setenv("APS5_FAST_DISPATCH", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::puts("fast dispatch tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
