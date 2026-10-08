#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

// The validate path's copy counters (APS5_PROFILE_DRAW, ValidateCopyCounters).
double ElapsedUs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - since).count();
}

void NoteResultCopy(ValidateCopyCounters& counters, ResultCopySite site, const ShaderRecompiler::RecompileResult& copy) {
    const auto cost = ResultCopyCost(copy);
    ++counters.resultCopies[site];
    counters.resultBytes[site] += cost.bytes;
    counters.resultAllocations[site] += cost.allocations;
}

// The vectors a shifted variant copied from its source (shiftVariant: its words and runs, the data
// positions and slots, don't-care bits, base slots and, when the page layout kept the runs, the
// rule), one allocation each when not empty, and the variant itself; relocateVariant's run starts
// and shiftVariant's run copy before the layout are left out.
CopyCost ShiftedVariantCost(const DispatchVariant& variant) {
    CopyCost cost{0, 1};
    const auto add = [&](const auto& items) {
        cost.bytes += items.size() * sizeof(*items.data());
        if (!items.empty()) ++cost.allocations;
    };
    add(variant.words);
    add(variant.runs);
    add(variant.dataPositions);
    add(variant.dataSlots);
    add(variant.ignoredBits);
    add(variant.baseSlots);
    add(variant.pointerPositions);
    add(variant.movedRuns);
    add(variant.shiftSlots);
    add(variant.pushShiftSlots);
    add(variant.innerPointers);
    return cost;
}

}

