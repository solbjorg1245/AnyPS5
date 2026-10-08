#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastWalk.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
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
#include <unordered_map>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

using ShaderRecompiler::WalkStatus;

// Why a stage's walk declined: the walk's own statuses, then the direct reader's reasons.
enum class WalkDecline : std::uint8_t { NoSource, IncompletePlan, NoProgram, Bindless, UnsupportedRoot, OpFailed, Pending, Unmapped, QueuedLabel, Boundary, Failed, Count };
constexpr std::array<const char*, static_cast<std::size_t>(WalkDecline::Count)> WalkDeclineNames{"no source", "incomplete plan", "no program", "bindless", "unsupported root", "op failed", "pending block", "unmapped", "queued label", "boundary", "failed"};

// What differed between the walk's populated variant and the old path's result.
enum class WalkMismatch : std::uint8_t { Specialization, Variant, Layout, Buffer, Image, Sampler, Flat, Data, Other, Push, Vertex, Count };
constexpr std::array<const char*, static_cast<std::size_t>(WalkMismatch::Count)> WalkMismatchNames{"specialization", "variant", "layout", "buffer", "image", "sampler", "flat", "data", "other binding", "push", "vertex"};

constexpr std::uint64_t NullPageBytes = 0x10000;
constexpr std::uint64_t ReaderPageBytes = 0x1000;

// The direct reader's state for one stage: the draw's registered regions (served first, as
// ShaderMemory serves them), the page last found readable, and why a read declined.
struct FastReader {
    std::span<const DrawProgram> programs;
    std::uint64_t page = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t reads = 0;
    std::uint64_t queries = 0;
    std::optional<WalkDecline> declined;
};

// FastSrtRead (design section 2.3): the null page reads zero; a read in a pending block, over a
// queued label of this thread or in a page not mapped declines; otherwise a plain load of the live
// word (guest addresses are host pointers). No page copy, no flush hook, no snapshot.
bool fastSrtRead(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& reader = *static_cast<FastReader*>(context);
    ++reader.reads;
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        reader.declined = WalkDecline::Boundary;
        return false;
    }
    for (const auto& program : reader.programs) {
        for (const auto& region : program.memory) {
            if (address < region.guestAddress || address - region.guestAddress >= region.bytes.size()) continue;
            const auto offset = static_cast<std::size_t>(address - region.guestAddress);
            if (region.bytes.size() - offset < sizeof(*value)) {
                reader.declined = WalkDecline::Boundary;
                return false;
            }
            std::memcpy(value, region.bytes.data() + offset, sizeof(*value));
            return true;
        }
    }
    if (address < NullPageBytes) {
        *value = 0;
        return true;
    }
    if (Graphics::Recorder::BlockPending(address)) {
        reader.declined = WalkDecline::Pending;
        return false;
    }
    if (Graphics::Recorder::QueuedLabelOverlapsThisThread(address, sizeof(*value))) {
        reader.declined = WalkDecline::QueuedLabel;
        return false;
    }
    const auto page = address & ~(ReaderPageBytes - 1);
    if (page != reader.page) {
        bool queried = false;
        const bool readable = GuestMemory::ReadableWord(address, &queried);
        if (queried) ++reader.queries;
        if (!readable) {
            reader.declined = WalkDecline::Unmapped;
            return false;
        }
        reader.page = page;
    }
    std::memcpy(value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(*value));
    return true;
}

WalkDecline declineOf(WalkStatus status, const FastReader& reader) {
    switch (status) {
        case WalkStatus::NoSource: return WalkDecline::NoSource;
        case WalkStatus::IncompletePlan: return WalkDecline::IncompletePlan;
        case WalkStatus::NoProgram: return WalkDecline::NoProgram;
        case WalkStatus::Bindless: return WalkDecline::Bindless;
        case WalkStatus::UnsupportedRoot: return WalkDecline::UnsupportedRoot;
        case WalkStatus::OpFailed: return WalkDecline::OpFailed;
        default: return reader.declined.value_or(WalkDecline::Failed);
    }
}

// A stage's walk and compare time, by program (the costliest few are reported).
struct ProgramCost {
    std::uint64_t stages = 0, ns = 0;
};

