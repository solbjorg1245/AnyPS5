#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace AgcDriver::Graphics {

namespace {

// Whether a compiled program may store through its BDA table (resident read-only copies, see
// GuestBufferMemory.cpp "Resident reads"): the recompiler defines its write lookup, named
// "get_bda_write_pointer", only for a program with BDA stores, and every BDA store calls it
// (SpirvBdaLookup.cpp, SpirvBdaRead.cpp EmitBdaWrite). A module that lost its names (no
// "get_bda_pointer" or "probe_bda_pointer" either, an optimizer that inlined them) counts as
// storing, as does every program under APS5_RESIDENT_READS_ALL_WRITERS=1. Memoized by variant.
bool programStoresThroughBda(const ShaderRecompiler::RecompileResult& program) {
    static const bool all = std::getenv("APS5_RESIDENT_READS_ALL_WRITERS") != nullptr;
    if (all) return true;
    static HostMutex memoMutex;
    static std::unordered_map<std::uint64_t, bool> memo;
    if (program.variantId != 0) {
        std::lock_guard lock(memoMutex);
        if (const auto found = memo.find(program.variantId); found != memo.end()) return found->second;
    }
    const auto& words = program.spirv.Words();
    bool reads = false;
    bool writes = false;
    constexpr std::uint32_t OpName = 5;
    constexpr std::uint32_t OpFunction = 54;
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16u;
        const auto opcode = words[at] & 0xffffu;
        if (count == 0 || opcode == OpFunction || at + count > words.size()) break;
        if (opcode == OpName && count >= 3) {
            const auto* text = reinterpret_cast<const char*>(&words[at + 2]);
            const std::string_view name(text, strnlen(text, static_cast<std::size_t>(count - 2) * 4u));
            if (name == "get_bda_write_pointer") writes = true;
            else if (name == "get_bda_pointer" || name == "probe_bda_pointer") reads = true;
        }
        at += count;
    }
    const bool stores = writes || !reads;
    if (program.variantId != 0) {
        std::lock_guard lock(memoMutex);
        memo.emplace(program.variantId, stores);
    }
    return stores;
}

}

void ShaderResources::prepareAddressBindings(std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots) {
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        std::uint32_t tables = 0;
        std::uint32_t faults = 0;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::BdaPagetable && binding.role != ShaderRecompiler::DescriptorRole::FaultBuffer) continue;
            Require(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer && binding.count == 1 && binding.guestDescriptor.empty() && !binding.readOnly, "invalid BDA descriptor contract");
            if (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable) ++tables;
            else ++faults;
        }
        const bool rectListFault = shader.stage == ShaderRecompiler::ShaderStage::TessellationControl && tables == 0 && faults == 1;
        Require((tables == faults || rectListFault) && tables <= 1 && faults <= 1, "invalid BDA table and fault descriptors");
        Require(shader.program->bdaAbiVersion == (faults == 0 ? 0u : ShaderRecompiler::BdaAbi::Version), "incompatible BDA ABI version");
        // Rect-list validation needs a fault buffer, but never accesses guest addresses.
        usesBda = usesBda || tables != 0;
        bdaStores = bdaStores || (tables != 0 && programStoresThroughBda(*shader.program));
        usesFaultBuffer = usesFaultBuffer || faults != 0;
    }
    if (usesBda) {
        Require(context.bufferDeviceAddress, "buffer device address is not enabled");
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        guestMemory.AcquireRegistered();
        const auto snapshotsStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        for (const auto& snapshot : snapshots) guestMemory.AddSnapshot(snapshot);
        if (profile) GuestBufferMemory::CountAddressBuild(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - snapshotsStart).count());
    }
}

VkDescriptorBufferInfo ShaderResources::descriptor(Allocation& allocation) {
    if (allocation.guest) return guestMemory.Descriptor(allocation.address, allocation.size, allocation.adjustment);
    if (allocation.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
        Require(context.emptyBuffer != VK_NULL_HANDLE, "no placeholder buffer for a null V#");
        return {context.emptyBuffer, 0, allocation.size};
    }
    if (allocation.role == ShaderRecompiler::DescriptorRole::BdaPagetable || allocation.role == ShaderRecompiler::DescriptorRole::FaultBuffer) {
        Require(bda != nullptr, "BDA descriptors have no memory owner");
        return allocation.role == ShaderRecompiler::DescriptorRole::BdaPagetable ? bda->Table() : bda->Fault();
    }
    Require(allocation.buffer != nullptr, "shader data has no buffer owner");
    return {allocation.buffer->Handle(), 0, allocation.size};
}

}
