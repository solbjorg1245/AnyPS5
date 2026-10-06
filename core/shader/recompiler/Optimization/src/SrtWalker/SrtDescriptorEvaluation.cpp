#include "Optimization/SrtWalker/SrtDescriptorEvaluation.hpp"
#include "Optimization/SrtWalker/SrtEvaluator.hpp"
#include "Optimization/SrtWalker/WalkProgram.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

namespace ShaderRecompiler::Detail {

namespace {

std::string& failureReason() {
    struct FailureReasonStorage {};
    return HostThreadLocal<std::string, FailureReasonStorage>();
}

std::string DescribeValue(const IrValue* value, std::uint32_t depth) {
    if (value == nullptr) return "null";
    value = value->Resolve();
    std::string text(IrOpcodeName(value->Opcode()));
    if (value->HasImmediate() && value->Type() == IrType::U32) return text + "(" + std::to_string(value->ImmediateU32()) + ")";
    if (depth == 0 || value->ArgumentCount() == 0) return text;
    text += "(";
    for (std::size_t index = 0; index < value->ArgumentCount(); ++index) {
        if (index != 0) text += ", ";
        text += DescribeValue(value->Argument(index), depth - 1);
    }
    return text + ")";
}

bool Fail(std::string reason) {
    failureReason() = std::move(reason);
    return false;
}

const DescriptorSource* Source(const IrResourcePlan& program, std::uint32_t source) {
    if (source >= program.descriptorSources.size()) {
        return nullptr;
    }
    return &program.descriptorSources[source];
}

}

// The interpreter: Evaluator over the IR graph, reading through runtime.readMemory.
static bool evaluateInterpreted(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, bool evaluateFlat, std::span<const std::uint8_t> cleanFlatSlots, std::vector<std::uint8_t>& activeSources) {
    failureReason().clear();
    static const bool debug = std::getenv("APS5_SRT_DEBUG") != nullptr;
    if (debug) {
        for (std::size_t slot = 0; slot < program.srtReads.size(); ++slot) std::fprintf(stderr, "[srt] slot %zu = %s"  "\n", slot, DescribeValue(program.srtReads[slot].value, 6).c_str());
    }
    if (!program.srtPlanComplete) {
        return Fail("SRT plan is incomplete");
    }
    if (std::any_of(cleanFlatSlots.begin(), cleanFlatSlots.end(), [](std::uint8_t clean) { return clean != 0u; }) && runtime.readSpecializationMemory == nullptr) {
        return Fail("clean flat slots need specialization memory");
    }
    SrtRuntime cleanRuntime = runtime;
    cleanRuntime.readMemory = runtime.readSpecializationMemory;
    Evaluator cleanEvaluator(program, cleanRuntime);
    Evaluator evaluator(program, runtime, cleanFlatSlots, &cleanEvaluator);
    std::vector<std::uint8_t> active;
    if (evaluateFlat) {
        active.assign(program.descriptorSources.size(), 1u);
    }
    if (evaluateFlat && !program.controlFlow.empty()) {
        for (const auto& block : program.controlFlow) {
            for (const auto source : block.sources) {
                active.at(source) = 0u;
            }
        }
        std::vector<std::uint8_t> visited(program.controlFlow.size());
        std::vector<std::uint32_t> pending {0};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (visited.at(index)) {
                continue;
            }
            visited[index] = 1u;
            const auto& block = program.controlFlow[index];
            for (const auto source : block.sources) {
                active[source] = 1u;
            }
            std::uint32_t condition = 0;
            const bool cleanEvaluable = block.condition != nullptr && runtime.readSpecializationMemory != nullptr && cleanEvaluator.Evaluate(block.condition, condition);
            if (cleanEvaluable) {
                pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
            } else {
                pending.insert(pending.end(), block.successors.begin(), block.successors.end());
            }
        }
    }
    std::vector<DescriptorValue> evaluated;
    evaluated.reserve(sources.size());
    for (const auto sourceIndex : sources) {
        const auto* source = Source(program, sourceIndex);
        if (source == nullptr) {
            return Fail("descriptor source " + std::to_string(sourceIndex) + " does not exist");
        }
        DescriptorValue value;
        value.dwordCount = source->dwordCount;
        if (!evaluateFlat || active[sourceIndex]) {
            for (std::uint32_t index = 0; index < source->dwordCount; index++) {
                if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
                    std::string detail = DescribeValue(source->dwords[index], 4);
                    const IrValue* dword = source->dwords[index]->Resolve();
                    if (dword->Opcode() == IrOpcode::ReadConst && dword->ArgumentCount() == 2 && dword->Argument(1)->Resolve()->HasImmediate()) {
                        const auto slot = dword->Argument(1)->Resolve()->ImmediateU32();
                        if (slot < program.srtReads.size()) detail += " where slot " + std::to_string(slot) + " = " + DescribeValue(program.srtReads[slot].value, 8);
                    }
                    return Fail("descriptor source " + std::to_string(sourceIndex) + " dword " + std::to_string(index) + ": " + detail);
                }
            }
        }
        evaluated.push_back(value);
    }
    std::vector<std::uint32_t> flattened;
    if (evaluateFlat) {
        flattened.resize(program.srtReads.size());
        for (const auto& read : program.srtReads) {
            const bool clean = read.flatOffset < cleanFlatSlots.size() && cleanFlatSlots[read.flatOffset] != 0u;
            auto& selected = clean ? cleanEvaluator : evaluator;
            // A pure slot's raw read is reachable from no root, so it was not evaluated (nor
            // cached) before this loop: its dereference happens here, once, and is recorded as
            // the slot's leaf; reads nested in its address cone land among the other reads.
            auto* trace = runtime.readTrace;
            const bool pure = trace != nullptr && read.flatOffset < program.pureFlatSlots.size() && program.pureFlatSlots[read.flatOffset] != 0u;
            if (pure) {
                trace->leaf = read.value->Resolve();
                trace->leafSlot = read.flatOffset;
            }
            const bool evaluated = read.flatOffset < flattened.size() && selected.Evaluate(read.value, flattened[read.flatOffset]);
            if (pure) trace->leaf = nullptr;
            if (!evaluated) {
                return Fail(std::string(clean ? "clean " : "") + "SRT read at flat offset " + std::to_string(read.flatOffset) + ": " + DescribeValue(read.value, 4));
            }
        }
    }
    results = std::move(evaluated);
    activeSources = std::move(active);
    if (evaluateFlat) {
        flat = std::move(flattened);
    }
    return true;
}

