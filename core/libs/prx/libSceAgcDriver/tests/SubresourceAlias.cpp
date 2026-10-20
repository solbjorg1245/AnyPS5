#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/SubresourceAlias.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

// The title's pair (port/reports/s64-perf-diag.md item 4): a 2560x1440 32-bit render target array
// of 15 MiB layers and the slice images over its layers.
SubresourceShape array4() {
    SubresourceShape shape;
    shape.baseAddress = 0x2c4900000ull;
    shape.layerBytes = 0xf00000ull;
    shape.layers = 4;
    shape.guestBytes = shape.layerBytes * shape.layers;
    shape.width = 2560;
    shape.height = 1440;
    shape.mipCount = 1;
    shape.elementBytes = 4;
    shape.blockWidth = 1;
    shape.tileMode = TextureTileMode::kR64KBX;
    shape.dimension = TextureDimension::k2DArray;
    shape.flat = true;
    shape.blockUnits = true;
    return shape;
}

SubresourceShape slice(std::uint32_t layer, std::uint32_t swizzleSlice) {
    auto shape = array4();
    shape.baseAddress += shape.layerBytes * layer;
    shape.layers = 1;
    shape.guestBytes = shape.layerBytes;
    shape.dimension = TextureDimension::k2D;
    shape.swizzleSlice = swizzleSlice;
    return shape;
}

void requireOffset(const SubresourceShape& reader, const SubresourceShape& writer, std::optional<std::int64_t> expected, const char* what) {
    const auto offset = SubresourceLayerOffset(reader, writer);
    Require(offset == expected, std::string("subresource pairing: ") + what);
}

void layerOffsetTests() {
    const auto array = array4();
    // A slice view of layer 1 (swizzled with slice 1, APS5_FIX_R0B) pairs both ways.
    requireOffset(array, slice(1, 1), -1, "the array takes layer 1 from the slice's layer 0");
    requireOffset(slice(1, 1), array, 1, "the slice takes its layer 0 from the array's layer 1");
    requireOffset(slice(3, 3), array, 3, "the last layer pairs");
    // Two slices of one array are not nested in each other.
    requireOffset(slice(1, 1), slice(2, 2), std::nullopt, "two slices do not pair");
    // The same surface is the same-surface alias's case.
    requireOffset(array, array, std::nullopt, "the same surface does not pair");
    // An XOR swizzle must take the same slice index for the paired layers.
    requireOffset(array, slice(1, 0), std::nullopt, "a slice swizzled as slice 0 does not pair with layer 1");
    auto standard = array;
    standard.tileMode = TextureTileMode::kStandard64KB;
    auto standardSlice = slice(1, 0);
    standardSlice.tileMode = TextureTileMode::kStandard64KB;
    requireOffset(standard, standardSlice, -1, "a tiling without XOR swizzle ignores the slice index");
    // Same extent, mips, element size and tiling.
    auto other = slice(1, 1);
    other.width = 1280;
    requireOffset(array, other, std::nullopt, "another width does not pair");
    other = slice(1, 1);
    other.height = 720;
    requireOffset(array, other, std::nullopt, "another height does not pair");
    other = slice(1, 1);
    other.elementBytes = 8;
    requireOffset(array, other, std::nullopt, "another element size does not pair");
    other = slice(1, 1);
    other.tileMode = TextureTileMode::kD64KBX;
    requireOffset(array, other, std::nullopt, "another tiling does not pair");
    other = slice(1, 1);
    other.mipCount = 2;
    requireOffset(array, other, std::nullopt, "another mip count does not pair");
    other = slice(1, 1);
    other.blockWidth = 4;
    requireOffset(array, other, std::nullopt, "another block width does not pair");
    // Layer ranges: whole layers inside the other surface.
    other = slice(1, 1);
    other.baseAddress += 0x10000;
    requireOffset(array, other, std::nullopt, "a slice off the layer boundary does not pair");
    requireOffset(array, slice(4, 4), std::nullopt, "a slice past the array does not pair");
    auto pair = array;
    pair.layers = 2;
    pair.guestBytes = pair.layerBytes * 2;
    pair.baseAddress += pair.layerBytes * 2;
    pair.swizzleSlice = 2;
    requireOffset(array, pair, -2, "a two-layer range pairs at its first layer");
    pair.baseAddress += pair.layerBytes;
    pair.swizzleSlice = 3;
    requireOffset(array, pair, std::nullopt, "a layer range reaching past the array does not pair");
    // DCC, untracked units, thick or volume surfaces and linear ones are excluded.
    other = slice(1, 1);
    other.dcc = true;
    requireOffset(array, other, std::nullopt, "a DCC writer does not pair");
    requireOffset(other, array, std::nullopt, "a DCC reader does not pair");
    other = slice(1, 1);
    other.blockUnits = false;
    requireOffset(array, other, std::nullopt, "a surface without block units does not pair");
    other = slice(1, 1);
    other.flat = false;
    requireOffset(array, other, std::nullopt, "a thick surface does not pair");
    other = slice(1, 1);
    other.dimension = TextureDimension::k3D;
    requireOffset(array, other, std::nullopt, "a volume does not pair");
    other = slice(1, 1);
    other.dimension = TextureDimension::kCube;
    requireOffset(array, other, std::nullopt, "a cube does not pair");
    auto linear = array;
    linear.tileMode = TextureTileMode::kLinear;
    auto linearSlice = slice(1, 1);
    linearSlice.tileMode = TextureTileMode::kLinear;
    requireOffset(linear, linearSlice, std::nullopt, "linear surfaces do not pair");
    auto inconsistent = array;
    inconsistent.guestBytes += 0x10000;
    requireOffset(inconsistent, slice(1, 1), std::nullopt, "a surface whose layers do not fill it does not pair");
}

