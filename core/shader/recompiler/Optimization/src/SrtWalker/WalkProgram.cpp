#include "Optimization/SrtWalker/WalkProgram.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace ShaderRecompiler::Detail {

namespace {

using Code = WalkOp::Code;

// A value under an evaluation context: the active mask of the enclosing ReadFirstLane (null at
// the top) and whether the clean evaluator is evaluating it (a clean slot's value and everything
// below it, which the main evaluator hands to the clean one without a mask).
struct MemoKey {
    const IrValue* value;
    const IrValue* mask;
    bool clean;
    bool operator==(const MemoKey& other) const = default;
};

struct MemoKeyHash {
    std::size_t operator()(const MemoKey& key) const {
        auto hash = reinterpret_cast<std::uintptr_t>(key.value) * 0x9e3779b97f4a7c15ull;
        hash ^= reinterpret_cast<std::uintptr_t>(key.mask) * 0xc2b2ae3d27d4eb4full;
        hash ^= key.clean ? 0x165667b19e3779f9ull : 0u;
        return static_cast<std::size_t>(hash ^ (hash >> 29u));
    }
};

class Compiler {
public:
    explicit Compiler(const IrResourcePlan& plan, WalkProgram& program) : plan(plan), program(program) {}

    // Compiles one root; the ops and memo entries a failed root added are rolled back, so the
    // program holds only ops some supported root needs.
    std::uint32_t Root(IrValue* value, const IrValue* mask, bool clean) {
        const auto ops = program.ops.size();
        const auto added = addedKeys.size();
        std::uint32_t index = WalkNoOp;
        visiting.clear();
        if (compile(value, mask, clean, index)) return index;
        program.ops.resize(ops);
        for (auto i = added; i < addedKeys.size(); ++i) memo.erase(addedKeys[i]);
        addedKeys.resize(added);
        return WalkNoOp;
    }

    std::vector<std::uint32_t> Closure(std::uint32_t root) const {
        std::vector<std::uint32_t> result;
        if (root == WalkNoOp) return result;
        std::vector<std::uint8_t> seen(program.ops.size(), 0);
        std::vector<std::uint32_t> pending {root};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (seen[index]) continue;
            seen[index] = 1;
            result.push_back(index);
            const auto& op = program.ops[index];
            const std::uint32_t operands[] = {op.a, op.b, op.c, op.d, op.e};
            for (std::uint8_t i = 0; i < op.operands; ++i) pending.push_back(operands[i]);
        }
        // Every op was emitted after its operands, so index order is dependency order.
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    std::uint32_t emit(Code code, std::uint8_t operands, std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0, std::uint32_t e = 0, std::uint64_t immediate = 0, std::uint8_t component = 0) {
        WalkOp op;
        op.code = code;
        op.operands = operands;
        op.component = component;
        op.a = a;
        op.b = b;
        op.c = c;
        op.d = d;
        op.e = e;
        op.immediate = immediate;
        program.ops.push_back(op);
        return static_cast<std::uint32_t>(program.ops.size() - 1);
    }

    static float Float32(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }
    static std::uint64_t Float32Bits(float value) { return std::bit_cast<std::uint32_t>(value); }

