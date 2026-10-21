#include "BdaShader.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvMemory/SpirvTypes.hpp"
#include "SpirvBackend/SpirvMemory/SpirvConstants.hpp"
#include <array>

namespace {

constexpr std::array<std::uint32_t, 2> subgroupBallot{spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot};

}

std::vector<std::uint32_t> MakeBdaTestShader(std::uint64_t address, std::uint32_t bits, std::int64_t offset, bool uniform) {
    using namespace ShaderRecompiler;
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    if (uniform) {
        MemoryInfo memory{};
        memory.kind = ResourceKind::ScalarAddress;
        memory.addressIsFull = true;
        program.Resources().memoryInfo.push_back(memory);
    }
    SpirvEmitterState state(program, {});
    state.supportedCapabilities = subgroupBallot;
    EmitBaseHeader(state.module, program);
    const auto define = [&](std::uint32_t binding) {
        const auto variable = state.module.DefineGlobalVariable(TypeStorageBufferPointer(state), spv::StorageClassStorageBuffer);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0u);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
        return variable;
    };
    state.bdaPagetableVariable = define(0);
    state.faultBufferVariable = define(1);
    const auto output = define(2);
    DefineGetBdaPointer(state);
    const auto main = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, TypeVoid(state), main, spv::FunctionControlMaskNone, TypeFunction(state));
    EmitLabel(state, state.module.AllocateId());
    EmitBdaLookupInit(state);
    SpirvValueEmitContext ctx(state);
    auto& instruction = program.CreateValue(IrOpcode::LoadAddressU32, IrType::U32);
    MemoryFlags flags{};
    flags.pc = 0x1234;
    instruction.SetFlags(flags);
    auto base = BdaConstant(state, address);
    if (offset != 0) base = AddBdaAddress(ctx, instruction, base, BdaConstant(state, offset < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(offset) : static_cast<std::uint64_t>(offset)), offset < 0);
    const auto value = EmitBdaRead(ctx, instruction, base, bits);
    state.module.AddFunction(spv::OpStore, BdaWord(state, output, ConstantU32(state, 0u)), value);
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.AddExecutionMode(main, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
    state.module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {});
    return state.module.Finalize();
}

std::vector<std::uint32_t> MakeBdaLookupTestShader(std::span<const BdaTestAccess> accesses, std::uint32_t lanes, bool uniform, bool ballot) {
    using namespace ShaderRecompiler;
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    MemoryInfo memory{};
    memory.kind = uniform ? ResourceKind::ScalarAddress : ResourceKind::Global;
    memory.addressIsFull = true;
    program.Resources().memoryInfo.push_back(memory);
    SpirvEmitterState state(program, {});
    if (ballot) state.supportedCapabilities = subgroupBallot;
    EmitBaseHeader(state.module, program);
    const auto define = [&](std::uint32_t binding) {
        const auto variable = state.module.DefineGlobalVariable(TypeStorageBufferPointer(state), spv::StorageClassStorageBuffer);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0u);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
        return variable;
    };
    state.bdaPagetableVariable = define(0);
    state.faultBufferVariable = define(1);
    const auto output = define(2);
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto laneIndex = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, u32), spv::StorageClassInput);
    state.module.AddAnnotation(spv::OpDecorate, laneIndex, spv::DecorationBuiltIn, spv::BuiltInLocalInvocationIndex);
    DefineGetBdaPointer(state);
    const auto main = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, TypeVoid(state), main, spv::FunctionControlMaskNone, TypeFunction(state));
    EmitLabel(state, state.module.AllocateId());
    EmitBdaLookupInit(state);
    SpirvValueEmitContext ctx(state);
    const auto lane = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, u32, lane, laneIndex);
    const auto lane64 = Unary(state, spv::OpUConvert, u64, lane);
    const auto count = static_cast<std::uint32_t>(accesses.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& instruction = program.CreateValue(IrOpcode::LoadAddressU32, IrType::U32);
        MemoryFlags flags{};
        flags.pc = 0x1234 + index;
        instruction.SetFlags(flags);
        const auto address = Binary(state, spv::OpIAdd, u64, BdaConstant(state, accesses[index].address), Binary(state, spv::OpIMul, u64, lane64, BdaConstant(state, accesses[index].laneStride)));
        const auto value = EmitBdaRead(ctx, instruction, address, 32u);
        const auto word = Binary(state, spv::OpIAdd, u32, Binary(state, spv::OpIMul, u32, lane, ConstantU32(state, count)), ConstantU32(state, index));
        state.module.AddFunction(spv::OpStore, BdaWord(state, output, word), value);
    }
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.AddExecutionMode(main, spv::ExecutionModeLocalSize, lanes, 1u, 1u);
    state.module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {laneIndex});
    return state.module.Finalize();
}
