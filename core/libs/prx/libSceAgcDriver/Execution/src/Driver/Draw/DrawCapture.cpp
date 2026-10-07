#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <string>

namespace AgcDriver::DriverDetail {

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::compileDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs) {
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushConstantBytes) - pushOffset},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    if (profile) {
        // APS5_PROFILE_DRAW: how many stage captures repeat a (program, user data) pair seen before,
        // the bound on what a capture memo keyed by them could serve ([capture-repeat], every 10 s).
        static std::mutex repeatMutex;
        static std::unordered_map<std::uint64_t, std::uint64_t> seen;
        static std::uint64_t repeats = 0, total = 0, frameRepeats = 0;
        static auto lastReport = std::chrono::steady_clock::now();
        std::uint64_t hash = 14695981039346656037ull;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        mix(program.binary.codeAddress);
        for (const auto word : program.userData) mix(word);
        std::lock_guard lock(repeatMutex);
        ++total;
        const auto now = std::chrono::steady_clock::now();
        const auto stamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        if (const auto it = seen.find(hash); it != seen.end()) {
            ++repeats;
            if (stamp - it->second < 2000) ++frameRepeats;
            it->second = stamp;
        } else {
            if (seen.size() > (1u << 20u)) seen.clear();
            seen.emplace(hash, stamp);
        }
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            std::fprintf(stderr, "[capture-repeat] %llu stage captures (10 s): %llu repeat a (program, user data) pair seen before, %llu of them within 2 s; %zu pairs known\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(repeats), static_cast<unsigned long long>(frameRepeats), seen.size());
            repeats = total = frameRepeats = 0;
        }
    }
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const auto handle = SourceHandleFor(*program.snapshot, program.codeOffset, localDevice->Serial(), request, false);
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        return shaderMemory.Capture(request, handle.get());
    }();

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    stageCapture.capture = capture;
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    if (drawHit) {
        for (std::size_t j = 0; j < programs.size(); ++j) {
            if (matched[j] != nullptr && !recompiled[j]) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
        }
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    // APS5_DUMP_DRAW_PROGRAM=<hex code address>: the request of each capture of that stage program
    // (shader_<address>.req for agc_shader_replay).
    static const std::uint64_t dumpProgram = [] { const char* text = std::getenv("APS5_DUMP_DRAW_PROGRAM"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();
    if (dumpProgram != 0 && program.binary.codeAddress == dumpProgram) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    static const bool reuseCapture = std::getenv("APS5_NO_CAPTURE_REUSE") == nullptr;
    phaseTiming.Phase(DrawRowCapture);

    stageCapture.compiled = reuseCapture ? ShaderRecompiler::Recompile(request, *capture) : std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
    phaseTiming.Phase(DrawRowRecompile);
    return stageCapture.compiled;
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const DrawStageHits& hits, std::vector<std::shared_ptr<DispatchVariant>>& fresh, const DrawKey& drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, DrawRelocation* relocation) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu)) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (stageCapture.compiled == nullptr) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            for (const auto& region : regions) {
                variant->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
                const auto count = region.bytes.size() / sizeof(std::uint32_t);
                const auto offset = variant->words.size();
                variant->words.resize(offset + count);
                std::memcpy(variant->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
            }
            // The sorted addresses of every read the walk made besides the pure leaves, plus the
            // vertex input decode's (otherReads), for the data positions and the buffer base slots.
            const bool traced = dataHits() && !stampValidate() && stageCapture.capture != nullptr;
            std::vector<std::uint64_t> otherReads;
            if (traced) {
                const auto& trace = stageCapture.capture->readTrace;
                otherReads.assign(trace.otherReads.begin(), trace.otherReads.end());
                for (const auto& read : decodeReads[i]) {
                    for (auto address = read.address & ~std::uint64_t{3}; address < read.address + read.bytes.size(); address += sizeof(std::uint32_t)) otherReads.push_back(address);
                }
                if (!decodeReads[i].empty()) std::sort(otherReads.begin(), otherReads.end());
            }
            // Data positions (DrawStageHits): the pure flat-SRT leaves among the words. The vertex
            // input decode's reads count as other reads: a hit reuses the stage info they shaped.
            if (traced && !stageCapture.capture->readTrace.leaves.empty()) {
                const auto& bindings = variant->compiled->bindings;
                for (std::size_t b = 0; b < bindings.size(); ++b) {
                    if (bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
                    variant->flatBinding = static_cast<std::uint32_t>(b);
                    static_cast<void>(DataWordPositions(variant->runs, stageCapture.capture->readTrace.leaves, otherReads, variant->words, bindings[b].guestDescriptor, variant->dataPositions, variant->dataSlots));
                    if (!variant->dataPositions.empty()) {
                        std::lock_guard cacheLock(drawCacheMutex);
                        ++drawEntryCounters.dataInserts;
                        drawEntryCounters.dataPositionsInserted += variant->dataPositions.size();
                    }
                    break;
                }
            }
            // Buffer base slots: the read-only guest-buffer V#s the walk read among the words (a V#
            // the vertex input decode read shaped the stage info: it compares exactly).
            if (traced && vsharpBases()) {
                const auto& trace = stageCapture.capture->readTrace;
                std::vector<std::uint64_t> walkReads(trace.otherReads.begin(), trace.otherReads.end());
                if (!decodeReads[i].empty()) {
                    walkReads.erase(std::remove_if(walkReads.begin(), walkReads.end(), [&](std::uint64_t address) {
                        return std::any_of(decodeReads[i].begin(), decodeReads[i].end(), [&](const Graphics::DecodeRead& read) { return address >= (read.address & ~std::uint64_t{3}) && address < read.address + read.bytes.size(); });
                    }), walkReads.end());
                }
                const auto counts = BufferBaseWords(variant->runs, variant->words, variant->compiled->bindings, variant->dataPositions, walkReads, variant->baseSlots);
                if (counts.located + counts.written + counts.unlocated + counts.unread + counts.ambiguous + counts.data != 0) {
                    std::lock_guard cacheLock(drawCacheMutex);
                    auto& counters = drawEntryCounters;
                    if (!variant->baseSlots.empty()) {
                        ++counters.baseInserts;
                        counters.baseSlotsInserted += variant->baseSlots.size();
                    }
                    counters.baseWritten += counts.written;
                    counters.baseUnlocated += counts.unlocated;
                    counters.baseUnread += counts.unread;
                    counters.baseAmbiguous += counts.ambiguous;
                    counters.baseData += counts.data;
                }
            }
            // Don't-care bits: the sampled-image T#s' streaming-feedback fields among the words.
            if (tsharpMask() && !stampValidate()) {
                static_cast<void>(IgnoredWordBits(variant->runs, variant->words, variant->compiled->bindings, variant->dataPositions, variant->ignoredBits));
                if (!variant->ignoredBits.empty()) {
                    std::lock_guard cacheLock(drawCacheMutex);
                    ++drawEntryCounters.ignoredInserts;
                    drawEntryCounters.ignoredPositionsInserted += variant->ignoredBits.size();
                }
            }
            // A data hit's stage holds the live words and a patched result: both must be what the
            // capture made (APS5_VERIFY_DATA_HITS); the compares look through the don't-care bits.
            const bool dataStage = verifyHit && hits.data && matched[i] != nullptr && i < hits.liveWords.size() && !hits.liveWords[i].empty();
            if (dataStage) {
                const auto& patched = *hits.results[i];
                const auto& captured = *stageCapture.compiled;
                // The result is what the draw binds, so a different one aborts; runs or words that
                // differ with the same result are reported and counted as mismatches. A flat word
                // either capture left to the GPU (DescriptorBinding::deferredWords: a placeholder
                // in its descriptor, copied from guest memory per draw) compares as equal: a
                // variant captured while GPU work still wrote the leaf's page did not read that
                // word, so a later capture reads one word more (vertex program 0x249376600, 2 runs
                // / 16 words against 3 / 17, PROGRESS t264) and holds its value instead. So does a
                // flat copy of a sampled T# word the stage compare accepted through the don't-care
                // mask (DispatchVariant::ignoredBits): the hit binds the stored word there.
                const auto deferredAt = [](const ShaderRecompiler::DescriptorBinding& binding, std::size_t word) {
                    return std::any_of(binding.deferredWords.begin(), binding.deferredWords.end(), [&](const std::pair<std::uint32_t, std::uint64_t>& item) { return item.first == word; });
                };
                const auto tsharpCopy = [&](std::uint32_t capturedWord, std::uint32_t patchedWord) {
                    const auto& stored = matched[i]->words;
                    for (const auto& [position, mask] : matched[i]->ignoredBits) {
                        if (position < stored.size() && stored[position] == patchedWord && ((capturedWord ^ patchedWord) & ~mask) == 0) return true;
                    }
                    return false;
                };
                std::string difference;
                std::size_t badBinding = captured.bindings.size(), badWord = 0;
                bool same = captured.variantId == patched.variantId && captured.pushConstants == patched.pushConstants && captured.bindings.size() == patched.bindings.size();
                if (!same) difference = " variant, push constants or binding count";
                for (std::size_t b = 0; same && b < captured.bindings.size(); ++b) {
                    const auto& left = captured.bindings[b];
                    const auto& right = patched.bindings[b];
                    same = left.kind == right.kind && left.role == right.role && left.binding == right.binding && left.count == right.count && left.guestDescriptor.size() == right.guestDescriptor.size();
                    if (same && left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt) {
                        for (std::size_t w = 0; same && w < left.guestDescriptor.size(); ++w) same = left.guestDescriptor[w] == right.guestDescriptor[w] || deferredAt(left, w) || deferredAt(right, w) || tsharpCopy(left.guestDescriptor[w], right.guestDescriptor[w]);
                    } else if (same) {
                        same = SameDescriptorIgnoringTsharpBits(left, left.guestDescriptor, right.guestDescriptor);
                    }
                    if (!same) {
                        std::size_t w = 0;
                        while (w < left.guestDescriptor.size() && w < right.guestDescriptor.size() && left.guestDescriptor[w] == right.guestDescriptor[w]) ++w;
                        char text[192];
                        std::snprintf(text, sizeof(text), " binding %zu (role %d kind %d, %zu / %zu words, %zu / %zu deferred) word %zu: captured %08x, patched %08x", b, static_cast<int>(left.role), static_cast<int>(left.kind), left.guestDescriptor.size(), right.guestDescriptor.size(), left.deferredWords.size(), right.deferredWords.size(), w, w < left.guestDescriptor.size() ? left.guestDescriptor[w] : 0u, w < right.guestDescriptor.size() ? right.guestDescriptor[w] : 0u);
                        difference = text;
                        badBinding = b;
                        badWord = w;
                    }
                }
                // Where a differing flat word comes from, for the report: the stored variant's data
                // slot for it, the fresh capture's leaf address for it and that address among the
                // stored runs (position, stored and live words).
                if (!same && badBinding < captured.bindings.size() && captured.bindings[badBinding].role == ShaderRecompiler::DescriptorRole::FlattenedSrt) {
                    const auto& stored = *matched[i];
                    char text[160];
                    for (std::size_t k = 0; k < stored.dataSlots.size(); ++k) {
                        if (stored.dataSlots[k] != badWord) continue;
                        const auto p = stored.dataPositions[k];
                        std::snprintf(text, sizeof(text), "; stored data slot at position %u (stored %08x, live %08x)", p, p < stored.words.size() ? stored.words[p] : 0u, p < hits.liveWords[i].size() ? hits.liveWords[i][p] : 0u);
                        difference += text;
                    }
                    for (std::size_t k = 0; k < variant->dataSlots.size(); ++k) {
                        if (variant->dataSlots[k] != badWord) continue;
                        std::snprintf(text, sizeof(text), "; fresh data slot at position %u", variant->dataPositions[k]);
                        difference += text;
                    }
                    if (stageCapture.capture != nullptr) {
                        for (const auto& [slot, address] : stageCapture.capture->readTrace.leaves) {
                            if (slot != badWord) continue;
                            std::size_t base = 0;
                            bool located = false;
                            for (const auto& [begin, end] : stored.runs) {
                                if (address >= begin && address + sizeof(std::uint32_t) <= end) {
                                    const auto p = base + static_cast<std::size_t>((address - begin) / sizeof(std::uint32_t));
                                    std::snprintf(text, sizeof(text), "; fresh leaf @%llx = stored position %zu (stored %08x, live %08x)", static_cast<unsigned long long>(address), p, p < stored.words.size() ? stored.words[p] : 0u, p < hits.liveWords[i].size() ? hits.liveWords[i][p] : 0u);
                                    difference += text;
                                    located = true;
                                }
                                base += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
                            }
                            if (!located) {
                                std::snprintf(text, sizeof(text), "; fresh leaf @%llx outside the stored runs", static_cast<unsigned long long>(address));
                                difference += text;
                            }
                        }
                        std::snprintf(text, sizeof(text), "; %zu fresh leaves, %zu other reads", stageCapture.capture->readTrace.leaves.size(), stageCapture.capture->readTrace.otherReads.size());
                        difference += text;
                    }
                }
                if (!same) {
                    // The words the compare saw against memory now: a word the title wrote between
                    // the compare and this capture (t270: a flat float of vertex program 0x248a85700
                    // at the save load) explains the disagreement without a cache fault; reported
                    // and counted, since the draw's GPU reads see the same write in place.
                    std::vector<ShaderRecompiler::MemoryRegion> seen;
                    appendEntryRegions(*matched[i], seen, &hits.liveWords[i]);
                    bool changed = false;
                    for (const auto& region : seen) changed = changed || GuestMemory::CompareMapped(region.guestAddress, region.bytes) != GuestMemory::Compare::Equal;
                    if (!changed) {
                        std::fprintf(stderr, "[draw-cache] APS5_VERIFY_DATA_HITS: stage %zu (program 0x%llx) of a data hit disagrees with its capture:%s (%zu runs / %zu words matched, %zu / %zu fresh)\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), difference.c_str(), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
                        std::fflush(stderr);
                        std::abort();
                    }
                    ++mismatches;
                    static std::atomic<std::uint64_t> raceReports{0};
                    if (raceReports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] APS5_VERIFY_DATA_HITS: stage %zu (program 0x%llx) of a data hit disagrees with its capture:%s, and its memory changed since the compare (a guest write under the draw)\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), difference.c_str());
                }
                if (matched[i]->runs != variant->runs || !WordsEqualIgnoring(hits.liveWords[i], variant->words, matched[i]->ignoredBits)) {
                    ++mismatches;
                    static std::atomic<std::uint64_t> shapeReports{0};
                    if (shapeReports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] APS5_VERIFY_DATA_HITS: stage %zu (program 0x%llx) of a data hit captured differently with the same result: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
                }
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.dataVerified;
            }
            if (verifyHit && !dataStage && matched[i] != nullptr && (matched[i]->runs != variant->runs || !WordsEqualIgnoring(matched[i]->words, variant->words, matched[i]->ignoredBits))) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        // A miss under a relocation candidate teaches the fresh variant of each shifted stage the
        // rule against the candidate's variant at its push offset (DrawRelocation.cpp).
        // The candidates in turn: the first whose variant at a shifted stage's push offset
        // teaches the fresh variant the rule (a learned rule means the same runs, words and
        // compiled result, moved by the delta: the same object) is the entry the new key takes
        // over; none learning leaves the candidates alone and inserts a fresh entry.
        if (relocation != nullptr && !stampValidate()) {
            std::uint64_t attempts = 0, learned = 0;
            std::array<std::uint64_t, static_cast<std::size_t>(RelocationVerdict::Count)> verdicts{};
            const auto validated = relocation->entry;
            relocation->entry = nullptr;
            for (const auto& candidate : relocation->candidates) {
                std::uint64_t learnedHere = 0;
                for (std::size_t i = 0; i < fresh.size() && i < candidate.deltas.size() && i < candidate.entry->stages.size(); ++i) {
                    if (fresh[i] == nullptr || candidate.deltas[i] == 0) continue;
                    for (const auto& old : candidate.entry->stages[i]) {
                        if (old->pushOffset != fresh[i]->pushOffset) continue;
                        ++attempts;
                        const auto verdict = learnRelocation(*old, *fresh[i], candidate.deltas[i]);
                        ++verdicts[static_cast<std::size_t>(verdict)];
                        if (verdict == RelocationVerdict::Learned) ++learnedHere;
                        break;
                    }
                }
                if (learnedHere == 0) continue;
                learned += learnedHere;
                relocation->entry = candidate.entry;
                relocation->key = candidate.key;
                relocation->deltas = candidate.deltas;
                break;
            }
            // The stages kept through a shifted variant validated under another candidate are
            // still valid variants at the new address (they compared against live memory).
            if (relocation->entry != validated && relocation->relocated.size() != fresh.size()) relocation->relocated.assign(fresh.size(), false);
            if (attempts != 0 || relocation->entry != nullptr) {
                std::lock_guard cacheLock(drawCacheMutex);
                drawEntryCounters.relocationLearnAttempts += attempts;
                drawEntryCounters.relocationLearned += learned;
                if (relocation->entry != nullptr) ++drawEntryCounters.relocationLearnedFrom;
                for (std::size_t v = 0; v < verdicts.size(); ++v) drawEntryCounters.relocationVerdicts[v] += verdicts[v];
            }
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr, relocation, &matched);
        if (unstable != 0 || mismatches != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
