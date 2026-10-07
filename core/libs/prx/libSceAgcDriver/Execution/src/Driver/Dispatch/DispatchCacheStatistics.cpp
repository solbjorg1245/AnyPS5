#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace AgcDriver::DriverDetail {

void Driver::classifyDiffering(std::uint64_t program, std::uint64_t key, const DispatchVariant& old, const DispatchVariant& fresh, const ShaderRecompiler::ResourceCapture* capture, EntryCounters& counters) {
    ++counters.differingClassified;
    ++counters.differingByProgram[program];
    auto& ring = priorValueSets[key];
    if (std::any_of(ring.begin(), ring.end(), [&](const ValueSet& set) { return set.first == fresh.runs && set.second == fresh.words; })) ++counters.differingMatchedPrior;
    ring.emplace_front(old.runs, old.words);
    while (ring.size() > 3) ring.pop_back();
    if (priorValueSets.size() > dispatchCacheEntries()) priorValueSets.clear();
    if (old.runs != fresh.runs || old.words.size() != fresh.words.size()) {
        // Debug aid APS5_TRACE_RUNS_CHANGED=1 or =<program hex> (with APS5_PROFILE_DRAW): the first such misses show
        // the read ranges of the stored variant and of the fresh capture (what the walk read
        // elsewhere), with the words of short ranges.
        static const char* traceRuns = std::getenv("APS5_TRACE_RUNS_CHANGED");
        static const std::uint64_t traceProgram = traceRuns != nullptr ? std::strtoull(traceRuns, nullptr, 16) : 0;
        static std::atomic<int> traced{0};
        if (traceRuns != nullptr && (traceProgram <= 1 || traceProgram == program) && traced.fetch_add(1, std::memory_order_relaxed) < 12) {
            const auto print = [](const char* which, const DispatchVariant& variant) {
                std::string text;
                char item[64];
                std::size_t offset = 0;
                for (const auto& [begin, end] : variant.runs) {
                    const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
                    std::snprintf(item, sizeof(item), " %llx+%llx", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
                    text += item;
                    for (std::size_t i = 0; count <= 4 && i < count && offset + i < variant.words.size(); ++i) {
                        std::snprintf(item, sizeof(item), "%c%08x", i == 0 ? '=' : ',', variant.words[offset + i]);
                        text += item;
                    }
                    offset += count;
                }
                std::fprintf(stderr, "[runs-changed]   %s %zu runs:%s\n", which, variant.runs.size(), text.c_str());
            };
            std::fprintf(stderr, "[runs-changed] program 0x%llx key 0x%llx:\n", static_cast<unsigned long long>(program), static_cast<unsigned long long>(key));
            print("stored", old);
            print("fresh ", fresh);
        }
        ++counters.differingRunsChanged;
        ++counters.differingWalk;
        return;
    }
    const auto baseWord = [&](const std::vector<ShaderRecompiler::DescriptorValue>& values, std::uint32_t value) {
        return std::any_of(values.begin(), values.end(), [&](const ShaderRecompiler::DescriptorValue& descriptor) { return descriptor.dwordCount >= 2 && (descriptor.dwords[0] == value || descriptor.dwords[1] == value); });
    };
    std::size_t words = 0, addressWords = 0, dataWords = 0;
    for (std::size_t i = 0; i < fresh.words.size(); ++i) {
        if (old.words[i] == fresh.words[i]) continue;
        ++words;
        counters.differingPositions.insert(i);
        counters.differingFirstPosition = std::min(counters.differingFirstPosition, i);
        counters.differingLastPosition = std::max(counters.differingLastPosition, i);
        const auto value = fresh.words[i];
        if (capture != nullptr && (baseWord(capture->snapshot.buffers, value) || baseWord(capture->snapshot.images, value))) ++addressWords;
        else if (capture != nullptr && std::find(capture->snapshot.flattenedSrt.begin(), capture->snapshot.flattenedSrt.end(), value) != capture->snapshot.flattenedSrt.end()) ++dataWords;
    }
    counters.differingWords += words;
    ++counters.differingWordBuckets[words <= 1 ? 0 : words <= 4 ? 1 : words <= 16 ? 2 : 3];
    if (words == 0) ++counters.differingWalk;
    else if (addressWords == words) ++counters.differingAddress;
    else if (dataWords == words) ++counters.differingData;
    else if (addressWords + dataWords == 0) ++counters.differingWalk;
    else ++counters.differingMixed;
}

void Driver::noteDispatchKey(std::uint32_t queue, std::uint64_t program, std::uint64_t key, int outcome, const std::vector<std::uint32_t>& userData, const std::array<std::uint32_t, 5>& registers, const void* shader) {
    std::lock_guard cacheLock(dispatchCacheMutex);
    auto& counters = entryCounters;
    auto& perQueue = counters.queueKeys[queue];
    auto& perProgram = counters.programKeys[(static_cast<std::uint64_t>(queue) << 48u) ^ program];
    perProgram.queue = queue;
    ++perQueue.lookups;
    ++perProgram.lookups;
    auto& last = lastKeyInputs[(static_cast<std::uint64_t>(queue) << 48u) ^ program];
    if (outcome == 0) ++perQueue.hits;
    else if (outcome == 2) ++perQueue.missed;
    else {
        ++perQueue.absent;
        ++perProgram.absent;
        if (last.keys.empty()) {
            ++perQueue.firstSeen;
        } else if (std::find(last.keys.begin(), last.keys.end(), key) != last.keys.end()) {
            ++perQueue.seenBefore;
            ++perProgram.seenBefore;
        } else {
            bool user = false;
            std::string sample;
            for (std::size_t i = 0; i < userData.size(); ++i) {
                if (i < last.userData.size() && last.userData[i] == userData[i]) continue;
                user = true;
                ++perProgram.userPositions[static_cast<std::uint32_t>(i)];
                if (sample.size() < 160) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " u%zu %x->%x", i, i < last.userData.size() ? last.userData[i] : 0u, userData[i]);
                    sample += text;
                }
            }
            if (last.userData.size() != userData.size()) user = true;
            if (user) {
                ++perQueue.userChanged;
                perProgram.sample = std::move(sample);
            }
            if (last.registers != registers) {
                ++perQueue.registersChanged;
                ++perProgram.registersChanged;
            }
            if (last.shader != shader) {
                ++perQueue.shaderChanged;
                ++perProgram.shaderChanged;
            }
            if (!user && last.registers == registers && last.shader == shader) ++perQueue.sameInputs;
        }
    }
    last.userData = userData;
    last.registers = registers;
    last.shader = shader;
    if (std::find(last.keys.begin(), last.keys.end(), key) == last.keys.end()) {
        last.keys.push_front(key);
        if (last.keys.size() > 64) last.keys.pop_back();
    }
    if (lastKeyInputs.size() > 65536) lastKeyInputs.clear();
}

