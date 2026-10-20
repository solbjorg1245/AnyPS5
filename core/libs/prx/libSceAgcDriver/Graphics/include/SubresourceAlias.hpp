#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SUBRESOURCEALIAS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SUBRESOURCEALIAS_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace AgcDriver::Graphics {

// APS5_SUBALIAS_REFRESH=1 (default off): a storage refresh takes the pending results of images
// over a layer range of its memory (a slice image inside this array, or the array this slice is
// one layer of) from their images on the device, a copy with a layer offset, instead of having
// them written back to guest memory and read in again (StorageTexture::pendingSubAliases). The
// pending image stays responsible for storing them for a real reader of the memory.
inline bool SubaliasRefreshEnabled() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_SUBALIAS_REFRESH");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return on;
}

// What the pairing rules read of a storage image.
struct SubresourceShape {
    std::uint64_t baseAddress = 0;
    std::uint64_t guestBytes = 0;
    // One array layer's guest bytes (every mip of the layer: a layer's slices sit together).
    std::uint64_t layerBytes = 0;
    std::uint32_t layers = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 1;
    std::uint32_t elementBytes = 0;
    std::uint32_t blockWidth = 1;
    TextureTileMode tileMode = TextureTileMode::kLinear;
    TextureDimension dimension = TextureDimension::k2D;
    std::uint32_t swizzleSlice = 0;
    bool dcc = false;
    // Thin layers, one image depth, layers * layerBytes == guestBytes.
    bool flat = false;
    // 64 KiB write-stamp units at a 64 KiB aligned base (StorageTexture::blockUnits).
    bool blockUnits = false;
};

// Why two storage images do not pair (SubresourcePairing), counted per reason on the [storage]
// subalias line.
enum class SubresourceMismatch {
    None,
    // Either surface is not 2D or a 2D array.
    Dimension,
    // Thick, volume, or a layer count the image does not have.
    Layout,
    // No 64 KiB block units (an unaligned base, or block tracking off).
    Units,
    Dcc,
    Linear,
    Same,
    Extent,
    Mips,
    ElementSize,
    Tiling,
    LayerBytes,
    NotNested,
    LayerAlignment,
    Swizzle,
    Count
};

inline const char* SubresourceMismatchName(SubresourceMismatch mismatch) {
    static constexpr const char* names[static_cast<std::size_t>(SubresourceMismatch::Count)] = {"none", "dimension", "layout", "units", "dcc", "linear", "same surface", "extent", "mips", "element size", "tiling", "layer bytes", "not nested", "layer alignment", "swizzle"};
    return mismatch < SubresourceMismatch::Count ? names[static_cast<std::size_t>(mismatch)] : "?";
}

