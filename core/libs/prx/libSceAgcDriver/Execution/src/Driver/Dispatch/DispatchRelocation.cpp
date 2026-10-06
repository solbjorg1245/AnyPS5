#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <cstring>

// Relocated dispatch-cache hits. Per-object dispatches (0x248994d00 and its siblings, ~1000 per
// frame in Boletaria) read a per-frame constant block through a pointer the game moves every frame
// (a ring of frame allocations), so every key missed once per frame on 'runs changed' and was
// captured and recompiled again although the walk and its result were the same. A miss whose fresh
// capture differs from the stored variant only by runs moved by one delta D and by the 64-bit
// pointers leading there (stored + D == fresh), with the same compiled variant and bindings outside
// the flat-SRT data slots, teaches the variant a relocation rule; a later lookup that matches no
// variant reads those pointers, shifts the moved runs by their common delta and validates the
// shifted variant like any other (data words masked), so the walk is not repeated.
// APS5_NO_RELOCATED_HITS=1 disables it; APS5_VERIFY_DATA_HITS=1 also captures every relocated hit
// again and aborts on a difference.

namespace AgcDriver::DriverDetail {

namespace {

std::uint64_t pointerAt(std::span<const std::uint32_t> words, std::size_t position) {
    return static_cast<std::uint64_t>(words[position]) | (static_cast<std::uint64_t>(words[position + 1]) << 32u);
}

// The descriptor words at `w` and `w + 1` with the address they hold moved by `delta`: a flat-SRT
// pointer (64 bits) or a buffer V# starting at `w` (48-bit base in word 0 and the low half of word 1).
std::pair<std::uint32_t, std::uint32_t> ShiftDescriptorAddress(std::span<const std::uint32_t> words, std::size_t w, bool flat, std::uint64_t delta) {
    if (flat) {
        const auto moved = pointerAt(words, w) + delta;
        return {static_cast<std::uint32_t>(moved), static_cast<std::uint32_t>(moved >> 32u)};
    }
    const auto base = (static_cast<std::uint64_t>(words[w]) | (static_cast<std::uint64_t>(words[w + 1] & 0xffffu) << 32u)) + delta;
    return {static_cast<std::uint32_t>(base), (words[w + 1] & 0xffff0000u) | static_cast<std::uint32_t>((base >> 32u) & 0xffffu)};
}

// Debug aid APS5_TRACE_RELOCATION=<seconds>: the first refusals and failed relocations from that
// many seconds after the first dispatch on are described.
bool traceRelocation() {
    static const char* text = std::getenv("APS5_TRACE_RELOCATION");
    if (text == nullptr) return false;
    static const auto from = std::chrono::steady_clock::now() + std::chrono::seconds(std::strtoull(text, nullptr, 10));
    return std::chrono::steady_clock::now() >= from;
}

std::string describeRuns(const DispatchVariant& variant) {
    std::string text;
    char item[48];
    for (const auto& [begin, end] : variant.runs) {
        std::snprintf(item, sizeof(item), " %llx+%llx", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
        text += item;
    }
    return text;
}

bool dataPosition(const DispatchVariant& variant, std::size_t position) {
    return std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), static_cast<std::uint32_t>(position));
}

}

bool Driver::relocatedHits() {
    static const bool enabled = std::getenv("APS5_NO_RELOCATED_HITS") == nullptr && dataHits();
    return enabled;
}

