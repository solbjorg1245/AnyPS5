#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastWalk.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

using ShaderRecompiler::WalkStatus;

struct WalkCounters {
    std::uint64_t draws = 0, stages = 0, walked = 0, walkNs = 0, compareNs = 0, reads = 0, queries = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(WalkDecline::Count)> declines{};
    // [0]: plain old results, [1]: results the old path bound by a heuristic.
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(WalkMismatch::Count)>, 2> mismatches{};
    std::array<std::uint64_t, 2> stagesMismatched{};
    std::uint64_t feedbackOnly = 0, deferredSkipped = 0, exceptions = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

HostMutex countersMutex;
WalkCounters counters;

// The memoized header part of the vertex fetch, per registered program.
struct FetchPlanEntry {
    const ShaderSnapshot* raw = nullptr;
    std::weak_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
    Graphics::VertexFetchPlan plan;
};

struct WalkScratch {
    ShaderRecompiler::ResourceSnapshot snapshot;
    ShaderRecompiler::ResourceSpecialization specialization;
    ShaderRecompiler::RecompileResult walked;
    ShaderRecompiler::ShaderVertexStageInfo vertex{};
    std::vector<FetchPlanEntry> plans;
};
struct WalkScratchTag {};

const Graphics::VertexFetchPlan& fetchPlanFor(WalkScratch& scratch, const DrawProgram& program) {
    for (const auto& entry : scratch.plans) {
        if (entry.raw == program.snapshot.get() && entry.codeOffset == program.codeOffset && !entry.snapshot.expired()) return entry.plan;
    }
    constexpr std::size_t Entries = 256;
    if (scratch.plans.size() >= Entries) scratch.plans.erase(scratch.plans.begin());
    scratch.plans.push_back({program.snapshot.get(), program.snapshot, program.codeOffset, Graphics::DecodeVertexFetchPlan(program.binary.header, program.binary.headerAddress)});
    return scratch.plans.back().plan;
}

bool sameVertexFetch(const ShaderRecompiler::ShaderVertexStageInfo& left, const ShaderRecompiler::ShaderVertexStageInfo& right) {
    if (left.resourcesNum != right.resourcesNum || left.fetchEmbedded != right.fetchEmbedded || left.fetchAttribReg != right.fetchAttribReg || left.fetchBufferReg != right.fetchBufferReg) return false;
    for (std::uint32_t i = 0; i < left.resourcesNum && i < ShaderRecompiler::ShaderVertexStageInfo::MaxResources; ++i) {
        const auto& a = left.resourcesDst[i];
        const auto& b = right.resourcesDst[i];
        if (left.resources[i].fields != right.resources[i].fields || a.registerStart != b.registerStart || a.registersNum != b.registersNum || a.attrId != b.attrId || a.fetchIndex != b.fetchIndex) return false;
    }
    return true;
}

void report(WalkCounters& total, std::uint32_t every) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::string declines;
    std::string kinds;
    char item[96];
    for (std::size_t reason = 0; reason < total.declines.size(); ++reason) {
        std::snprintf(item, sizeof(item), "%s%s %llu", reason == 0 ? "" : ", ", WalkDeclineNames[reason], count(total.declines[reason]));
        declines += item;
    }
    for (std::size_t kind = 0; kind < WalkMismatchNames.size(); ++kind) {
        std::snprintf(item, sizeof(item), "%s%s %llu/%llu", kind == 0 ? "" : ", ", WalkMismatchNames[kind], count(total.mismatches[0][kind]), count(total.mismatches[1][kind]));
        kinds += item;
    }
    const auto stages = static_cast<double>(total.stages != 0 ? total.stages : 1);
    const auto walked = static_cast<double>(total.walked != 0 ? total.walked : 1);
    std::fprintf(stderr, "[fastpath] walk (10 s, every %u draws): %llu draws, %llu stages, %llu walked (%.1f%%); walk %.2f us per stage, %.1f reads and %.2f page queries per stage; compare %.2f us per walked stage; declines: %s; mismatched stages %llu, after heuristic hits %llu; mismatches by kind (plain/heuristic): %s; T# feedback-only differences %llu, deferred words skipped %llu, exceptions %llu\n", every, count(total.draws), count(total.stages), count(total.walked), 100.0 * static_cast<double>(total.walked) / stages, static_cast<double>(total.walkNs) / 1000.0 / stages, static_cast<double>(total.reads) / stages, static_cast<double>(total.queries) / stages, static_cast<double>(total.compareNs) / 1000.0 / walked, declines.c_str(), count(total.stagesMismatched[0]), count(total.stagesMismatched[1]), kinds.c_str(), count(total.feedbackOnly), count(total.deferredSkipped), count(total.exceptions));
}

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

}

std::uint32_t FastWalkEvery() {
    static const std::uint32_t every = [] {
        const char* text = std::getenv("APS5_FAST_WALK");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 0u;
    }();
    return every;
}

