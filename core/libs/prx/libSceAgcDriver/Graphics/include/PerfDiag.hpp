#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PERFDIAG_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PERFDIAG_HPP

#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics::PerfDiag {

// APS5_PERF_DIAG=1 (PERF-DIAG, default off; counters only, nothing is recorded differently): the
// questions of the perf plan's items 3-5 (port/reports/s64-perf-plan.md section 3), answered on the
// existing 10 s digest lines:
// - item 3, after the [gputime] draw-pass line (APS5_PROFILE_GPU): per pass target, the draws of
//   address-based builds and the (vertex, pixel) program pairs by draws with their SPIR-V words;
// - item 4, after the [storage] line: each image a storage refresh stores (W) against the image
//   refreshed (R), by surface, shape, element size, tiling, DCC and a CPU store into R;
// - item 5, after the [barriers] line: per copy-back of a staged region, whether anything noted a
//   read of the import before the next copy-in of the range.
// Nothing but a flag test while the switch is off.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PERF_DIAG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

inline void Append(std::string& out, const char* format, auto... values) {
    char text[256];
    const int written = std::snprintf(text, sizeof(text), format, values...);
    if (written > 0) out.append(text, std::min<std::size_t>(static_cast<std::size_t>(written), sizeof(text) - 1));
}

inline double Mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / 1048576.0;
}

// Item 3: the draws recorded into each pass target (Draw.cpp noteDrawTiming), keyed by the guest code
// addresses of their vertex and pixel stages: the draw capture names each compiled result (Name; one
// never named counts as '?', a missing stage as '-'). One program pair has as many compiled results
// as its captures made (variants by the bound data).
class TargetPrograms {
public:
    struct Pair {
        std::uint64_t draws = 0;
        std::uint64_t leased = 0;
        std::size_t vertexWords = 0;
        std::size_t pixelWords = 0;
        std::uint64_t results = 0;
    };
    struct Target {
        std::uint64_t draws = 0;
        std::uint64_t leased = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t format = 0;
        std::map<std::pair<std::uint64_t, std::uint64_t>, Pair> pairs;
        std::set<std::pair<const void*, const void*>> results;
    };
    // A pass target as the [gputime] digest ranked it: its GPU ms, draws and address-based draws
    // per present.
    struct Ranked {
        std::uint64_t target = 0;
        double ms = 0;
        double draws = 0;
        double leased = 0;
    };
    void Name(const void* program, std::uint64_t address) {
        std::lock_guard lock(mutex);
        names[program] = address;
    }
    void Draw(std::uint64_t target, const void* vertex, std::size_t vertexWords, const void* pixel, std::size_t pixelWords, bool leased, std::uint32_t width, std::uint32_t height, std::uint32_t format) {
        std::lock_guard lock(mutex);
        auto& entry = targets[target];
        ++entry.draws;
        if (leased) ++entry.leased;
        entry.width = width;
        entry.height = height;
        entry.format = format;
        auto& pair = entry.pairs[{address(vertex), address(pixel)}];
        ++pair.draws;
        if (leased) ++pair.leased;
        pair.vertexWords = std::max(pair.vertexWords, vertexWords);
        pair.pixelWords = std::max(pair.pixelWords, pixelWords);
        if (entry.results.emplace(vertex, pixel).second) ++pair.results;
    }

