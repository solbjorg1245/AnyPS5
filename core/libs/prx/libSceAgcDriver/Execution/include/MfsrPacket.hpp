#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MFSRPACKET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MFSRPACKET_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// The PS5 Pro upscaler hand-over as one PM4 packet (docs/design/dlss.md S0): libScePsml_debug writes it,
// the driver reads it (header only, so the Psml module includes it without the driver). The title's
// dispatchMfsr FUN_141e83ac0 asks scePsmlMfsrGetDispatchMfsrPacketSizeInDwords for n, makes room in the
// DCB and has scePsmlMfsrGetDispatchMfsrPacket1100 write exactly n dwords at the cursor. The packet is a
// NOP (type 3, opcode 0x10, sub-op 0, flags 0) that Pm4.cpp validates (payload high half not 0x6875) and
// skips; its payload: 'APS5', kind, version, which pointers were set, then COPIES of the 0x158-byte
// parameter block (0x142f73f10), the five 32-byte T#s its image pointers name (CTexture+0xbc) and the
// 32-byte descriptor at +0x28 (the title's stack, gone once the call returns).
namespace AgcDriver::MfsrPacket {

inline constexpr std::uint32_t Magic = 0x35535041u;  // "APS5" in memory order
inline constexpr std::uint32_t KindDispatch = 1;
inline constexpr std::uint32_t Version = 1;
inline constexpr std::size_t BlockBytes = 0x158;
// T# pointers: +0x00 the output (kRTT index 7, which the post chain reads), +0x08 MainColor, +0x10 Motion
// (R16G16_SFLOAT), +0x20 MainDepth (D32), +0x30 the title's motion pre-pass result. +0x18 is null.
inline constexpr std::array<std::size_t, 5> ImageOffsets{0x00, 0x08, 0x10, 0x20, 0x30};
inline constexpr std::size_t DescriptorOffset = 0x28;
// Render size (u32 pair), jitter in render pixels (float pair, y negated), MV scale (-w, -h floats),
// reset (byte).
inline constexpr std::size_t RenderSizeOffset = 0xf0;
inline constexpr std::size_t JitterOffset = 0x100;
inline constexpr std::size_t MotionScaleOffset = 0x108;
inline constexpr std::size_t ResetOffset = 0x11c;

// Dwords: PM4 header, magic, kind, version, present mask; the block; the T#s; the descriptor.
inline constexpr std::size_t HeaderWords = 5;
inline constexpr std::size_t BlockWord = HeaderWords;
inline constexpr std::size_t ImageWord = BlockWord + BlockBytes / 4;
inline constexpr std::size_t DescriptorWord = ImageWord + ImageOffsets.size() * 8;
inline constexpr std::size_t Words = DescriptorWord + 8;
using Descriptor = std::array<std::uint32_t, 8>;
static_assert(BlockBytes % 4 == 0 && Words == 139 && sizeof(std::array<Descriptor, 5>) == 5 * sizeof(Descriptor));

struct Dispatch {
    std::array<unsigned char, BlockBytes> block{};
    std::array<Descriptor, ImageOffsets.size()> images{};
    Descriptor descriptor{};
    // Bit i: images[i] was set; bit 5: the descriptor was.
    std::uint32_t present = 0;

    template<typename T> T Field(std::size_t offset) const {
        T value{};
        std::memcpy(&value, block.data() + offset, sizeof(T));
        return value;
    }
};

inline constexpr std::uint32_t Header() { return 0xc0000000u | (static_cast<std::uint32_t>(Words - 2) << 16u) | (0x10u << 8u); }

// Copies of the block and of what its pointers name (a null pointer leaves zeros and a clear bit).
inline Dispatch Capture(const void* block) {
    Dispatch dispatch;
    std::memcpy(dispatch.block.data(), block, BlockBytes);
    const auto copy = [&](std::size_t offset, Descriptor& target, std::uint32_t bit) {
        const auto* source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(dispatch.Field<std::uint64_t>(offset)));
        if (source == nullptr) return;
        std::memcpy(target.data(), source, sizeof(Descriptor));
        dispatch.present |= bit;
    };
    for (std::size_t index = 0; index < ImageOffsets.size(); ++index) copy(ImageOffsets[index], dispatch.images[index], 1u << index);
    copy(DescriptorOffset, dispatch.descriptor, 1u << ImageOffsets.size());
    return dispatch;
}

inline void Encode(const Dispatch& dispatch, std::span<std::uint32_t, Words> words) {
    const std::array<std::uint32_t, HeaderWords> header{Header(), Magic, KindDispatch, Version, dispatch.present};
    std::memcpy(words.data(), header.data(), sizeof(header));
    std::memcpy(words.data() + BlockWord, dispatch.block.data(), BlockBytes);
    std::memcpy(words.data() + ImageWord, dispatch.images.data(), sizeof(dispatch.images));
    std::memcpy(words.data() + DescriptorWord, dispatch.descriptor.data(), sizeof(Descriptor));
}

// The dispatch a packet carries; nullopt for any other packet.
inline std::optional<Dispatch> Decode(std::span<const std::uint32_t> words) {
    if (words.size() != Words || words[0] != Header() || words[1] != Magic || words[2] != KindDispatch || words[3] != Version) return std::nullopt;
    Dispatch dispatch;
    dispatch.present = words[4];
    std::memcpy(dispatch.block.data(), words.data() + BlockWord, BlockBytes);
    std::memcpy(dispatch.images.data(), words.data() + ImageWord, sizeof(dispatch.images));
    std::memcpy(dispatch.descriptor.data(), words.data() + DescriptorWord, sizeof(Descriptor));
    return dispatch;
}

struct ImageSize { std::uint32_t width, height, format; };

// A T#'s width, height and format field, decoded as Graphics/src/GuestTextureResource.cpp:56-58 does
// (that header pulls in the shader recompiler).
inline constexpr ImageSize DecodeImageSize(const Descriptor& words) {
    return {(((words[1] >> 30u) & 0x3u) | ((words[2] & 0xfffu) << 2u)) + 1u, ((words[2] >> 14u) & 0x3fffu) + 1u, (words[1] >> 20u) & 0x1ffu};
}

}

#endif
