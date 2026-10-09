#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXELSTATS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TEXELSTATS_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

// Debug aid (APS5_PASS_DUMP, see Execution/include/Driver/PassDump.hpp): texel decoding and the
// per-image statistics of a pass dump. Pure functions, no device.

// IEEE half to float (NaN and infinity kept).
float HalfToFloat(std::uint16_t bits);
// The unsigned 5-bit-exponent floats of B10G11R11 (`mantissaBits` 6 for the 11-bit channels, 5 for
// the 10-bit one): exponent 31 is infinity (mantissa 0) or NaN, exponent 0 denormal.
float UnsignedSmallFloat(std::uint32_t bits, std::uint32_t mantissaBits);
// E5B9G9R9: three 9-bit mantissas over one 5-bit exponent (bias 15). The format has no infinity or
// NaN encoding: exponent 31 is an ordinary (the largest) exponent.
std::array<float, 3> SharedExponentRgb(std::uint32_t word);

// Bytes per texel of a format DecodeTexel knows, 0 otherwise.
std::uint32_t TexelBytes(VkFormat format);
// The format's short name (R16G16B16A16_SFLOAT ...), "VK<number>" for one DecodeTexel does not know.
const char* TexelFormatName(VkFormat format);
// The channels of one texel as floats (unorm/snorm normalized, integers as their values; sRGB
// channels as stored, not linearized): false for a format it does not know. `channels` receives the
// channel count (alpha last); depth formats have one channel.
bool DecodeTexel(VkFormat format, const std::byte* texel, std::array<float, 4>& values, std::uint32_t& channels);

// What one image holds: texels with a NaN or an infinity in any channel, texels whose channels are
// all 0 (alpha left out: a black opaque texel counts), texels with a negative channel; and over
// TileSize x TileSize tiles, the tiles where at least 90 % of the texels are zero or NaN ("bad"),
// and the bad tiles whose right or lower neighbour is not bad (edges of a blocky hole).
struct TexelStats {
    static constexpr std::uint32_t TileSize = 16;
    std::uint64_t texels = 0;
    std::uint64_t nan = 0;
    std::uint64_t inf = 0;
    std::uint64_t zero = 0;
    std::uint64_t negative = 0;
    std::uint64_t tiles = 0;
    std::uint64_t badTiles = 0;
    std::uint64_t edgeTiles = 0;
    // The smallest and largest finite channel value (meaningful when `finite`).
    float minimum = 0;
    float maximum = 0;
    bool finite = false;
};

// Adds the `width` x `height` texels of `format` in `bytes` (rows `rowBytes` apart) to `stats`;
// false (nothing added) for an unknown format or rows past the end of `bytes`.
bool AccumulateTexelStats(VkFormat format, std::span<const std::byte> bytes, std::uint32_t width, std::uint32_t height, std::size_t rowBytes, TexelStats& stats);

// One image read back to the host for a pass dump (StorageTexture::ReadBack, ReadBackDepthSurface):
// `layers` slices of mip `level`, each `height` rows of `rowTexels` texels of `texelBytes`.
struct ImageReadback {
    std::uint64_t address = 0;
    std::uint64_t guestBytes = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t guestFormat = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t level = 0;
    std::uint32_t firstLayer = 0;
    std::uint32_t layers = 1;
    std::uint32_t rowTexels = 0;
    std::uint32_t texelBytes = 0;
    // The image's index among the live images at the address, and their count (several images of
    // one surface in different formats or shapes alias its memory).
    std::uint32_t alias = 0;
    std::uint32_t aliases = 1;
    std::span<const std::byte> bytes;
};

}

#endif