    // The line for the ranked targets (per present over `presents`), then a fresh window.
    std::string Line(std::span<const Ranked> ranked, std::uint64_t presents, std::size_t topPairs = 3) {
        std::lock_guard lock(mutex);
        const double per = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
        std::string line = "[gputime] perf-diag draw programs by target (APS5_PERF_DIAG, per present; GPU ms, draws, L draws of address-based builds; recorded draws on the CPU side, then the top (vertex, pixel) pairs by draws with their SPIR-V words):";
        for (const auto& rank : ranked) {
            Append(line, " 0x%llx %.2fms %.1fd L%.1f", static_cast<unsigned long long>(rank.target), rank.ms, rank.draws, rank.leased);
            const auto found = targets.find(rank.target);
            if (found == targets.end()) {
                line += " (no recorded draws);";
                continue;
            }
            const auto& entry = found->second;
            Append(line, " (recorded %.1fd L%.1f = %.0f%%, %ux%u format %u, %zu pairs, %zu compiled results):", static_cast<double>(entry.draws) * per, static_cast<double>(entry.leased) * per, entry.draws != 0 ? 100.0 * static_cast<double>(entry.leased) / static_cast<double>(entry.draws) : 0.0, entry.width, entry.height, entry.format, entry.pairs.size(), entry.results.size());
            std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, const Pair*>> sorted;
            sorted.reserve(entry.pairs.size());
            for (const auto& [key, pair] : entry.pairs) sorted.emplace_back(key, &pair);
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second->draws > b.second->draws; });
            for (std::size_t i = 0; i < sorted.size() && i < topPairs; ++i) {
                const auto name = [](std::uint64_t address) {
                    std::string text = address == Missing ? "-" : address == Unnamed ? "?" : "";
                    if (text.empty()) Append(text, "0x%llx", static_cast<unsigned long long>(address));
                    return text;
                };
                const auto& pair = *sorted[i].second;
                Append(line, " vs %s ps %s %.1fd L%.1f spv %zu/%zu (%llu results)", name(sorted[i].first.first).c_str(), name(sorted[i].first.second).c_str(), static_cast<double>(pair.draws) * per, static_cast<double>(pair.leased) * per, pair.vertexWords, pair.pixelWords, static_cast<unsigned long long>(pair.results));
                line += i + 1 < sorted.size() && i + 1 < topPairs ? "," : "";
            }
            line += ";";
        }
        targets.clear();
        return line;
    }

private:
    static constexpr std::uint64_t Missing = 0;
    static constexpr std::uint64_t Unnamed = 1;
    std::uint64_t address(const void* program) const {
        if (program == nullptr) return Missing;
        const auto found = names.find(program);
        return found != names.end() ? found->second : Unnamed;
    }

    HostMutex mutex;
    std::unordered_map<const void*, std::uint64_t> names;
    std::map<std::uint64_t, Target> targets;
};

inline TargetPrograms& Targets() {
    static TargetPrograms* const targets = new TargetPrograms;
    return *targets;
}

// Item 4: what a storage image (Texture.hpp StorageTexture) looks like to the pair count.
struct Surface {
    std::uint64_t base = 0;
    std::uint64_t bytes = 0;
    std::uint32_t format = 0;
    std::uint32_t elementBytes = 0;
    std::uint32_t tileMode = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t baseArray = 0;
    std::uint32_t mips = 0;
    std::uint32_t dimension = 0;
    bool dcc = false;
};

enum PairFact : std::size_t { SameSurface, SameShape, SameFormat, SameElementBytes, SameTiling, AnyDcc, CpuStored, GpuOnlySameShape, Nested, SameExtent, GpuOnlySubresource, GpuOnlyCopyable, PairFacts };
inline constexpr std::array<const char*, PairFacts> PairFactNames{"same surface", "same shape", "same format", "same element size", "same tiling", "DCC", "CPU-stored", "GPU-only same-shape", "nested (one inside the other)", "same extent", "GPU-only subresource", "GPU-only copyable"};

// A pair is "GPU-only same-shape" when an image -> image copy could stand in for the write-back and
// the re-upload: the same surface (base and bytes), the same extent, layers and mips, the same
// element size and tile mode, and no CPU store into R's memory since R's generations. A
// "GPU-only subresource" pair is the other surface of one memory: one image lies inside the other
// (a slice or layer range of an array, a view of one mip) with the same extent, mips, element size
// and tiling, and no CPU store; a layer-range image -> image copy could stand in there. Either is
// "GPU-only copyable".
inline std::array<bool, PairFacts> ClassifyPair(const Surface& writer, const Surface& reader, bool cpuStored) {
    std::array<bool, PairFacts> facts{};
    facts[SameSurface] = writer.base == reader.base && writer.bytes == reader.bytes;
    facts[SameShape] = writer.width == reader.width && writer.height == reader.height && writer.depth == reader.depth && writer.baseArray == reader.baseArray && writer.mips == reader.mips && writer.dimension == reader.dimension;
    facts[SameFormat] = writer.format == reader.format;
    facts[SameElementBytes] = writer.elementBytes == reader.elementBytes;
    facts[SameTiling] = writer.tileMode == reader.tileMode;
    facts[AnyDcc] = writer.dcc || reader.dcc;
    facts[CpuStored] = cpuStored;
    facts[GpuOnlySameShape] = facts[SameSurface] && facts[SameShape] && facts[SameElementBytes] && facts[SameTiling] && !cpuStored;
    const auto inside = [](const Surface& inner, const Surface& outer) { return inner.base >= outer.base && inner.base + inner.bytes <= outer.base + outer.bytes; };
    facts[Nested] = inside(writer, reader) || inside(reader, writer);
    facts[SameExtent] = writer.width == reader.width && writer.height == reader.height && writer.mips == reader.mips;
    facts[GpuOnlySubresource] = !facts[SameSurface] && facts[Nested] && facts[SameExtent] && facts[SameElementBytes] && facts[SameTiling] && !cpuStored;
    facts[GpuOnlyCopyable] = facts[GpuOnlySameShape] || facts[GpuOnlySubresource];
    return facts;
}

