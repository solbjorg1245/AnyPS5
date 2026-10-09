#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// The template refresh through the fast ring (APS5_TEMPLATE_REFRESH_RING=1, set below) on a real
// device, through VulkanDevice::Dispatch: a kernel adds a constant it loads through its SRT pointer
// (a flattened-SRT word, so the constant is data, not code) to each thread's word of one buffer, in
// place. Dispatched with +1 (the template is built with these words), +5 (the same variant with the
// addend's data word patched, as the driver's data-only hits do), +1, +5, +5 in one batch:
// the +5 hits differ from the template's words and bind a copy of its set whose data binding reads
// the ring (no vkCmdUpdateBuffer, no leading barrier for it), the +1 hits bind the template's own
// set. Each element must end up 17 higher, and three of the hits must have gone through the ring.

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Stride = 4;
constexpr std::size_t Words = Threads * Stride;
constexpr std::size_t BlockBytes = 65536;

// buffer[tid * 4] += table[0]: the V# in user SGPRs 0-3 (load and store), the table pointer in 4-5.
//   v_lshlrev_b32 v1, 2, v0; s_load_dword s8, s[4:5], 0x0; buffer_load_dword v4, v1, s[0:3], 0 idxen;
//   s_waitcnt vmcnt(0) lgkmcnt(0); v_add_nc_u32 v4, s8, v4; buffer_store_dword v4, v1, s[0:3], 0 idxen;
//   s_endpgm
alignas(256) constexpr std::array<std::uint32_t, 10> AddCode{
    0x34020082, 0xf4000202, 0xfa000000, 0xe0302000, 0x80000401, 0xbf8c0070, 0x4a080808, 0xe0702000,
    0x80000401, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

// The kernel compiled against `table` (its first word is the addend the capture flattens).
ShaderRecompiler::RecompileResult Compile(const AgcDriver::VulkanDevice& device, const std::uint32_t* buffer, const std::array<std::uint32_t, 4>& table) {
    std::vector<std::uint32_t> userData(6, 0u);
    const auto descriptor = BufferDescriptor(buffer, static_cast<std::uint32_t>(Words));
    std::copy(descriptor.begin(), descriptor.end(), userData.begin());
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(table.data());
    userData[4] = static_cast<std::uint32_t>(tableAddress);
    userData[5] = static_cast<std::uint32_t>(tableAddress >> 32u);
    const std::span<const std::uint32_t> code(AddCode);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {tableAddress, std::as_bytes(std::span(table))}}};
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

void* AllocateBlock() {
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(BlockBytes, BlockBytes);
#endif
    Require(block != nullptr, "cannot allocate the template refresh test block");
    std::memset(block, 0, BlockBytes);
    return block;
}

// The data word the addend became: the one word of a data binding that differs between two
// compiles against different tables, as (binding, word); none when the capture did not flatten it.
std::optional<std::pair<std::size_t, std::size_t>> AddendWord(const ShaderRecompiler::RecompileResult& one, const ShaderRecompiler::RecompileResult& other) {
    std::optional<std::pair<std::size_t, std::size_t>> found;
    if (one.bindings.size() != other.bindings.size()) return std::nullopt;
    for (std::size_t binding = 0; binding < one.bindings.size(); ++binding) {
        const auto& words = one.bindings[binding].guestDescriptor;
        const auto& otherWords = other.bindings[binding].guestDescriptor;
        const auto role = one.bindings[binding].role;
        if (words.size() != otherWords.size()) return std::nullopt;
        for (std::size_t word = 0; word < words.size(); ++word) {
            if (words[word] == otherWords[word]) continue;
            if (found || (role != ShaderRecompiler::DescriptorRole::FlattenedSrt && role != ShaderRecompiler::DescriptorRole::ShaderData) || words[word] != 1u) return std::nullopt;
            found = std::make_pair(binding, word);
        }
    }
    return found;
}

int Run(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver;
    void* block = AllocateBlock();
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, BlockBytes, true, true);
    }
    auto* buffer = static_cast<std::uint32_t*>(block);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) buffer[tid * Stride] = tid * 0x01010101u + 7u;
    std::vector<std::uint32_t> initial(buffer, buffer + Words);
    alignas(16) static const std::array<std::uint32_t, 4> plusOne{1, 0, 0, 0};
    alignas(16) static const std::array<std::uint32_t, 4> probeTable{0x5a5a5a5au, 0, 0, 0};
    const auto one = Compile(device, buffer, plusOne);
    // The +5 dispatch is the +1 compile with its addend word patched, as the driver's data-only
    // hits patch a variant's words: one variant, so one template serves both.
    const auto addend = AddendWord(one, Compile(device, buffer, probeTable));
    if (!addend) {
        std::printf("skipped, the capture did not make the addend one data word\n");
        return VulkanTestSkipped;
    }
    auto patched = one;
    patched.bindings[addend->first].guestDescriptor[addend->second] = 5u;
    const auto& five = patched;
    const auto program = reinterpret_cast<std::uintptr_t>(AddCode.data());
    std::lock_guard gpu(GuestMemory::GpuMutex());
    const auto before = VulkanDevice::TemplateRefreshes();
    for (const auto* shader : {&one, &five, &one, &five, &five}) device.Dispatch(*shader, 1, 1, 1, {}, program);
    device.WaitIdle();
    const auto after = VulkanDevice::TemplateRefreshes();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto expected = initial[tid * Stride] + 17u;
        const auto actual = buffer[tid * Stride];
        Require(actual == expected, "thread " + std::to_string(tid) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected) + " (+1 +5 +1 +5 +5)");
    }
    const auto refreshed = after.refreshed - before.refreshed;
    const auto throughRing = after.throughRing - before.throughRing;
    const auto same = after.sameWords - before.sameWords;
    std::printf("template hits: %llu refreshed (%llu through the ring), %llu with the template's words\n", static_cast<unsigned long long>(refreshed), static_cast<unsigned long long>(throughRing), static_cast<unsigned long long>(same));
    if (refreshed + same == 0) {
        // No host imports (or no texture caches): the buffer is copied, so no build is a template.
        std::printf("skipped, no dispatch was a template hit\n");
        return VulkanTestSkipped;
    }
    Require(refreshed == 3 && throughRing == 3 && same == 1, "the +5 hits did not all refresh through the ring");
    return 0;
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv("APS5_TEMPLATE_REFRESH_RING=1");
#else
        setenv("APS5_TEMPLATE_REFRESH_RING", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto result = Run(*device);
        if (result == 0) std::puts("template refresh ring tests passed");
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
