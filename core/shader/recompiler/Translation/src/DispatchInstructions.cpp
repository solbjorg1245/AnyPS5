#include "Translation/DispatchInstructions.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "Translation/TranslationContext.hpp"
#include "Recompiler.hpp"
#include <mutex>
#include <cstdio>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ShaderRecompiler {

void DispatchInstruction(IrBuilder& builder, const RdnaInstruction& instruction, const ControlFlowGraph& cfg, const TranslateOptions& options) {
    throw std::runtime_error("DispatchInstruction not implemented");
}

void TranslationContext::TranslateInstruction(const RdnaInstruction& decoded) {
    RdnaInstruction instruction = decoded;
    instruction.destination = destinationOperand(decoded);
    currentOpcode = instruction.op;
    currentProgramCounter = instruction.programCounter;
    if (instruction.op == RdnaOpcode::Unknown || instruction.op == RdnaOpcode::Count) {
        throw std::runtime_error("decoded opcode has no IR translation at pc " + std::to_string(instruction.programCounter));
    }
    if (instruction.op == RdnaOpcode::Unsupported) {
        throw std::runtime_error(instruction.unsupportedReason.empty() ? "unsupported decoded instruction at pc " + std::to_string(instruction.programCounter) : std::string(instruction.unsupportedReason));
    }
    // The probe register starts at -12345.0, so lanes that never reach the probed instruction show.
    if (const DebugProbe probe = DebugProbeConfig(); probe.enabled && instruction.programCounter == 0u) {
        RdnaOperand target{};
        target.kind = RdnaOperandKind::VectorRegister;
        target.reg = 255u;
        writeOperand(target, &ir.Constant(0xc640e400u));
    }
    bool translated = false;
    switch (instruction.family) {
        case RdnaInstructionFamily::SOP1:
        case RdnaInstructionFamily::SOP2:
        case RdnaInstructionFamily::SOPK:
        case RdnaInstructionFamily::SOPC:
        case RdnaInstructionFamily::SOPP:
            translated = emitScalar(instruction);
            break;
        case RdnaInstructionFamily::VOP1:
        case RdnaInstructionFamily::VOP2:
        case RdnaInstructionFamily::VOP3:
        case RdnaInstructionFamily::VOP3P:
        case RdnaInstructionFamily::VOPC:
            translated = emitVector(instruction);
            break;
        case RdnaInstructionFamily::SMEM:
        case RdnaInstructionFamily::MUBUF:
        case RdnaInstructionFamily::MTBUF:
        case RdnaInstructionFamily::FLAT:
        case RdnaInstructionFamily::DS:
        case RdnaInstructionFamily::MIMG:
            translated = emitMemory(instruction);
            break;
        case RdnaInstructionFamily::VINTRP:
            translated = emitInterpolation(instruction);
            break;
        case RdnaInstructionFamily::EXP:
            eXP(instruction);
            translated = true;
            break;
        default:
            break;
    }
    if (!translated) {
        throw std::runtime_error("opcode has no IR translation at pc " + std::to_string(instruction.programCounter));
    }
    if (const DebugProbe probe = DebugProbeConfig(); probe.enabled && instruction.programCounter == probe.programCounter) {
        RdnaOperand source{};
        source.kind = probe.scalar ? RdnaOperandKind::ScalarRegister : RdnaOperandKind::VectorRegister;
        source.reg = probe.vgpr;
        RdnaOperand target{};
        target.kind = RdnaOperandKind::VectorRegister;
        target.reg = 255u;
        writeOperand(target, &readRawU32(source).Value());
    }
}

namespace {
std::atomic<bool> g_debugProbeActive{false};
}

void SetDebugProbeActive(bool active) {
    g_debugProbeActive.store(active);
}

bool DebugProbeActive() {
    return g_debugProbeActive.load();
}

bool RayTracingStrict() {
    static const bool strict = [] {
        const char* text = std::getenv("APS5_RAYTRACING");
        return text != nullptr && std::strcmp(text, "strict") == 0;
    }();
    return strict;
}

bool RayTracingMiss() {
    static const bool miss = [] {
        const char* text = std::getenv("APS5_RAYTRACING");
        return text != nullptr && std::strcmp(text, "miss") == 0;
    }();
    return miss;
}

bool LegacyMadRule() {
    static const bool enabled = std::getenv("APS5_NO_LEGACY_MAD_RULE") == nullptr;
    return enabled;
}

bool MullitRule() {
    static const bool enabled = std::getenv("APS5_NO_MULLIT_RULE") == nullptr;
    return enabled;
}

bool ClampNanRule() {
    static const bool enabled = std::getenv("APS5_NO_CLAMP_NAN_RULE") == nullptr;
    return enabled;
}

namespace {
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(LegacyFloatSite::Count)> g_legacyFloatSites{};
}

void CountLegacyFloatSite(LegacyFloatSite site) {
    g_legacyFloatSites[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t LegacyFloatSites(LegacyFloatSite site) {
    return g_legacyFloatSites[static_cast<std::size_t>(site)].load(std::memory_order_relaxed);
}

namespace {

DebugProbe parseProbe(const char* text) {
    DebugProbe result;
    if (text == nullptr) return result;
    char* end = nullptr;
    result.programCounter = static_cast<std::uint32_t>(std::strtoul(text, &end, 16));
    if (end == nullptr || *end != ':') return result;
    result.scalar = end[1] == 's';
    result.vgpr = static_cast<std::uint32_t>(std::strtoul(end + (result.scalar ? 2 : 1), &end, 10));
    if (end != nullptr && *end == ':') result.shift = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10));
    result.enabled = result.vgpr < 255u;
    return result;
}

// APS5_PROBE, or the first line of the file APS5_PROBE_FILE names (read again at most once a second),
// so a probe moves to another instruction without restarting the title.
DebugProbe currentProbe() {
    static const DebugProbe fromEnvironment = parseProbe(std::getenv("APS5_PROBE"));
    static const char* file = std::getenv("APS5_PROBE_FILE");
    if (file == nullptr) return fromEnvironment;
    static HostMutex mutex;
    static DebugProbe fromFile;
    static std::chrono::steady_clock::time_point readAt{};
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - readAt >= std::chrono::seconds(1)) {
        readAt = now;
        fromFile = {};
        if (std::FILE* input = std::fopen(file, "r")) {
            char line[64] = {};
            if (std::fgets(line, sizeof(line), input) != nullptr) fromFile = parseProbe(line);
            std::fclose(input);
        }
    }
    return fromFile;
}

}

std::uint64_t DebugProbeKey() {
    const auto probe = DebugProbeConfig();
    return probe.enabled ? (static_cast<std::uint64_t>(probe.programCounter) << 32u) | (static_cast<std::uint64_t>(probe.scalar) << 30u) | (static_cast<std::uint64_t>(probe.vgpr) << 8u) | probe.shift | 0x80000000ull : 0u;
}

DebugProbe DebugProbeConfig() {
    DebugProbe probe = currentProbe();
    probe.enabled = probe.enabled && DebugProbeActive();
    return probe;
}

void DispatchInstruction(TranslationContext& context, const RdnaInstruction& instruction) {
    throw std::runtime_error("DispatchInstruction not implemented");
}

}
