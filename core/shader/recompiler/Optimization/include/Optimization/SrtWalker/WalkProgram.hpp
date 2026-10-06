#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_WALKPROGRAM_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_WALKPROGRAM_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ShaderRecompiler::Detail {

// A resource plan compiled once into straight-line operations over a value array: the express
// walk. Evaluator walks the IR graph recursively for every capture, memoizing each value in a
// hash table and reading memory through a bookkeeping reader; with ~50 words per capture that
// walk was ~18 us of a ~31 us capture (Demon's Souls, [capture] sub-phases). The compiled form
// follows the interpreter's semantics exactly, including its laziness: a Phi resolves to its
// invariant operand at compile time, a ReadFirstLane compiles its operand under its active mask
// (the runtime selects on that mask take their first branch), a ReadConst compiles the slot's
// value (a clean slot under no mask, as the clean evaluator has none), and every other operand
// is compiled eagerly, as the interpreter evaluates it. A root the compiler cannot express (an
// opcode the interpreter rejects, an unresolved phi, a cycle) stays unsupported, and a capture
// that needs it runs the interpreter instead; a value-dependent failure (a buffer read out of
// bounds, an unrepresentable conversion) or a memory read the express reader declines is
// reported as an outcome and the interpreter decides.
struct WalkOp {
    enum class Code : std::uint8_t {
        Const, UserData, ShaderBase,
        Add32, Add64, Sub32, Sub64, Mul32, Mul64, UMin32,
        And32, And64, Or32, Xor32, Not32,
        Shl32, Shl64, Shr32, Shr64, Sar32, Sar64,
        BfeU, BfeS, Bfi, Select,
        Eq32, Ne32, ULt32, UGt32, LAnd, LOr, LXor, LNot,
        CvtF32U32, CvtU32F32, FMul32, FTrunc32, FIsNan32, FLe32, FGe32,
        Construct64, Extract64, ExtractCarry,
        Load, ConstBuffer,
    };
    Code code = Code::Const;
    std::uint8_t operands = 0;
    std::uint8_t component = 0;
    std::uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
    // Const: the value; UserData: the word offset from the plan's user data base; Load and
    // ConstBuffer: the memory operand's immediate offset (MemoryInfo::offset).
    std::uint64_t immediate = 0;
};

inline constexpr std::uint32_t WalkNoOp = 0xffffffffu;

struct WalkRoot {
    // The op whose value is the root's, WalkNoOp when the root is unsupported.
    std::uint32_t op = WalkNoOp;
    // Every op the root needs, in dependency order (operands precede their users).
    std::vector<std::uint32_t> closure;
};

struct WalkSource {
    std::uint32_t dwordCount = 0;
    std::array<WalkRoot, 8> dwords {};
    bool supported = false;
};

struct WalkProgram {
    std::vector<WalkOp> ops;
    // One per plan.materializationSources entry, plan.srtReads entry and plan.controlFlow block.
    std::vector<WalkSource> sources;
    std::vector<WalkRoot> flat;
    std::vector<WalkRoot> conditions;
    std::uint32_t unsupportedRoots = 0;
};

// Compiles a completed plan (srtPlanComplete); null for an incomplete one.
std::shared_ptr<const WalkProgram> CompileWalkProgram(const IrResourcePlan& plan);

enum class WalkOutcome : std::uint8_t { Ran, NoProgram, UnsupportedRoot, OpFailed, ReadBailed, Count };
const char* WalkOutcomeName(WalkOutcome outcome);
// The outcome of the last ExecuteWalkProgram on the calling thread, for the driver's counters;
// NoteWalkOutcome records one for a capture that never reached the program.
WalkOutcome LastWalkOutcome();
void NoteWalkOutcome(WalkOutcome outcome);
// APS5_PROFILE_DRAW: the time ExecuteWalkProgram spent over every call (its reads included).
std::uint64_t ExpressWalkNanoseconds();

// Evaluates the plan's materialization sources, flat slots and control-flow activity as
// EvaluateRuntimeSourcesImpl does for them, reading memory through runtime.expressRead
// (a reader that returns false to decline the read: ReadBailed). Ran means results, flat and
// activeSources hold what the interpreter computes; any other outcome leaves them unspecified.
WalkOutcome ExecuteWalkProgram(const WalkProgram& program, const IrResourcePlan& plan, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources);

}

#endif
