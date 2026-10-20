#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvMemory/SpirvTypes.hpp"
#include "SpirvBackend/SpirvMemory/SpirvConstants.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ShaderRecompiler {

namespace {

int uniformStageForTests = -1;

template<typename TFunction>
void ReturnBdaValueIf(SpirvEmitterState& state, std::uint32_t condition, std::uint32_t value, TFunction&& before) {
    const auto taken = state.module.AllocateId();
    const auto next = state.module.AllocateId();
    state.module.AddFunction(spv::OpSelectionMerge, next, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, condition, taken, next);
    EmitLabel(state, taken);
    before();
    state.module.AddFunction(spv::OpReturnValue, value);
    EmitLabel(state, next);
}

void ReturnBdaZeroIf(SpirvEmitterState& state, std::uint32_t condition) {
    ReturnBdaValueIf(state, condition, BdaConstant(state, 0u), [] {});
}

std::uint32_t LoadPrivate(SpirvEmitterState& state, std::uint32_t type, std::uint32_t variable) {
    const auto value = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, type, value, variable);
    return value;
}

struct BdaRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t base = 0;
    std::uint32_t permissions = 0;
};

// (u64 address, u32 bytes, u32 instruction) -> u64 device address of the range holding all the bytes,
// or 0 after a fault (recorded when recordFaults). With APS5_SPIRV_BDA_UNIFORM (state.bdaUniformStage, see BdaUniformStage) the header is validated once per
// invocation (1), a range the full lookup accepted answers the next accesses inside it (2), and a uniform
// lookup first searches for the subgroup's first active address (3). The table is sorted and disjoint
// (GuestBufferMemory::AddressRanges), so a range holding [address, address + bytes) is the one the search
// finds, and a shortcut returns only what the full lookup would return without a fault.
std::uint32_t DefineBdaLookup(SpirvEmitterState& state, const char* name, bool recordFaults, std::uint32_t permission = BdaAbi::Read, bool uniform = false) {
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return ConstantU32(state, value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return Binary(state, op, type, left, right); };
    const auto both = [&](std::uint32_t left, std::uint32_t right) { return binary(spv::OpLogicalAnd, boolean, left, right); };
    const auto load = [&](std::uint32_t pointer) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, u32, value, pointer);
        return value;
    };
    const auto stage = state.bdaUniformStage;
    const auto function = state.module.AllocateId();
    state.module.AddName(function, name);
    state.module.AddFunction(spv::OpFunction, u64, function, spv::FunctionControlDontInlineMask, state.module.Type(spv::OpTypeFunction, u64, u64, u32, u32));
    const auto address = state.module.AllocateId();
    const auto bytes = state.module.AllocateId();
    const auto instruction = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, u64, address);
    state.module.AddFunction(spv::OpFunctionParameter, u32, bytes);
    state.module.AddFunction(spv::OpFunctionParameter, u32, instruction);
    EmitLabel(state, state.module.AllocateId());
    const auto low = state.module.AllocateId();
    const auto high = state.module.AllocateId();
    const auto pointer = TypePointer(state, spv::StorageClassFunction, u32);
    state.module.AddFunction(spv::OpVariable, pointer, low, spv::StorageClassFunction);
    state.module.AddFunction(spv::OpVariable, pointer, high, spv::StorageClassFunction);
    const auto fail = [&](std::uint32_t condition, BdaAbi::FaultReason reason) {
        if (recordFaults) {
            ReturnBdaFailureIf(state, condition, address, bytes, instruction, reason);
        } else {
            ReturnBdaZeroIf(state, condition);
        }
    };
    const auto end = binary(spv::OpIAdd, u64, address, Unary(state, spv::OpUConvert, u64, bytes));
    // Whether [address, end) lies in the range with the permission and maps without wrapping (every check
    // after the search passes), and the device address.
    const auto translate = [&](const BdaRange& range) {
        const auto result = binary(spv::OpIAdd, u64, range.base, binary(spv::OpISub, u64, address, range.begin));
        const auto deviceEnd = binary(spv::OpIAdd, u64, result, Unary(state, spv::OpUConvert, u64, bytes));
        auto hit = both(binary(spv::OpUGreaterThan, boolean, end, address), binary(spv::OpUGreaterThanEqual, boolean, address, range.begin));
        hit = both(hit, binary(spv::OpULessThanEqual, boolean, end, range.end));
        hit = both(hit, binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, range.permissions, constant(permission)), constant(0)));
        hit = both(hit, binary(spv::OpUGreaterThanEqual, boolean, result, range.base));
        hit = both(hit, binary(spv::OpUGreaterThan, boolean, deviceEnd, result));
        return std::pair{hit, result};
    };
    const auto remember = [&](const BdaRange& range) {
        if (stage < 2) return;
        state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[0], range.begin);
        state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[1], range.end);
        state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[2], range.base);
        state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[3], range.permissions);
    };
    if (stage >= 2) {
        const BdaRange last{LoadPrivate(state, u64, state.bdaLastRangeVariables[0]), LoadPrivate(state, u64, state.bdaLastRangeVariables[1]), LoadPrivate(state, u64, state.bdaLastRangeVariables[2]), LoadPrivate(state, u32, state.bdaLastRangeVariables[3])};
        const auto cached = translate(last);
        ReturnBdaValueIf(state, cached.first, cached.second, [] {});
    }
    std::uint32_t count = 0;
    if (stage >= 1) {
        // Every header check faults as InvalidTable, so one combined check per invocation records the same fault.
        EmitIfCondition(state, binary(spv::OpIEqual, boolean, LoadPrivate(state, u32, state.bdaTableStateVariable), constant(0)), [&] {
            const auto length = state.module.AllocateId();
            state.module.AddFunction(spv::OpArrayLength, u32, length, state.bdaPagetableVariable, 0u);
            state.module.AddFunction(spv::OpStore, state.bdaTableStateVariable, constant(2));
            EmitIfCondition(state, binary(spv::OpUGreaterThanEqual, boolean, length, constant(4)), [&] {
                const auto entries = BdaLoadWord(state, constant(1));
                const auto available = binary(spv::OpISub, u32, length, constant(4));
                auto valid = binary(spv::OpIEqual, boolean, BdaLoadWord(state, constant(0)), constant(BdaAbi::Version));
                valid = both(valid, binary(spv::OpIEqual, boolean, BdaLoadWord(state, constant(2)), constant(sizeof(BdaAbi::Range))));
                valid = both(valid, binary(spv::OpIEqual, boolean, BdaLoadWord(state, constant(3)), constant(0)));
                valid = both(valid, binary(spv::OpIEqual, boolean, binary(spv::OpBitwiseAnd, u32, available, constant(7)), constant(0)));
                valid = both(valid, binary(spv::OpIEqual, boolean, entries, binary(spv::OpShiftRightLogical, u32, available, constant(3))));
                state.module.AddFunction(spv::OpStore, state.bdaTableCountVariable, entries);
                state.module.AddFunction(spv::OpStore, state.bdaTableStateVariable, Select(state, u32, valid, constant(1), constant(2)));
            });
        });
        fail(binary(spv::OpINotEqual, boolean, LoadPrivate(state, u32, state.bdaTableStateVariable), constant(1)), BdaAbi::FaultReason::InvalidTable);
        count = LoadPrivate(state, u32, state.bdaTableCountVariable);
    } else {
        const auto length = state.module.AllocateId();
        state.module.AddFunction(spv::OpArrayLength, u32, length, state.bdaPagetableVariable, 0u);
        fail(binary(spv::OpULessThan, boolean, length, constant(4)), BdaAbi::FaultReason::InvalidTable);
        fail(binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(0)), constant(BdaAbi::Version)), BdaAbi::FaultReason::InvalidTable);
        fail(binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(2)), constant(sizeof(BdaAbi::Range))), BdaAbi::FaultReason::InvalidTable);
        fail(binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(3)), constant(0)), BdaAbi::FaultReason::InvalidTable);
        count = BdaLoadWord(state, constant(1));
        const auto available = binary(spv::OpISub, u32, length, constant(4));
        fail(binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, available, constant(7)), constant(0)), BdaAbi::FaultReason::InvalidTable);
        fail(binary(spv::OpINotEqual, boolean, count, binary(spv::OpShiftRightLogical, u32, available, constant(3))), BdaAbi::FaultReason::InvalidTable);
    }
    fail(binary(spv::OpIEqual, boolean, bytes, constant(0)), BdaAbi::FaultReason::Overflow);
    fail(binary(spv::OpULessThanEqual, boolean, end, address), BdaAbi::FaultReason::Overflow);
    // The one-based index of the last range beginning at or below key (0: none).
    const auto search = [&](std::uint32_t key) {
        state.module.AddFunction(spv::OpStore, low, constant(0));
        state.module.AddFunction(spv::OpStore, high, count);
        const auto header = state.module.AllocateId();
        const auto body = state.module.AllocateId();
        const auto continuation = state.module.AllocateId();
        const auto merge = state.module.AllocateId();
        state.module.AddFunction(spv::OpBranch, header);
        EmitLabel(state, header);
        const auto lower = load(low);
        const auto upper = load(high);
        const auto searching = binary(spv::OpULessThan, boolean, lower, upper);
        state.module.AddFunction(spv::OpLoopMerge, merge, continuation, spv::LoopControlMaskNone);
        state.module.AddFunction(spv::OpBranchConditional, searching, body, merge);
        EmitLabel(state, body);
        const auto midpoint = binary(spv::OpIAdd, u32, lower, binary(spv::OpShiftRightLogical, u32, binary(spv::OpISub, u32, upper, lower), constant(1)));
        const auto entry = binary(spv::OpIAdd, u32, constant(4), binary(spv::OpIMul, u32, midpoint, constant(8)));
        const auto before = binary(spv::OpULessThan, boolean, key, BdaLoadAddress(state, entry));
        const auto newLow = state.module.AllocateId();
        const auto newHigh = state.module.AllocateId();
        state.module.AddFunction(spv::OpSelect, u32, newLow, before, lower, binary(spv::OpIAdd, u32, midpoint, constant(1)));
        state.module.AddFunction(spv::OpSelect, u32, newHigh, before, midpoint, upper);
        state.module.AddFunction(spv::OpStore, low, newLow);
        state.module.AddFunction(spv::OpStore, high, newHigh);
        state.module.AddFunction(spv::OpBranch, continuation);
        EmitLabel(state, continuation);
        state.module.AddFunction(spv::OpBranch, header);
        EmitLabel(state, merge);
        return load(low);
    };
    const auto entryWord = [&](std::uint32_t index) { return binary(spv::OpIAdd, u32, constant(4), binary(spv::OpIMul, u32, binary(spv::OpISub, u32, index, constant(1)), constant(8))); };
    if (uniform) {
        // One search for the subgroup's first active address: lanes inside that range skip their own.
        const auto broadcast = [&](std::uint32_t value) {
            const auto result = state.module.AllocateId();
            state.module.AddFunction(spv::OpGroupNonUniformBroadcastFirst, u32, result, constant(spv::ScopeSubgroup), value);
            return Unary(state, spv::OpUConvert, u64, result);
        };
        const auto leaderLow = broadcast(Unary(state, spv::OpUConvert, u32, address));
        const auto leaderHigh = broadcast(Unary(state, spv::OpUConvert, u32, binary(spv::OpShiftRightLogical, u64, address, BdaConstant(state, 32u))));
        const auto leader = binary(spv::OpBitwiseOr, u64, leaderLow, binary(spv::OpShiftLeftLogical, u64, leaderHigh, BdaConstant(state, 32u)));
        const auto candidate = search(leader);
        EmitIfCondition(state, binary(spv::OpINotEqual, boolean, candidate, constant(0)), [&] {
            const auto selected = entryWord(candidate);
            const auto at = [&](std::uint32_t offset) { return binary(spv::OpIAdd, u32, selected, constant(offset)); };
            const BdaRange range{BdaLoadAddress(state, selected), BdaLoadAddress(state, at(2)), BdaLoadAddress(state, at(4)), BdaLoadWord(state, at(6))};
            auto valid = binary(spv::OpIEqual, boolean, BdaLoadWord(state, at(7)), constant(0));
            valid = both(valid, binary(spv::OpULessThan, boolean, range.begin, range.end));
            valid = both(valid, binary(spv::OpINotEqual, boolean, range.base, BdaConstant(state, 0)));
            const auto shared = translate(range);
            ReturnBdaValueIf(state, both(valid, shared.first), shared.second, [&] { remember(range); });
        });
    }
    const auto index = search(address);
    fail(binary(spv::OpIEqual, boolean, index, constant(0)), BdaAbi::FaultReason::Unmapped);
    const auto selected = entryWord(index);
    const auto at = [&](std::uint32_t offset) { return binary(spv::OpIAdd, u32, selected, constant(offset)); };
    const auto begin = BdaLoadAddress(state, selected);
    const auto finish = BdaLoadAddress(state, at(2));
    const auto base = BdaLoadAddress(state, at(4));
    const auto permissions = BdaLoadWord(state, at(6));
    fail(binary(spv::OpINotEqual, boolean, BdaLoadWord(state, at(7)), constant(0)), BdaAbi::FaultReason::InvalidTable);
    fail(binary(spv::OpUGreaterThanEqual, boolean, begin, finish), BdaAbi::FaultReason::InvalidTable);
    fail(binary(spv::OpUGreaterThan, boolean, end, finish), BdaAbi::FaultReason::Unmapped);
    fail(binary(spv::OpIEqual, boolean, binary(spv::OpBitwiseAnd, u32, permissions, constant(permission)), constant(0)), BdaAbi::FaultReason::Permission);
    fail(binary(spv::OpIEqual, boolean, base, BdaConstant(state, 0)), BdaAbi::FaultReason::InvalidTable);
    const auto offset = binary(spv::OpISub, u64, address, begin);
    const auto result = binary(spv::OpIAdd, u64, base, offset);
    fail(binary(spv::OpULessThan, boolean, result, base), BdaAbi::FaultReason::Overflow);
    const auto deviceEnd = binary(spv::OpIAdd, u64, result, Unary(state, spv::OpUConvert, u64, bytes));
    fail(binary(spv::OpULessThanEqual, boolean, deviceEnd, result), BdaAbi::FaultReason::Overflow);
    remember({begin, finish, base, permissions});
    state.module.AddFunction(spv::OpReturnValue, result);
    state.module.AddFunction(spv::OpFunctionEnd);
    return function;
}

