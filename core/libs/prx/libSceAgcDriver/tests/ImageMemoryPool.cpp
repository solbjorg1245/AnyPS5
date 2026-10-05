#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ImageMemoryPool.hpp"
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
}
