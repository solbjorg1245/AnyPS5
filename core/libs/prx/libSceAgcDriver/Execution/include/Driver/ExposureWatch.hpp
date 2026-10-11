#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_EXPOSUREWATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_EXPOSUREWATCH_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace AgcDriver::ExposureWatch {

// Debug aid (APS5_EXPOSURE_WATCH=1; off, nothing runs): follows Demon's Souls' auto exposure (R7,
// dark scene with the HUD intact). Per frame the game sorts per-tile luminance (cs ead34ad3 writes
// one Rec.709 luminance per 16x16 tile, non-finite texels count 0; cs 79ab2667 sorts the list), then
// cs 0x2488a1200 (fnv 0249ec9a) averages a window of the sorted list into a history ring, averages the
// ring, takes log2 against a key, clamps it, adapts the previous exposure toward it and stores the
// result in a 20-float state buffer that the tone map reads. AtDispatch decodes that dispatch's table
// (APS5_EXPOSURE_WATCH_CS=<hex address> names another program); AtPresent reads the state, the ring
// and the sorted list from guest memory (what the last landed batch left there, a frame or two
// behind the GPU) and prints an [exposure] line when the exposure moves by more than
// APS5_EXPOSURE_WATCH_RATIO (default 1.5x) between presents or goes non-finite, with the previous
// presents' records (=2: a line per present); under APS5_PROFILE_DRAW a 10 s digest line;
// APS5_EXPOSURE_WATCH_DUMP=<dir> also writes the list and the state at jumps and digests.
bool Enabled();
// The packet loop, after a dispatch packet: decodes the table when the program is the watched one.
void AtDispatch(const QueueState& queue, std::uint32_t queueId);
// The presenter, after the present (no lock held).
void AtPresent();

// A buffer resource (V#): base address, stride and record count.
struct Buffer {
    std::uint64_t address = 0;
    std::uint32_t stride = 0;
    std::uint32_t records = 0;

    // Elements of `elementBytes` the descriptor covers (a raw buffer counts bytes), capped.
    std::uint32_t Elements(std::uint32_t elementBytes, std::uint32_t cap) const {
        const auto count = stride == 0 ? records / elementBytes : records;
        return std::min(count, cap);
    }
    std::uint32_t Step(std::uint32_t elementBytes) const { return stride == 0 ? elementBytes : stride; }
};

inline Buffer DecodeBuffer(const std::uint32_t* words) {
    Buffer buffer;
    buffer.address = static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u);
    buffer.stride = (words[1] >> 16u) & 0x3fffu;
    buffer.records = words[2];
    return buffer;
}

// The exposure program's resource table (byte offsets as the program's scalar loads read them).
struct Table {
    static constexpr std::size_t Bytes = 0xc0;
    Buffer sorted;       // +0x60: sorted tile luminance; the window [sortedOffset, +sortedCount) is averaged
    Buffer percentiles;  // +0x70: read at (n-1)/2, n-1, 90 % and 75 % of n-1 (n = percentileCount)
    Buffer second;       // +0x80: a second averaged list [secondOffset, +secondCount)
    Buffer first;        // +0x90: element 0 is stored (x1000) into the state
    Buffer state;        // +0xa0: 20 floats, [0] the exposure the tone map applies
    Buffer history;      // +0xb0: ring of window means (x1000), historyCount entries
    std::uint32_t percentileCount = 0, sortedOffset = 0, sortedCount = 0, secondOffset = 0, secondCount = 0, historyIndex = 0, historyCount = 0;
    float key = 0, minEv = 0, maxEv = 0, speedDown = 0, speedUp = 0, targetBias = 0, exposureBias = 0;
};