void ShadowWalkDraw(const FastWalkDraw& draw) {
    const auto every = FastWalkEvery();
    static std::atomic<std::uint64_t> calls{0};
    if (every == 0 || calls.fetch_add(1, std::memory_order_relaxed) % every != 0) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    WalkCounters local;
    local.draws = 1;
    try {
        auto& scratch = HostThreadLocal<WalkScratch, WalkScratchTag>();
        for (std::size_t i = 0; i < draw.programs.size() && i < draw.results.size(); ++i) {
            if (draw.roles[i] == Role::GeometryBack || draw.results[i] == nullptr || draw.programs[i].snapshot == nullptr) continue;
            const auto& program = draw.programs[i];
            const auto& old = *draw.results[i];
            ++local.stages;
            // The request compileDrawStage builds, without the memory regions (the walk reads live).
            const auto waveSize = program.binary.stage == Stage::Fragment ? draw.graphics.stages.fragmentWaveSize : draw.graphics.stages.vertexWaveSize;
            const auto pushOffset = draw.pushOffsets[i];
            ShaderRecompiler::RecompileRequest request{
                program.binary,
                {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(draw.pixel) : std::nullopt, draw.vertexInfos[i], {}},
                draw.device.Target(),
                {0, 0, pushOffset, (draw.graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes) - pushOffset},
                ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, draw.linked, draw.graphics.stages.mesh, draw.graphics.stages.tessellation, {draw.drawParameters.indexAddress, draw.drawParameters.indexCount, draw.drawParameters.indexSize, draw.drawParameters.instanceCount}}
            };
            const auto started = std::chrono::steady_clock::now();
            const std::string* poisoned = nullptr;
            const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, draw.device.Serial(), request, false, &poisoned);
            if (handle == nullptr) {
                ++local.declines[static_cast<std::size_t>(WalkDecline::NoSource)];
                local.walkNs += nanosecondsSince(started);
                continue;
            }
            FastReader reader{draw.programs};
            ShaderRecompiler::SrtRuntime runtime;
            runtime.userContext = &reader;
            runtime.readMemory = &FastSrtRead;
            runtime.readSpecializationMemory = &FastSrtRead;
            runtime.expressRead = &FastSrtRead;
            auto status = ShaderRecompiler::WalkResources(*handle, program.userData, program.binary.codeAddress, runtime, scratch.snapshot, scratch.specialization);
            // The vertex V#s the stage info is built from, through the same reader.
            const bool vertexFetch = status == WalkStatus::Walked && draw.vertexInfos[i].has_value() && program.binary.stage != Stage::Fragment;
            if (vertexFetch && !Graphics::ResolveVertexFetch(fetchPlanFor(scratch, program), program.userData, &FastSrtRead, &reader, scratch.vertex)) status = WalkStatus::ReadDeclined;
            local.walkNs += nanosecondsSince(started);
            local.reads += reader.reads;
            local.queries += reader.queries;
            if (status != WalkStatus::Walked) {
                ++local.declines[static_cast<std::size_t>(WalkDeclineOf(status, reader))];
                continue;
            }
            ++local.walked;
            const auto compareStarted = std::chrono::steady_clock::now();
            WalkDifference first;
            std::uint32_t kinds = 0;
            if (!ShaderRecompiler::PopulateVariant(*handle, request, scratch.snapshot, scratch.specialization, scratch.walked)) {
                kinds = 1u << static_cast<unsigned>(WalkMismatch::Specialization);
                first.kind = WalkMismatch::Specialization;
            } else {
                kinds = CompareWalkedResults(old, scratch.walked, first, local.feedbackOnly, local.deferredSkipped);
            }
            if (vertexFetch && !sameVertexFetch(*draw.vertexInfos[i], scratch.vertex)) {
                if (kinds == 0) first.kind = WalkMismatch::Vertex;
                kinds |= 1u << static_cast<unsigned>(WalkMismatch::Vertex);
            }
            local.compareNs += nanosecondsSince(compareStarted);
            if (kinds == 0) continue;
            const auto column = draw.heuristic ? 1u : 0u;
            ++local.stagesMismatched[column];
            for (std::size_t kind = 0; kind < WalkMismatchNames.size(); ++kind) {
                if ((kinds & (1u << kind)) != 0) ++local.mismatches[column][kind];
            }
            static std::atomic<std::uint32_t> printed{0};
            if (printed.fetch_add(1, std::memory_order_relaxed) < 20) {
                std::fprintf(stderr, "[fastpath] walk mismatch: stage %zu (program 0x%llx)%s: %s (kinds 0x%x), binding %zu word %zu: old %08x, walk %08x; variant old %llu, walk %llu\n", i, static_cast<unsigned long long>(program.binary.codeAddress), draw.heuristic ? " after a heuristic hit" : "", first.kind < WalkMismatch::Count ? WalkMismatchNames[static_cast<std::size_t>(first.kind)] : "?", kinds, first.binding, first.word, first.old, first.walked, static_cast<unsigned long long>(old.variantId), static_cast<unsigned long long>(scratch.walked.variantId));
            }
        }
    } catch (const std::exception&) {
        ++local.exceptions;
    } catch (...) {
        ++local.exceptions;
    }
    std::lock_guard lock(countersMutex);
    auto& total = counters;
    total.draws += local.draws;
    total.stages += local.stages;
    total.walked += local.walked;
    total.walkNs += local.walkNs;
    total.compareNs += local.compareNs;
    total.reads += local.reads;
    total.queries += local.queries;
    for (std::size_t reason = 0; reason < total.declines.size(); ++reason) total.declines[reason] += local.declines[reason];
    for (std::size_t column = 0; column < 2; ++column) {
        total.stagesMismatched[column] += local.stagesMismatched[column];
        for (std::size_t kind = 0; kind < WalkMismatchNames.size(); ++kind) total.mismatches[column][kind] += local.mismatches[column][kind];
    }
    total.feedbackOnly += local.feedbackOnly;
    total.deferredSkipped += local.deferredSkipped;
    total.exceptions += local.exceptions;
    if (!profile || std::chrono::steady_clock::now() - total.lastReport < std::chrono::seconds(10)) return;
    report(total, every);
    total = WalkCounters{};
}

}