// The per-invocation lookup state of APS5_SPIRV_BDA_UNIFORM: the header check (0 not yet, 1 valid, 2
// invalid), the validated range count and the last range found. Stage 3 needs subgroup ballot support.
void DefineBdaLookupState(SpirvEmitterState& state) {
    auto stage = BdaUniformStage();
    const auto& capabilities = state.supportedCapabilities;
    if (stage >= 3u && std::find(capabilities.begin(), capabilities.end(), static_cast<std::uint32_t>(spv::CapabilityGroupNonUniformBallot)) == capabilities.end()) stage = 2u;
    state.bdaUniformStage = stage;
    if (stage == 0u) return;
    const auto define = [&](std::uint32_t type, const char* name) {
        const auto variable = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassPrivate, type), spv::StorageClassPrivate);
        state.module.AddName(variable, name);
        return variable;
    };
    state.bdaTableStateVariable = define(TypeU32(state), "bda_table_state");
    state.bdaTableCountVariable = define(TypeU32(state), "bda_table_count");
    if (stage >= 2u) {
        state.bdaLastRangeVariables = {define(TypeScalarU64(state), "bda_last_begin"), define(TypeScalarU64(state), "bda_last_end"), define(TypeScalarU64(state), "bda_last_base"), define(TypeU32(state), "bda_last_permissions")};
    }
    if (stage >= 3u) {
        state.module.RequireVersion(0x00010300u);
        state.module.EmitCapability(spv::CapabilityGroupNonUniform);
        state.module.EmitCapability(spv::CapabilityGroupNonUniformBallot);
    }
}

}