struct WalkCounters {
    std::uint64_t draws = 0, stages = 0, walked = 0, walkNs = 0, compareNs = 0, reads = 0, queries = 0;
    // walkNs split: the request and its source handle, the walk itself (WalkResources), the vertex
    // V# fetch; compareNs split: populating the walked variant (the rest is the compare).
    std::uint64_t handleNs = 0, materializeNs = 0, vertexNs = 0, populateNs = 0;
    // Stages and their walk plus compare time by packet: [0] direct draws (DRAW_INDEX_OFFSET_2,
    // DRAW_INDEX_AUTO), [1] indirect draws.
    std::array<std::uint64_t, 2> kindStages{}, kindNs{};
    std::array<std::uint64_t, static_cast<std::size_t>(WalkDecline::Count)> declines{};
    // [0]: plain old results, [1]: results the old path bound by a heuristic.
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(WalkMismatch::Count)>, 2> mismatches{};
    std::array<std::uint64_t, 2> stagesMismatched{};
    std::uint64_t feedbackOnly = 0, flatFeedback = 0, deferredSkipped = 0, exceptions = 0;
    std::unordered_map<std::uint64_t, ProgramCost> programs;
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

// A sampled image's T# words a hit may differ in (the streaming-feedback fields: word 5 bit 25,
// word 6 bits 0-7; DispatchVariant::ignoredBits), which the old path keeps as stored.
std::uint32_t feedbackBits(std::size_t word) {
    if (word % 8 == 5) return 1u << 25u;
    if (word % 8 == 6) return 0xffu;
    return 0;
}

WalkMismatch mismatchOf(ShaderRecompiler::DescriptorRole role) {
    using Role = ShaderRecompiler::DescriptorRole;
    switch (role) {
        case Role::GuestBuffers: return WalkMismatch::Buffer;
        case Role::GuestImages: return WalkMismatch::Image;
        case Role::GuestSamplers: return WalkMismatch::Sampler;
        case Role::FlattenedSrt: return WalkMismatch::Flat;
        case Role::ShaderData: return WalkMismatch::Data;
        default: return WalkMismatch::Other;
    }
}

// The first difference of a stage, for the report lines.
struct WalkDifference {
    WalkMismatch kind = WalkMismatch::Count;
    std::size_t binding = 0;
    std::size_t word = 0;
    std::uint32_t old = 0;
    std::uint32_t walked = 0;
};

// The mismatch kinds of the walk's result against the old one, as a bit mask.
std::uint32_t compareResults(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, WalkDifference& first, std::uint64_t& feedbackOnly, std::uint64_t& flatFeedback, std::uint64_t& deferredSkipped) {
    std::uint32_t kinds = 0;
    const auto note = [&](WalkMismatch kind, std::size_t binding, std::size_t word, std::uint32_t oldWord, std::uint32_t walkedWord) {
        if (kinds == 0) first = {kind, binding, word, oldWord, walkedWord};
        kinds |= 1u << static_cast<unsigned>(kind);
    };
    if (old.variantId != walked.variantId) note(WalkMismatch::Variant, 0, 0, static_cast<std::uint32_t>(old.variantId), static_cast<std::uint32_t>(walked.variantId));
    if (old.bindings.size() != walked.bindings.size()) {
        note(WalkMismatch::Layout, old.bindings.size(), 0, 0, static_cast<std::uint32_t>(walked.bindings.size()));
        return kinds;
    }
    for (std::size_t index = 0; index < old.bindings.size(); ++index) {
        const auto& left = old.bindings[index];
        const auto& right = walked.bindings[index];
        if (left.kind != right.kind || left.role != right.role || left.descriptorSet != right.descriptorSet || left.binding != right.binding || left.count != right.count || left.guestDescriptor.size() != right.guestDescriptor.size()) {
            note(WalkMismatch::Layout, index, 0, 0, 0);
            continue;
        }
        const bool image = left.role == ShaderRecompiler::DescriptorRole::GuestImages && left.guestDescriptor.size() % 8 == 0;
        bool feedback = false;
        for (std::size_t word = 0; word < left.guestDescriptor.size(); ++word) {
            const auto a = left.guestDescriptor[word];
            const auto b = right.guestDescriptor[word];
            if (a == b) continue;
            // A word the old capture left to the GPU holds a placeholder there.
            if (left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt && std::any_of(left.deferredWords.begin(), left.deferredWords.end(), [&](const auto& deferred) { return deferred.first == word; })) {
                ++deferredSkipped;
                continue;
            }
            // The flat copy of a sampled T# word the old stage compare accepted through its
            // don't-care bits (FlatTsharpFeedbackCopy): the hit binds the stored word, the walk the
            // live one (t387: every flat mismatch, plain and heuristic, was this).
            if (left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt && FlatTsharpFeedbackCopy(old.bindings, walked.bindings, a, b)) {
                ++flatFeedback;
                continue;
            }
            if (image && ((a ^ b) & ~feedbackBits(word)) == 0) {
                feedback = true;
                continue;
            }
            note(mismatchOf(left.role), index, word, a, b);
            break;
        }
        if (feedback) ++feedbackOnly;
    }
    if (old.pushConstants != walked.pushConstants) {
        std::size_t at = 0;
        while (at < old.pushConstants.size() && at < walked.pushConstants.size() && old.pushConstants[at] == walked.pushConstants[at]) ++at;
        note(WalkMismatch::Push, 0, at, 0, 0);
    }
    return kinds;
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
    const auto perStage = [&](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0 / stages; };
    const auto perKind = [&](std::size_t kind) { return total.kindStages[kind] != 0 ? static_cast<double>(total.kindNs[kind]) / 1000.0 / static_cast<double>(total.kindStages[kind]) : 0.0; };
    // The costliest programs of the window by walk plus compare time.
    std::vector<std::pair<std::uint64_t, ProgramCost>> costliest(total.programs.begin(), total.programs.end());
    const auto shown = std::min<std::size_t>(costliest.size(), 3);
    std::partial_sort(costliest.begin(), costliest.begin() + static_cast<std::ptrdiff_t>(shown), costliest.end(), [](const auto& left, const auto& right) { return left.second.ns > right.second.ns; });
    std::string programs;
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [program, cost] = costliest[i];
        std::snprintf(item, sizeof(item), "%s0x%llx %llu stages %.1f us each (%.1f ms)", i == 0 ? " " : ", ", static_cast<unsigned long long>(program), count(cost.stages), cost.stages != 0 ? static_cast<double>(cost.ns) / 1000.0 / static_cast<double>(cost.stages) : 0.0, static_cast<double>(cost.ns) / 1e6);
        programs += item;
    }
    std::fprintf(stderr, "[fastpath] walk (10 s, every %u draws): %llu draws, %llu stages, %llu walked (%.1f%%); walk %.2f us per stage (request and source handle %.2f, walk %.2f, vertex fetch %.2f), %.1f reads and %.2f page queries per stage; compare %.2f us per walked stage (populate %.2f); walk and compare us per stage by packet: direct draws %.2f over %llu stages, indirect draws %.2f over %llu stages; costliest programs:%s; declines: %s; mismatched stages %llu, after heuristic hits %llu; mismatches by kind (plain/heuristic): %s; T# feedback-only differences %llu, flat T# copies differing in feedback bits only %llu, deferred words skipped %llu, exceptions %llu\n", every, count(total.draws), count(total.stages), count(total.walked), 100.0 * static_cast<double>(total.walked) / stages, perStage(total.walkNs), perStage(total.handleNs), perStage(total.materializeNs), perStage(total.vertexNs), static_cast<double>(total.reads) / stages, static_cast<double>(total.queries) / stages, static_cast<double>(total.compareNs) / 1000.0 / walked, static_cast<double>(total.populateNs) / 1000.0 / walked, perKind(0), count(total.kindStages[0]), perKind(1), count(total.kindStages[1]), programs.empty() ? " none" : programs.c_str(), declines.c_str(), count(total.stagesMismatched[0]), count(total.stagesMismatched[1]), kinds.c_str(), count(total.feedbackOnly), count(total.flatFeedback), count(total.deferredSkipped), count(total.exceptions));
}

