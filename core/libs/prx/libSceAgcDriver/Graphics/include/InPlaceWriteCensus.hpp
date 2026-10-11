#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INPLACEWRITECENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INPLACEWRITECENSUS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

// The [inplace-writes] census (APS5_PROFILE_DRAW; GuestBufferMemory.cpp feeds and prints it): the
// dispatch uses of written guest ranges bound in place (a host import or an image mirror) rather
// than staged device-local, by why they were not staged, and per range its uses, programs and how
// often the CPU changed it between two uses (the re-uploads a resident copy would need). One
// window at a time (Report prints it and starts the next), at most `capacity` ranges per window
// (uses of further ones count in the totals only). The caller locks.
class InPlaceWriteCensus {
public:
    // GuestBufferMemory::stagingEligible's refusals: over APS5_WRITTEN_SHADOW_MAX_KIB, under
    // APS5_WRITTEN_SHADOW_MIN_KIB, an address-based build, an image mirror, uncommitted pages, a
    // shadow the device refused, a build that may not stage (a draw), an atomic element over
    // APS5_ATOMIC_STAGE_MAX_KIB outside the written window, APS5_CPU_COPIES, anything else.
    enum class Reason : std::uint8_t { TooLarge, TooSmall, AddressBased, Mirror, Sparse, Unstaged, NotAllowed, AtomicCap, NoGpuCopies, Other };
    static constexpr std::size_t ReasonCount = 10;
    // A use's answer from the write tracker: its generation now (the range's next `previous`) and
    // the bytes the CPU changed since `previous`.
    struct Change {
        std::uint64_t generation = 0;
        std::uint64_t dirtyBytes = 0;
    };
    // A range of the window: its first program, the distinct programs seen (`morePrograms` past
    // the four kept) and the reason of its last use.
    struct RangeCounts {
        std::uint64_t begin = 0, end = 0, uses = 0, changed = 0, dirtyBytes = 0, program = 0;
        std::size_t programs = 0;
        bool morePrograms = false;
        Reason reason = Reason::Other;
    };

    explicit InPlaceWriteCensus(std::size_t capacity = 4096) : capacity(capacity) { ranges.reserve(capacity); }

    // One use of [begin, end) by `program` (the [gputime] key). `changedSince(previous)` gives the
    // Change since the generation the range's previous use in this window got (0: none, and the
    // use counts as unchanged).
    template<typename ChangedSince>
    void Note(std::uint64_t begin, std::uint64_t end, Reason reason, std::uint64_t program, ChangedSince&& changedSince) {
        ++reasonUses[static_cast<std::size_t>(reason)];
        logicalBytes += end - begin;
        const std::pair key{begin, end};
        auto found = ranges.find(key);
        if (found == ranges.end()) {
            if (ranges.size() >= capacity) {
                ++overflowed;
                return;
            }
            found = ranges.emplace(key, Range{}).first;
        }
        auto& range = found->second;
        const Change change = changedSince(range.generation);
        if (range.generation != 0 && change.dirtyBytes != 0) {
            ++range.changed;
            range.dirtyBytes += change.dirtyBytes;
        }
        range.generation = change.generation;
        ++range.uses;
        range.reason = reason;
        const auto known = range.programs.begin() + range.programCount;
        if (std::find(range.programs.begin(), known, program) != known) return;
        if (range.programCount < range.programs.size()) range.programs[range.programCount++] = program;
        else range.morePrograms = true;
    }
    // One use of an address-based build's space: the ranges its BDA table may store to, `bytes` in all.
    void NoteSpace(std::uint64_t bytes) {
        ++reasonUses[static_cast<std::size_t>(Reason::AddressBased)];
        ++spaceUses;
        logicalBytes += bytes;
        spaceBytes = std::max(spaceBytes, bytes);
    }