    // Evaluator::EvaluateWide.
    bool compile(IrValue* raw, const IrValue* mask, bool clean, std::uint32_t& out) {
        IrValue* value = raw->Resolve();
        if (value->HasImmediate()) {
            std::uint64_t immediate = 0;
            switch (value->Type()) {
                case IrType::Bool: immediate = value->ImmediateBool() ? 1u : 0u; break;
                case IrType::U8: immediate = value->ImmediateU8(); break;
                case IrType::U16: immediate = value->ImmediateU16(); break;
                case IrType::U32: immediate = value->ImmediateU32(); break;
                case IrType::U64: immediate = value->ImmediateU64(); break;
                case IrType::F32: immediate = Float32Bits(value->ImmediateF32()); break;
                default: return false;
            }
            out = emit(Code::Const, 0, 0, 0, 0, 0, 0, immediate);
            return true;
        }
        if (value->Opcode() == IrOpcode::Void) return false;
        IrValue* inst = value;
        if (mask != nullptr && IsRuntimeSelect(inst->Opcode()) && inst->ArgumentCount() == 3 && inst->Argument(0)->Resolve() == mask) return compile(inst->Argument(1), mask, clean, out);
        const MemoKey key {inst, mask, clean};
        if (const auto found = memo.find(key); found != memo.end()) {
            out = found->second;
            return true;
        }
        if (std::find(visiting.begin(), visiting.end(), inst) != visiting.end()) return false;
        visiting.push_back(inst);
        const bool compiled = compileInst(*inst, mask, clean, out);
        visiting.pop_back();
        if (!compiled) return false;
        memo.emplace(key, out);
        addedKeys.push_back(key);
        return true;
    }

    bool arg(IrValue& inst, std::size_t index, const IrValue* mask, bool clean, std::uint32_t& out) { return compile(inst.Argument(index), mask, clean, out); }

    // Evaluator::EvaluateExtract.
    bool compileExtract(IrValue& inst, const IrValue* mask, bool clean, std::uint32_t& out) {
        IrValue* index = inst.Argument(1)->Resolve();
        if (!index->HasImmediate() || index->Type() != IrType::U32) return false;
        const auto component = index->ImmediateU32();
        if (component >= 2u) return false;
        if (inst.Opcode() == IrOpcode::CompositeExtractU64) {
            std::uint32_t packed = 0;
            if (!arg(inst, 0, mask, clean, packed)) return false;
            out = emit(Code::Extract64, 1, packed, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(component));
            return true;
        }
        IrValue* source = inst.Argument(0)->Resolve();
        if (source->Opcode() == IrOpcode::Void) return false;
        if (source->Opcode() == IrOpcode::CompositeConstructU32x2) return compile(source->Argument(component), mask, clean, out);
        if (source->Opcode() == IrOpcode::IAddCarry32) {
            std::uint32_t lhs = 0, rhs = 0;
            if (!arg(*source, 0, mask, clean, lhs) || !arg(*source, 1, mask, clean, rhs)) return false;
            out = emit(Code::ExtractCarry, 2, lhs, rhs, 0, 0, 0, 0, static_cast<std::uint8_t>(component));
            return true;
        }
        return false;
    }

    // Evaluator::EvaluateRawRead (its address arithmetic runs at execution).
    bool compileRawRead(IrValue& inst, const IrValue* mask, bool clean, std::uint32_t& out) {
        const auto flags = inst.Flags<MemoryFlags>();
        if (flags.index >= plan.memoryInfo.size()) return false;
        const auto& mem = plan.memoryInfo[flags.index];
        IrValue* handle = inst.Argument(0)->Resolve();
        if (handle->Opcode() == IrOpcode::Void) return false;
        std::uint32_t low = 0, high = 0, offset = 0;
        if (!arg(*handle, 0, mask, clean, low) || !arg(*handle, 1, mask, clean, high) || !arg(inst, 1, mask, clean, offset)) return false;
        if (inst.Opcode() == IrOpcode::ReadConstBuffer) {
            std::uint32_t records = 0, word3 = 0;
            if (handle->ArgumentCount() != 4u || !arg(*handle, 2, mask, clean, records) || !arg(*handle, 3, mask, clean, word3)) return false;
            out = emit(Code::ConstBuffer, 5, low, high, offset, records, word3, mem.offset);
            return true;
        }
        out = emit(Code::Load, 3, low, high, offset, 0, 0, mem.offset);
        return true;
    }

    bool unary(IrValue& inst, const IrValue* mask, bool clean, Code code, std::uint32_t& out) {
        std::uint32_t a = 0;
        if (!arg(inst, 0, mask, clean, a)) return false;
        out = emit(code, 1, a);
        return true;
    }