// The pairs one 10 s window counted (StorageTexture::Refresh), with the refreshes' alias outcome:
// an alias found (pendingAlias), kept past its key and import checks, its units borrowed.
class RefreshPairs {
public:
    struct Writer {
        Surface surface;
        std::uint64_t overlap = 0;
    };
    void Note(const Surface& reader, std::span<const Writer> writers, bool cpuStored, bool aliasFound, bool aliasKept, bool borrowed) {
        std::lock_guard lock(mutex);
        ++refreshes;
        if (aliasFound) ++aliasesFound;
        if (aliasKept) ++aliasesKept;
        if (borrowed) ++aliasesBorrowed;
        for (const auto& writer : writers) {
            const auto facts = ClassifyPair(writer.surface, reader, cpuStored);
            ++pairs;
            bytes += writer.overlap;
            for (std::size_t i = 0; i < PairFacts; ++i) {
                if (!facts[i]) continue;
                ++factCounts[i];
                factBytes[i] += writer.overlap;
            }
            auto& top = tops[std::make_tuple(writer.surface.base, reader.base, writer.surface.format, reader.format)];
            ++top.count;
            top.bytes += writer.overlap;
            top.writer = writer.surface;
            top.reader = reader;
            top.gpuOnly = facts[GpuOnlySameShape] ? 1 : facts[GpuOnlySubresource] ? 2 : 0;
        }
    }
    std::string Line(std::uint64_t presents, std::size_t topPairs = 6) {
        std::lock_guard lock(mutex);
        const double per = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
        std::string line;
        Append(line, "[storage] perf-diag refresh write-backs (APS5_PERF_DIAG, per present over %llu presents): %.1f pairs %.1f MiB (overlap bytes) in %.1f refreshes with a write-back or an alias;", static_cast<unsigned long long>(presents), static_cast<double>(pairs) * per, Mib(bytes) * per, static_cast<double>(refreshes) * per);
        for (std::size_t i = 0; i < PairFacts; ++i) Append(line, " %s %.1f (%.0f%%) %.1f MiB%s", PairFactNames[i], static_cast<double>(factCounts[i]) * per, pairs != 0 ? 100.0 * static_cast<double>(factCounts[i]) / static_cast<double>(pairs) : 0.0, Mib(factBytes[i]) * per, i + 1 < PairFacts ? "," : ";");
        Append(line, " alias (same surface, borrowable shape): found %.1f, kept past keys/import %.1f, units borrowed %.1f; top pairs W -> R (format, tile, WxH depth/last array @ base array, mips, MiB):", static_cast<double>(aliasesFound) * per, static_cast<double>(aliasesKept) * per, static_cast<double>(aliasesBorrowed) * per);
        std::vector<const Top*> sorted;
        sorted.reserve(tops.size());
        for (const auto& [key, top] : tops) sorted.push_back(&top);
        std::sort(sorted.begin(), sorted.end(), [](const Top* a, const Top* b) { return a->count > b->count; });
        for (std::size_t i = 0; i < sorted.size() && i < topPairs; ++i) {
            const auto& top = *sorted[i];
            const auto surface = [](const Surface& one) {
                std::string text;
                Append(text, "0x%llx (%u, %u, %ux%u d%u@%u, %u, %.1f%s)", static_cast<unsigned long long>(one.base), one.format, one.tileMode, one.width, one.height, one.depth, one.baseArray, one.mips, Mib(one.bytes), one.dcc ? ", dcc" : "");
                return text;
            };
            Append(line, " %s -> %s x%.1f %.1f MiB%s;", surface(top.writer).c_str(), surface(top.reader).c_str(), static_cast<double>(top.count) * per, Mib(top.bytes) * per, top.gpuOnly == 1 ? " gpu-only same-shape" : top.gpuOnly == 2 ? " gpu-only subresource" : "");
        }
        Append(line, " (%zu distinct)", tops.size());
        refreshes = pairs = bytes = aliasesFound = aliasesKept = aliasesBorrowed = 0;
        factCounts = {};
        factBytes = {};
        tops.clear();
        return line;
    }

private:
    struct Top {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
        Surface writer;
        Surface reader;
        int gpuOnly = 0;
    };
    HostMutex mutex;
    std::uint64_t refreshes = 0, pairs = 0, bytes = 0, aliasesFound = 0, aliasesKept = 0, aliasesBorrowed = 0;
    std::array<std::uint64_t, PairFacts> factCounts{}, factBytes{};
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t>, Top> tops;
};

