#include "prx/libSceAgcDriver/Graphics/include/TexelStats.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

enum class Kind : std::uint8_t { Unorm, Snorm, Uint, Sint, Float, UFloat, SharedExponent };

struct Channel {
    std::uint8_t offset;
    std::uint8_t bits;
};

// A texel layout: `count` channels of one kind at bit offsets within `bytes` little-endian bytes.
struct Layout {
    const char* name;
    std::uint32_t bytes;
    Kind kind;
    std::uint32_t count;
    std::array<Channel, 4> channels;
};

constexpr Layout uniform(const char* name, Kind kind, std::uint32_t count, std::uint32_t bits) {
    Layout layout{name, count * bits / 8u, kind, count, {}};
    for (std::uint32_t index = 0; index < count; ++index) layout.channels[index] = {static_cast<std::uint8_t>(index * bits), static_cast<std::uint8_t>(bits)};
    return layout;
}

bool layoutOf(VkFormat format, Layout& layout) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: layout = uniform("R8_UNORM", Kind::Unorm, 1, 8); return true;
        case VK_FORMAT_R8_SRGB: layout = uniform("R8_SRGB", Kind::Unorm, 1, 8); return true;
        case VK_FORMAT_R8_SNORM: layout = uniform("R8_SNORM", Kind::Snorm, 1, 8); return true;
        case VK_FORMAT_R8_UINT: layout = uniform("R8_UINT", Kind::Uint, 1, 8); return true;
        case VK_FORMAT_R8_SINT: layout = uniform("R8_SINT", Kind::Sint, 1, 8); return true;
        case VK_FORMAT_S8_UINT: layout = uniform("S8_UINT", Kind::Uint, 1, 8); return true;
        case VK_FORMAT_R8G8_UNORM: layout = uniform("R8G8_UNORM", Kind::Unorm, 2, 8); return true;
        case VK_FORMAT_R8G8_SRGB: layout = uniform("R8G8_SRGB", Kind::Unorm, 2, 8); return true;
        case VK_FORMAT_R8G8_SNORM: layout = uniform("R8G8_SNORM", Kind::Snorm, 2, 8); return true;
        case VK_FORMAT_R8G8_UINT: layout = uniform("R8G8_UINT", Kind::Uint, 2, 8); return true;
        case VK_FORMAT_R8G8_SINT: layout = uniform("R8G8_SINT", Kind::Sint, 2, 8); return true;
        case VK_FORMAT_R8G8B8A8_UNORM: layout = uniform("R8G8B8A8_UNORM", Kind::Unorm, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_SRGB: layout = uniform("R8G8B8A8_SRGB", Kind::Unorm, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_SNORM: layout = uniform("R8G8B8A8_SNORM", Kind::Snorm, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_UINT: layout = uniform("R8G8B8A8_UINT", Kind::Uint, 4, 8); return true;
        case VK_FORMAT_R8G8B8A8_SINT: layout = uniform("R8G8B8A8_SINT", Kind::Sint, 4, 8); return true;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: layout = uniform("A8B8G8R8_UNORM", Kind::Unorm, 4, 8); return true;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: layout = uniform("A8B8G8R8_SRGB", Kind::Unorm, 4, 8); return true;
        // Blue first: the channel order does not change any statistic, alpha stays last.
        case VK_FORMAT_B8G8R8A8_UNORM: layout = uniform("B8G8R8A8_UNORM", Kind::Unorm, 4, 8); return true;
        case VK_FORMAT_B8G8R8A8_SRGB: layout = uniform("B8G8R8A8_SRGB", Kind::Unorm, 4, 8); return true;
        case VK_FORMAT_R16_UNORM: layout = uniform("R16_UNORM", Kind::Unorm, 1, 16); return true;
        case VK_FORMAT_D16_UNORM: layout = uniform("D16_UNORM", Kind::Unorm, 1, 16); return true;
        case VK_FORMAT_R16_SNORM: layout = uniform("R16_SNORM", Kind::Snorm, 1, 16); return true;
        case VK_FORMAT_R16_UINT: layout = uniform("R16_UINT", Kind::Uint, 1, 16); return true;
        case VK_FORMAT_R16_SINT: layout = uniform("R16_SINT", Kind::Sint, 1, 16); return true;
        case VK_FORMAT_R16_SFLOAT: layout = uniform("R16_SFLOAT", Kind::Float, 1, 16); return true;
        case VK_FORMAT_R16G16_UNORM: layout = uniform("R16G16_UNORM", Kind::Unorm, 2, 16); return true;
        case VK_FORMAT_R16G16_SNORM: layout = uniform("R16G16_SNORM", Kind::Snorm, 2, 16); return true;
        case VK_FORMAT_R16G16_UINT: layout = uniform("R16G16_UINT", Kind::Uint, 2, 16); return true;
        case VK_FORMAT_R16G16_SINT: layout = uniform("R16G16_SINT", Kind::Sint, 2, 16); return true;
        case VK_FORMAT_R16G16_SFLOAT: layout = uniform("R16G16_SFLOAT", Kind::Float, 2, 16); return true;
        case VK_FORMAT_R16G16B16A16_UNORM: layout = uniform("R16G16B16A16_UNORM", Kind::Unorm, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SNORM: layout = uniform("R16G16B16A16_SNORM", Kind::Snorm, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_UINT: layout = uniform("R16G16B16A16_UINT", Kind::Uint, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SINT: layout = uniform("R16G16B16A16_SINT", Kind::Sint, 4, 16); return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT: layout = uniform("R16G16B16A16_SFLOAT", Kind::Float, 4, 16); return true;
        case VK_FORMAT_R32_UINT: layout = uniform("R32_UINT", Kind::Uint, 1, 32); return true;
        case VK_FORMAT_R32_SINT: layout = uniform("R32_SINT", Kind::Sint, 1, 32); return true;
        case VK_FORMAT_R32_SFLOAT: layout = uniform("R32_SFLOAT", Kind::Float, 1, 32); return true;
        case VK_FORMAT_D32_SFLOAT: layout = uniform("D32_SFLOAT", Kind::Float, 1, 32); return true;
        case VK_FORMAT_R32G32_UINT: layout = uniform("R32G32_UINT", Kind::Uint, 2, 32); return true;
        case VK_FORMAT_R32G32_SINT: layout = uniform("R32G32_SINT", Kind::Sint, 2, 32); return true;
        case VK_FORMAT_R32G32_SFLOAT: layout = uniform("R32G32_SFLOAT", Kind::Float, 2, 32); return true;
        case VK_FORMAT_R32G32B32_UINT: layout = uniform("R32G32B32_UINT", Kind::Uint, 3, 32); return true;
        case VK_FORMAT_R32G32B32_SINT: layout = uniform("R32G32B32_SINT", Kind::Sint, 3, 32); return true;
        case VK_FORMAT_R32G32B32_SFLOAT: layout = uniform("R32G32B32_SFLOAT", Kind::Float, 3, 32); return true;
        case VK_FORMAT_R32G32B32A32_UINT: layout = uniform("R32G32B32A32_UINT", Kind::Uint, 4, 32); return true;
        case VK_FORMAT_R32G32B32A32_SINT: layout = uniform("R32G32B32A32_SINT", Kind::Sint, 4, 32); return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT: layout = uniform("R32G32B32A32_SFLOAT", Kind::Float, 4, 32); return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: layout = {"A2B10G10R10_UNORM", 4, Kind::Unorm, 4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}}; return true;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: layout = {"A2B10G10R10_UINT", 4, Kind::Uint, 4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}}; return true;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: layout = {"A2R10G10B10_UNORM", 4, Kind::Unorm, 4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}}; return true;
        case VK_FORMAT_A2R10G10B10_UINT_PACK32: layout = {"A2R10G10B10_UINT", 4, Kind::Uint, 4, {{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}}; return true;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: layout = {"R5G6B5_UNORM", 2, Kind::Unorm, 3, {{{0, 5}, {5, 6}, {11, 5}, {0, 0}}}}; return true;
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: layout = {"A1R5G5B5_UNORM", 2, Kind::Unorm, 4, {{{0, 5}, {5, 5}, {10, 5}, {15, 1}}}}; return true;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: layout = {"R4G4B4A4_UNORM", 2, Kind::Unorm, 4, {{{0, 4}, {4, 4}, {8, 4}, {12, 4}}}}; return true;
        // Red in the low 11 bits.
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: layout = {"B10G11R11_UFLOAT", 4, Kind::UFloat, 3, {{{0, 11}, {11, 11}, {22, 10}, {0, 0}}}}; return true;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: layout = {"E5B9G9R9_UFLOAT", 4, Kind::SharedExponent, 3, {}}; return true;
        default: return false;
    }
}

// `bits` (at most 32) at bit `offset` of a little-endian texel.
std::uint32_t extract(const std::byte* texel, std::uint32_t offset, std::uint32_t bits) {
    std::uint64_t value = 0;
    const auto first = offset / 8u;
    const auto last = (offset + bits + 7u) / 8u;
    for (auto index = first; index < last; ++index) value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(texel[index])) << (8u * (index - first));
    value >>= offset % 8u;
    return static_cast<std::uint32_t>(bits >= 32 ? value & 0xffffffffull : value & ((1ull << bits) - 1u));
}

}

