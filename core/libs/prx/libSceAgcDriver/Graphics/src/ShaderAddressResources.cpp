#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace AgcDriver::Graphics {

bool SpirvMayStoreThroughBda(std::span<const std::uint32_t> words) {
    // Opcodes (SPIR-V 1.6 unified): types, the pointer producers typed below (each has its result
    // type and id in words 1 and 2), and the stores with the word of their pointer operand.
    constexpr std::uint32_t OpTypePointer = 32, OpTypeForwardPointer = 39;
    constexpr std::uint32_t PhysicalStorageBuffer = 5349;
    constexpr std::uint32_t producers[] = {
        1,    // OpUndef
        55,   // OpFunctionParameter
        57,   // OpFunctionCall
        59,   // OpVariable
        61,   // OpLoad
        65,   // OpAccessChain
        66,   // OpInBoundsAccessChain
        67,   // OpPtrAccessChain
        70,   // OpInBoundsPtrAccessChain
        83,   // OpCopyObject
        120,  // OpConvertUToPtr
        122,  // OpGenericCastToPtr
        124,  // OpBitcast
        169,  // OpSelect
        245,  // OpPhi
        400,  // OpCopyLogical
    };
    const auto storeOperand = [](std::uint32_t opcode) -> std::uint32_t {
        switch (opcode) {
        case 62:   // OpStore
        case 63:   // OpCopyMemory (target)
        case 64:   // OpCopyMemorySized (target)
        case 228:  // OpAtomicStore
        case 319:  // OpAtomicFlagClear
            return 1;
        case 229: case 230: case 231: case 232: case 233: case 234: case 235: case 236: case 237:
        case 238: case 239: case 240: case 241: case 242:  // OpAtomicExchange .. OpAtomicXor
        case 318:   // OpAtomicFlagTestAndSet
        case 5614: case 5615: case 6035:  // OpAtomicFMinEXT, OpAtomicFMaxEXT, OpAtomicFAddEXT
            return 3;
        default:
            return 0;
        }
    };
    if (words.size() < 5) return true;
    std::unordered_set<std::uint32_t> physical;
    std::unordered_map<std::uint32_t, std::uint32_t> typeOf;
    // Two passes: a pointer's type is known wherever it is used.
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16u;
        const auto opcode = words[at] & 0xffffu;
        if (count == 0 || at + count > words.size()) return true;
        if (opcode == OpTypePointer && count >= 4 && words[at + 2] == PhysicalStorageBuffer) physical.insert(words[at + 1]);
        else if (opcode == OpTypeForwardPointer && count >= 3 && words[at + 2] == PhysicalStorageBuffer) physical.insert(words[at + 1]);
        else if (count >= 3 && std::find(std::begin(producers), std::end(producers), opcode) != std::end(producers)) typeOf.emplace(words[at + 2], words[at + 1]);
        at += count;
    }
    for (std::size_t at = 5; at < words.size();) {
        const auto count = words[at] >> 16u;
        const auto opcode = words[at] & 0xffffu;
        if (const auto operand = storeOperand(opcode); operand != 0) {
            if (count <= operand) return true;
            const auto pointer = words[at + operand];
            const auto found = typeOf.find(pointer);
            if (found == typeOf.end() || physical.contains(found->second)) return true;
        }
        at += count;
    }
    return false;
}

namespace {

// Whether a compiled program may store through its BDA table (resident read-only copies, see
// GuestBufferMemory.cpp "Resident reads"): the recompiler defines its write lookup, named
// "get_bda_write_pointer", only for a program with BDA stores, and every BDA store calls it
// (SpirvBdaLookup.cpp, SpirvBdaRead.cpp EmitBdaWrite). A module that lost its names (no
// "get_bda_pointer" or "probe_bda_pointer" either, an optimizer that inlined them) counts as
// storing, as does every program under APS5_RESIDENT_READS_ALL_WRITERS=1. A module whose names
// say it only reads is scanned as well (SpirvMayStoreThroughBda): a store through a physical
// pointer counts whatever the names (an optimizer that inlined only the write lookup). Memoized
// by variant.
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
    const bool stores = writes || !reads || SpirvMayStoreThroughBda(words);
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
        // The scan only when resident reads may use it; otherwise every BDA program counts as
        // storing (nothing reads the flag without a resident copy).
        bdaStores = bdaStores || (tables != 0 && (!Recorder::ResidentReadsConfigured() || programStoresThroughBda(*shader.program)));
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
