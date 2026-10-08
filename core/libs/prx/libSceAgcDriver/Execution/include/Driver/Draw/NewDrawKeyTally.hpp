#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

// The shader program registers a draw key hashes (SPI_SHADER_PGM_LO/HI at these shader offsets).
inline constexpr std::array<std::uint32_t, 5> DrawProgramRegisters{0x008, 0x088, 0x0c8, 0x108, 0x148};

// The never-seen draw keys of one report window grouped by base key (DrawKey::base: the key
// without the pointer words), to tell which draws get a fresh key every frame (queue item 6d(b)):
// per base, the keys, those whose base had a cache entry, those whose pointer words equal the
// last never-seen key's under the base (the same key absent again: never inserted), the pointer
// words that changed between them (DrawPointerRegisters bits), and the program addresses.
// APS5_PROFILE_DRAW only (with the evicted-key window); under drawCacheMutex.
struct NewDrawKeyTally {
    struct Base {
        std::uint64_t keys = 0, known = 0, sameWords = 0;
        std::uint32_t changed = 0, present = 0;
        std::array<std::uint32_t, 8> words{};
        std::array<std::uint64_t, DrawProgramRegisters.size()> programs{};
    };
    // Bases tracked per window; a new base beyond it only counts.
    static constexpr std::size_t Bound = 65536;

    std::unordered_map<std::uint64_t, Base> bases;
    std::uint64_t keys = 0, untracked = 0;

    void Note(std::uint64_t base, bool known, const std::array<std::uint32_t, 8>& words, std::uint32_t present, const std::array<std::uint64_t, DrawProgramRegisters.size()>& programs) {
        ++keys;
        auto found = bases.find(base);
        if (found == bases.end()) {
            if (bases.size() >= Bound) {
                ++untracked;
                return;
            }
            found = bases.emplace(base, Base{}).first;
        } else {
            auto& entry = found->second;
            std::uint32_t changed = entry.present ^ present;
            for (std::size_t i = 0; i < words.size(); ++i) {
                if (((present >> i) & 1u) != 0 && words[i] != entry.words[i]) changed |= 1u << i;
            }
            if (changed == 0) ++entry.sameWords;
            entry.changed |= changed;
        }
        auto& entry = found->second;
        ++entry.keys;
        if (known) ++entry.known;
        entry.words = words;
        entry.present = present;
        entry.programs = programs;
    }

    // The bases with the most keys, most first (ties: the lower base first).
    std::vector<std::pair<std::uint64_t, const Base*>> Top(std::size_t count) const {
        std::vector<std::pair<std::uint64_t, const Base*>> sorted;
        sorted.reserve(bases.size());
        for (const auto& [base, entry] : bases) sorted.emplace_back(base, &entry);
        const auto order = [](const auto& a, const auto& b) { return a.second->keys != b.second->keys ? a.second->keys > b.second->keys : a.first < b.first; };
        const auto keep = std::min(count, sorted.size());
        std::partial_sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(keep), sorted.end(), order);
        sorted.resize(keep);
        return sorted;
    }

    void Reset() {
        bases.clear();
        keys = untracked = 0;
    }
};

}
