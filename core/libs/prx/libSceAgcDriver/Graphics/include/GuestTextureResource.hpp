#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTTEXTURERESOURCE_HPP

#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

enum class TextureTileMode {
    kLinear,
    kStandard256B,
    kStandard4KB,
    kStandard64KB,
    // 64 KiB XOR swizzles; their address equations live in TextureSwizzleEquations.hpp.
    kZ64KBX,
    kS64KBX,
    kD64KBX,
    kR64KBX,
    // Upstream's name for SW_64KB_R_X (tile mode 0x1b).
    RenderTarget64KB = kR64KBX
};

// The hardware SW_MODE of an XOR swizzle tile mode, or 0 for the modes addressed without an equation table.
constexpr std::uint32_t XorSwizzleMode(TextureTileMode mode) {
    switch (mode) {
        case TextureTileMode::kZ64KBX: return 24u;
        case TextureTileMode::kS64KBX: return 25u;
        case TextureTileMode::kD64KBX: return 26u;
        case TextureTileMode::kR64KBX: return 27u;
        default: return 0u;
    }
}

enum class TextureDimension {
    k1D,
    k2D,
    k2DArray,
    kCube,
    k3D
};

struct GuestTextureResource {
    std::uint64_t baseAddress;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depthOrLastArray;
    std::uint32_t baseArray;
    std::uint32_t mipCount;
    std::uint32_t baseLevel;
    TextureTileMode tileMode;
    TextureDimension dimension;
    std::uint32_t format;
    std::uint8_t dstSelX;
    std::uint8_t dstSelY;
    std::uint8_t dstSelZ;
    std::uint8_t dstSelW;
    // Last mip level the view exposes; the surface itself holds mipCount levels.
    std::uint32_t lastLevel = 0;
    // DCC metadata of a compressed surface, or 0 (see DccMetadata.hpp).
    std::uint64_t dccAddress = 0;
    bool dccAlphaOnMsb = false;
    std::uint32_t minLod = 0;
};

float EffectiveMinLod(const GuestTextureResource& resource);

GuestTextureResource DecodeTextureResource(std::span<const std::uint32_t> words);

// APS5_ARRAY_PITCH=1 (off by default): words 4-7 of a layered T# (3D, cube, 2D array) the decoder
// rejects are repaired instead of dropping the draw (see DecodeTextureResource). Whether words 4-7
// hold what the decoder rejects: an array pitch, corner sampling, a partially resident default
// color, MSAA depth, or a base array the view cannot have.
bool TextureUpperHalfRejected(std::span<const std::uint32_t> words);
// `words` with `intact` as words 4-7, or (null) a 3D T# whose ARRAY_PITCH is the view bit 1 read
// from slice 0 at level 0, where it names the whole volume as 0 does; false: no repair.
bool RepairTextureUpperHalf(std::span<const std::uint32_t> words, const std::array<std::uint32_t, 4>* intact, std::array<std::uint32_t, 8>& repaired);
// Decodes repaired from an intact copy, 3D view bits read, and rejected T#s left to throw.
enum class UpperHalfRepair { Copied, View3D, Unrepaired, Count };
// The decodes of `kind` since the last call (the APS5_PROFILE_DRAW digest).
std::uint64_t TakeUpperHalfRepairs(UpperHalfRepair kind);
bool UpperHalfRepairEnabled();
bool MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape shape, TextureDimension dimension);

}

#endif