void mismatchTests() {
    std::int64_t offset = 7;
    Require(SubresourcePairing(array4(), slice(1, 1), offset) == SubresourceMismatch::None && offset == -1, "a pairing names no mismatch");
    Require(SubresourcePairing(array4(), slice(1, 0), offset) == SubresourceMismatch::Swizzle && offset == 0, "a swizzle mismatch is named");
    auto other = slice(1, 1);
    other.width = 1280;
    Require(SubresourcePairing(array4(), other, offset) == SubresourceMismatch::Extent, "an extent mismatch is named");
    Require(SubresourcePairing(slice(1, 1), slice(2, 2), offset) == SubresourceMismatch::NotNested, "two slices are not nested");
    other = slice(1, 1);
    other.dcc = true;
    Require(SubresourcePairing(array4(), other, offset) == SubresourceMismatch::Dcc, "a DCC writer is named");
    Require(std::string(SubresourceMismatchName(SubresourceMismatch::LayerAlignment)) == "layer alignment", "mismatch names");
}

void storedOverTests() {
    constexpr std::uint64_t base = 0x2c4900000;
    // A reader of 8 units; lender A has units 1-2 pending, lender B unit 6.
    std::vector<SubresourceLender> lenders(2);
    lenders[0] = {base + 0x10000, base + 0x30000, std::vector<bool>{false, true, true, false, false, false, false, false}};
    lenders[1] = {base + 0x60000, base + 0x70000, std::vector<bool>{false, false, false, false, false, false, true, false}};
    auto given = SubresourceStoredOver(base, 8, {}, lenders);
    Require(!given[0] && !given[1], "no other store keeps every lender");
    given = SubresourceStoredOver(base, 8, {{base + 0x48000, base + 0x50000}}, lenders);
    Require(!given[0] && !given[1], "a store over units no lender has pending keeps every lender");
    given = SubresourceStoredOver(base, 8, {{base + 0x2ffff, base + 0x30001}}, lenders);
    Require(given[0] && !given[1], "a store reaching a lent unit by one byte gives that lender back");
    // Given back, A is stored over its whole range: a lender pending inside it follows.
    lenders[0].end = base + 0x70000;
    given = SubresourceStoredOver(base, 8, {{base + 0x20000, base + 0x21000}}, lenders);
    Require(given[0] && given[1], "a lender given back gives back the lenders under its range");
    given = SubresourceStoredOver(base, 8, {{base - 0x10000, base}, {base + 0x80000, base + 0x90000}}, lenders);
    Require(!given[0] && !given[1], "stores outside the reader keep every lender");
}

void unitPlanTests() {
    using Plan = SubresourceUnitPlan;
    SubresourceUnit unit;
    Require(PlanSubresourceUnit(unit) == Plan::Leave, "a unit the writer has nothing pending in is left");
    unit.writerPending = true;
    Require(PlanSubresourceUnit(unit) == Plan::Borrow, "a pending unstamped unit is borrowed");
    unit.held = true;
    Require(PlanSubresourceUnit(unit) == Plan::Leave, "a unit held at the writer's version is left");
    unit.held = false;
    // A CPU (or driver) store since the writer's generation wins over its results at their
    // write-back: the reader reads memory, the pair stays.
    unit.writerStamped = 1;
    Require(PlanSubresourceUnit(unit) == Plan::Leave, "a CPU-stamped unit is not borrowed");
    unit.readerPending = true;
    Require(PlanSubresourceUnit(unit) == Plan::Leave, "a CPU-stamped unit pending in the reader takes the reader's own rule");
    unit.writerStamped = 0;
    Require(PlanSubresourceUnit(unit) == Plan::Reject, "a unit pending in both images takes the old order");
    unit.readerPending = false;
    unit.writerStamped = 2;
    Require(PlanSubresourceUnit(unit) == Plan::Reject, "an unknown stamp takes the old order");
    unit.writerStamped = 0;
    unit.writerTracked = false;
    Require(PlanSubresourceUnit(unit) == Plan::Reject, "an untracked writer unit takes the old order");
    unit.writerTracked = true;
    unit.sameExtent = false;
    Require(PlanSubresourceUnit(unit) == Plan::Reject, "units of different extents take the old order");
    unit.sameExtent = true;
    unit.writerPending = false;
    unit.readerPending = true;
    Require(PlanSubresourceUnit(unit) == Plan::Leave, "a unit pending in the reader alone is left");
}

}

void RunSubresourceAliasTests() {
    layerOffsetTests();
    mismatchTests();
    storedOverTests();
    unitPlanTests();
}