    bool binary(IrValue& inst, const IrValue* mask, bool clean, Code code, std::uint32_t& out) {
        std::uint32_t a = 0, b = 0;
        if (!arg(inst, 0, mask, clean, a) || !arg(inst, 1, mask, clean, b)) return false;
        out = emit(code, 2, a, b);
        return true;
    }

    bool ternary(IrValue& inst, const IrValue* mask, bool clean, Code code, std::uint32_t& out) {
        std::uint32_t a = 0, b = 0, c = 0;
        if (!arg(inst, 0, mask, clean, a) || !arg(inst, 1, mask, clean, b) || !arg(inst, 2, mask, clean, c)) return false;
        out = emit(code, 3, a, b, c);
        return true;
    }

    // Evaluator::EvaluateInst.
    bool compileInst(IrValue& inst, const IrValue* mask, bool clean, std::uint32_t& out) {
        switch (inst.Opcode()) {
            case IrOpcode::GetUserData: {
                const auto reg = RegIndex(static_cast<ScalarReg>(inst.Argument(0)->Register().index));
                if (reg < plan.userDataBase) return false;
                out = emit(Code::UserData, 0, 0, 0, 0, 0, 0, reg - plan.userDataBase);
                return true;
            }
            case IrOpcode::GetShaderBase: out = emit(Code::ShaderBase, 0); return true;
            case IrOpcode::Phi: {
                IrValue* value = ResolveInvariantPhi(plan, &inst);
                return value != nullptr && compile(value, mask, clean, out);
            }
            case IrOpcode::ReadFirstLane: return compile(inst.Argument(0), inst.Argument(1)->Resolve(), clean, out);
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32: return arg(inst, 0, mask, clean, out);
            case IrOpcode::CompositeExtractU64:
            case IrOpcode::CompositeExtractU32x2: return compileExtract(inst, mask, clean, out);
            case IrOpcode::CompositeConstructU64: return binary(inst, mask, clean, Code::Construct64, out);
            case IrOpcode::ReadConst: {
                IrValue* slot = inst.Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return false;
                const auto index = slot->ImmediateU32();
                // The main evaluator hands a clean slot to the clean evaluator (no active mask);
                // inside the clean evaluator every slot is its own.
                if (!clean && index < plan.cleanFlatSlots.size() && plan.cleanFlatSlots[index] != 0u) return compile(plan.srtReads[index].value, nullptr, true, out);
                return compile(plan.srtReads[index].value, mask, clean, out);
            }
            case IrOpcode::LoadAddressU32:
            case IrOpcode::ReadConstBuffer: return IsRawRead(plan, inst) && compileRawRead(inst, mask, clean, out);
            case IrOpcode::IAdd32: return binary(inst, mask, clean, Code::Add32, out);
            case IrOpcode::IAdd64: return binary(inst, mask, clean, Code::Add64, out);
            case IrOpcode::ISub32: return binary(inst, mask, clean, Code::Sub32, out);
            case IrOpcode::ISub64: return binary(inst, mask, clean, Code::Sub64, out);
            case IrOpcode::IMul32: return binary(inst, mask, clean, Code::Mul32, out);
            case IrOpcode::IMul64: return binary(inst, mask, clean, Code::Mul64, out);
            case IrOpcode::UMin32: return binary(inst, mask, clean, Code::UMin32, out);
            case IrOpcode::ConvertF32U32: return unary(inst, mask, clean, Code::CvtF32U32, out);
            case IrOpcode::ConvertU32F32: return unary(inst, mask, clean, Code::CvtU32F32, out);
            case IrOpcode::FPMul32: return binary(inst, mask, clean, Code::FMul32, out);
            case IrOpcode::FPTrunc32: return unary(inst, mask, clean, Code::FTrunc32, out);
            case IrOpcode::FPIsNan32: return unary(inst, mask, clean, Code::FIsNan32, out);
            case IrOpcode::FPOrdLessThanEqual32: return binary(inst, mask, clean, Code::FLe32, out);
            case IrOpcode::FPOrdGreaterThanEqual32: return binary(inst, mask, clean, Code::FGe32, out);
            case IrOpcode::BitwiseAnd32: return binary(inst, mask, clean, Code::And32, out);
            case IrOpcode::BitwiseAnd64: return binary(inst, mask, clean, Code::And64, out);
            case IrOpcode::BitwiseOr32: return binary(inst, mask, clean, Code::Or32, out);
            case IrOpcode::BitwiseXor32: return binary(inst, mask, clean, Code::Xor32, out);
            case IrOpcode::BitwiseNot32: return unary(inst, mask, clean, Code::Not32, out);
            case IrOpcode::ShiftLeftLogical32: return binary(inst, mask, clean, Code::Shl32, out);
            case IrOpcode::ShiftLeftLogical64: return binary(inst, mask, clean, Code::Shl64, out);
            case IrOpcode::ShiftRightLogical32: return binary(inst, mask, clean, Code::Shr32, out);
            case IrOpcode::ShiftRightLogical64: return binary(inst, mask, clean, Code::Shr64, out);
            case IrOpcode::ShiftRightArithmetic32: return binary(inst, mask, clean, Code::Sar32, out);
            case IrOpcode::ShiftRightArithmetic64: return binary(inst, mask, clean, Code::Sar64, out);
            case IrOpcode::BitFieldUExtract: return ternary(inst, mask, clean, Code::BfeU, out);
            case IrOpcode::BitFieldSExtract: return ternary(inst, mask, clean, Code::BfeS, out);
            case IrOpcode::BitFieldInsert: {
                std::uint32_t a = 0, b = 0, c = 0, d = 0;
                if (!arg(inst, 0, mask, clean, a) || !arg(inst, 1, mask, clean, b) || !arg(inst, 2, mask, clean, c) || !arg(inst, 3, mask, clean, d)) return false;
                out = emit(Code::Bfi, 4, a, b, c, d);
                return true;
            }
            case IrOpcode::SelectU32:
            case IrOpcode::SelectU1:
            case IrOpcode::SelectF32: return ternary(inst, mask, clean, Code::Select, out);
            case IrOpcode::IEqual32: return binary(inst, mask, clean, Code::Eq32, out);
            case IrOpcode::INotEqual32: return binary(inst, mask, clean, Code::Ne32, out);
            case IrOpcode::ULessThan32: return binary(inst, mask, clean, Code::ULt32, out);
            case IrOpcode::UGreaterThan32: return binary(inst, mask, clean, Code::UGt32, out);
            case IrOpcode::LogicalAnd: return binary(inst, mask, clean, Code::LAnd, out);
            case IrOpcode::LogicalOr: return binary(inst, mask, clean, Code::LOr, out);
            case IrOpcode::LogicalXor: return binary(inst, mask, clean, Code::LXor, out);
            case IrOpcode::LogicalNot: return unary(inst, mask, clean, Code::LNot, out);
            default: return false;
        }
    }

