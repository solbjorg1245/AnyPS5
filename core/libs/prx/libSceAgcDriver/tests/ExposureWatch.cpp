#include "prx/libSceAgcDriver/Execution/include/Driver/ExposureWatch.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace AgcDriver::ExposureWatch;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void putWord(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t word) {
    std::memcpy(bytes.data() + offset, &word, sizeof(word));
}

void putReal(std::vector<std::byte>& bytes, std::size_t offset, float value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void putBuffer(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t address, std::uint32_t stride, std::uint32_t records) {
    putWord(bytes, offset, static_cast<std::uint32_t>(address));
    putWord(bytes, offset + 4, static_cast<std::uint32_t>(address >> 32u) | (stride << 16u) | 0x80000000u);
    putWord(bytes, offset + 8, records);
    putWord(bytes, offset + 12, 0x2f00f000u);
}

void tableTests() {
    std::vector<std::byte> bytes(Table::Bytes);
    putWord(bytes, 0x00, 32400);
    putWord(bytes, 0x04, 1620);
    putWord(bytes, 0x08, 25920);
    putWord(bytes, 0x14, 3);
    putWord(bytes, 0x18, 7);
    putReal(bytes, 0x20, 0.0f);
    putReal(bytes, 0x24, -4.0f);
    putReal(bytes, 0x28, 6.5f);
    putWord(bytes, 0x40, 2);
    putWord(bytes, 0x44, 4);
    putReal(bytes, 0x50, 0.25f);
    putReal(bytes, 0x54, 0.125f);
    putReal(bytes, 0x58, 0.5f);
    putReal(bytes, 0x5c, -1.0f);
    putBuffer(bytes, 0x60, 0x248ca5480ull, 4, 32768);
    putBuffer(bytes, 0x70, 0x248ca5480ull, 4, 32768);
    putBuffer(bytes, 0x80, 0x248cb5480ull, 0, 64064);
    putBuffer(bytes, 0x90, 0x248ca5480ull, 4, 1);
    putBuffer(bytes, 0xa0, 0x33a600000ull, 0, 80);
    putBuffer(bytes, 0xb0, 0x24ec7fc90ull, 4, 4);
    const auto table = DecodeTable(bytes.data());
    Expect(table.percentileCount == 32400 && table.sortedOffset == 1620 && table.sortedCount == 25920, "table counts");
    Expect(table.secondOffset == 3 && table.secondCount == 7 && table.historyIndex == 2 && table.historyCount == 4, "table second list and ring");
    Expect(table.key == 1e-5f, "a zero key is raised to 1e-5 as the program does");
    Expect(table.minEv == -4.0f && table.maxEv == 6.5f, "EV clamp");
    Expect(table.speedDown == 0.25f && table.speedUp == 0.125f && table.targetBias == 0.5f && table.exposureBias == -1.0f, "speeds and biases");
    Expect(table.sorted.address == 0x248ca5480ull && table.sorted.stride == 4 && table.sorted.records == 32768, "sorted V#");
    Expect(table.state.address == 0x33a600000ull && table.state.stride == 0 && table.state.records == 80, "state V# (raw)");
    Expect(table.history.address == 0x24ec7fc90ull && table.history.records == 4, "history V#");
    Expect(table.sorted.Elements(4, 1u << 16u) == 32768 && table.sorted.Step(4) == 4, "structured elements");
    Expect(table.state.Elements(4, 1u << 16u) == 20 && table.state.Step(4) == 4, "raw buffer elements count bytes");
    Expect(table.sorted.Elements(4, 1000) == 1000, "element cap");
}

void listTests() {
    // Sorted ascending, one huge garbage value at the end: the window excludes it.
    std::vector<float> values;
    for (int index = 0; index < 100; ++index) values.push_back(0.001f * static_cast<float>(index + 1));
    values.push_back(61888.0f);
    auto stats = Summarize(values, 10, 80);
    Expect(stats.count == 101 && stats.nan == 0 && stats.inf == 0 && stats.negative == 0, "clean counts");
    Expect(stats.descents == 0, "a sorted list has no descents");
    Expect(stats.huge == 1, "the 61888 tile is huge against the median");
    Expect(stats.max == 61888.0f && stats.min == 0.001f, "range");
    Expect(std::fabs(stats.median - 0.051f) < 1e-6f, "median");
    Expect(std::fabs(stats.windowMean - 0.0505f) < 1e-5f, "window mean (elements 10..89 = 0.011..0.090)");
    Expect(stats.zeros == 0 && stats.firstNonZero == 0 && stats.lastNonZero == 100 && stats.windowNonZero == 80, "no zeros");
    // The game's list at 2560x1440: 32400 slots, the tiles outside the image are 0 and sort first.
    values.assign(32400, 0.0f);
    for (int index = 18000; index < 32400; ++index) values[index] = 0.01f;
    stats = Summarize(values, 4319, 7921);
    Expect(stats.zeros == 18000 && stats.firstNonZero == 18000 && stats.lastNonZero == 32399, "zero run");
    Expect(stats.windowNonZero == 0 && stats.windowMean == 0, "the window inside the zero run");
    Expect(stats.median == 0.0f && stats.descents == 0, "median of a half-zero list");
    // Unsorted, non-finite and negative values.
    values = {0.5f, 0.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -2.0f, 0.3f};
    stats = Summarize(values, 0, 6);
    Expect(stats.nan == 1 && stats.inf == 1 && stats.negative == 1, "non-finite and negative counts");
    Expect(stats.descents == 2, "descents: 0.5>0.1 and inf>-2 (comparisons with NaN are false)");
    Expect(stats.min == -2.0f && stats.max == 0.5f, "finite range");
    Expect(std::fabs(stats.windowMean - (0.5f + 0.1f - 2.0f + 0.3f) / 4.0f) < 1e-6f, "window mean over finite values");
    // Empty and all non-finite lists.
    stats = Summarize({}, 0, 0);
    Expect(stats.count == 0 && stats.windowMean == 0 && stats.median == 0, "empty list");
    values = {std::numeric_limits<float>::quiet_NaN()};
    stats = Summarize(values, 0, 1);
    Expect(stats.nan == 1 && stats.huge == 0 && stats.mean == 0, "only NaN");
}

void jumpTests() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Expect(!Jumped(1.0f, 1.4f, 1.5f), "1.4x is not a jump");
    Expect(Jumped(1.0f, 1.6f, 1.5f), "1.6x up is a jump");
    Expect(Jumped(1.0f, 0.6f, 1.5f), "1.67x down is a jump");
    Expect(Jumped(1.0f, nan, 1.5f) && Jumped(nan, 1.0f, 1.5f), "into and out of NaN");
    Expect(!Jumped(nan, nan, 1.5f), "NaN to NaN is not a new jump");
    Expect(Jumped(1.0f, 0.0f, 1.5f) && !Jumped(0.0f, 0.0f, 1.5f), "to zero once");
}

}

int main() {
    tableTests();
    listTests();
    jumpTests();
    if (failures != 0) {
        std::fprintf(stderr, "%d exposure watch test(s) failed\n", failures);
        return 1;
    }
    std::printf("exposure watch tests passed\n");
    return 0;
}