inline Table DecodeTable(const std::byte* bytes) {
    std::uint32_t words[Table::Bytes / 4];
    std::memcpy(words, bytes, sizeof(words));
    const auto real = [&](std::size_t offset) {
        float value = 0;
        std::memcpy(&value, &words[offset / 4], sizeof(value));
        return value;
    };
    Table table;
    table.percentileCount = words[0x00 / 4];
    table.sortedOffset = words[0x04 / 4];
    table.sortedCount = words[0x08 / 4];
    table.secondOffset = words[0x14 / 4];
    table.secondCount = words[0x18 / 4];
    table.key = std::max(1e-5f, real(0x20));
    table.minEv = real(0x24);
    table.maxEv = real(0x28);
    table.historyIndex = words[0x40 / 4];
    table.historyCount = words[0x44 / 4];
    table.speedDown = real(0x50);
    table.speedUp = real(0x54);
    table.targetBias = real(0x58);
    table.exposureBias = real(0x5c);
    table.sorted = DecodeBuffer(&words[0x60 / 4]);
    table.percentiles = DecodeBuffer(&words[0x70 / 4]);
    table.second = DecodeBuffer(&words[0x80 / 4]);
    table.first = DecodeBuffer(&words[0x90 / 4]);
    table.state = DecodeBuffer(&words[0xa0 / 4]);
    table.history = DecodeBuffer(&words[0xb0 / 4]);
    return table;
}

// One look at a luminance list: non-finite and negative counts, the finite range and mean, the mean
// of the window the program averages, adjacent pairs out of ascending order (a sorted list has 0) and
// elements above 1000x the finite median (texels no lit scene produces: garbage halfs saturate near
// 65504).
struct ListStats {
    std::uint32_t count = 0, nan = 0, inf = 0, negative = 0, descents = 0, huge = 0, zeros = 0, windowNonZero = 0, firstNonZero = 0, lastNonZero = 0;
    float min = 0, max = 0, mean = 0, median = 0, windowMean = 0;
};

inline ListStats Summarize(std::span<const float> values, std::uint32_t windowOffset, std::uint32_t windowCount) {
    ListStats stats;
    stats.count = static_cast<std::uint32_t>(values.size());
    double sum = 0, window = 0;
    std::uint32_t finite = 0, windowed = 0;
    bool first = true, nonZero = false;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const float value = values[index];
        if (std::isnan(value)) ++stats.nan;
        else if (std::isinf(value)) ++stats.inf;
        if (index != 0 && values[index - 1] > value) ++stats.descents;
        if (!std::isfinite(value)) continue;
        if (value < 0) ++stats.negative;
        if (value == 0) {
            ++stats.zeros;
        } else {
            if (!nonZero) stats.firstNonZero = static_cast<std::uint32_t>(index);
            nonZero = true;
            stats.lastNonZero = static_cast<std::uint32_t>(index);
            if (index >= windowOffset && index - windowOffset < windowCount) ++stats.windowNonZero;
        }
        stats.min = first ? value : std::min(stats.min, value);
        stats.max = first ? value : std::max(stats.max, value);
        first = false;
        sum += value;
        ++finite;
        if (index >= windowOffset && index - windowOffset < windowCount) {
            window += value;
            ++windowed;
        }
    }
    stats.mean = finite != 0 ? static_cast<float>(sum / finite) : 0.0f;
    stats.windowMean = windowed != 0 ? static_cast<float>(window / windowed) : 0.0f;
    if (finite == 0) return stats;
    // The median by selection over a copy of the finite values (a list is at most 64 Ki floats).
    static thread_local std::vector<float> scratch;
    scratch.clear();
    for (const float value : values) if (std::isfinite(value)) scratch.push_back(value);
    const auto middle = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
    std::nth_element(scratch.begin(), middle, scratch.end());
    stats.median = *middle;
    const float limit = std::max(stats.median, 1e-6f) * 1000.0f;
    for (const float value : scratch) if (value > limit) ++stats.huge;
    return stats;
}

// Whether the exposure moved by more than `ratio` (either way) or became, or stopped being, non-finite.
inline bool Jumped(float previous, float now, float ratio) {
    if (!std::isfinite(previous) || !std::isfinite(now)) return std::isfinite(previous) != std::isfinite(now);
    if (previous <= 0 || now <= 0) return (previous <= 0) != (now <= 0);
    return now > previous * ratio || previous > now * ratio;
}

}

#endif