// The layer offset d such that layer j of `reader` holds the texels of layer j + d of `writer`
// (a device copy moves them bit for bit, as a write-back and a re-upload through guest memory
// would), or why there is none. The two must be distinct surfaces of one memory, one inside the
// other, with the same per-layer layout: extent, mips, element size and tiling, 2D or 2D array,
// thin, both tracked in 64 KiB units, no DCC (its keys are stored by the write-back), and an XOR
// swizzle taking the same slice index for the paired layers.
inline SubresourceMismatch SubresourcePairing(const SubresourceShape& reader, const SubresourceShape& writer, std::int64_t& offset) {
    offset = 0;
    const auto planar = [](const SubresourceShape& shape) {
        if (shape.dimension != TextureDimension::k2D && shape.dimension != TextureDimension::k2DArray) return SubresourceMismatch::Dimension;
        if (!shape.flat || shape.layers == 0 || shape.layerBytes == 0 || shape.layerBytes * shape.layers != shape.guestBytes) return SubresourceMismatch::Layout;
        if (!shape.blockUnits) return SubresourceMismatch::Units;
        if (shape.dcc) return SubresourceMismatch::Dcc;
        if (shape.tileMode == TextureTileMode::kLinear) return SubresourceMismatch::Linear;
        return SubresourceMismatch::None;
    };
    if (const auto mismatch = planar(reader); mismatch != SubresourceMismatch::None) return mismatch;
    if (const auto mismatch = planar(writer); mismatch != SubresourceMismatch::None) return mismatch;
    if (reader.baseAddress == writer.baseAddress && reader.guestBytes == writer.guestBytes) return SubresourceMismatch::Same;
    if (reader.width != writer.width || reader.height != writer.height) return SubresourceMismatch::Extent;
    if (reader.mipCount != writer.mipCount) return SubresourceMismatch::Mips;
    if (reader.elementBytes != writer.elementBytes || reader.blockWidth != writer.blockWidth) return SubresourceMismatch::ElementSize;
    if (reader.tileMode != writer.tileMode) return SubresourceMismatch::Tiling;
    if (reader.layerBytes != writer.layerBytes) return SubresourceMismatch::LayerBytes;
    const auto readerEnd = reader.baseAddress + reader.guestBytes;
    const auto writerEnd = writer.baseAddress + writer.guestBytes;
    const bool readerInside = writer.baseAddress <= reader.baseAddress && readerEnd <= writerEnd;
    const bool writerInside = reader.baseAddress <= writer.baseAddress && writerEnd <= readerEnd;
    if (!readerInside && !writerInside) return SubresourceMismatch::NotNested;
    const auto distance = reader.baseAddress >= writer.baseAddress ? reader.baseAddress - writer.baseAddress : writer.baseAddress - reader.baseAddress;
    if (distance % reader.layerBytes != 0) return SubresourceMismatch::LayerAlignment;
    const auto layers = static_cast<std::int64_t>(distance / reader.layerBytes);
    const auto pairing = reader.baseAddress >= writer.baseAddress ? layers : -layers;
    if (XorSwizzleMode(reader.tileMode) != 0 && static_cast<std::int64_t>(reader.swizzleSlice) != static_cast<std::int64_t>(writer.swizzleSlice) + pairing) return SubresourceMismatch::Swizzle;
    offset = pairing;
    return SubresourceMismatch::None;
}

inline std::optional<std::int64_t> SubresourceLayerOffset(const SubresourceShape& reader, const SubresourceShape& writer) {
    std::int64_t offset = 0;
    if (SubresourcePairing(reader, writer, offset) != SubresourceMismatch::None) return std::nullopt;
    return offset;
}

// One 64 KiB unit of the reader where the writer has a unit at the same address.
struct SubresourceUnit {
    bool readerPending = false;
    bool writerPending = false;
    // The two units cover the same bytes (a short last unit of either does not).
    bool sameExtent = true;
    // The writer's unit has a write generation (untracked units are stored whole).
    bool writerTracked = true;
    // GuestMemory::ChangedBlocks of the block since the writer unit's generation: 0 unchanged,
    // 1 written (a CPU or driver store: it wins over the results at their write-back), 2 unknown.
    std::uint8_t writerStamped = 0;
    // The reader already holds the writer's results at the writer's current version.
    bool held = false;
};

enum class SubresourceUnitPlan {
    // Nothing of the writer's to take: not pending, held already, or dead to its write-back.
    Leave,
    // Copy the writer's results into the reader's unit.
    Borrow,
    // The pair takes the old order (the writer is stored, the reader reads memory).
    Reject
};

inline SubresourceUnitPlan PlanSubresourceUnit(const SubresourceUnit& unit) {
    if (!unit.writerPending) return SubresourceUnitPlan::Leave;
    if (!unit.sameExtent || !unit.writerTracked || unit.writerStamped > 1) return SubresourceUnitPlan::Reject;
    if (unit.writerStamped != 0) return SubresourceUnitPlan::Leave;
    // A unit pending in both images: whose results are newer is not known here.
    if (unit.readerPending) return SubresourceUnitPlan::Reject;
    if (unit.held) return SubresourceUnitPlan::Leave;
    return SubresourceUnitPlan::Borrow;
}

}

#endif