inline RefreshPairs& Refreshes() {
    static RefreshPairs* const refreshes = new RefreshPairs;
    return *refreshes;
}

// Item 5: the readers of Recorder::ReadKind in its order (Recorder.hpp), for the reader bits.
inline constexpr std::size_t ReaderKinds = 7;
inline constexpr std::size_t AddressBasedReader = 2;
inline constexpr std::array<const char*, ReaderKinds> ReaderNames{"dispatch element", "gpu copy", "address-based", "indirect", "storage upload", "copy source", "draw input"};

// Every recorded copy-back of a staged region is watched (CopyBack) until the next copy-in of an
// overlapping range (CopyIn: the next use of the range, whether it copies from the import or takes
// the previous shadow), until another copy-back over it, or for `maxAge` presents (Take). The reads
// the recorder notes in between (Recorder::NotePendingRead*: in-place readers, an address-based
// build's whole leased space, GPU copies out of the import) set the entry's reader bits. A CPU read
// of the import is not visible here. Entries are disjoint; a bit per 64 KiB block lets Read reject
// a range without the mutex. The bytes stand for the copy-back's ms (a narrow copy-back compares
// every byte of its range).
class CopyBackWatch {
public:
    enum Class : std::size_t { NeverRead, AddressBasedOnly, ReadInPlace, Classes };
    enum Close : std::size_t { ClosedByCopyIn, ClosedByAge, ClosedByCopyBack, Closes };
    struct Totals {
        std::array<std::uint64_t, Classes> count{};
        std::array<std::uint64_t, Classes> bytes{};
        std::array<std::uint64_t, Closes> closed{};
        std::array<std::uint64_t, ReaderKinds> readerBytes{};
        std::uint64_t watched = 0;
        std::uint64_t watchedBytes = 0;
        std::uint64_t open = 0;
        std::uint64_t openBytes = 0;
    };
    explicit CopyBackWatch(unsigned addressBits = 40) : blockLimit(std::uint64_t{1} << (addressBits - BlockShift)) {}
    ~CopyBackWatch() { delete[] words.load(std::memory_order_relaxed); }
    CopyBackWatch(const CopyBackWatch&) = delete;
    CopyBackWatch& operator=(const CopyBackWatch&) = delete;