// APS5_SPIRV_BDA_UNIFORM=<1-3>, default off (see SpirvBda.hpp). The value enters the shader disk cache key
// with the other recompiler switches.
std::uint32_t BdaUniformStage() {
    if (uniformStageForTests >= 0) return static_cast<std::uint32_t>(uniformStageForTests);
    static const std::uint32_t stage = [] {
        const char* value = std::getenv("APS5_SPIRV_BDA_UNIFORM");
        if (value == nullptr) return 0u;
        const auto parsed = static_cast<std::uint32_t>(std::min<unsigned long>(std::strtoul(value, nullptr, 10), 3ul));
        std::fprintf(stderr, "[spirv] APS5_SPIRV_BDA_UNIFORM=%u: BDA lookups %s\n", parsed, parsed == 0u ? "unchanged" : parsed == 1u ? "validate the table once" : parsed == 2u ? "validate once, keep the last range" : "validate once, keep the last range, search once per subgroup for uniform bases");
        return parsed;
    }();
    return stage;
}

void SetBdaUniformStageForTests(int stage) {
    uniformStageForTests = stage;
}

void EmitBdaLookupInit(SpirvEmitterState& state) {
    if (state.bdaTableStateVariable == 0) return;
    // Stored in the entry function, so the variables stay Private: a pass moving a Private variable that only
    // one function uses into that function would reset the state on every call.
    state.module.AddFunction(spv::OpStore, state.bdaTableStateVariable, ConstantU32(state, 0u));
    state.module.AddFunction(spv::OpStore, state.bdaTableCountVariable, ConstantU32(state, 0u));
    if (state.bdaLastRangeVariables[0] == 0) return;
    for (std::uint32_t word = 0; word < 3u; ++word) state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[word], BdaConstant(state, 0u));
    state.module.AddFunction(spv::OpStore, state.bdaLastRangeVariables[3], ConstantU32(state, 0u));
}