RelocationVerdict Driver::learnRelocation(const DispatchVariant& old, DispatchVariant& fresh) {
    if (old.runs.size() != fresh.runs.size() || old.words.size() != fresh.words.size()) return RelocationVerdict::Shape;
    if (old.compiled == nullptr || fresh.compiled == nullptr || old.flatBinding != fresh.flatBinding || old.dataPositions != fresh.dataPositions || old.dataSlots != fresh.dataSlots) return RelocationVerdict::DataPositions;
    std::uint64_t delta = 0;
    std::vector<std::uint32_t> moved;

    std::vector<std::uint32_t> runOf(fresh.words.size());
    std::size_t offset = 0;
    for (std::size_t i = 0; i < fresh.runs.size(); ++i) {
        const auto& [oldBegin, oldEnd] = old.runs[i];
        const auto& [begin, end] = fresh.runs[i];
        if (oldEnd - oldBegin != end - begin) return RelocationVerdict::Shape;
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        std::fill_n(runOf.begin() + static_cast<std::ptrdiff_t>(offset), count, static_cast<std::uint32_t>(i));
        offset += count;
        if (begin == oldBegin) continue;
        if (delta != 0 && begin - oldBegin != delta) {
            static std::atomic<int> traced{0};
            if (traceRelocation() && traced.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr, "[relocation] deltas: stored%s\n", describeRuns(old).c_str());
                std::fprintf(stderr, "[relocation] deltas: fresh %s\n", describeRuns(fresh).c_str());
            }
            return RelocationVerdict::Deltas;
        }
        delta = begin - oldBegin;
        moved.push_back(static_cast<std::uint32_t>(i));
    }
    if (moved.empty()) return RelocationVerdict::NothingMoved;

    // Every differing word outside the moved runs (data words aside) must belong to a pointer that
    // moved by the delta, held in one run.
    std::vector<std::uint32_t> pointers;
    for (std::size_t p = 0; p < fresh.words.size(); ++p) {
        if (old.words[p] == fresh.words[p] || dataPosition(fresh, p)) continue;
        if (std::binary_search(moved.begin(), moved.end(), runOf[p])) continue;
        if (!pointers.empty() && (pointers.back() == p || pointers.back() + 1 == p)) continue;
        const auto fits = [&](std::size_t low) { return low + 1 < fresh.words.size() && runOf[low] == runOf[low + 1] && pointerAt(fresh.words, low) - pointerAt(old.words, low) == delta && !dataPosition(fresh, low) && !dataPosition(fresh, low + 1); };
        if (fits(p)) pointers.push_back(static_cast<std::uint32_t>(p));
        else if (p > 0 && fits(p - 1)) pointers.push_back(static_cast<std::uint32_t>(p - 1));
        else return RelocationVerdict::OtherWords;
    }
    if (pointers.empty()) return RelocationVerdict::NoPointer;

    // The compiled result may depend on the moved addresses only additively: the same variant and
    // bindings, except the flat-SRT data slots, words holding an address inside the moved runs
    // (a flat-SRT pointer's 64 bits or a buffer V#'s 48-bit base, shifted by the delta: patched on
    // relocation) and, only when the moved runs' own non-data words changed, words derived from them
    // (equal again whenever a relocation validates those words).
    bool contentChanged = false;
    for (std::size_t p = 0; p < fresh.words.size() && !contentChanged; ++p) {
        contentChanged = old.words[p] != fresh.words[p] && !dataPosition(fresh, p) && std::binary_search(moved.begin(), moved.end(), runOf[p]);
    }
    // An address the walk read from the moved runs' contents (a V# stored in the moved block) is
    // content even when it moved by the delta too (frame allocations move together).
    const auto inMovedRuns = [&](const DispatchVariant& variant, std::uint32_t value) {
        for (std::size_t p = 0; p < variant.words.size(); ++p) {
            if (variant.words[p] == value && std::binary_search(moved.begin(), moved.end(), runOf[p])) return true;
        }
        return false;
    };
    const auto& a = *old.compiled;
    const auto& b = *fresh.compiled;
    if (a.variantId == 0 || a.variantId != b.variantId || a.bindings.size() != b.bindings.size() || a.pushConstants != b.pushConstants) return RelocationVerdict::Compiled;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shifts;
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        const auto& x = a.bindings[i];
        const auto& y = b.bindings[i];
        if (x.kind != y.kind || x.role != y.role || x.binding != y.binding || x.count != y.count || x.guestDescriptor.size() != y.guestDescriptor.size()) return RelocationVerdict::Compiled;
        const auto& from = x.guestDescriptor;
        const auto& to = y.guestDescriptor;
        for (std::size_t w = 0; w < from.size(); ++w) {
            if (from[w] == to[w]) continue;
            if (i == fresh.flatBinding && std::find(fresh.dataSlots.begin(), fresh.dataSlots.end(), static_cast<std::uint32_t>(w)) != fresh.dataSlots.end()) continue;
            if (w + 1 < from.size() && (i == fresh.flatBinding || w % 4 == 0) && !inMovedRuns(old, from[w]) && !inMovedRuns(fresh, to[w]) && ShiftDescriptorAddress(from, w, i == fresh.flatBinding, delta) == std::pair{to[w], to[w + 1]}) {
                shifts.emplace_back(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(w));
                ++w;
                continue;
            }
            if (contentChanged) continue;
            // Debug aid APS5_TRACE_RELOCATION=1: the first refusals on descriptors show the binding words.
            const bool trace = traceRelocation();
            static std::atomic<int> traced{0};
            if (trace && traced.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::string text;
                char item[96];
                for (std::size_t k = 0; k < from.size() && k < 64; ++k) {
                    if (from[k] == to[k]) continue;
                    std::snprintf(item, sizeof(item), " [%zu] %08x->%08x", k, from[k], to[k]);
                    text += item;
                }
                std::fprintf(stderr, "[relocation] delta 0x%llx binding %zu (role %d kind %d count %u, flat %u, %zu words):%s\n", static_cast<unsigned long long>(delta), i, static_cast<int>(x.role), static_cast<int>(x.kind), x.count, fresh.flatBinding, from.size(), text.c_str());
            }
            return RelocationVerdict::Descriptors;
        }
    }
    fresh.pointerPositions = std::move(pointers);
    fresh.movedRuns = std::move(moved);
    fresh.shiftSlots = std::move(shifts);
    return RelocationVerdict::Learned;
}