float HalfToFloat(std::uint16_t bits) {
    const std::uint32_t sign = (bits >> 15u) & 1u;
    const std::uint32_t exponent = (bits >> 10u) & 0x1fu;
    const std::uint32_t mantissa = bits & 0x3ffu;
    float value;
    if (exponent == 0x1fu) value = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    else if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
    else value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    return sign != 0 ? -value : value;
}

float UnsignedSmallFloat(std::uint32_t bits, std::uint32_t mantissaBits) {
    const std::uint32_t exponent = (bits >> mantissaBits) & 0x1fu;
    const std::uint32_t mantissa = bits & ((1u << mantissaBits) - 1u);
    if (exponent == 0x1fu) return mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissaBits));
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissaBits)), static_cast<int>(exponent) - 15 - static_cast<int>(mantissaBits));
}

std::array<float, 3> SharedExponentRgb(std::uint32_t word) {
    const int exponent = static_cast<int>(word >> 27u) - 15 - 9;
    return {std::ldexp(static_cast<float>(word & 0x1ffu), exponent), std::ldexp(static_cast<float>((word >> 9u) & 0x1ffu), exponent), std::ldexp(static_cast<float>((word >> 18u) & 0x1ffu), exponent)};
}

std::uint32_t TexelBytes(VkFormat format) {
    Layout layout{};
    return layoutOf(format, layout) ? layout.bytes : 0u;
}

