#include "Translation/TranslationContext.hpp"
#include "RdnaDecoder/RdnaVectorOpDecoder.hpp"
#include "Recompiler.hpp"
#include <array>
#include <stdexcept>

using namespace ShaderRecompiler;
static void Require(bool value) { if (!value) throw std::runtime_error("vector legacy mad regression"); }
// Translates one VOP3 instruction (word0 above the vdst byte, sources v1, v2, v3) and returns the
// first instruction of `expected`, or nullptr.
static IrValue* Translate(IrProgram& program, std::uint32_t word0, RdnaOpcode opcode, IrOpcode expected) {
    const std::array<std::uint32_t, 2> code{word0 | 1u, 1u | (2u << 9u) | (3u << 18u)};
    const RdnaInstruction instruction = DecodeRdnaVop3(0u, code, 0u);
    Require(instruction.op == opcode);
    Require(instruction.family == RdnaInstructionFamily::VOP3);
    Require(IsVectorAluOpcode(instruction.op));
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256);
    context.TranslateInstruction(instruction);
    for (auto* value : block.Instructions()) {
        if (value->Opcode() == expected) return value;
    }
    return nullptr;
}
static void Check(std::uint32_t encoding, RdnaOpcode opcode, IrOpcode expected) {
    IrProgram program;
    Require(Translate(program, encoding << 16u, opcode, expected) != nullptr);
}
// The DX9 rule (LegacyMadRule, default on): both factors of the multiply-add are selected to +0
// when either multiplicand is +-0, so 0 * Inf + c and 0 * NaN + c give c.
static void CheckLegacyMad() {
    IrProgram program;
    IrValue* fma = Translate(program, 0x140u << 16u, RdnaOpcode::VMadLegacyF32, IrOpcode::FPFma32);
    Require(fma != nullptr && fma->ArgumentCount() == 3u);
    for (std::size_t index = 0; index < 2u; ++index) {
        IrValue* factor = fma->Argument(index);
        Require(factor->Opcode() == IrOpcode::SelectF32);
        Require(factor->Argument(0)->Opcode() == IrOpcode::LogicalOr);
    }
    Require(fma->Argument(0)->Argument(0) == fma->Argument(1)->Argument(0));
    Require(fma->Argument(2)->Opcode() != IrOpcode::SelectF32);
}
// v_mullit_f32 takes the v_mul_legacy_f32 rule (MullitRule, default on): a select over the product.
static void CheckMullit() {
    IrProgram program;
    IrValue* select = Translate(program, 0x150u << 16u, RdnaOpcode::VMullitF32, IrOpcode::SelectF32);
    Require(select != nullptr && select->Argument(2)->Opcode() == IrOpcode::FPMul32);
}
// The clamp output modifier (v_mul_f32 ... clamp): FClamp alone by default; APS5_CLAMP_NAN=zero or
// keep selects 0 or the unclamped value for a NaN.
static void CheckClampNan() {
    IrProgram program;
    IrValue* select = Translate(program, (0x108u << 16u) | (1u << 15u), RdnaOpcode::VMulF32, IrOpcode::SelectF32);
    if (ClampNanRule() == ClampNan::Driver) {
        Require(select == nullptr);
        IrProgram plain;
        Require(Translate(plain, (0x108u << 16u) | (1u << 15u), RdnaOpcode::VMulF32, IrOpcode::FPSaturate32) != nullptr);
        return;
    }
    Require(select != nullptr && select->Argument(0)->Opcode() == IrOpcode::UGreaterThan32 && select->Argument(2)->Opcode() == IrOpcode::FPSaturate32);
    Require(ClampNanRule() == ClampNan::Zero ? select->Argument(1)->Opcode() != IrOpcode::FPMul32 : select->Argument(1)->Opcode() == IrOpcode::FPMul32);
}
int main() {
    Check(0x140u, RdnaOpcode::VMadLegacyF32, IrOpcode::FPFma32);
    Check(0x150u, RdnaOpcode::VMullitF32, IrOpcode::FPMul32);
    CheckLegacyMad();
    CheckMullit();
    CheckClampNan();
}