std::shared_ptr<DispatchVariant> Driver::relocateVariant(const DispatchVariant& variant, std::uint64_t& counterUnordered) {
    if (variant.pointerPositions.empty()) return nullptr;
    std::vector<std::uint64_t> starts;
    starts.reserve(variant.runs.size());
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        starts.push_back(offset);
        offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    std::uint64_t delta = 0;
    auto words = variant.words;
    for (const auto position : variant.pointerPositions) {
        const auto run = static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(), position) - starts.begin() - 1);
        const auto address = variant.runs[run].first + (position - starts[run]) * sizeof(std::uint32_t);
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(std::uint64_t))) return nullptr;
        std::uint64_t live = 0;
        std::memcpy(&live, reinterpret_cast<const void*>(address), sizeof(live));
        const auto moved = live - pointerAt(variant.words, position);
        if (moved == 0 || (delta != 0 && moved != delta)) return nullptr;
        delta = moved;
        words[position] = static_cast<std::uint32_t>(live);
        words[position + 1] = static_cast<std::uint32_t>(live >> 32u);
    }
    auto runs = variant.runs;
    for (const auto run : variant.movedRuns) {
        runs[run].first += delta;
        runs[run].second += delta;
    }
    for (std::size_t i = 1; i < runs.size(); ++i) {
        if (runs[i].first < runs[i - 1].second) {
            ++counterUnordered;
            return nullptr;
        }
    }
    // A capture's runs are the maximal read runs within each ShaderMemory page: merge shifted runs
    // that now touch inside a page and split those that now cross one. The words keep their order.
    constexpr std::uint64_t page = 4096;  // ShaderMemory::PageBytes
    std::vector<std::pair<std::uint64_t, std::uint64_t>> layout;
    layout.reserve(runs.size() + variant.movedRuns.size());
    for (auto [begin, end] : runs) {
        if (!layout.empty() && layout.back().second == begin && begin % page != 0) {
            begin = layout.back().first;
            layout.pop_back();
        }
        while ((begin & ~(page - 1)) != ((end - 1) & ~(page - 1))) {
            const auto boundary = (begin & ~(page - 1)) + page;
            layout.emplace_back(begin, boundary);
            begin = boundary;
        }
        layout.emplace_back(begin, end);
    }
    // The rule names runs by index, so it moves on with the variant only when the layout kept them.
    const bool keepRule = layout.size() == runs.size();
    runs = std::move(layout);
    auto compiled = variant.compiled;
    if (!variant.shiftSlots.empty()) {
        auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
        for (const auto& [binding, w] : variant.shiftSlots) {
            auto& descriptor = patched->bindings[binding].guestDescriptor;
            std::tie(descriptor[w], descriptor[w + 1]) = ShiftDescriptorAddress(descriptor, w, binding == variant.flatBinding, delta);
        }
        compiled = std::move(patched);
    }
    auto relocated = std::make_shared<DispatchVariant>();
    relocated->runs = std::move(runs);
    relocated->words = std::move(words);
    relocated->forgetSerial = GuestMemory::ForgetSerial();
    relocated->compiled = std::move(compiled);
    relocated->shader = variant.shader;
    relocated->dataPositions = variant.dataPositions;
    relocated->dataSlots = variant.dataSlots;
    relocated->flatBinding = variant.flatBinding;
    if (keepRule) {
        relocated->pointerPositions = variant.pointerPositions;
        relocated->movedRuns = variant.movedRuns;
        relocated->shiftSlots = variant.shiftSlots;
    }
    return relocated;
}

void Driver::traceFailedRelocation(std::uint64_t program, const DispatchVariant& candidate) {
    static std::atomic<int> traced{0};
    if (!traceRelocation() || traced.fetch_add(1, std::memory_order_relaxed) >= 12) return;
    std::string text;
    char item[80];
    std::size_t offset = 0;
    int shown = 0;
    for (std::size_t r = 0; r < candidate.runs.size(); ++r) {
        const auto [begin, end] = candidate.runs[r];
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        const bool moved = std::find(candidate.movedRuns.begin(), candidate.movedRuns.end(), static_cast<std::uint32_t>(r)) != candidate.movedRuns.end();
        for (std::size_t k = 0; k < count; ++k) {
            const auto address = begin + k * sizeof(std::uint32_t);
            if (dataPosition(candidate, offset + k) || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(std::uint32_t))) continue;
            std::uint32_t live = 0;
            std::memcpy(&live, reinterpret_cast<const void*>(address), sizeof(live));
            if (live == candidate.words[offset + k] || ++shown > 12) continue;
            std::snprintf(item, sizeof(item), " run %zu%s %llx: %08x->%08x", r, moved ? "(moved)" : "", static_cast<unsigned long long>(address), candidate.words[offset + k], live);
            text += item;
        }
        offset += count;
    }
    std::fprintf(stderr, "[relocation] program 0x%llx failed (%d non-data words differ):%s; runs%s\n", static_cast<unsigned long long>(program), shown, text.c_str(), describeRuns(candidate).c_str());
}

}