const char* TexelFormatName(VkFormat format) {
    Layout layout{};
    if (layoutOf(format, layout)) return layout.name;
    // Unknown formats by number (one buffer per thread, as the callers print at once).
    static thread_local char text[24];
    std::snprintf(text, sizeof(text), "VK%d", static_cast<int>(format));
    return text;
}

namespace {

void decodeWith(const Layout& layout, const std::byte* texel, std::array<float, 4>& values) {
    values = {};
    if (layout.kind == Kind::SharedExponent) {
        const auto rgb = SharedExponentRgb(extract(texel, 0, 32));
        values = {rgb[0], rgb[1], rgb[2], 0.0f};
        return;
    }
    for (std::uint32_t index = 0; index < layout.count; ++index) {
        const auto& channel = layout.channels[index];
        const auto bits = extract(texel, channel.offset, channel.bits);
        const auto top = channel.bits >= 32 ? 0xffffffffu : (1u << channel.bits) - 1u;
        switch (layout.kind) {
            case Kind::Unorm: values[index] = static_cast<float>(bits) / static_cast<float>(top); break;
            case Kind::Uint: values[index] = static_cast<float>(bits); break;
            case Kind::Snorm:
            case Kind::Sint: {
                const auto sign = 1u << (channel.bits - 1u);
                const auto signedValue = static_cast<std::int64_t>(bits) - ((bits & sign) != 0 ? (static_cast<std::int64_t>(top) + 1) : 0);
                values[index] = layout.kind == Kind::Sint ? static_cast<float>(signedValue) : std::max(-1.0f, static_cast<float>(signedValue) / static_cast<float>(sign - 1u));
                break;
            }
            case Kind::Float: {
                if (channel.bits == 16) {
                    values[index] = HalfToFloat(static_cast<std::uint16_t>(bits));
                } else {
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    values[index] = value;
                }
                break;
            }
            case Kind::UFloat: values[index] = UnsignedSmallFloat(bits, channel.bits - 5u); break;
            case Kind::SharedExponent: break;
        }
    }
}

}