    std::uint64_t Uses(Reason reason) const { return reasonUses[static_cast<std::size_t>(reason)]; }
    std::size_t Ranges() const { return ranges.size(); }
    std::uint64_t Overflowed() const { return overflowed; }
    // The `count` ranges with the most uses x bytes (ties: the lower address first).
    std::vector<RangeCounts> Top(std::size_t count) const {
        std::vector<RangeCounts> top;
        for (const auto& [key, range] : ranges) top.push_back({key.first, key.second, range.uses, range.changed, range.dirtyBytes, range.programs[0], range.programCount, range.morePrograms, range.reason});
        const auto weight = [](const RangeCounts& counts) { return counts.uses * (counts.end - counts.begin); };
        count = std::min(count, top.size());
        std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(count), top.end(), [&](const RangeCounts& left, const RangeCounts& right) { return weight(left) != weight(right) ? weight(left) > weight(right) : left.begin < right.begin; });
        top.resize(count);
        return top;
    }
    // The window's lines (the totals by reason, then the top `count` ranges) over `seconds`; the
    // next window starts empty.
    std::string Report(double seconds, std::size_t count = 12) {
        static constexpr std::array<const char*, ReasonCount> names{"too-large", "too-small", "address-based", "mirror", "sparse", "unstaged", "not-allowed", "atomic-cap", "no-gpu-copies", "other"};
        std::uint64_t uses = 0;
        for (const auto value : reasonUses) uses += value;
        const auto of = [&](Reason reason) { return static_cast<unsigned long long>(Uses(reason)); };
        char text[768];
        std::snprintf(text, sizeof(text), "[inplace-writes] (%.0f s): %llu uses of written regions bound in place (%.0f MiB logical) by reason: too-large %llu, too-small %llu, address-based %llu (%llu space uses, space writable %.0f MiB), mirror %llu, sparse %llu, unstaged %llu, not-allowed %llu, atomic-cap %llu, no-gpu-copies %llu, other %llu; %zu distinct ranges (%llu uses overflowed)\n", seconds, static_cast<unsigned long long>(uses), logicalBytes / 1048576.0, of(Reason::TooLarge), of(Reason::TooSmall), of(Reason::AddressBased), static_cast<unsigned long long>(spaceUses), spaceBytes / 1048576.0, of(Reason::Mirror), of(Reason::Sparse), of(Reason::Unstaged), of(Reason::NotAllowed), of(Reason::AtomicCap), of(Reason::NoGpuCopies), of(Reason::Other), ranges.size(), static_cast<unsigned long long>(overflowed));
        std::string report = text;
        if (const auto top = Top(count); !top.empty()) {
            std::snprintf(text, sizeof(text), "[inplace-writes] top %zu by uses x MiB (changed: a CPU store some collect saw since the range's previous use; dirty: its 64 KiB blocks):", top.size());
            report += text;
            for (const auto& range : top) {
                std::snprintf(text, sizeof(text), " 0x%llx %.4g MiB %s x%llu (%llu changed by the CPU, %.0f KiB dirty) prog 0x%llx", static_cast<unsigned long long>(range.begin), (range.end - range.begin) / 1048576.0, names[static_cast<std::size_t>(range.reason)], static_cast<unsigned long long>(range.uses), static_cast<unsigned long long>(range.changed), range.dirtyBytes / 1024.0, static_cast<unsigned long long>(range.program));
                report += text;
                if (range.programs > 1) report += " +" + std::to_string(range.programs - 1) + (range.morePrograms ? " or more" : "") + " programs";
                report += ';';
            }
            report += '\n';
        }
        reasonUses = {};
        logicalBytes = spaceUses = spaceBytes = overflowed = 0;
        ranges.clear();
        return report;
    }

private:
    struct KeyHash {
        std::size_t operator()(const std::pair<std::uint64_t, std::uint64_t>& key) const { return static_cast<std::size_t>(key.first * 0x9e3779b97f4a7c15ull ^ (key.second - key.first)); }
    };
    struct Range {
        // `generation`: the tracker's at the previous use (0: none in this window).
        std::uint64_t uses = 0, changed = 0, dirtyBytes = 0, generation = 0;
        std::array<std::uint64_t, 4> programs{};
        std::uint8_t programCount = 0;
        bool morePrograms = false;
        Reason reason = Reason::Other;
    };
    std::size_t capacity;
    std::unordered_map<std::pair<std::uint64_t, std::uint64_t>, Range, KeyHash> ranges;
    std::array<std::uint64_t, ReasonCount> reasonUses{};
    std::uint64_t logicalBytes = 0, spaceUses = 0, spaceBytes = 0, overflowed = 0;
};

}

#endif