void Driver::lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering, std::shared_ptr<DispatchVariant>& relocated, std::uint64_t baseKey, std::span<const std::uint32_t> userData, std::vector<UserPointerCandidate>& baseCandidates) {
    if (!noDispatchCache) {

        static const bool validateUnlocked = std::getenv("APS5_NO_UNLOCKED_VALIDATE") == nullptr;
        std::unique_lock cacheLock(dispatchCacheMutex);
        ++entryCounters.lookups;
        const auto found = dispatchCache.find(key);
        std::shared_ptr<DispatchEntry> entry = found != dispatchCache.end() ? found->second : nullptr;
        if (entry == nullptr) ++entryCounters.absent;
        if (entry == nullptr && userPointerRelocation()) collectUserPointerCandidates(baseKey, userData, baseCandidates);
        if (validateUnlocked || !baseCandidates.empty()) cacheLock.unlock();
        phaseTiming.Phase(PhaseLookup);
        if (entry != nullptr) {
            validated = true;
            const auto& variants = entry->variants;

            std::shared_ptr<DispatchVariant> variant;
            std::size_t rank = 0;
            const auto generation = variants.front()->generation.load(std::memory_order_acquire);
            bool current = false;
            std::uint64_t restamped = 0;

            auto outcome = EntryOutcome::Differing;
            std::uint64_t imagesFlushed = 0, runsSynced = 0, retriesEqual = 0, retriesMoved = 0, compared = 0;
            std::vector<ShaderRecompiler::MemoryRegion> regions;

            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            std::uint64_t relocatedUnordered = 0, relocatedDiffering = 0;
            std::uint64_t relocatedFirst = 0, relocatedFirstDiffering = 0, storedSkipped = 0;
            // The validate path's copies and sub-steps ([dispatch-cache] validate copies), added to
            // the counters with the rest under the lock.
            const bool countCopies = variantCopyCount();
            ValidateCopyCounters copies;
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            if (!stampValidate()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);

                std::optional<SampledReadScope> sampling;

                // relocateVariant, counted with the copies: the shift copied the RecompileResult when
                // the candidate holds its own copy (shiftVariant's `patched`); an unordered shift
                // copied the words and runs before it gave up.
                const auto relocate = [&](const DispatchVariant& source) {
                    if (!countCopies) return relocateVariant(source, relocatedUnordered);
                    const auto unorderedBefore = relocatedUnordered;
                    const auto started = std::chrono::steady_clock::now();
                    auto candidate = relocateVariant(source, relocatedUnordered);
                    const auto us = ElapsedUs(started);
                    auto kind = RelocateNone;
                    if (candidate != nullptr) {
                        kind = candidate->patched != nullptr ? RelocateWithCopy : RelocateWithoutCopy;
                        ++copies.shiftCalls;
                        const auto cost = ShiftedVariantCost(*candidate);
                        copies.shiftedBytes += cost.bytes;
                        copies.shiftedAllocations += cost.allocations;
                        if (candidate->patched != nullptr) NoteResultCopy(copies, CopyShift, *candidate->patched);
                    } else if (relocatedUnordered != unorderedBefore) {
                        ++copies.shiftCalls;
                        ++copies.unordered;
                        copies.shiftedBytes += source.words.size() * sizeof(std::uint32_t) + source.runs.size() * sizeof(std::pair<std::uint64_t, std::uint64_t>);
                        copies.shiftedAllocations += (source.words.empty() ? 0 : 1) + (source.runs.empty() ? 0 : 1);
                    }
                    ++copies.relocateCalls[kind];
                    copies.relocateUs[kind] += us;
                    return candidate;
                };

                // A shifted candidate (relocateVariant) validated like a stored variant; on a hit its
                // data words are refreshed in place: the shifted compiled copy is the candidate's own
                // (shiftVariant's `patched`), so it is patched instead of copied again.
                const auto tryRelocated = [&](std::shared_ptr<DispatchVariant> candidate) {
                    regions.clear();
                    appendEntryRegions(*candidate, regions);
                    const auto compareStarted = countCopies ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    const auto waitedAtCompare = countCopies ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
                    const auto result = validateVariant(address, submission.queue, *candidate, regions, imagesFlushed, runsSynced, sampling, &liveData);
                    if (countCopies) {
                        // Without the GPU waits inside (the phase books them as "validate GPU wait").
                        copies.compareUs += ElapsedUs(compareStarted) - (Graphics::Recorder::ThreadWaitedMs() - waitedAtCompare) * 1000.0;
                        ++copies.shiftedCompares;
                    }
                    if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) {
                        if (result == EntryOutcome::Differing) traceFailedRelocation(address, *candidate);
                        ++relocatedDiffering;
                        if (countCopies && candidate->patched != nullptr) ++copies.shiftCopiesDiffering;
                        return false;
                    }
                    for (const auto& [position, value] : liveData) candidate->words[position] = value;
                    if (!liveData.empty()) {
                        const auto patchStarted = countCopies ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        std::shared_ptr<ShaderRecompiler::RecompileResult> patched;
                        if (relocationFirst() && candidate->patched != nullptr && candidate->patched.get() == candidate->compiled.get()) patched = candidate->patched;
                        else patched = std::make_shared<ShaderRecompiler::RecompileResult>(*candidate->compiled);
                        auto& descriptor = patched->bindings[candidate->flatBinding].guestDescriptor;
                        for (std::size_t k = 0; k < candidate->dataPositions.size(); ++k) {
                            if (candidate->dataSlots[k] < descriptor.size()) descriptor[candidate->dataSlots[k]] = candidate->words[candidate->dataPositions[k]];
                        }
                        if (countCopies) {
                            copies.patchUs += ElapsedUs(patchStarted);
                            if (patched == candidate->patched) ++copies.shiftCopiesPatched;
                            else NoteResultCopy(copies, CopyRelocatedData, *patched);
                        }
                        candidate->compiled = std::move(patched);
                    }
                    relocated = std::move(candidate);
                    return true;
                };

                // Relocation first (APS5_NO_RELOCATION_FIRST=1: the stored variants first, as before).
                // When the front variant's rule pointers moved, the front variant itself cannot
                // compare equal (its pointer words differ outside the data mask), so the shifted
                // candidate is validated before the k stored variants; an older variant that sits
                // at the live addresses would have hit exactly, but the candidate holds the same
                // non-data words shifted, so it hits in its place. In Boletaria (t291) 56.5k of the
                // 60k queue-0 hits per 10 s were relocated after 3.6 differing stored compares each.
                const bool frontRule = relocatedHits() && !variants.front()->pointerPositions.empty();
                if (frontRule && relocationFirst()) {
                    if (auto candidate = relocate(*variants.front()); candidate != nullptr) {
                        ++relocatedFirst;
                        if (tryRelocated(std::move(candidate))) storedSkipped = variants.size();
                        else ++relocatedFirstDiffering;
                    }
                }

                if (relocated == nullptr) {
                    for (std::size_t i = 0; i < variants.size(); ++i) {
                        regions.clear();
                        appendEntryRegions(*variants[i], regions);
                        ++compared;
                        const auto compareStarted = countCopies ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        const auto waitedAtCompare = countCopies ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
                        auto result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData);

                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                            result = validateVariant(address, submission.queue, *variants[i], regions, imagesFlushed, runsSynced, sampling, &liveData);
                            ++(result == EntryOutcome::Equal || result == EntryOutcome::EqualData ? retriesEqual : retriesMoved);
                        }
                        if (countCopies) {
                            copies.compareUs += ElapsedUs(compareStarted) - (Graphics::Recorder::ThreadWaitedMs() - waitedAtCompare) * 1000.0;
                            ++copies.storedCompares;
                        }
                        if (i == 0) outcome = result;
                        if (result == EntryOutcome::Equal || result == EntryOutcome::EqualData) {
                            outcome = result;
                            variant = variants[i];
                            rank = i;
                            dataHit = result == EntryOutcome::EqualData;
                            break;
                        }
                    }
                    current = variant != nullptr;
                }

                // The first variant with a rule decides (a candidate that fails ends the attempt);
                // when that is the front variant, relocation first already tried it.
                if (!current && relocated == nullptr && relocatedHits() && !(frontRule && relocationFirst())) {
                    for (const auto& stored : variants) {
                        if (stored->pointerPositions.empty()) continue;
                        auto candidate = relocate(*stored);
                        if (candidate == nullptr) break;
                        tryRelocated(std::move(candidate));
                        break;
                    }
                }
            } else {
                variant = variants.front();
                compared = 1;
                current = true;
                for (const auto& [begin, bytes] : variant->spans) {
                    GuestMemory::CollectWrites(begin, bytes);
                    if (!GuestMemory::UnchangedSince(begin, bytes, generation)) {
                        current = false;
                        break;
                    }
                }
                if (!current) {

                    std::uint64_t collected = 0;
                    for (const auto& [begin, bytes] : variant->spans) collected = std::max(collected, GuestMemory::CollectWrites(begin, bytes));
                    bool same = collected != 0;
                    if (same) {
                        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                        PendingView pending;
                        pending.Load();
                        same = validateCaptured(address, submission.queue, variant->captured, *variant->compiled, false, pending);
                    }
                    if (same) {
                        restamped = collected;
                        current = true;
                    }
                }
                if (current) outcome = EntryOutcome::Equal;
            }
            phaseTiming.Phase(PhaseValidate);
            if (profile) {
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[PhaseValidate]);
                phaseMs[PhaseValidate] -= waited;
                phaseMs[PhaseValidateWait] += waited;
            }
            if (!cacheLock.owns_lock()) cacheLock.lock();

            const auto again = dispatchCache.find(key);
            const bool untouched = again != dispatchCache.end() && again->second == entry && variants.front()->generation.load(std::memory_order_acquire) == generation;
            auto& counters = entryCounters;
            counters.validateUs += phaseMs[PhaseValidate] * 1000;
            counters.imagesFlushed += imagesFlushed;
            counters.runsSynced += runsSynced;
            for (std::size_t i = 0; i < compared && i < variants.size(); ++i) counters.runsValidated += variants[i]->runs.size();
            counters.retriesEqual += retriesEqual;
            counters.retriesMoved += retriesMoved;
            counters.variantsCompared += compared;
            counters.relocatedUnordered += relocatedUnordered;
            counters.relocatedDiffering += relocatedDiffering;
            counters.relocatedFirst += relocatedFirst;
            counters.relocatedFirstDiffering += relocatedFirstDiffering;
            counters.storedValidationsSkipped += storedSkipped;
            if (countCopies) {
                ++copies.validations;
                counters.copies.Add(copies);
            }
            switch (outcome) {
                case EntryOutcome::Equal: ++counters.equal; break;
                case EntryOutcome::EqualData: ++counters.equal; break;
                case EntryOutcome::Differing: ++counters.differing; break;
                case EntryOutcome::Inaccessible: ++counters.inaccessible; break;
                case EntryOutcome::QueuedLabel: ++counters.queuedLabel; break;
                case EntryOutcome::FlushingImage: ++counters.flushingImage; break;
                case EntryOutcome::PublishMoved: ++counters.publishMoved; break;
                case EntryOutcome::PendingMoved: ++counters.pendingMoved; break;
                case EntryOutcome::ForgetMoved: ++counters.forgetMoved; break;
            }
            if (current) {
                ++counters.variantHitsByRank[rank];
                compiledResult = variant->compiled;
                if (dataHit) {
                    const auto patchStarted = countCopies ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    liveWords = variant->words;
                    for (const auto& [position, value] : liveData) liveWords[position] = value;
                    regions.clear();
                    appendEntryRegions(*variant, regions, &liveWords);
                    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant->compiled);
                    auto& descriptor = patched->bindings[variant->flatBinding].guestDescriptor;
                    for (std::size_t k = 0; k < variant->dataPositions.size(); ++k) {
                        if (variant->dataSlots[k] < descriptor.size()) descriptor[variant->dataSlots[k]] = liveWords[variant->dataPositions[k]];
                    }
                    compiledResult = std::move(patched);
                    if (countCopies) {
                        // Under the cache lock (the relock phase).
                        counters.copies.storedPatchUs += ElapsedUs(patchStarted);
                        NoteResultCopy(counters.copies, CopyStoredData, *compiledResult);
                        counters.copies.liveWordsBytes += liveWords.size() * sizeof(std::uint32_t);
                    }
                    ++counters.dataHits;
                    counters.dataWordsRefreshed += liveData.size();
                    ++counters.dataHitsByRank[rank];
                }
                if (stampValidate()) {
                    captured = variant->captured;
                } else {

                    captured.reserve(memory.size() + regions.size());
                    captured.assign(memory.begin(), memory.end());
                    captured.insert(captured.end(), regions.begin(), regions.end());
                    if (variant->forgetSerial != GuestMemory::ForgetSerial()) ++counters.forgetSinceInsert;
                }
                keepVariant = variant;
                cached = true;
                ++dispatchCacheHits;
                if (untouched) {
                    if (restamped != 0) variant->generation.store(restamped, std::memory_order_release);

                    if (rank != 0) {
                        auto rotated = std::make_shared<DispatchEntry>();
                        rotated->variants.reserve(variants.size());
                        rotated->variants.push_back(variant);
                        for (std::size_t i = 0; i < variants.size(); ++i) {
                            if (i != rank) rotated->variants.push_back(variants[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }

                    if (dispatchCacheHits - again->second->touched > dispatchCacheEntries() / 8) {
                        dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, again->second->order);
                        again->second->touched = dispatchCacheHits;
                        ++counters.touches;
                    }
                }
            } else if (relocated != nullptr) {
                ++counters.relocatedHits;
                ++dispatchCacheHits;
                compiledResult = relocated->compiled;
                regions.clear();
                appendEntryRegions(*relocated, regions);
                captured.reserve(memory.size() + regions.size());
                captured.assign(memory.begin(), memory.end());
                captured.insert(captured.end(), regions.begin(), regions.end());
                cached = true;
                if (untouched) {
                    // The relocated variant goes first, as a fresh capture's would.
                    auto replacement = std::make_shared<DispatchEntry>();
                    replacement->variants.reserve(dispatchVariants());
                    accountVariant(*relocated, true);
                    replacement->variants.push_back(relocated);
                    for (const auto& kept : variants) {
                        if (replacement->variants.size() < dispatchVariants()) {
                            replacement->variants.push_back(kept);
                        } else {
                            accountVariant(*kept, false);
                            ++counters.variantsEvicted;
                        }
                    }
                    ++counters.variantsInserted;
                    replacement->touched = dispatchCacheHits;
                    replacement->order = entry->order;
                    dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, replacement->order);
                    again->second = std::move(replacement);
                }
            } else {
                if (traceCache) std::fprintf(stderr, "[dispatch-cache] 0x%llx captured memory changed\n", static_cast<unsigned long long>(address));

                missedEntry = entry;
                missedDiffering = outcome == EntryOutcome::Differing;
                if (!untouched) ++counters.replaced;
            }
            if (profile && std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) {
                counters.lastReport = std::chrono::steady_clock::now();
                reportDispatchCache(counters);
            }
            phaseTiming.Phase(PhaseRelock);
        }
        if (entry == nullptr && !baseCandidates.empty()) {
            relocateByUserPointer(address, submission, key, baseKey, userData, memory, baseCandidates, compiledResult, captured, cached, relocated);
            phaseTiming.Phase(PhaseValidate);
        }
    }
}

}