static std::uint32_t DefineBdaNoteWrite(SpirvEmitterState& state) {
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return ConstantU32(state, value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return Binary(state, op, type, left, right); };
    const auto voidType = state.module.Type(spv::OpTypeVoid);
    const auto function = state.module.AllocateId();
    state.module.AddName(function, "note_bda_write");
    state.module.AddFunction(spv::OpFunction, voidType, function, spv::FunctionControlMaskNone, state.module.Type(spv::OpTypeFunction, voidType, TypeScalarU64(state)));
    const auto address = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, TypeScalarU64(state), address);
    EmitLabel(state, state.module.AllocateId());
    const auto done = state.module.AllocateId();
    state.module.AddFunction(spv::OpVariable, TypePointer(state, spv::StorageClassFunction, boolean), done, spv::StorageClassFunction);
    state.module.AddFunction(spv::OpStore, done, ConstantBool(state, false));
    const auto page = binary(spv::OpIAdd, u32, Unary(state, spv::OpUConvert, u32, binary(spv::OpShiftRightLogical, TypeScalarU64(state), address, BdaConstant(state, BdaAbi::WrittenPageShift))), constant(1));
    const auto hash = binary(spv::OpShiftRightLogical, u32, binary(spv::OpIMul, u32, page, constant(0x9e3779b1u)), constant(32u - static_cast<std::uint32_t>(std::countr_zero(BdaAbi::WrittenPageSlots))));
    const auto scope = constant(spv::ScopeDevice);
    const auto relaxed = constant(spv::MemorySemanticsMaskNone);
    for (std::uint32_t probe = 0; probe < BdaAbi::WrittenPageProbes; probe++) {
        const auto pending = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, boolean, pending, done);
        EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, pending), [&] {
            const auto slot = binary(spv::OpIAdd, u32, constant(BdaAbi::WrittenSlotsWord), binary(spv::OpBitwiseAnd, u32, binary(spv::OpIAdd, u32, hash, constant(probe)), constant(BdaAbi::WrittenPageSlots - 1u)));
            const auto previous = state.module.AllocateId();
            state.module.AddFunction(spv::OpAtomicCompareExchange, u32, previous, BdaWord(state, state.faultBufferVariable, slot), scope, relaxed, relaxed, page, constant(0u));
            const auto taken = binary(spv::OpLogicalOr, boolean, binary(spv::OpIEqual, boolean, previous, constant(0u)), binary(spv::OpIEqual, boolean, previous, page));
            state.module.AddFunction(spv::OpStore, done, taken);
        });
    }
    const auto noted = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, boolean, noted, done);
    EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, noted), [&] {
        state.module.AddFunction(spv::OpAtomicStore, BdaWord(state, state.faultBufferVariable, constant(BdaAbi::WrittenOverflowWord)), scope, relaxed, constant(1u));
    });
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    return function;
}

