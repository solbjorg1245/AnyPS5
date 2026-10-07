#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <span>

// Relocated draw-cache hits. The title gives ~6% of its draws a per-frame block of SRT memory and
// passes its address in the vertex (and pixel) user SGPRs 0-1, so their draw keys were new every
// frame (~8k new keys per 10 s in Boletaria, two stage captures each, PROGRESS t263) although the
// walk and its result were the same as last frame's at the old address. The draw key now keeps
// those pointer registers (DrawPointerRegisters) out of a base key; a new key whose base key has an
// entry whose pointer pairs are equal or moved as 64-bit values (one delta per program) stands in
// for it: a stage whose pointers moved compares its variants shifted by the delta (shiftVariant:
// the runs the rule names, the pointers among the words, the compiled descriptors' addresses and
// the push-constant pointers), the others compare in place, and the entry moves to the new key
// (rekey) with the shifted variants replacing the dead ones. A miss under a candidate captures as
// usual and teaches the fresh variant the rule against the candidate's (learnRelocation with the
// delta), so the next frame relocates. The entry's decode gets the live pointer words
// (relocatedDecode): the same registers otherwise, by the base key. APS5_NO_DRAW_RELOCATION=1
// disables it; APS5_VERIFY_DATA_HITS=1 captures relocated hits again and compares what they bind.

namespace AgcDriver::DriverDetail {

// Whether a variant of a stage whose pointer moved stays valid in place: its rule shifts nothing.
bool Driver::staysOnMove(const DispatchVariant& variant) {
    return variant.relocationLearned && variant.movedRuns.empty() && variant.shiftSlots.empty() && variant.pushShiftSlots.empty();
}

bool Driver::drawRelocation() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr && dataHits() && !stampValidate();
    return enabled;
}

// The user-data index of shader register `reg` in `program`, or SIZE_MAX when the register is
// not one of its inputs (a merged front stage carries eight hidden words first; the fragment
// stage none).
std::size_t Driver::pointerWordIndex(const DrawProgram& program, std::uint32_t reg) {
    if (reg < program.userDataBase) return std::numeric_limits<std::size_t>::max();
    const auto hidden = program.firstUserSgpr == 0 && program.binary.stage != ShaderRecompiler::ShaderStage::Fragment ? 8u : 0u;
    const auto index = static_cast<std::size_t>(reg - program.userDataBase) + hidden;
    return index < program.userData.size() ? index : std::numeric_limits<std::size_t>::max();
}

// Under drawCacheMutex. The entries under the key's base key whose pointer pairs are equal or
// moved as 64-bit values, with the programs' inputs agreeing on one delta each (`deltas`, 0 for a
// program whose inputs did not move), oldest first.
void Driver::findRelocationCandidates(const DrawKey& key, std::vector<DrawRelocationCandidate>& candidates) {
    auto& counters = drawEntryCounters;
    candidates.clear();
    const auto index = drawBaseIndex.find(key.base);
    if (index == drawBaseIndex.end()) {
        ++counters.relocationNoCandidate;
        return;
    }
    auto& keys = index->second;
    constexpr auto none = std::numeric_limits<std::size_t>::max();
    std::vector<std::uint64_t> deltas;
    for (std::size_t k = 0; k < keys.size();) {
        const auto found = drawCache.find(keys[k]);
        if (found == drawCache.end()) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        ++counters.relocationTried;
        const auto& entry = *found->second;
        if (entry.decode == nullptr || entry.pointerPresent != key.present || keys[k] == key.key) {
            ++k;
            continue;
        }
        // Each pair: both words present and moved as one 64-bit value (or equal); a lone present
        // word must be equal.
        std::array<std::uint64_t, DrawPointerRegisters.size() / 2> pairDeltas{};
        bool fits = true;
        for (std::size_t p = 0; p < pairDeltas.size() && fits; ++p) {
            const auto lowBit = 1u << (2 * p);
            const auto highBit = lowBit << 1u;
            const bool low = (key.present & lowBit) != 0, high = (key.present & highBit) != 0;
            if (low && high) {
                const auto stored = static_cast<std::uint64_t>(entry.pointerWords[2 * p]) | (static_cast<std::uint64_t>(entry.pointerWords[2 * p + 1]) << 32u);
                const auto live = static_cast<std::uint64_t>(key.words[2 * p]) | (static_cast<std::uint64_t>(key.words[2 * p + 1]) << 32u);
                pairDeltas[p] = live - stored;
            } else if (low || high) {
                const auto w = low ? 2 * p : 2 * p + 1;
                fits = entry.pointerWords[w] == key.words[w];
            }
        }
        if (!fits) {
            ++counters.relocationDeltas;
            ++k;
            continue;
        }
        const auto& programs = entry.decode->programs;
        deltas.assign(programs.size(), 0);
        for (std::size_t i = 0; i < programs.size() && fits; ++i) {
            for (std::size_t p = 0; p < pairDeltas.size() && fits; ++p) {
                if (pairDeltas[p] == 0) continue;
                const auto low = pointerWordIndex(programs[i], DrawPointerRegisters[2 * p]);
                const auto high = pointerWordIndex(programs[i], DrawPointerRegisters[2 * p + 1]);
                if (low == none && high == none) continue;
                // Half a pair as an input is a plain constant that changed.
                if (low == none || high == none || (deltas[i] != 0 && deltas[i] != pairDeltas[p])) fits = false;
                else deltas[i] = pairDeltas[p];
            }
        }
        if (!fits) {
            ++counters.relocationDeltas;
            ++k;
            continue;
        }
        candidates.push_back({found->second, keys[k], deltas});
        ++k;
    }
    if (candidates.empty()) ++counters.relocationNoCandidate;
}