    void CopyBack(std::uint64_t begin, std::uint64_t end, std::uint64_t present) {
        if (end <= begin) return;
        std::lock_guard lock(mutex);
        if (words.load(std::memory_order_relaxed) == nullptr) words.store(new std::atomic<std::uint64_t>[(blockLimit + 63) / 64](), std::memory_order_release);
        closeOverlapping(begin, end, ClosedByCopyBack);
        entries.emplace(begin, Entry{end, present, ++serial, 0});
        ++totals.watched;
        totals.watchedBytes += end - begin;
        setBlocks(begin, end);
    }
    void Read(std::uint64_t begin, std::uint64_t end, std::size_t kind) {
        if (end <= begin || kind >= ReaderKinds || !marked(begin, end)) return;
        std::lock_guard lock(mutex);
        for (auto it = firstOverlapping(begin); it != entries.end() && it->first < end; ++it) {
            if (it->second.end > begin) it->second.readers |= static_cast<std::uint8_t>(1u << kind);
        }
    }
    // A shared read list (Recorder::NotePendingReadSet: sorted, merged; an address-based build's
    // thousands of ranges, noted per dispatch): tested against the entries made since the list's
    // last note, one binary search each. The lists seen are kept until Take (so a freed list's
    // address is not taken for another's).
    void ReadSet(const std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>>& ranges, std::size_t kind) {
        if (ranges == nullptr || ranges->empty() || kind >= ReaderKinds) return;
        std::lock_guard lock(mutex);
        auto& seen = sets[{ranges.get(), kind}];
        if (seen.first == nullptr) seen.first = ranges;
        if (seen.second == serial) return;
        const auto since = seen.second;
        seen.second = serial;
        for (auto& [begin, entry] : entries) {
            if (entry.serial <= since) continue;
            const auto first = std::upper_bound(ranges->begin(), ranges->end(), begin, [](std::uint64_t at, const std::pair<std::uint64_t, std::uint64_t>& range) { return at < range.second; });
            if (first != ranges->end() && first->first < entry.end) entry.readers |= static_cast<std::uint8_t>(1u << kind);
        }
    }
    void CopyIn(std::uint64_t begin, std::uint64_t end) {
        if (end <= begin || !marked(begin, end)) return;
        std::lock_guard lock(mutex);
        closeOverlapping(begin, end, ClosedByCopyIn);
    }
    // The window's totals (entries older than `maxAge` presents closed first), then a fresh window.
    Totals Take(std::uint64_t present, std::uint64_t maxAge) {
        std::lock_guard lock(mutex);
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.present + maxAge >= present) {
                ++it;
                continue;
            }
            const auto begin = it->first, end = it->second.end;
            classify(it->first, it->second, ClosedByAge);
            it = entries.erase(it);
            clearBlocks(begin, end);
        }
        sets.clear();
        auto taken = totals;
        taken.open = entries.size();
        for (const auto& [begin, entry] : entries) taken.openBytes += entry.end - begin;
        totals = Totals{};
        return taken;
    }
    static std::string Line(const Totals& totals, std::uint64_t presents, std::uint64_t maxAge) {
        const double per = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
        std::uint64_t closedBytes = 0;
        for (const auto bytes : totals.bytes) closedBytes += bytes;
        const auto share = [&](std::uint64_t bytes) { return closedBytes != 0 ? 100.0 * static_cast<double>(bytes) / static_cast<double>(closedBytes) : 0.0; };
        std::string line;
        Append(line, "[perf-diag] copy-backs (APS5_PERF_DIAG, per present over %llu presents): watched x%.1f %.1f MiB; closed by the next copy-in x%.1f, by another copy-back x%.1f, after %llu presents x%.1f;", static_cast<unsigned long long>(presents), static_cast<double>(totals.watched) * per, Mib(totals.watchedBytes) * per, static_cast<double>(totals.closed[ClosedByCopyIn]) * per, static_cast<double>(totals.closed[ClosedByCopyBack]) * per, static_cast<unsigned long long>(maxAge), static_cast<double>(totals.closed[ClosedByAge]) * per);
        Append(line, " never read x%.1f %.1f MiB (%.1f%% of closed bytes), read only by address-based builds x%.1f %.1f MiB (%.1f%%), read in place x%.1f %.1f MiB (%.1f%%);", static_cast<double>(totals.count[NeverRead]) * per, Mib(totals.bytes[NeverRead]) * per, share(totals.bytes[NeverRead]), static_cast<double>(totals.count[AddressBasedOnly]) * per, Mib(totals.bytes[AddressBasedOnly]) * per, share(totals.bytes[AddressBasedOnly]), static_cast<double>(totals.count[ReadInPlace]) * per, Mib(totals.bytes[ReadInPlace]) * per, share(totals.bytes[ReadInPlace]));
        line += " read by kind (MiB):";
        for (std::size_t kind = 0; kind < ReaderKinds; ++kind) Append(line, " %s %.1f", ReaderNames[kind], Mib(totals.readerBytes[kind]) * per);
        Append(line, "; open %llu (%.1f MiB)", static_cast<unsigned long long>(totals.open), Mib(totals.openBytes));
        return line;
    }