void DefineGetBdaPointer(SpirvEmitterState& state) {
    if (!state.program.Info().usesDma) return;
    DefineBdaLookupState(state);
    state.bdaPointerFunction = DefineBdaLookup(state, "get_bda_pointer", true);
    if (!BdaByteReadsForced()) state.bdaProbeFunction = DefineBdaLookup(state, "probe_bda_pointer", false);
    if (state.program.Info().bdaWrites) {
        state.bdaWritePointerFunction = DefineBdaLookup(state, "get_bda_write_pointer", true, BdaAbi::Write);
        state.bdaNoteWriteFunction = DefineBdaNoteWrite(state);
    }
    if (state.bdaUniformStage < 3u) return;
    state.bdaUniformPointerFunction = DefineBdaLookup(state, "get_bda_pointer_uniform", true, BdaAbi::Read, true);
    if (state.bdaProbeFunction != 0) state.bdaUniformProbeFunction = DefineBdaLookup(state, "probe_bda_pointer_uniform", false, BdaAbi::Read, true);
    if (state.bdaWritePointerFunction != 0) state.bdaUniformWritePointerFunction = DefineBdaLookup(state, "get_bda_write_pointer_uniform", true, BdaAbi::Write, true);
    state.bdaUniformValues = state.laneCount == 2u ? state.sharedLaneValues : WaveUniformValues(state.program);
}

}
