#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TexelStats.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

template<typename TWord>
std::vector<std::byte> bytesOf(const std::vector<TWord>& words) {
    std::vector<std::byte> bytes(words.size() * sizeof(TWord));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

std::array<float, 4> decode(VkFormat format, std::uint64_t texel) {
    std::array<std::byte, 8> bytes{};
    std::memcpy(bytes.data(), &texel, bytes.size());
    std::array<float, 4> values{};
    std::uint32_t channels = 0;
    Require(DecodeTexel(format, bytes.data(), values, channels), std::string("pass dump decoder must know ") + TexelFormatName(format));
    return values;
}

}

// APS5_PASS_DUMP's decoders: NaN and infinity found where each float encoding has them, and the
// tile counts of a blocky hole.
void RunTexelStatsTests() {
    // IEEE half: 0x7c00 infinity, 0x7e00 and 0xfc01 NaN, 0x3c00 one, 0xc000 minus two, 0x0001 the
    // smallest denormal.
    Require(std::isinf(HalfToFloat(0x7c00)) && HalfToFloat(0x7c00) > 0, "half 0x7c00 must be +infinity");
    Require(std::isinf(HalfToFloat(0xfc00)) && HalfToFloat(0xfc00) < 0, "half 0xfc00 must be -infinity");
    Require(std::isnan(HalfToFloat(0x7e00)) && std::isnan(HalfToFloat(0xfc01)), "half exponent 31 with a mantissa must be NaN");
    Require(HalfToFloat(0x3c00) == 1.0f && HalfToFloat(0xc000) == -2.0f, "half 1 and -2 must decode exactly");
    Require(HalfToFloat(0x0001) == std::ldexp(1.0f, -24), "half denormals must decode");
    const auto half = decode(VK_FORMAT_R16G16B16A16_SFLOAT, 0x3c00'7e00'7c00'0000ull);
    Require(half[0] == 0.0f && std::isinf(half[1]) && std::isnan(half[2]) && half[3] == 1.0f, "RGBA16F channels must decode in order");

    // B10G11R11: red bits 0-10 and green 11-21 (6-bit mantissa), blue 22-31 (5-bit mantissa).
    Require(UnsignedSmallFloat(15u << 6, 6) == 1.0f && UnsignedSmallFloat(15u << 5, 5) == 1.0f, "exponent 15 must be one");
    Require(std::isinf(UnsignedSmallFloat(31u << 6, 6)) && std::isnan(UnsignedSmallFloat((31u << 6) | 1u, 6)), "11-bit exponent 31 must be infinity or NaN");
    Require(std::isinf(UnsignedSmallFloat(31u << 5, 5)) && std::isnan(UnsignedSmallFloat((31u << 5) | 3u, 5)), "10-bit exponent 31 must be infinity or NaN");
    const std::uint32_t r11g11b10 = (15u << 6) | (((31u << 6) | 2u) << 11) | ((31u << 5) << 22);
    const auto packed = decode(VK_FORMAT_B10G11R11_UFLOAT_PACK32, r11g11b10);
    Require(packed[0] == 1.0f && std::isnan(packed[1]) && std::isinf(packed[2]), "B10G11R11 must decode red, green and blue from their bits");

    // E5B9G9R9 has no NaN or infinity: exponent 31 with full mantissas is its largest value.
    const auto largest = SharedExponentRgb(0xffffffffu);
    Require(largest[0] == 65408.0f && largest[1] == 65408.0f && largest[2] == 65408.0f, "E5B9G9R9 all ones must be 65408");
    const auto one = SharedExponentRgb((16u << 27) | 256u);
    Require(one[0] == 1.0f && one[1] == 0.0f && one[2] == 0.0f, "E5B9G9R9 mantissa 256 at exponent 16 must be one");
    const std::vector<std::uint32_t> shared{0xffffffffu, 0xf8000000u};
    TexelStats sharedStats;
    Require(AccumulateTexelStats(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, bytesOf(shared), 2, 1, 8, sharedStats), "E5B9G9R9 stats must be computed");
    Require(sharedStats.nan == 0 && sharedStats.inf == 0 && sharedStats.zero == 1, "E5B9G9R9 must count no NaN or infinity and its zero mantissas as zero");

    // Unorm and snorm: snorm's two smallest codes are both -1.
    const auto unorm = decode(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0x3ffu | (3u << 30));
    Require(unorm[0] == 1.0f && unorm[1] == 0.0f && unorm[3] == 1.0f, "A2B10G10R10 unorm channels must normalize");
    const auto snorm = decode(VK_FORMAT_R8G8B8A8_SNORM, 0x7f'81'80'00u);
    Require(snorm[0] == 0.0f && snorm[1] == -1.0f && snorm[2] == -1.0f && snorm[3] == 1.0f, "R8G8B8A8 snorm channels must normalize and clamp");

    // A 48x32 RGBA16F image: its left 16x32 columns zero (two tiles high), one NaN texel, one
    // infinity, one negative texel; zero texels ignore alpha.
    const std::uint32_t width = 48, height = 32;
    std::vector<std::uint16_t> image(static_cast<std::size_t>(width) * height * 4u, 0x3c00u);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            auto* texel = &image[(static_cast<std::size_t>(y) * width + x) * 4u];
            texel[0] = texel[1] = texel[2] = 0;
        }
    }
    image[(5u * width + 20u) * 4u + 1u] = 0x7e00u;
    image[(6u * width + 40u) * 4u + 0u] = 0x7c00u;
    image[(7u * width + 41u) * 4u + 2u] = 0xbc00u;
    TexelStats stats;
    Require(AccumulateTexelStats(VK_FORMAT_R16G16B16A16_SFLOAT, bytesOf(image), width, height, width * 8u, stats), "RGBA16F stats must be computed");
    Require(stats.texels == width * height && stats.nan == 1 && stats.inf == 1 && stats.negative == 1, "NaN, infinity and negative texels must be counted once each");
    Require(stats.zero == 16u * height, "every texel of the zero columns must count as zero");
    Require(stats.tiles == 6 && stats.badTiles == 2 && stats.edgeTiles == 2, "the zero columns must be two bad tiles, both on an edge");
    Require(stats.finite && stats.minimum == -1.0f && stats.maximum == 1.0f, "the finite range must leave NaN and infinity out");

    // A row pitch wider than the texels, and a buffer too short for it, are refused.
    TexelStats refused;
    Require(!AccumulateTexelStats(VK_FORMAT_R32_SFLOAT, bytesOf(std::vector<float>(7)), 4, 2, 16, refused) && refused.texels == 0, "rows past the end must be refused");
    Require(!AccumulateTexelStats(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, bytesOf(std::vector<float>(8)), 4, 2, 16, refused), "unknown formats must be refused");
    Require(TexelBytes(VK_FORMAT_R32G32B32A32_SFLOAT) == 16 && TexelBytes(VK_FORMAT_D16_UNORM) == 2 && TexelBytes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) == 0, "texel sizes must follow the format");
}