private:
    static constexpr unsigned BlockShift = 16;
    struct Entry {
        std::uint64_t end;
        std::uint64_t present;
        std::uint64_t serial;
        std::uint8_t readers;
    };
    using Entries = std::map<std::uint64_t, Entry>;

    // The first entry that may overlap [begin, ...): entries are disjoint and sorted, so only the
    // last one beginning at or below `begin` can reach past it.
    Entries::iterator firstOverlapping(std::uint64_t begin) {
        auto it = entries.upper_bound(begin);
        if (it != entries.begin()) {
            auto previous = std::prev(it);
            if (previous->second.end > begin) return previous;
        }
        return it;
    }
    void classify(std::uint64_t begin, const Entry& entry, Close how) {
        const auto bytes = entry.end - begin;
        const auto which = entry.readers == 0 ? NeverRead : entry.readers == (1u << AddressBasedReader) ? AddressBasedOnly : ReadInPlace;
        ++totals.count[which];
        totals.bytes[which] += bytes;
        ++totals.closed[how];
        for (std::size_t kind = 0; kind < ReaderKinds; ++kind) {
            if ((entry.readers & (1u << kind)) != 0) totals.readerBytes[kind] += bytes;
        }
    }
    void closeOverlapping(std::uint64_t begin, std::uint64_t end, Close how) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> closed;
        for (auto it = firstOverlapping(begin); it != entries.end() && it->first < end;) {
            if (it->second.end <= begin) {
                ++it;
                continue;
            }
            classify(it->first, it->second, how);
            closed.emplace_back(it->first, it->second.end);
            it = entries.erase(it);
        }
        for (const auto& [from, to] : closed) clearBlocks(from, to);
    }
    std::pair<std::uint64_t, std::uint64_t> blocks(std::uint64_t begin, std::uint64_t end) const {
        const auto first = begin >> BlockShift;
        const auto last = std::min((end - 1) >> BlockShift, blockLimit - 1);
        return {first, last};
    }
    bool marked(std::uint64_t begin, std::uint64_t end) const {
        const auto* bits = words.load(std::memory_order_acquire);
        if (bits == nullptr) return false;
        const auto [first, last] = blocks(begin, end);
        for (auto block = first; block <= last && block < blockLimit;) {
            const auto word = block >> 6u;
            const auto top = std::min(last, (word << 6u) + 63);
            const auto low = block & 63u, high = top & 63u;
            const auto mask = (~std::uint64_t{0} >> (63u - high)) & (~std::uint64_t{0} << low);
            if ((bits[word].load(std::memory_order_relaxed) & mask) != 0) return true;
            block = top + 1;
        }
        return false;
    }
    void setBlocks(std::uint64_t begin, std::uint64_t end) {
        auto* bits = words.load(std::memory_order_relaxed);
        const auto [first, last] = blocks(begin, end);
        for (auto block = first; block <= last && block < blockLimit; ++block) bits[block >> 6u].fetch_or(std::uint64_t{1} << (block & 63u), std::memory_order_relaxed);
    }
    // A block stays marked while another entry still covers part of it.
    void clearBlocks(std::uint64_t begin, std::uint64_t end) {
        auto* bits = words.load(std::memory_order_relaxed);
        if (bits == nullptr) return;
        const auto [first, last] = blocks(begin, end);
        for (auto block = first; block <= last && block < blockLimit; ++block) {
            const auto from = block << BlockShift, to = (block + 1) << BlockShift;
            auto it = entries.lower_bound(to);
            const bool covered = it != entries.begin() && std::prev(it)->second.end > from;
            if (!covered) bits[block >> 6u].fetch_and(~(std::uint64_t{1} << (block & 63u)), std::memory_order_relaxed);
        }
    }

    std::uint64_t blockLimit;
    std::atomic<std::atomic<std::uint64_t>*> words{nullptr};
    HostMutex mutex;
    Entries entries;
    Totals totals;
    std::uint64_t serial = 0;
    std::map<std::pair<const void*, std::size_t>, std::pair<std::shared_ptr<const void>, std::uint64_t>> sets;
};

inline CopyBackWatch& Watch() {
    static CopyBackWatch* const watch = new CopyBackWatch;
    return *watch;
}

}

#endif