// The express walk first, for the standard materialization call (every materialization source,
// the flat slots, the plan's own clean slots, one reader for both evaluators): a run that
// completes is the interpreter's result. APS5_VERIFY_EXPRESS=1 runs the interpreter as well and
// aborts on a difference; the recompiler's source cache compiles no program under
// APS5_NO_EXPRESS=1, so the interpreter alone runs.
bool EvaluateRuntimeSourcesImpl(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, bool evaluateFlat, std::span<const std::uint8_t> cleanFlatSlots, std::vector<std::uint8_t>& activeSources) {
    const bool standard = runtime.walk != nullptr && runtime.expressRead != nullptr && evaluateFlat && runtime.readMemory == runtime.readSpecializationMemory && sources.data() == program.materializationSources.data() && sources.size() == program.materializationSources.size() && cleanFlatSlots.data() == program.cleanFlatSlots.data() && cleanFlatSlots.size() == program.cleanFlatSlots.size();
    if (!standard) {
        NoteWalkOutcome(WalkOutcome::NoProgram);
        return evaluateInterpreted(program, sources, runtime, results, flat, evaluateFlat, cleanFlatSlots, activeSources);
    }
    static const bool verify = std::getenv("APS5_VERIFY_EXPRESS") != nullptr;
    std::vector<DescriptorValue> expressResults;
    std::vector<std::uint32_t> expressFlat;
    std::vector<std::uint8_t> expressActive;
    SrtRuntime express = runtime;
    // Under verification the interpreter's walk alone traces the reads.
    if (verify) express.readTrace = nullptr;
    if (ExecuteWalkProgram(*runtime.walk, program, express, expressResults, expressFlat, expressActive) != WalkOutcome::Ran) {
        return evaluateInterpreted(program, sources, runtime, results, flat, evaluateFlat, cleanFlatSlots, activeSources);
    }
    if (!verify) {
        results = std::move(expressResults);
        flat = std::move(expressFlat);
        activeSources = std::move(expressActive);
        return true;
    }
    if (!evaluateInterpreted(program, sources, runtime, results, flat, evaluateFlat, cleanFlatSlots, activeSources)) {
        std::fprintf(stderr, "[express] verify: shader 0x%llx: the interpreter failed (%s) where the express walk ran\n", static_cast<unsigned long long>(runtime.shaderBase), failureReason().c_str());
        std::abort();
    }
    static std::atomic<std::uint64_t> verified{0};
    const auto count = verified.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto differ = [&](const char* what, std::size_t index, std::uint32_t expected, std::uint32_t got) {
        std::fprintf(stderr, "[express] verify: shader 0x%llx: %s %zu differs: interpreter %08x, express %08x (after %llu verified)\n", static_cast<unsigned long long>(runtime.shaderBase), what, index, expected, got, static_cast<unsigned long long>(count - 1));
        std::abort();
    };
    if (results.size() != expressResults.size()) differ("source count", results.size(), 0, static_cast<std::uint32_t>(expressResults.size()));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].dwordCount != expressResults[i].dwordCount) differ("source dword count", i, results[i].dwordCount, expressResults[i].dwordCount);
        for (std::uint32_t dword = 0; dword < results[i].dwordCount; ++dword) {
            if (results[i].dwords[dword] != expressResults[i].dwords[dword]) differ("source dword", i * 8 + dword, results[i].dwords[dword], expressResults[i].dwords[dword]);
        }
    }
    if (flat.size() != expressFlat.size()) differ("flat slot count", flat.size(), 0, static_cast<std::uint32_t>(expressFlat.size()));
    for (std::size_t i = 0; i < flat.size(); ++i) {
        if (flat[i] != expressFlat[i]) differ("flat slot", i, flat[i], expressFlat[i]);
    }
    if (activeSources != expressActive) differ("active sources", activeSources.size(), 0, static_cast<std::uint32_t>(expressActive.size()));
    if (count % 10000 == 0) std::fprintf(stderr, "[express] verify: %llu captures agreed with the interpreter\n", static_cast<unsigned long long>(count));
    return true;
}

const std::string& RuntimeSourceFailureReason() {
    return failureReason();
}

}