    const IrResourcePlan& plan;
    WalkProgram& program;
    std::unordered_map<MemoKey, std::uint32_t, MemoKeyHash> memo;
    std::vector<MemoKey> addedKeys;
    std::vector<IrValue*> visiting;
};

WalkRoot makeRoot(Compiler& compiler, IrValue* value, const IrValue* mask, bool clean, std::uint32_t& unsupported) {
    WalkRoot root;
    root.op = compiler.Root(value, mask, clean);
    if (root.op == WalkNoOp) {
        ++unsupported;
        return root;
    }
    root.closure = compiler.Closure(root.op);
    return root;
}

enum class Step : std::uint8_t { Ok, Failed, Bailed };

struct Execution {
    const WalkProgram& program;
    const SrtRuntime& runtime;
    std::vector<std::uint64_t>& values;
    std::vector<std::uint8_t>& done;

    static float Float32(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }
    static std::uint64_t Float32Bits(float value) { return std::bit_cast<std::uint32_t>(value); }

    Step read(const WalkOp& op, std::uint32_t leafSlot, std::uint64_t& result) const {
        const auto low = values[op.a];
        const auto high = values[op.b];
        const auto offset = values[op.c];
        const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
        const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(op.immediate)));
        std::uint64_t address = 0;
        if (op.code == Code::ConstBuffer) {
            const auto records = values[op.d];
            if (immediate < 0) return Step::Failed;
            const auto byteOffset = static_cast<std::uint64_t>(immediate) + static_cast<std::uint32_t>(offset);
            const auto aligned = byteOffset & ~std::uint64_t {3};
            const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
            const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
            if (aligned > size || size - aligned < sizeof(std::uint32_t)) return Step::Failed;
            address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
        } else {
            const auto relative = (immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
            if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) return Step::Failed;
        }
        if (auto* trace = runtime.readTrace; trace != nullptr) {
            if (leafSlot != WalkNoOp) trace->leaves.emplace_back(leafSlot, address);
            else trace->otherReads.push_back(address);
        }
        std::uint32_t word = 0;
        // A pure slot's leaf the driver defers to the GPU (SrtRuntime::deferPureLeaf) is not read.
        if (leafSlot != WalkNoOp && runtime.deferPureLeaf != nullptr && runtime.deferPureLeaf(runtime.userContext, address, &word)) {
            result = word;
            return Step::Ok;
        }
        if (!runtime.expressRead(runtime.userContext, address, &word)) return Step::Bailed;
        result = word;
        return Step::Ok;
    }

    Step execute(std::uint32_t index, std::uint32_t leafSlot) const {
        const auto& op = program.ops[index];
        auto& result = values[index];
        const auto a = op.operands > 0 ? values[op.a] : 0u;
        const auto b = op.operands > 1 ? values[op.b] : 0u;
        const auto c = op.operands > 2 ? values[op.c] : 0u;
        switch (op.code) {
            case Code::Const: result = op.immediate; return Step::Ok;
            case Code::UserData:
                if (op.immediate >= runtime.userData.size()) return Step::Failed;
                result = runtime.userData[static_cast<std::size_t>(op.immediate)];
                return Step::Ok;
            case Code::ShaderBase: result = runtime.shaderBase; return Step::Ok;
            case Code::Add32: result = static_cast<std::uint32_t>(a + b); return Step::Ok;
            case Code::Add64: result = a + b; return Step::Ok;
            case Code::Sub32: result = static_cast<std::uint32_t>(a - b); return Step::Ok;
            case Code::Sub64: result = a - b; return Step::Ok;
            case Code::Mul32: result = static_cast<std::uint32_t>(a * b); return Step::Ok;
            case Code::Mul64: result = a * b; return Step::Ok;
            case Code::UMin32: result = std::min(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)); return Step::Ok;
            case Code::And32: result = static_cast<std::uint32_t>(a & b); return Step::Ok;
            case Code::And64: result = a & b; return Step::Ok;
            case Code::Or32: result = static_cast<std::uint32_t>(a | b); return Step::Ok;
            case Code::Xor32: result = static_cast<std::uint32_t>(a ^ b); return Step::Ok;
            case Code::Not32: result = ~static_cast<std::uint32_t>(a); return Step::Ok;
            case Code::Shl32: result = static_cast<std::uint32_t>(a) << (b & 31u); return Step::Ok;
            case Code::Shl64: result = a << (b & 63u); return Step::Ok;
            case Code::Shr32: result = static_cast<std::uint32_t>(a) >> (b & 31u); return Step::Ok;
            case Code::Shr64: result = a >> (b & 63u); return Step::Ok;
            case Code::Sar32: result = static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) >> (b & 31u)); return Step::Ok;
            case Code::Sar64: result = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(a) >> (b & 63u)); return Step::Ok;
            case Code::BfeU: {
                const auto offset = static_cast<std::uint32_t>(b);
                const auto width = static_cast<std::uint32_t>(c);
                if (offset > 32u || width > 32u - offset) return Step::Failed;
                const auto mask = width == 32u ? 0xffffffffu : width == 0u ? 0u : (std::uint32_t {1} << width) - 1u;
                result = width == 0u ? 0u : (static_cast<std::uint32_t>(a) >> offset) & mask;
                return Step::Ok;
            }
            case Code::BfeS: {
                const auto offset = static_cast<std::uint32_t>(b);
                const auto width = static_cast<std::uint32_t>(c);
                if (offset > 32u || width > 32u - offset) return Step::Failed;
                if (width == 0u) {
                    result = 0;
                    return Step::Ok;
                }
                const auto mask = width == 32u ? 0xffffffffu : (std::uint32_t {1} << width) - 1u;
                auto bits = (static_cast<std::uint32_t>(a) >> offset) & mask;
                if (width < 32u && (bits & (std::uint32_t {1} << (width - 1u))) != 0u) bits |= ~mask;
                result = bits;
                return Step::Ok;
            }
            case Code::Bfi: {
                const auto d = values[op.d];
                const auto offset = static_cast<std::uint32_t>(c);
                const auto width = static_cast<std::uint32_t>(d);
                if (offset > 32u || width > 32u - offset) return Step::Failed;
                if (width == 0u) {
                    result = static_cast<std::uint32_t>(a);
                    return Step::Ok;
                }
                const auto mask = width == 32u ? 0xffffffffu : ((std::uint32_t {1} << width) - 1u) << offset;
                result = (static_cast<std::uint32_t>(a) & ~mask) | ((static_cast<std::uint32_t>(b) << offset) & mask);
                return Step::Ok;
            }
            case Code::Select: result = a != 0u ? b : c; return Step::Ok;
            case Code::Eq32: result = static_cast<std::uint32_t>(a) == static_cast<std::uint32_t>(b) ? 1u : 0u; return Step::Ok;
            case Code::Ne32: result = static_cast<std::uint32_t>(a) != static_cast<std::uint32_t>(b) ? 1u : 0u; return Step::Ok;
            case Code::ULt32: result = static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b) ? 1u : 0u; return Step::Ok;
            case Code::UGt32: result = static_cast<std::uint32_t>(a) > static_cast<std::uint32_t>(b) ? 1u : 0u; return Step::Ok;
            case Code::LAnd: result = (a != 0u) && (b != 0u) ? 1u : 0u; return Step::Ok;
            case Code::LOr: result = (a != 0u) || (b != 0u) ? 1u : 0u; return Step::Ok;
            case Code::LXor: result = (a != 0u) != (b != 0u) ? 1u : 0u; return Step::Ok;
            case Code::LNot: result = a == 0u ? 1u : 0u; return Step::Ok;
            case Code::CvtF32U32: result = Float32Bits(static_cast<float>(static_cast<std::uint32_t>(a))); return Step::Ok;
            case Code::CvtU32F32: {
                const auto value = Float32(a);
                if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > 4294967295.0) return Step::Failed;
                result = static_cast<std::uint32_t>(value);
                return Step::Ok;
            }
            case Code::FMul32: result = Float32Bits(Float32(a) * Float32(b)); return Step::Ok;
            case Code::FTrunc32: result = Float32Bits(std::trunc(Float32(a))); return Step::Ok;
            case Code::FIsNan32: result = std::isnan(Float32(a)) ? 1u : 0u; return Step::Ok;
            case Code::FLe32: result = Float32(a) <= Float32(b) ? 1u : 0u; return Step::Ok;
            case Code::FGe32: result = Float32(a) >= Float32(b) ? 1u : 0u; return Step::Ok;
            case Code::Construct64: result = static_cast<std::uint32_t>(a) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)) << 32u); return Step::Ok;
            case Code::Extract64: result = static_cast<std::uint32_t>(a >> (op.component * 32u)); return Step::Ok;
            case Code::ExtractCarry: {
                const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) + static_cast<std::uint32_t>(b);
                result = op.component == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
                return Step::Ok;
            }
            case Code::Load:
            case Code::ConstBuffer: return read(op, leafSlot, result);
        }
        return Step::Failed;
    }

    // Runs the ops a root needs that no earlier root ran; `leafSlot` names the root as a pure flat
    // slot whose own read is traced as its leaf (the interpreter traces the leaf only while that
    // slot's evaluation dereferences it: an earlier evaluation of the same read traced it as an
    // other read, and the memo serves the slot).
    Step run(const WalkRoot& root, std::uint32_t leafSlot) const {
        for (const auto index : root.closure) {
            if (done[index]) continue;
            const auto step = execute(index, index == root.op ? leafSlot : WalkNoOp);
            if (step != Step::Ok) return step;
            done[index] = 1;
        }
        return Step::Ok;
    }
};