// Whether the first words of the variant's first moved run, shifted by the delta, hold the stored
// words (data, ignored and base-slot positions aside): a cheap test of a candidate before the
// full compare, which tells another object's entry under the same base key from this one's.
bool Driver::quickShiftedMatch(const DispatchVariant& variant, std::uint64_t delta) {
    if (variant.movedRuns.empty()) return true;
    const auto run = variant.movedRuns.front();
    if (run >= variant.runs.size()) return false;
    std::size_t offset = 0;
    for (std::size_t r = 0; r < run; ++r) offset += static_cast<std::size_t>((variant.runs[r].second - variant.runs[r].first) / sizeof(std::uint32_t));
    const auto [begin, end] = variant.runs[run];
    const auto count = std::min<std::size_t>(16, static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t)));
    if (count == 0 || offset + count > variant.words.size()) return false;
    std::array<std::uint32_t, 16> live{};
    if (GuestMemory::CopyMapped(begin + delta, std::as_writable_bytes(std::span(live).subspan(0, count))) != GuestMemory::Compare::Equal) return false;
    for (std::size_t k = 0; k < count; ++k) {
        const auto position = static_cast<std::uint32_t>(offset + k);
        if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), position)) continue;
        const auto mask = IgnoredMaskAt(variant.ignoredBits, position) | PatchMaskAt(variant.baseSlots, position);
        if (((live[k] ^ variant.words[position]) & ~mask) != 0) return false;
    }
    return true;
}

