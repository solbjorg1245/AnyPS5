#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

// The head of a draw skip reason, for counting reasons that differ only in their numbers: the text
// up to its first line break or " [" suffix, at most `limit` characters, with every hex number
// (0x...) and every decimal run of three or more digits replaced by '#' (addresses, sizes and
// hashes vary per draw; small numbers such as "dword 0" or "stage 1" name the case and are kept).
inline std::string DrawSkipReasonKey(std::string_view what, std::size_t limit = 72) {
    const auto end = std::min(what.find('\n'), what.find(" ["));
    if (end != std::string_view::npos) what = what.substr(0, end);
    std::string key;
    key.reserve(std::min(what.size(), limit));
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto hexDigit = [&](char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    for (std::size_t i = 0; i < what.size() && key.size() < limit;) {
        if (what[i] == '0' && i + 2 < what.size() && (what[i + 1] == 'x' || what[i + 1] == 'X') && hexDigit(what[i + 2])) {
            i += 2;
            while (i < what.size() && hexDigit(what[i])) ++i;
            key += '#';
        } else if (digit(what[i])) {
            auto run = i;
            while (run < what.size() && digit(what[run])) ++run;
            if (run - i >= 3) key += '#';
            else key.append(what.substr(i, run - i));
            i = run;
        } else {
            key += what[i++];
        }
    }
    if (key.size() > limit) key.resize(limit);
    return key;
}

// Draw packets that drew nothing, counted per reason head (DrawSkipReasonKey) with their time in
// the driver, for the "[draws] skips by reason" line (APS5_PROFILE_DRAW). At most MaxKeys distinct
// heads per window; skips with a further head are counted as `overflow`. Not thread-safe: the
// caller holds the draw profile's mutex.
class DrawSkipReasonTally {
public:
    static constexpr std::size_t MaxKeys = 256;

    struct Entry {
        std::string key;
        std::uint64_t count = 0;
        double us = 0;
    };

    void Add(std::string_view what, double us) {
        auto key = DrawSkipReasonKey(what);
        auto found = entries.find(key);
        if (found == entries.end()) {
            if (entries.size() >= MaxKeys) {
                ++overflow;
                overflowUs += us;
                return;
            }
            found = entries.emplace(std::move(key), Totals{}).first;
        }
        ++found->second.count;
        found->second.us += us;
    }

    // The `n` heads with the most skips (ties: more time first, then the key).
    std::vector<Entry> Top(std::size_t n) const {
        std::vector<Entry> sorted;
        sorted.reserve(entries.size());
        for (const auto& [key, totals] : entries) sorted.push_back({key, totals.count, totals.us});
        std::sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.us != b.us) return a.us > b.us;
            return a.key < b.key;
        });
        if (sorted.size() > n) sorted.resize(n);
        return sorted;
    }

    std::size_t Keys() const { return entries.size(); }
    std::uint64_t Overflow() const { return overflow; }
    double OverflowUs() const { return overflowUs; }

    void Clear() {
        entries.clear();
        overflow = 0;
        overflowUs = 0;
    }

private:
    struct Totals {
        std::uint64_t count = 0;
        double us = 0;
    };
    std::unordered_map<std::string, Totals> entries;
    std::uint64_t overflow = 0;
    double overflowUs = 0;
};

}
