#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

template<typename TAction>
void reject(TAction action, std::string_view reason) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected texture tiling test error: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("expected texture tiling rejection: ") + std::string(reason));
}

}

void RunTextureTilingTests() {
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kLinear, 1, 4, 4, 2);
        Require(mips.size() == 2, "linear mip chain must contain the requested mip count");
        Require(mips[0].tiledOffset == 512 && mips[0].tiledSize == 1024, "linear mip 0 offset or size changed");
        Require(mips[0].width == 4 && mips[0].height == 4, "linear mip 0 dimensions changed");
        Require(mips[0].blocksPerRow == 256 && mips[0].pitchBytes == 256, "linear mip 0 row layout changed");
        Require(!mips[0].tail, "linear mips must never fall into a mip tail");
        Require(mips[1].tiledOffset == 0 && mips[1].tiledSize == 512, "linear mip 1 offset or size changed");
        Require(mips[1].width == 2 && mips[1].height == 2, "linear mip 1 dimensions changed");
        Require(mips[1].linearOffset == mips[1].tiledOffset && mips[1].linearSize == mips[1].tiledSize, "linear tiling must keep linear and tiled layout identical");
    }
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kLinear, 169, 8, 8, 1);
        Require(mips.size() == 1, "compressed linear layout must contain one mip");
        Require(mips[0].width == 2 && mips[0].height == 2, "compressed linear mip block dimensions changed");
        Require(mips[0].blocksPerRow == 32 && mips[0].pitchBytes == 256, "compressed linear mip row layout changed");
        Require(mips[0].tiledSize == 512 && mips[0].linearSize == 512, "compressed linear mip size changed");
    }
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kStandard256B, 1, 64, 64, 1);
        Require(mips.size() == 1, "standard 256B layout must contain one mip");
        Require(mips[0].tiledOffset == 0 && mips[0].tiledSize == 4096, "standard 256B mip 0 offset or size changed");
        Require(mips[0].width == 64 && mips[0].height == 64, "standard 256B mip 0 dimensions changed");
        Require(mips[0].blocksPerRow == 4 && mips[0].pitchBytes == 64, "standard 256B mip 0 row layout changed");
        Require(!mips[0].tail, "standard 256B textures must never use a mip tail");

        const auto surfaceSize = ComputeSurfaceSize(mips, 3);
        Require(surfaceSize == 4096ull * 3ull, "surface size must multiply the slice size by the array layer count");
    }
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kStandard256B, 1, 32, 32, 6);
        Require(mips.size() == 6, "standard 256B mip chain must contain the requested mip count");
        for (const auto& mip : mips) Require(!mip.tail, "standard 256B tile mode must never produce a mip tail");
    }
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kStandard64KB, 1, 1024, 1024, 11);
        Require(mips.size() == 11, "standard 64KB mip chain must contain the requested mip count");
        auto tailSeen = false;
        for (const auto& mip : mips) {
            Require(mip.width != 0 && mip.height != 0, "every standard 64KB mip must have nonzero dimensions");
            Require(mip.tiledSize != 0 && mip.linearSize != 0, "every standard 64KB mip must have a nonzero size");
            if (mip.tail) {
                tailSeen = true;
                Require(mip.blocksPerRow == 1, "mip tail levels must report a single block per row");
                Require(mip.tiledOffset == 0, "mip tail levels must share the tiled tail block offset");
            }
        }
        Require(tailSeen, "a deep standard 64KB mip chain must fall into the mip tail");
        Require(!mips.front().tail, "the base level of a deep mip chain must not be in the mip tail");
    }
    {
        const auto mips = ComputeMipLayout(TextureTileMode::kStandard4KB, 1, 512, 512, 10);
        Require(mips.size() == 10, "standard 4KB mip chain must contain the requested mip count");
        auto tailSeen = false;
        for (const auto& mip : mips) {
            if (mip.tail) tailSeen = true;
        }
        Require(tailSeen, "a deep standard 4KB mip chain must fall into the mip tail");
    }

    {
        const auto mips = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 56, 257, 129, 1);
        Require(mips[0].blocksPerRow == 3 && mips[0].tiledSize == 393216, "render target surfaces must pad to complete 128 by 128 blocks for 32-bit pixels");
        Require(mips[0].pitchBytes == 1536 && mips[0].linearSize == 1536u * 129u, "detiled render target rows must span the padded block width");
        Require(ComputeSurfaceSize(mips, 6) == 2359296, "render target cube faces must retain the padded guest slice stride");
    }
    for (const auto format : std::array<std::uint32_t, 5>{1, 7, 56, 71, 77}) {
        const auto mips = ComputeMipLayout(TextureTileMode::RenderTarget64KB, format, 1024, 513, 11);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        bool tailSeen = false;
        for (const auto& mip : mips) {
            Require(mip.linearOffset % 4 == 0, "detiled mip levels must start word-aligned");
            Require(mip.linearSize >= static_cast<std::uint64_t>(mip.pitchBytes) * mip.height, "detiled mip allocation must contain every row");
            Require(mip.linearSize % 4 == 0, "detiled mip sizes must preserve word alignment between array layers");
            ranges.emplace_back(mip.linearOffset, mip.linearOffset + mip.linearSize);
            if (mip.tail) {
                tailSeen = true;
                Require(mip.tiledOffset == 0 && mip.tiledSize == 65536, "render target mip tails must share one guest 64KB block");
            }
        }
        std::sort(ranges.begin(), ranges.end());
        for (std::size_t index = 1; index < ranges.size(); ++index) Require(ranges[index].first >= ranges[index - 1].second, "detiled mip levels must occupy separate ranges");
        Require(tailSeen && !mips.front().tail, "render target mip chains must cover both regular blocks and mip tails");
    }
    const auto compressed = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 169, 64, 64, 1);
    Require(compressed.size() == 1 && compressed[0].tiledSize == 65536 && compressed[0].linearSize != 0, "block compressed render target layout is wrong");
    Require(ComputeMipLayout(TextureTileMode::RenderTarget64KB, 132, 64, 64, 1).size() == 1, "format 132 render target layout is missing");
    reject([] { ComputeMipLayout(TextureTileMode::RenderTarget64KB, 74, 64, 64, 1); }, "unsupported bytes per element");

    {
        // The retile's written bytes (the GPU-direct write-back seeds only the rest of the stored
        // ranges): whole tile blocks inside the extent, row by row; a linear row's elements.
        using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
        const auto exact = ComputeElementMipLayout(TextureTileMode::kStandard4KB, 4, 64, 64, 1);
        Require(RetileWrittenRanges(TextureTileMode::kStandard4KB, 4, exact[0], false) == Ranges{{0, exact[0].tiledSize}}, "a mip of whole tile blocks must be written whole");
        const auto edges = ComputeElementMipLayout(TextureTileMode::kStandard256B, 4, 20, 17, 1);
        Require(edges[0].blocksPerRow == 3 && edges[0].tiledSize == 9u * 256u, "standard 256B 20x17 layout changed");
        Require(RetileWrittenRanges(TextureTileMode::kStandard256B, 4, edges[0], false) == Ranges{{0, 512}, {768, 1280}}, "tile blocks the extent covers in part must be left to the seed");
        const auto target = ComputeMipLayout(TextureTileMode::RenderTarget64KB, 56, 257, 129, 1);
        Require(RetileWrittenRanges(TextureTileMode::RenderTarget64KB, 4, target[0], false) == Ranges{{0, 131072}}, "the padded third block column and second block row must be left to the seed");
        const auto linear = ComputeElementMipLayout(TextureTileMode::kLinear, 4, 4, 2, 1);
        Require(linear[0].pitchBytes == 256, "linear 4x2 pitch changed");
        Require(RetileWrittenRanges(TextureTileMode::kLinear, 4, linear[0], false) == Ranges{{0, 16}, {256, 272}}, "linear row padding must be left to the seed");
        const auto dense = ComputeElementMipLayout(TextureTileMode::kLinear, 4, 64, 3, 1);
        Require(RetileWrittenRanges(TextureTileMode::kLinear, 4, dense[0], false) == Ranges{{0, 768}}, "unpadded linear rows must merge");
        const auto chain = ComputeElementMipLayout(TextureTileMode::kStandard64KB, 4, 1024, 1024, 11);
        Require(RetileWrittenRanges(TextureTileMode::kStandard64KB, 4, chain[0], false) == Ranges{{0, chain[0].tiledSize}}, "a 1024x1024 base level must be written whole");
        Require(RetileWrittenRanges(TextureTileMode::kStandard64KB, 4, chain[0], true).empty(), "thick mips must be seeded whole");
        bool tailSeen = false;
        for (const auto& mip : chain) {
            const auto written = RetileWrittenRanges(TextureTileMode::kStandard64KB, 4, mip, false);
            if (mip.tail) {
                tailSeen = true;
                Require(written.empty(), "tail mips must be seeded whole");
            }
            for (std::size_t index = 0; index < written.size(); ++index) {
                const auto [begin, end] = written[index];
                Require(begin < end && end <= mip.tiledSize && begin % 65536 == 0 && end % 65536 == 0, "written ranges must be whole blocks of the mip");
                Require(index == 0 || written[index - 1].second < begin, "written ranges must be sorted and merged");
            }
        }
        Require(tailSeen, "the 64KB chain must reach the mip tail");

        Require(SubtractByteRanges({{200, 300}, {0, 100}}, {{50, 250}}) == Ranges{{0, 50}, {250, 300}}, "range subtraction must split and sort");
        Require(SubtractByteRanges({{0, 100}}, {{80, 100}, {0, 40}, {30, 60}}) == Ranges{{60, 80}}, "range subtraction must take overlapping removals in any order");
        Require(SubtractByteRanges({{0, 10}, {10, 20}}, {}) == Ranges{{0, 20}}, "range subtraction must merge adjacent ranges");
        Require(SubtractByteRanges({{0, 100}}, {{0, 100}}).empty() && SubtractByteRanges({}, {{0, 100}}).empty(), "range subtraction must drop covered ranges");

        // A block wholly inside the extent is written whole only if its swizzle sends the block's
        // elements to distinct element slots of the block, whatever the block's position and slice
        // (the XOR swizzles mix those in).
        for (const auto& equation : kTextureSwizzleEquations) {
            if (equation.swizzleMode >= 0x100u) continue;
            const auto block = ThinBlockLayout(TextureTileMode::kStandard64KB, equation.elementBytes);
            for (const auto& [blockX, blockY, slice] : std::array<std::array<std::uint32_t, 3>, 2>{{{0u, 0u, 0u}, {3u, 1u, 5u}}}) {
                std::vector<bool> seen(65536u / equation.elementBytes, false);
                for (std::uint32_t y = blockY * block[2]; y < (blockY + 1u) * block[2]; ++y) {
                    for (std::uint32_t x = blockX * block[1]; x < (blockX + 1u) * block[1]; ++x) {
                        std::uint32_t offset = 0;
                        for (std::uint32_t bit = 0; bit < 16u; ++bit) {
                            const auto mask = equation.bits[bit];
                            const auto selected = (x & (mask & 0xfffu)) ^ ((y << 12) & (mask & 0xfff000u)) ^ ((slice << 24) & (mask & 0xff000000u));
                            offset |= static_cast<std::uint32_t>(std::popcount(selected) & 1) << bit;
                        }
                        Require(offset % equation.elementBytes == 0 && !seen[offset / equation.elementBytes], "a swizzle equation must permute the elements of a block");
                        seen[offset / equation.elementBytes] = true;
                    }
                }
            }
        }
    }

    reject([] { ComputeMipLayout(TextureTileMode::kLinear, 1, 0, 4, 1); }, "zero-sized texture");
    reject([] { ComputeMipLayout(TextureTileMode::kLinear, 1, 4, 0, 1); }, "zero-sized texture");
    reject([] { ComputeMipLayout(TextureTileMode::kLinear, 1, 4, 4, 0); }, "mip count is out of range");
    reject([] { ComputeMipLayout(TextureTileMode::kLinear, 1, 4, 4, 17); }, "mip count is out of range");

    reject([] { ComputeSurfaceSize({}, 1); }, "empty mip chain");
    reject([] { ComputeSurfaceSize(ComputeMipLayout(TextureTileMode::kLinear, 1, 4, 4, 1), 0); }, "zero array layers");
}