// The candidate lookupDraw validates against: the first whose every moved stage has a variant
// with a rule and whose first such variant's shifted words are in place (or whose rule shifts
// nothing); a candidate whose inputs did not move at all compares in place as it is.
bool Driver::chooseRelocationCandidate(DrawRelocation& relocation) {
    std::uint64_t rejected = 0;
    for (const auto& candidate : relocation.candidates) {
        const auto& entry = *candidate.entry;
        bool usable = true;
        for (std::size_t i = 0; i < candidate.deltas.size() && usable; ++i) {
            if (candidate.deltas[i] == 0 || i >= entry.stages.size()) continue;
            if (entry.decode != nullptr && i < entry.decode->roles.size() && entry.decode->roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
            bool ruled = false;
            for (const auto& variant : entry.stages[i]) {
                if (!variant->relocationLearned) continue;
                ruled = true;
                usable = quickShiftedMatch(*variant, candidate.deltas[i]);
                break;
            }
            if (!ruled) usable = false;
        }
        if (!usable) {
            ++rejected;
            continue;
        }
        relocation.entry = candidate.entry;
        relocation.key = candidate.key;
        relocation.deltas = candidate.deltas;
        relocation.relocated.assign(candidate.deltas.size(), false);
        break;
    }
    if (rejected != 0 || relocation.entry == nullptr) {
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.relocationQuickRejected += rejected;
        if (relocation.entry == nullptr) ++drawEntryCounters.relocationUnchosen;
    }
    return relocation.entry != nullptr;
}

// The decode with the key's pointer words in each program's user data.
std::shared_ptr<DrawDecode> Driver::relocatedDecode(const DrawDecode& decode, const DrawKey& key) {
    auto copy = std::make_shared<DrawDecode>(decode);
    for (auto& program : copy->programs) {
        for (std::size_t w = 0; w < DrawPointerRegisters.size(); ++w) {
            if ((key.present & (1u << w)) == 0) continue;
            const auto index = pointerWordIndex(program, DrawPointerRegisters[w]);
            if (index != std::numeric_limits<std::size_t>::max()) program.userData[index] = key.words[w];
        }
    }
    return copy;
}

// Under drawCacheMutex. A hit through the candidate: its entry moves to the new key with the
// decode holding the live words; a shifted stage keeps the shifted variant alone (the others sit
// at the dead address), a stage compared in place keeps its list with the matched variant first.
void Driver::rekeyDrawEntryLocked(const DrawRelocation& relocation, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::size_t>& ranks) {
    const auto& key = relocation.target;
    const auto found = drawCache.find(relocation.key);
    if (found == drawCache.end() || found->second != relocation.entry || drawCache.contains(key.key)) return;
    auto& counters = drawEntryCounters;
    const auto& entry = *relocation.entry;
    auto replacement = std::make_shared<DrawEntry>();
    replacement->decode = relocation.decode != nullptr ? relocation.decode : entry.decode;
    replacement->stages.resize(entry.stages.size());
    for (std::size_t i = 0; i < entry.stages.size(); ++i) {
        auto& variants = replacement->stages[i];
        const bool moved = i < relocation.deltas.size() && relocation.deltas[i] != 0;
        const bool shifted = i < relocation.relocated.size() && relocation.relocated[i];
        if (moved) {
            // The shifted variant replaces the one it was shifted from; a variant whose rule
            // shifts nothing stays (independent of the pointer); the rest sit at dead addresses.
            for (std::size_t rank = 0; rank < entry.stages[i].size(); ++rank) {
                const auto& variant = entry.stages[i][rank];
                if (shifted && i < ranks.size() && rank == ranks[i]) {
                    // The variant the shifted one was made from leaves with it.
                    accountDrawVariant(*variant, false);
                    ++counters.variantsEvicted;
                    continue;
                }
                if (staysOnMove(*variant)) variants.push_back(variant);
                else {
                    accountDrawVariant(*variant, false);
                    ++counters.variantsEvicted;
                }
            }
            if (shifted && i < matched.size() && matched[i] != nullptr) {
                accountDrawVariant(*matched[i], true);
                variants.insert(variants.begin(), matched[i]);
                ++counters.variantsInserted;
            } else if (i < matched.size() && matched[i] != nullptr) {
                const auto at = std::find(variants.begin(), variants.end(), matched[i]);
                if (at != variants.end()) std::rotate(variants.begin(), at, at + 1);
            }
            continue;
        }
        variants = entry.stages[i];
        if (i < matched.size() && matched[i] != nullptr && i < ranks.size() && ranks[i] != 0 && ranks[i] < variants.size()) {
            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(ranks[i]));
            variants.insert(variants.begin(), matched[i]);
        }
    }
    replacement->recipes.store(entry.recipes.load());
    replacement->touched = drawCacheHits;
    replacement->order = entry.order;
    *replacement->order = key.key;
    replacement->baseKey = key.base;
    replacement->pointerWords = key.words;
    replacement->pointerPresent = key.present;
    const auto order = replacement->order;
    unindexDrawKeyLocked(entry.baseKey, relocation.key);
    drawCache.erase(found);
    drawCache.emplace(key.key, std::move(replacement));
    indexDrawKeyLocked(key);
    drawOrder.splice(drawOrder.begin(), drawOrder, order);
    ++counters.rekeys;
}

void Driver::indexDrawKeyLocked(const DrawKey& key) {
    if (key.base == 0) return;
    auto& keys = drawBaseIndex[key.base];
    if (std::find(keys.begin(), keys.end(), key.key) != keys.end()) return;
    if (keys.size() >= DrawBaseCandidates) keys.erase(keys.begin());
    keys.push_back(key.key);
}

void Driver::unindexDrawKeyLocked(std::uint64_t base, std::uint64_t key) {
    const auto index = drawBaseIndex.find(base);
    if (index == drawBaseIndex.end()) return;
    auto& keys = index->second;
    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    if (keys.empty()) drawBaseIndex.erase(index);
}

}