WalkOutcome& lastOutcome() {
    struct LastWalkOutcomeStorage {};
    return HostThreadLocal<WalkOutcome, LastWalkOutcomeStorage>();
}

WalkOutcome finish(WalkOutcome outcome) {
    lastOutcome() = outcome;
    return outcome;
}

std::atomic<std::uint64_t> expressNanoseconds{0};

bool ExpressProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct ExpressTimer {
    const bool profile = ExpressProfiled();
    const std::chrono::steady_clock::time_point started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~ExpressTimer() {
        if (profile) expressNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    }
};

}

std::uint64_t ExpressWalkNanoseconds() {
    return expressNanoseconds.load(std::memory_order_relaxed);
}

std::shared_ptr<const WalkProgram> CompileWalkProgram(const IrResourcePlan& plan) {
    if (!plan.srtPlanComplete) return nullptr;
    auto program = std::make_shared<WalkProgram>();
    Compiler compiler(plan, *program);
    program->sources.reserve(plan.materializationSources.size());
    for (const auto sourceIndex : plan.materializationSources) {
        WalkSource source;
        if (sourceIndex < plan.descriptorSources.size()) {
            const auto& descriptor = plan.descriptorSources[sourceIndex];
            source.dwordCount = descriptor.dwordCount;
            source.supported = descriptor.dwordCount <= source.dwords.size();
            for (std::uint32_t i = 0; i < descriptor.dwordCount && source.supported; ++i) {
                source.dwords[i] = makeRoot(compiler, descriptor.dwords[i], nullptr, false, program->unsupportedRoots);
                if (source.dwords[i].op == WalkNoOp) source.supported = false;
            }
        }
        program->sources.push_back(std::move(source));
    }
    program->flat.reserve(plan.srtReads.size());
    for (const auto& read : plan.srtReads) {
        const bool clean = read.flatOffset < plan.cleanFlatSlots.size() && plan.cleanFlatSlots[read.flatOffset] != 0u;
        program->flat.push_back(makeRoot(compiler, read.value, nullptr, clean, program->unsupportedRoots));
    }
    program->conditions.reserve(plan.controlFlow.size());
    for (const auto& block : plan.controlFlow) {
        WalkRoot root;
        // The clean evaluator decides a block; an unresolvable condition follows both successors.
        if (block.condition != nullptr) root = makeRoot(compiler, block.condition, nullptr, true, program->unsupportedRoots);
        program->conditions.push_back(std::move(root));
    }
    return program;
}

