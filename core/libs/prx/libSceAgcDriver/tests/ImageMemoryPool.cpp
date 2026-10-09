#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageMemoryPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

void RunImageMemoryPoolTests() {
    using AgcDriver::Graphics::RangeAllocator;
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(std::string("image memory pool test: ") + message);
    };
    RangeAllocator ranges(1024);
    const auto a = ranges.Allocate(100, 1);
    require(a && *a == 0, "first allocation at the start");
    const auto b = ranges.Allocate(100, 256);
    require(b && *b == 256, "alignment honoured");
    const auto c = ranges.Allocate(100, 64);
    require(c && *c == 128, "first fit reuses the gap before the aligned allocation");
    const auto d = ranges.Allocate(800, 1);
    require(!d, "oversized request fails");
    require(ranges.Used() == 300, "used bytes");
    ranges.Free(*a, 100);
    ranges.Free(*c, 100);
    ranges.Free(*b, 100);
    require(ranges.Empty() && ranges.FreeRanges() == 1 && ranges.LargestFree() == 1024, "frees coalesce into one range");
    const auto whole = ranges.Allocate(1024, 1);
    require(whole && *whole == 0 && ranges.LargestFree() == 0, "whole block allocatable after coalescing");
    ranges.Free(*whole, 1024);
    // Out-of-order frees merge from both sides.
    const auto x = ranges.Allocate(256, 1);
    const auto y = ranges.Allocate(256, 1);
    const auto z = ranges.Allocate(256, 1);
    ranges.Free(*x, 256);
    ranges.Free(*z, 256);
    require(ranges.FreeRanges() == 2, "two separate holes");
    ranges.Free(*y, 256);
    require(ranges.FreeRanges() == 1 && ranges.Empty(), "middle free merges both neighbours");

    // APS5_POISON_POOL's word and fill (Resources.hpp): the default reads as RGBA8 magenta and as a
    // moderate float32, "geometry" as a far-off but finite float32, a hex word as given; the fill
    // repeats the word's little-endian bytes, a partial last word too.
    using AgcDriver::Graphics::FillPoolPoison;
    using AgcDriver::Graphics::PoolPoisonWordFrom;
    require(PoolPoisonWordFrom("1") == 0x40ff00ffu && PoolPoisonWordFrom(nullptr) == 0x40ff00ffu, "default pool poison word");
    require(PoolPoisonWordFrom("geometry") == 0x47ff00ffu, "geometry pool poison word");
    require(PoolPoisonWordFrom("0x12345678") == 0x12345678u, "hex pool poison word");
    const auto word = PoolPoisonWordFrom("1");
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &word, bytes.size());
    require(bytes == std::array<std::uint8_t, 4>{0xff, 0x00, 0xff, 0x40}, "default pool poison is magenta in RGBA8");
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    require(value > 7.9f && value < 8.0f, "default pool poison is about 7.97 as a float");
    const auto far = PoolPoisonWordFrom("geometry");
    std::memcpy(&value, &far, sizeof(value));
    require(value > 1.0e5f && value < 2.0e5f, "geometry pool poison is a large finite float");
    std::array<std::byte, 10> filled{};
    FillPoolPoison(filled, word);
    require(filled[0] == std::byte{0xff} && filled[1] == std::byte{0x00} && filled[3] == std::byte{0x40} && filled[4] == std::byte{0xff} && filled[8] == std::byte{0xff} && filled[9] == std::byte{0x00}, "pool poison fill repeats the word");
}
