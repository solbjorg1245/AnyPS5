#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

bool patchedResultReuse() {
    static const bool reuse = std::getenv("APS5_NO_PATCHED_RESULT_REUSE") == nullptr;
    return reuse;
}

bool partialDrawHits() {
    static const bool partial = std::getenv("APS5_NO_PARTIAL_DRAW_HITS") == nullptr;
    return partial;
}

bool traceDrawMisses() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_MISSES") != nullptr;
    return trace;
}

// Debug aid: APS5_TRACE_DRAW_MISSES=1 reports every 10 s, for the stage misses that compared a
// variant and found it Differing, which of its words differ from guest memory now ([draw-miss]):
// per program the misses, how many words differed, and the first (run, word) positions with the
// stored and live values, to tell a moved pointer (a relocation candidate) from a per-frame
// constant the data positions left out. Reads through CopyMapped without a pending-write sync.
void probeDrawMiss(std::uint64_t program, ShaderRecompiler::ProgramRole role, const DispatchVariant& variant) {
    struct Position {
        std::uint64_t count = 0;
        std::uint32_t stored = 0, live = 0;
        std::uint64_t address = 0;
        // The stored words of the run from the 8-word group the position sits in (a T# or V#).
        std::array<std::uint32_t, 8> group{};
        std::size_t groupWords = 0;
    };
    struct Program {
        std::uint64_t misses = 0, wordsDiffering = 0, atDataPositions = 0, atIgnoredBits = 0, atBaseSlots = 0, unmapped = 0, equalNow = 0;
        std::array<std::uint64_t, 3> byCount{};
        ShaderRecompiler::ProgramRole role{};
        std::map<std::pair<std::uint32_t, std::uint32_t>, Position> positions;
    };
    static HostMutex mutex;
    static std::map<std::uint64_t, Program> programs;
    static std::uint64_t total = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    thread_local std::vector<std::byte> live;
    std::lock_guard lock(mutex);
    ++total;
    auto& entry = programs[program];
    entry.role = role;
    ++entry.misses;
    std::size_t base = 0;
    std::uint64_t differing = 0;
    for (std::size_t r = 0; r < variant.runs.size(); ++r) {
        const auto [begin, end] = variant.runs[r];
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        live.resize(count * sizeof(std::uint32_t));
        if (GuestMemory::CopyMapped(begin, live) != GuestMemory::Compare::Equal) {
            ++entry.unmapped;
            base += count;
            continue;
        }
        for (std::size_t w = 0; w < count && base + w < variant.words.size(); ++w) {
            std::uint32_t fresh = 0;
            std::memcpy(&fresh, live.data() + w * sizeof(std::uint32_t), sizeof(fresh));
            const auto stored = variant.words[base + w];
            if (stored == fresh) continue;
            const auto position = static_cast<std::uint32_t>(base + w);
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) {
                ++entry.atDataPositions;
                continue;
            }
            if (const auto mask = IgnoredMaskAt(variant.ignoredBits, position); mask != 0 && ((stored ^ fresh) & ~mask) == 0) {
                ++entry.atIgnoredBits;
                continue;
            }
            if (const auto mask = PatchMaskAt(variant.baseSlots, position); mask != 0 && ((stored ^ fresh) & ~mask) == 0) {
                ++entry.atBaseSlots;
                continue;
            }
            ++differing;
            if (entry.positions.size() < 6 || entry.positions.contains({static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(w)})) {
                auto& slot = entry.positions[{static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(w)}];
                ++slot.count;
                slot.stored = stored;
                slot.live = fresh;
                slot.address = begin + w * sizeof(std::uint32_t);
                const auto groupStart = w & ~std::size_t{7};
                slot.groupWords = std::min<std::size_t>(8, count - groupStart);
                for (std::size_t g = 0; g < slot.groupWords; ++g) slot.group[g] = variant.words[base + groupStart + g];
            }
        }
        base += count;
    }
    entry.wordsDiffering += differing;
    if (differing == 0) ++entry.equalNow;
    else ++entry.byCount[differing == 1 ? 0 : differing <= 4 ? 1 : 2];
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::vector<const std::pair<const std::uint64_t, Program>*> order;
    for (const auto& item : programs) order.push_back(&item);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->second.misses > b->second.misses; });
    std::string text;
    char item[256];
    for (std::size_t i = 0; i < order.size() && i < 8; ++i) {
        const auto& [code, data] = *order[i];
        std::snprintf(item, sizeof(item), " [%s 0x%llx: %llu misses, %llu words (1: %llu, 2-4: %llu, 5+: %llu), equal now %llu, at data positions %llu, at ignored bits %llu, at V# bases %llu, unmapped runs %llu, %zu runs/%zu words:", data.role == ShaderRecompiler::ProgramRole::Fragment ? "ps" : data.role == ShaderRecompiler::ProgramRole::Main ? "vs" : "other", static_cast<unsigned long long>(code), static_cast<unsigned long long>(data.misses), static_cast<unsigned long long>(data.wordsDiffering), static_cast<unsigned long long>(data.byCount[0]), static_cast<unsigned long long>(data.byCount[1]), static_cast<unsigned long long>(data.byCount[2]), static_cast<unsigned long long>(data.equalNow), static_cast<unsigned long long>(data.atDataPositions), static_cast<unsigned long long>(data.atIgnoredBits), static_cast<unsigned long long>(data.atBaseSlots), static_cast<unsigned long long>(data.unmapped), variant.runs.size(), variant.words.size());
        text += item;
        for (const auto& [where, position] : data.positions) {
            std::snprintf(item, sizeof(item), " r%u+%u@0x%llx %llux %08x->%08x grp", where.first, where.second, static_cast<unsigned long long>(position.address), static_cast<unsigned long long>(position.count), position.stored, position.live);
            text += item;
            for (std::size_t g = 0; g < position.groupWords; ++g) {
                std::snprintf(item, sizeof(item), " %08x", position.group[g]);
                text += item;
            }
        }
        text += "]";
    }
    std::fprintf(stderr, "[draw-miss] %llu differing-stage misses probed (10 s), %zu programs; top by misses (stage code: misses, differing words by count, first positions run+word@address count stored->live):%s\n", static_cast<unsigned long long>(total), programs.size(), text.c_str());
    programs.clear();
    total = 0;
}

}

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, DrawStageHits& hits, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, DrawRelocation* relocation) {
    using Role = ShaderRecompiler::ProgramRole;
    if (useDrawEntries) {
        if (!registerKey) {
            drawKey = 0xcbf29ce484222325ull;
            const auto mix = [&](std::uint64_t value) {
                drawKey ^= value;
                drawKey *= 0x100000001b3ull;
            };
            mix(localDevice->Serial());
            mix(static_cast<std::uint64_t>(graphics.stages.path));
            mix(graphics.stages.registerValue);
            mix(graphics.stages.vertexWaveSize);
            mix(graphics.stages.fragmentWaveSize);
            mix(graphics.stages.mesh.has_value());
            if (graphics.stages.mesh) {
                const auto& mesh = *graphics.stages.mesh;
                for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize}) mix(value);
            }
            mix(graphics.stages.tessellation.has_value());
            if (graphics.stages.tessellation) {
                const auto& tess = *graphics.stages.tessellation;
                for (const auto value : {tess.inputControlPoints, tess.outputControlPoints, tess.domain, tess.partitioning, tess.outputTopology}) mix(value);
            }
            mix(graphics.rectList);
            mix(programs.size());
            for (std::size_t i = 0; i < programs.size(); ++i) {
                const auto& program = programs[i];
                mix(reinterpret_cast<std::uintptr_t>(program.snapshot.get()));
                mix(program.codeOffset);
                mix(static_cast<std::uint64_t>(roles[i]));
                mix(static_cast<std::uint64_t>(program.binary.stage));
                mix(program.userDataBase);
                mix(program.firstUserSgpr);
                mix(program.userData.size());
                for (const auto word : program.userData) mix(word);
                mix(vertexInfos[i].has_value());
                if (!vertexInfos[i]) continue;
                const auto& vertex = *vertexInfos[i];
                require(vertex.resourcesNum <= vertex.resources.size(), "vertex stage info resource count exceeds its table");
                mix(vertex.resourcesNum);
                mix(vertex.fetchAttribReg);
                mix(vertex.fetchBufferReg);
                mix(vertex.fetchEmbedded);
                for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) {
                    for (const auto field : vertex.resources[r].fields) mix(field);
                    const auto& destination = vertex.resourcesDst[r];
                    mix(static_cast<std::uint32_t>(destination.registerStart));
                    mix(static_cast<std::uint32_t>(destination.registersNum));
                    mix(static_cast<std::uint32_t>(destination.attrId));
                    mix(destination.fetchIndex);
                }
            }
            require(pixel.interpolatorCount <= pixel.interpolatorSettings.size(), "pixel stage info interpolator count exceeds its table");
            mix(pixel.interpolatorCount);
            for (std::uint32_t i = 0; i < pixel.interpolatorCount; ++i) mix(pixel.interpolatorSettings[i]);
            mix(pixel.inputAddr);
            for (const bool flag : {pixel.wave32, pixel.hasPerspectiveCenterVgpr, pixel.perspectiveCentroid, pixel.posX, pixel.posY, pixel.posZ, pixel.posW, pixel.frontFace, pixel.ancillary, pixel.sampleShading, pixel.noPerspective, pixel.linearCentroid, pixel.pixelKillEnable, pixel.depthExportEnable, pixel.sampleMaskExportEnable, pixel.earlyZ, pixel.executeOnNoop}) mix(flag);
            for (const auto value : pixel.targetOutputMode) mix(value);
            for (const auto value : pixel.targetExportMapping) mix(value);
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.lookups;
            const auto found = drawCache.find(drawKey);
            if (found != drawCache.end()) {
                entry = found->second;
            } else {
                ++drawEntryCounters.absent;
                noteAbsentDrawKeyLocked(drawKey, 0);
            }
        }
        if (entry != nullptr) {
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            std::optional<DrawMiss> miss;
            std::vector<std::size_t> ranks(programs.size(), 0);
            // Per stage, the live words of the leaves a data hit refreshed (validateVariant).
            std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> liveData(programs.size());
            const bool dataAllowed = dataHits() && !verifyDrawEntries();
            std::uint64_t stageValidations = 0, stageEqual = 0, compared = 0, imagesFlushed = 0, runsSynced = 0;
            std::uint64_t relocatedStages = 0, relocatedInPlace = 0, relocationNoRule = 0, relocationDiffering = 0, relocationUnordered = 0;
            double compareUs = 0, patchUs = 0;
            std::uint64_t compareCalls = 0, patchedMade = 0, patchedReused = 0;
            if (entry->stages.size() != programs.size()) miss = DrawMiss::Stages;

            std::uint32_t cursor = 0;
            {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);

                std::optional<SampledReadScope> sampling;
                for (std::size_t i = 0; !miss && i < programs.size(); ++i) {
                    if (roles[i] == Role::GeometryBack) continue;
                    ++stageValidations;
                    const auto& variants = entry->stages[i];
                    // Under a relocation candidate, a stage whose pointer pair moved compares its
                    // variants shifted by the delta (DrawRelocation.cpp); one without a rule misses.
                    const auto delta = relocation != nullptr && i < relocation->deltas.size() ? relocation->deltas[i] : 0;
                    auto outcome = EntryOutcome::Differing;
                    bool anyLayout = false;
                    for (std::size_t rank = 0; rank < variants.size(); ++rank) {
                        const auto& stored = variants[rank];
                        if (stored->pushOffset != cursor) continue;
                        std::shared_ptr<DispatchVariant> shifted;
                        if (delta != 0) {
                            if (!stored->relocationLearned) {
                                ++relocationNoRule;
                                anyLayout = true;
                                continue;
                            }
                            // A rule that shifts nothing: the stage is independent of the pointer.
                            if (!stored->movedRuns.empty() || !stored->shiftSlots.empty() || !stored->pushShiftSlots.empty()) {
                                shifted = shiftVariant(*stored, delta, relocationUnordered);
                                if (shifted == nullptr) {
                                    anyLayout = true;
                                    continue;
                                }
                            }
                        }
                        const auto& variant = shifted != nullptr ? shifted : stored;
                        auto& regions = matchedRegions[i];
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        ++compared;
                        auto* live = dataAllowed ? &liveData[i] : nullptr;
                        const auto compareStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        const auto waitedAtCompare = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (profile) {
                            // Without the GPU waits inside (the phase books them as "validate GPU wait").
                            compareUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - compareStart).count() - (Graphics::Recorder::ThreadWaitedMs() - waitedAtCompare) * 1000.0;
                            ++compareCalls;
                        }
                        if (!anyLayout) outcome = result;
                        anyLayout = true;
                        if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) {
                            if (shifted != nullptr) {
                                ++relocationDiffering;
                                if (result == EntryOutcome::Differing) traceFailedRelocation(programs[i].binary.codeAddress, *shifted);
                            }
                            continue;
                        }
                        if (result != EntryOutcome::EqualData) liveData[i].clear();
                        matched[i] = variant;
                        ranks[i] = rank;
                        if (shifted != nullptr) {
                            relocation->relocated[i] = true;
                            ++relocatedStages;
                        } else if (relocation != nullptr) {
                            ++relocatedInPlace;
                        }
                        ++stageEqual;
                        cursor += static_cast<std::uint32_t>(variant->compiled->pushConstants.size());
                        break;
                    }
                    if (matched[i] != nullptr) continue;
                    if (!anyLayout) miss = DrawMiss::Layout;
                    else if (outcome != EntryOutcome::Differing) miss = DrawMiss::Gate;
                    else miss = roles[i] == Role::Fragment ? DrawMiss::FragmentDiffering : i == 0 ? DrawMiss::FrontDiffering : DrawMiss::OtherDiffering;
                    if (*miss != DrawMiss::Layout && *miss != DrawMiss::Gate && traceDrawMisses()) {
                        for (const auto& variant : variants) {
                            if (variant->pushOffset != cursor) continue;
                            probeDrawMiss(programs[i].binary.codeAddress, roles[i], *variant);
                            break;
                        }
                    }
                }
            }
            drawHit = !miss;
            // A miss keeps the stages validated before the differing one (matched) as a hit would,
            // so only the differing and later stages are captured again (Driver::draw binds a kept
            // stage's result like a hit's); off under the verify modes, which re-capture to compare.
            std::size_t stagesKept = 0;
            for (const auto& variant : matched) stagesKept += variant != nullptr ? 1 : 0;
            hits.partial = !drawHit && stagesKept != 0 && partialDrawHits() && !verifyDrawEntries() && !verifyDataHits();
            // The kept stages' results; a data hit's stages carry the live words and a patched copy.
            std::uint64_t dataStages = 0, dataWords = 0, baseStages = 0;
            if (drawHit || hits.partial) {
                const auto patchStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                hits.liveWords.assign(programs.size(), {});
                hits.results.assign(programs.size(), nullptr);
                hits.data = false;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (matched[i] == nullptr) continue;
                    auto& variant = *matched[i];
                    hits.results[i] = variant.compiled;
                    // A shifted stage binds a fresh variant object no recipe records (hits.data).
                    const bool shifted = relocation != nullptr && i < relocation->relocated.size() && relocation->relocated[i];
                    if (shifted) hits.data = true;
                    const bool flat = variant.flatBinding < variant.compiled->bindings.size();
                    // A shifted stage always carries its words and result (the verify mode compares
                    // the shifted descriptors against a fresh capture through them).
                    if (!shifted && (liveData[i].empty() || (!flat && variant.baseSlots.empty()))) continue;
                    auto& words = hits.liveWords[i];
                    words = variant.words;
                    for (const auto& [position, value] : liveData[i]) words[position] = value;
                    matchedRegions[i].clear();
                    appendEntryRegions(variant, matchedRegions[i], &words);
                    // The patched copy differs from `compiled` only at the flat binding's data slots
                    // and the buffer base slots, rewritten below on every hit, so one copy per variant
                    // serves every hit nobody else still holds (the previous draw's hits.results and
                    // results are cleared before this lookup; a holder elsewhere gets its own copy,
                    // which the variant then keeps instead).
                    std::shared_ptr<ShaderRecompiler::RecompileResult> patched;
                    if (shifted && variant.patched != nullptr && variant.patched.get() == variant.compiled.get()) {
                        // The shifted copy is the fresh variant's own (shiftVariant): patched in place.
                        patched = variant.patched;
                        ++patchedReused;
                    } else if (patchedResultReuse() && variant.patched != nullptr && variant.patched.use_count() == 1) {
                        patched = variant.patched;
                        ++patchedReused;
                    } else {
                        patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
                        if (patchedResultReuse()) variant.patched = patched;
                        ++patchedMade;
                    }
                    if (flat) {
                        auto& descriptor = patched->bindings[variant.flatBinding].guestDescriptor;
                        for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
                            if (variant.dataSlots[k] < descriptor.size()) descriptor[variant.dataSlots[k]] = words[variant.dataPositions[k]];
                        }
                    }
                    // The buffer base slots: each moved V# base into its binding (BufferBaseWords),
                    // the base bits only (the shader may have patched the rest of word 1).
                    for (const auto& slot : variant.baseSlots) {
                        auto& descriptor = patched->bindings[slot.binding].guestDescriptor;
                        if (slot.word < descriptor.size()) descriptor[slot.word] = (descriptor[slot.word] & ~slot.mask) | (words[slot.position] & slot.mask);
                    }
                    if (!variant.baseSlots.empty() && std::any_of(liveData[i].begin(), liveData[i].end(), [&](const std::pair<std::uint32_t, std::uint32_t>& item) { return PatchMaskAt(variant.baseSlots, item.first) != 0; })) ++baseStages;
                    hits.results[i] = std::move(patched);
                    hits.data = true;
                    ++dataStages;
                    dataWords += liveData[i].size();
                }
                if (profile) patchUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - patchStart).count();
            }
            std::lock_guard cacheLock(drawCacheMutex);
            auto& counters = drawEntryCounters;
            counters.stageValidations += stageValidations;
            counters.stageEqual += stageEqual;
            counters.variantsCompared += compared;
            counters.compareUs += compareUs;
            counters.compareCalls += compareCalls;
            counters.patchUs += patchUs;
            counters.patchedMade += patchedMade;
            counters.patchedReused += patchedReused;
            if (relocation != nullptr) {
                counters.relocatedStages += relocatedStages;
                counters.relocatedInPlace += relocatedInPlace;
                counters.relocationNoRule += relocationNoRule;
                counters.relocationDiffering += relocationDiffering;
                counters.relocationUnordered += relocationUnordered;
                if (drawHit) ++counters.relocatedHits;
                else if (hits.partial) ++counters.relocatedPartial;
            }
            if (hits.partial) {
                ++counters.partialHits;
                counters.partialStagesKept += stagesKept;
            }
            if (drawHit) {
                ++counters.hits;
                if (registerKey) ++counters.registerKeyHits;
                ++drawCacheHits;
                bool rotate = false;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (matched[i] == nullptr) continue;
                    ++counters.variantHitsByRank[ranks[i]];
                    if (ranks[i] != 0) rotate = true;
                }

                const auto again = drawCache.find(drawKey);
                if (again != drawCache.end() && again->second == entry) {
                    if (rotate) {
                        auto rotated = std::make_shared<DrawEntry>();
                        rotated->decode = entry->decode;
                        rotated->stages = entry->stages;
                        rotated->recipes.store(entry->recipes.load());
                        for (std::size_t i = 0; i < programs.size(); ++i) {
                            if (ranks[i] == 0) continue;
                            auto& variants = rotated->stages[i];
                            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(ranks[i]));
                            variants.insert(variants.begin(), matched[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }
                    if (drawCacheHits - again->second->touched > drawCacheEntries() / 8) {
                        drawOrder.splice(drawOrder.begin(), drawOrder, again->second->order);
                        again->second->touched = drawCacheHits;
                        ++counters.touches;
                    }
                } else if (relocation != nullptr && relocation->entry == entry) {
                    // A hit through the candidate: its entry moves to the new key.
                    const auto rekeyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    rekeyDrawEntryLocked(*relocation, matched, ranks);
                    if (profile) counters.relocationRekeyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - rekeyStart).count();
                }
                if (hits.data) {
                    ++counters.dataHits;
                    counters.dataStages += dataStages;
                    counters.baseStages += baseStages;
                    counters.dataWordsRefreshed += dataWords;
                    if (verifyDataHits()) {
                        // Captured again like a verified hit (cacheDrawStages compares the results).
                        verifyHit = true;
                        drawHit = false;
                    }
                }
                if (verifyDrawEntries()) {
                    ++counters.verifyHits;
                    verifyHit = true;
                    drawHit = false;
                }
            } else {
                ++counters.misses[static_cast<std::size_t>(*miss)];
                if (registerKey && entry->decode != nullptr) ++counters.decodePartial;
            }
            if (profile) {

                phaseTiming.Phase(DrawRowKeyLookupValidate);
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[DrawRowKeyLookupValidate]);
                phaseMs[DrawRowKeyLookupValidate] -= waited;
                phaseMs[DrawRowValidateWait] += waited;
                counters.validateUs += phaseMs[DrawRowKeyLookupValidate] * 1000;
                if (std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) {
                    counters.lastReport = std::chrono::steady_clock::now();
                    reportDrawCache(counters);
                }
            }
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