void Driver::reportDispatchCache(EntryCounters& counters) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups - counters.absent;
    std::fprintf(stderr, "[dispatch-cache] %llu lookups (10 s): %llu no entry, %llu validated by value in %.1f us each: %llu equal, %llu differing, %llu page not mapped, %llu queued label, %llu image being stored, %llu publish generation moved, %llu pending serial moved, %llu forget serial moved (%llu pending images stored first, %llu pending runs synced first, %llu equal entries inserted before a forget; gate retried %llu: %llu equal, %llu moved again); runs per entry %.1f validated / %.1f inserted; %llu misses found the entry replaced meanwhile; %llu inserts (%llu captures not kept: unstable), %llu evictions in total, %zu entries, %llu LRU moves\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.equal), count(counters.differing), count(counters.inaccessible), count(counters.queuedLabel), count(counters.flushingImage), count(counters.publishMoved), count(counters.pendingMoved), count(counters.forgetMoved), count(counters.imagesFlushed), count(counters.runsSynced), count(counters.forgetSinceInsert), count(counters.retriesEqual + counters.retriesMoved), count(counters.retriesEqual), count(counters.retriesMoved), validated != 0 ? static_cast<double>(counters.runsValidated) / static_cast<double>(validated) : 0.0, counters.inserts != 0 ? static_cast<double>(counters.runsInserted) / static_cast<double>(counters.inserts) : 0.0, count(counters.replaced), count(counters.inserts), count(counters.unstable), count(dispatchCacheEvictions), dispatchCache.size(), count(counters.touches));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> programs(counters.differingByProgram.begin(), counters.differingByProgram.end());
    std::sort(programs.begin(), programs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top;
    for (std::size_t i = 0; i < programs.size() && i < 8; ++i) {
        char text[48];
        std::snprintf(text, sizeof(text), " 0x%llx x%llu", static_cast<unsigned long long>(programs[i].first), count(programs[i].second));
        top += text;
    }
    std::fprintf(stderr, "[dispatch-cache] differing by class (10 s, %llu classified): address-only %llu, data-only %llu, walk %llu, mixed %llu (runs changed %llu); matched one of the last 3 value sets %llu; differing words %llu in total (misses with 1 / 2-4 / 5-16 / >16 words: %llu / %llu / %llu / %llu), %zu distinct positions (first %zu, last %zu); top programs by differing:%s\n", count(counters.differingClassified), count(counters.differingAddress), count(counters.differingData), count(counters.differingWalk), count(counters.differingMixed), count(counters.differingRunsChanged), count(counters.differingMatchedPrior), count(counters.differingWords), count(counters.differingWordBuckets[0]), count(counters.differingWordBuckets[1]), count(counters.differingWordBuckets[2]), count(counters.differingWordBuckets[3]), counters.differingPositions.size(), counters.differingPositions.empty() ? std::size_t{0} : counters.differingFirstPosition, counters.differingLastPosition, top.c_str());
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += text;
    }
    std::fprintf(stderr, "[dispatch-cache] variants (10 s, k = %zu): hits by rank 1..k %s; %.2f variants compared per validation; %llu inserted into an entry, %llu evicted beyond k; %.2f variants per entry (%llu over %zu entries, ~%.1f MiB)\n", dispatchVariants(), ranks.c_str(), validated != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(validated) : 0.0, count(counters.variantsInserted), count(counters.variantsEvicted), dispatchCache.empty() ? 0.0 : static_cast<double>(dispatchCacheVariants) / static_cast<double>(dispatchCache.size()), count(dispatchCacheVariants), dispatchCache.size(), static_cast<double>(dispatchCacheVariantBytes) / (1024.0 * 1024.0));
    std::string dataRanks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.dataHitsByRank[rank]));
        dataRanks += text;
    }
    // Taken before the call: the arguments' evaluation order is unspecified.
    const auto pendingKnownRefreshed = dataPendingKnownRefreshed.exchange(0, std::memory_order_relaxed);
    const auto pendingSyncedRefreshed = dataPendingSyncedRefreshed.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[dispatch-cache] data hits (10 s): %llu (%llu words refreshed; by rank 1..k %s; %llu data variants missed on pending runs, %llu pending runs refreshed through the mask: known bytes %llu, synced %llu), verified %llu; inserts with data positions %llu of %llu (%.1f positions each), leaves skipped: unmapped %llu, mismatched %llu, aliased %llu\n", count(counters.dataHits), count(counters.dataWordsRefreshed), dataRanks.c_str(), count(dataPendingMisses.exchange(0, std::memory_order_relaxed)), count(pendingKnownRefreshed + pendingSyncedRefreshed), count(pendingKnownRefreshed), count(pendingSyncedRefreshed), count(counters.dataVerified), count(counters.dataInserts), count(counters.inserts), counters.dataInserts != 0 ? static_cast<double>(counters.dataPositionsInserted) / static_cast<double>(counters.dataInserts) : 0.0, count(counters.dataLeavesUnmapped), count(counters.dataLeavesMismatched), count(counters.dataLeavesAliased));
    const auto& verdicts = counters.relocationVerdicts;
    std::fprintf(stderr, "[dispatch-cache] relocated hits (10 s): %llu; rules learned %llu, refused: shape %llu, data positions %llu, deltas %llu, nothing moved %llu, other words %llu, no pointer %llu, compiled %llu, descriptors %llu; relocations differing %llu, unordered %llu; relocation first: %llu tried, %llu differing, %llu stored validations skipped\n", count(counters.relocatedHits), count(verdicts[0]), count(verdicts[1]), count(verdicts[2]), count(verdicts[3]), count(verdicts[4]), count(verdicts[5]), count(verdicts[6]), count(verdicts[7]), count(verdicts[8]), count(counters.relocatedDiffering), count(counters.relocatedUnordered), count(counters.relocatedFirst), count(counters.relocatedFirstDiffering), count(counters.storedValidationsSkipped));
    {
        const auto& v = counters.userPointerVerdicts;
        std::fprintf(stderr, "[dispatch-cache] user-pointer relocation (10 s): %llu absent lookups with %llu candidates (%llu by signature first, %llu signatures unmatched, %llu at the stride first; %llu without a rule), %llu validated shifted: %llu hits (%llu by signature, %llu at the stride; %llu copies inserted), %llu differing, %llu unordered; misses: same instance found %llu, not found %llu; rules against a candidate: learned %llu, refused: shape %llu, data positions %llu, deltas %llu, nothing moved %llu, other words %llu, no pointer %llu, compiled %llu, descriptors %llu\n", count(counters.userPointerLookups), count(counters.userPointerCandidates), count(counters.userPointerSignatureFirst), count(counters.userPointerSignatureMissing), count(counters.userPointerStrideFirst), count(counters.userPointerNoRule), count(counters.userPointerValidated), count(counters.userPointerHits), count(counters.userPointerSignatureHits), count(counters.userPointerStrideHits), count(counters.userPointerCopies), count(counters.userPointerDiffering), count(counters.userPointerUnordered), count(counters.userPointerSameFound), count(counters.userPointerSameMissing), count(v[0]), count(v[1]), count(v[2]), count(v[3]), count(v[4]), count(v[5]), count(v[6]), count(v[7]), count(v[8]));
    }
    {
        std::string text;
        for (const auto& [queue, q] : counters.queueKeys) {
            char item[512];
            const auto& v = q.verdicts;
            std::snprintf(item, sizeof(item), "; q0x%x: %llu lookups, %llu hits, %llu missed an entry (rules learned %llu, refused: shape %llu, data positions %llu, deltas %llu, nothing moved %llu, other words %llu, no pointer %llu, compiled %llu, descriptors %llu), %llu no entry (first seen %llu, a recent key %llu, user data changed %llu, registers %llu, shader %llu, same inputs %llu)", queue, count(q.lookups), count(q.hits), count(q.missed), count(v[0]), count(v[1]), count(v[2]), count(v[3]), count(v[4]), count(v[5]), count(v[6]), count(v[7]), count(v[8]), count(q.absent), count(q.firstSeen), count(q.seenBefore), count(q.userChanged), count(q.registersChanged), count(q.shaderChanged), count(q.sameInputs));
            text += item;
        }
        std::fprintf(stderr, "[dispatch-keys] by queue (10 s)%s\n", text.c_str());
        std::vector<std::pair<std::uint64_t, const ProgramKeyCounters*>> absent;
        for (const auto& [id, p] : counters.programKeys) {
            if (p.absent != 0) absent.emplace_back(id, &p);
        }
        std::sort(absent.begin(), absent.end(), [](const auto& a, const auto& b) { return a.second->absent > b.second->absent; });
        std::string top;
        for (std::size_t i = 0; i < absent.size() && i < 12; ++i) {
            const auto& p = *absent[i].second;
            std::vector<std::pair<std::uint32_t, std::uint64_t>> positions(p.userPositions.begin(), p.userPositions.end());
            std::sort(positions.begin(), positions.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            char item[256];
            std::snprintf(item, sizeof(item), "; q0x%x 0x%llx: %llu of %llu lookups absent (recent key %llu, registers %llu, shader %llu; user words", p.queue, static_cast<unsigned long long>(absent[i].first & 0xffffffffffffull), count(p.absent), count(p.lookups), count(p.seenBefore), count(p.registersChanged), count(p.shaderChanged));
            top += item;
            for (std::size_t k = 0; k < positions.size() && k < 4; ++k) {
                std::snprintf(item, sizeof(item), " u%u x%llu", positions[k].first, count(positions[k].second));
                top += item;
            }
            top += "; sample" + p.sample + ")";
        }
        std::fprintf(stderr, "[dispatch-keys] top programs by no entry%s\n", top.c_str());
    }
    counters = EntryCounters{};
}

}