std::uint64_t nanosecondsBetween(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point ended) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
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
    // Each stage's walk plus compare time, for the by-packet and by-program rows.
    const std::size_t packetKind = draw.drawParameters.indirect.has_value() ? 1 : 0;
    std::array<std::pair<std::uint64_t, std::uint64_t>, 8> stageCosts{};
    std::size_t stageCount = 0;
    const auto noteStage = [&](std::uint64_t program, std::uint64_t ns) {
        ++local.kindStages[packetKind];
        local.kindNs[packetKind] += ns;
        if (stageCount < stageCosts.size()) stageCosts[stageCount++] = {program, ns};
    };
    try {
        auto& scratch = HostThreadLocal<WalkScratch, WalkScratchTag>();
        for (std::size_t i = 0; i < draw.programs.size() && i < draw.results.size(); ++i) {
            if (draw.roles[i] == Role::GeometryBack || draw.results[i] == nullptr || draw.programs[i].snapshot == nullptr) continue;
            const auto& program = draw.programs[i];
            const auto& old = *draw.results[i];
            ++local.stages;
            const auto started = std::chrono::steady_clock::now();
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
            const std::string* poisoned = nullptr;
            const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, draw.device.Serial(), request, false, &poisoned);
            const auto handled = std::chrono::steady_clock::now();
            local.handleNs += nanosecondsBetween(started, handled);
            if (handle == nullptr) {
                ++local.declines[static_cast<std::size_t>(WalkDecline::NoSource)];
                local.walkNs += nanosecondsBetween(started, handled);
                noteStage(program.binary.codeAddress, nanosecondsBetween(started, handled));
                continue;
            }
            FastReader reader{draw.programs};
            ShaderRecompiler::SrtRuntime runtime;
            runtime.userContext = &reader;
            runtime.readMemory = &fastSrtRead;
            runtime.readSpecializationMemory = &fastSrtRead;
            runtime.expressRead = &fastSrtRead;
            auto status = ShaderRecompiler::WalkResources(*handle, program.userData, program.binary.codeAddress, runtime, scratch.snapshot, scratch.specialization);
            const auto materialized = std::chrono::steady_clock::now();
            local.materializeNs += nanosecondsBetween(handled, materialized);
            // The vertex V#s the stage info is built from, through the same reader.
            const bool vertexFetch = status == WalkStatus::Walked && draw.vertexInfos[i].has_value() && program.binary.stage != Stage::Fragment;
            if (vertexFetch && !Graphics::ResolveVertexFetch(fetchPlanFor(scratch, program), program.userData, &fastSrtRead, &reader, scratch.vertex)) status = WalkStatus::ReadDeclined;
            const auto fetched = vertexFetch ? std::chrono::steady_clock::now() : materialized;
            local.vertexNs += nanosecondsBetween(materialized, fetched);
            local.walkNs += nanosecondsBetween(started, fetched);
            local.reads += reader.reads;
            local.queries += reader.queries;
            if (status != WalkStatus::Walked) {
                ++local.declines[static_cast<std::size_t>(declineOf(status, reader))];
                noteStage(program.binary.codeAddress, nanosecondsBetween(started, fetched));
                continue;
            }
            ++local.walked;
            WalkDifference first;
            std::uint32_t kinds = 0;
            const bool populated = ShaderRecompiler::PopulateVariant(*handle, request, scratch.snapshot, scratch.specialization, scratch.walked);
            const auto populatedAt = std::chrono::steady_clock::now();
            local.populateNs += nanosecondsBetween(fetched, populatedAt);
            if (!populated) {
                kinds = 1u << static_cast<unsigned>(WalkMismatch::Specialization);
                first.kind = WalkMismatch::Specialization;
            } else {
                kinds = compareResults(old, scratch.walked, first, local.feedbackOnly, local.flatFeedback, local.deferredSkipped);
            }
            if (vertexFetch && !sameVertexFetch(*draw.vertexInfos[i], scratch.vertex)) {
                if (kinds == 0) first.kind = WalkMismatch::Vertex;
                kinds |= 1u << static_cast<unsigned>(WalkMismatch::Vertex);
            }
            const auto compared = std::chrono::steady_clock::now();
            local.compareNs += nanosecondsBetween(fetched, compared);
            noteStage(program.binary.codeAddress, nanosecondsBetween(started, compared));
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
    total.handleNs += local.handleNs;
    total.materializeNs += local.materializeNs;
    total.vertexNs += local.vertexNs;
    total.populateNs += local.populateNs;
    for (std::size_t column = 0; column < 2; ++column) {
        total.kindStages[column] += local.kindStages[column];
        total.kindNs[column] += local.kindNs[column];
    }
    // Bounded: a window with more programs than this keeps the first ones' costs.
    constexpr std::size_t MaxPrograms = 4096;
    for (std::size_t stage = 0; stage < stageCount; ++stage) {
        const auto [program, ns] = stageCosts[stage];
        auto found = total.programs.find(program);
        if (found == total.programs.end()) {
            if (total.programs.size() >= MaxPrograms) continue;
            found = total.programs.emplace(program, ProgramCost{}).first;
        }
        ++found->second.stages;
        found->second.ns += ns;
    }
    total.reads += local.reads;
    total.queries += local.queries;
    for (std::size_t reason = 0; reason < total.declines.size(); ++reason) total.declines[reason] += local.declines[reason];
    for (std::size_t column = 0; column < 2; ++column) {
        total.stagesMismatched[column] += local.stagesMismatched[column];
        for (std::size_t kind = 0; kind < WalkMismatchNames.size(); ++kind) total.mismatches[column][kind] += local.mismatches[column][kind];
    }
    total.feedbackOnly += local.feedbackOnly;
    total.flatFeedback += local.flatFeedback;
    total.deferredSkipped += local.deferredSkipped;
    total.exceptions += local.exceptions;
    if (!profile || std::chrono::steady_clock::now() - total.lastReport < std::chrono::seconds(10)) return;
    report(total, every);
    total = WalkCounters{};
}

}