const char* WalkOutcomeName(WalkOutcome outcome) {
    switch (outcome) {
        case WalkOutcome::Ran: return "ran";
        case WalkOutcome::NoProgram: return "no program";
        case WalkOutcome::UnsupportedRoot: return "unsupported root";
        case WalkOutcome::OpFailed: return "op failed";
        case WalkOutcome::ReadBailed: return "read declined";
        default: return "?";
    }
}

WalkOutcome LastWalkOutcome() {
    return lastOutcome();
}

void NoteWalkOutcome(WalkOutcome outcome) {
    lastOutcome() = outcome;
}

WalkOutcome ExecuteWalkProgram(const WalkProgram& program, const IrResourcePlan& plan, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources) {
    if (runtime.expressRead == nullptr) return finish(WalkOutcome::NoProgram);
    const ExpressTimer timer;
    struct ValuesStorage {};
    struct DoneStorage {};
    auto& values = HostThreadLocal<std::vector<std::uint64_t>, ValuesStorage>();
    auto& done = HostThreadLocal<std::vector<std::uint8_t>, DoneStorage>();
    values.resize(program.ops.size());
    done.assign(program.ops.size(), 0);
    const Execution execution {program, runtime, values, done};

    // EvaluateRuntimeSourcesImpl's control-flow activity. The outputs are written in place: the
    // fast walk's (expressOnly) are per-thread vectors that keep their capacity.
    auto& active = activeSources;
    active.assign(plan.descriptorSources.size(), 1u);
    if (!plan.controlFlow.empty()) {
        for (const auto& block : plan.controlFlow) {
            for (const auto source : block.sources) {
                if (source >= active.size()) return finish(WalkOutcome::OpFailed);
                active[source] = 0u;
            }
        }
        std::vector<std::uint8_t> visited(plan.controlFlow.size(), 0u);
        std::vector<std::uint32_t> pending {0};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (index >= plan.controlFlow.size()) return finish(WalkOutcome::OpFailed);
            if (visited[index]) continue;
            visited[index] = 1u;
            const auto& block = plan.controlFlow[index];
            for (const auto source : block.sources) active[source] = 1u;
            const auto& condition = program.conditions[index];
            bool evaluable = block.condition != nullptr && runtime.readSpecializationMemory != nullptr && condition.op != WalkNoOp;
            if (evaluable) {
                const auto step = execution.run(condition, WalkNoOp);
                if (step == Step::Bailed) return finish(WalkOutcome::ReadBailed);
                evaluable = step == Step::Ok;
            }
            if (evaluable) {
                const auto successor = static_cast<std::uint32_t>(values[condition.op]) != 0u ? 0u : 1u;
                if (successor >= block.successors.size()) return finish(WalkOutcome::OpFailed);
                pending.push_back(block.successors[successor]);
            } else {
                pending.insert(pending.end(), block.successors.begin(), block.successors.end());
            }
        }
    }

    auto& evaluated = results;
    evaluated.clear();
    evaluated.reserve(program.sources.size());
    for (std::size_t i = 0; i < program.sources.size(); ++i) {
        const auto sourceIndex = plan.materializationSources[i];
        if (sourceIndex >= plan.descriptorSources.size()) return finish(WalkOutcome::OpFailed);
        const auto& source = program.sources[i];
        DescriptorValue value;
        value.dwordCount = source.dwordCount;
        if (active[sourceIndex]) {
            if (!source.supported) return finish(WalkOutcome::UnsupportedRoot);
            for (std::uint32_t dword = 0; dword < source.dwordCount; ++dword) {
                const auto step = execution.run(source.dwords[dword], WalkNoOp);
                if (step == Step::Bailed) return finish(WalkOutcome::ReadBailed);
                if (step != Step::Ok) return finish(WalkOutcome::OpFailed);
                value.dwords[dword] = static_cast<std::uint32_t>(values[source.dwords[dword].op]);
            }
        }
        evaluated.push_back(value);
    }

    auto& flattened = flat;
    flattened.assign(plan.srtReads.size(), 0u);
    for (std::size_t i = 0; i < plan.srtReads.size(); ++i) {
        const auto& read = plan.srtReads[i];
        const auto& root = program.flat[i];
        if (root.op == WalkNoOp) return finish(WalkOutcome::UnsupportedRoot);
        if (read.flatOffset >= flattened.size()) return finish(WalkOutcome::OpFailed);
        const bool pure = runtime.readTrace != nullptr && read.flatOffset < plan.pureFlatSlots.size() && plan.pureFlatSlots[read.flatOffset] != 0u;
        const auto step = execution.run(root, pure ? read.flatOffset : WalkNoOp);
        if (step == Step::Bailed) return finish(WalkOutcome::ReadBailed);
        if (step != Step::Ok) return finish(WalkOutcome::OpFailed);
        flattened[read.flatOffset] = static_cast<std::uint32_t>(values[root.op]);
    }

    return finish(WalkOutcome::Ran);
}

}