bool DecodeTexel(VkFormat format, const std::byte* texel, std::array<float, 4>& values, std::uint32_t& channels) {
    Layout layout{};
    if (!layoutOf(format, layout)) return false;
    channels = layout.count;
    decodeWith(layout, texel, values);
    return true;
}

bool AccumulateTexelStats(VkFormat format, std::span<const std::byte> bytes, std::uint32_t width, std::uint32_t height, std::size_t rowBytes, TexelStats& stats) {
    Layout layout{};
    if (!layoutOf(format, layout) || width == 0 || height == 0) return false;
    if (rowBytes < static_cast<std::size_t>(width) * layout.bytes) return false;
    if (bytes.size() < (static_cast<std::size_t>(height) - 1u) * rowBytes + static_cast<std::size_t>(width) * layout.bytes) return false;
    constexpr auto tile = TexelStats::TileSize;
    const auto tilesX = (width + tile - 1u) / tile;
    const auto tilesY = (height + tile - 1u) / tile;
    // Zero-or-NaN texels per tile.
    std::vector<std::uint32_t> holes(static_cast<std::size_t>(tilesX) * tilesY, 0u);
    // The channels a zero texel needs at 0: all but alpha (a 4-channel format's last).
    const auto colorChannels = layout.count == 4 ? 3u : layout.count;
    std::array<float, 4> values{};
    const auto channels = layout.count;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = bytes.data() + static_cast<std::size_t>(y) * rowBytes;
        auto* tileRow = holes.data() + static_cast<std::size_t>(y / tile) * tilesX;
        for (std::uint32_t x = 0; x < width; ++x) {
            decodeWith(layout, row + static_cast<std::size_t>(x) * layout.bytes, values);
            bool nan = false, inf = false, negative = false, zero = true;
            for (std::uint32_t index = 0; index < channels; ++index) {
                const auto value = values[index];
                if (std::isnan(value)) {
                    nan = true;
                    zero = false;
                    continue;
                }
                if (std::isinf(value)) inf = true;
                else {
                    if (!stats.finite) {
                        stats.minimum = stats.maximum = value;
                        stats.finite = true;
                    }
                    stats.minimum = std::min(stats.minimum, value);
                    stats.maximum = std::max(stats.maximum, value);
                }
                if (value < 0) negative = true;
                if (index < colorChannels && value != 0) zero = false;
            }
            stats.nan += nan;
            stats.inf += inf;
            stats.negative += negative;
            stats.zero += zero;
            if (nan || zero) ++tileRow[x / tile];
        }
    }
    stats.texels += static_cast<std::uint64_t>(width) * height;
    const auto bad = [&](std::uint32_t tx, std::uint32_t ty) {
        const auto w = std::min(tile, width - tx * tile);
        const auto h = std::min(tile, height - ty * tile);
        return static_cast<std::uint64_t>(holes[static_cast<std::size_t>(ty) * tilesX + tx]) * 10u >= static_cast<std::uint64_t>(w) * h * 9u;
    };
    stats.tiles += static_cast<std::uint64_t>(tilesX) * tilesY;
    for (std::uint32_t ty = 0; ty < tilesY; ++ty) {
        for (std::uint32_t tx = 0; tx < tilesX; ++tx) {
            if (!bad(tx, ty)) continue;
            ++stats.badTiles;
            if ((tx + 1 < tilesX && !bad(tx + 1, ty)) || (ty + 1 < tilesY && !bad(tx, ty + 1))) ++stats.edgeTiles;
        }
    }
    return true;
}

}
