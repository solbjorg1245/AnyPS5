#include "prx/libc/include/UiOverlay.hpp"
#include "prx/libc/include/ReadPatch.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "ui_overlay_tests: %s\n", what);
    std::abort();
}

// A button icon file as the dump has it: the header fields IsGlyphTexture reads, a zero surface.
static std::string GlyphFile() {
    std::string file(UiOverlay::CtxrHeaderBytes + UiOverlay::GlyphSurfaceBytes, '\0');
    file += std::string("RTXT\x6e\0\0\0\x41\0\x02\0", UiOverlay::CtxrFooterBytes);
    UiOverlay::WriteU32(file, UiOverlay::CtxrSizeOffset, static_cast<std::uint32_t>(UiOverlay::GlyphSurfaceBytes));
    const std::uint32_t words[] = {0x00000000u, 0xc8200000u, 0x801fc01fu, 0x90970facu, 0u, 0u, 0u, 0u};
    for (std::size_t i = 0; i < 8; ++i) UiOverlay::WriteU32(file, UiOverlay::CtxrDescriptorOffset + 4 * i, words[i]);
    file[0x10] = 1;
    return file;
}

static void TestTiling() {
    std::set<std::uint32_t> level0;
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const auto offset = UiOverlay::Standard64KbOffset(x, y);
            Require(offset % 4 == 0 && offset < UiOverlay::GlyphBlockBytes, "level 0 texel outside its block");
            level0.insert(offset);
        }
    }
    Require(level0.size() == 128 * 128, "level 0 texels must fill the 64 KiB block exactly once");
    std::set<std::uint32_t> tail;
    std::size_t texels = 0;
    for (std::uint32_t level = 1; level < UiOverlay::GlyphLevels; ++level) {
        const auto size = UiOverlay::LevelSize(level);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                tail.insert(UiOverlay::Standard64KbOffset(x + UiOverlay::GlyphTail[level - 1][0], y + UiOverlay::GlyphTail[level - 1][1]));
                ++texels;
            }
        }
    }
    Require(tail.size() == texels, "tail levels must not overlap");
    Require(UiOverlay::Standard64KbOffset(1, 0) == 4 && UiOverlay::Standard64KbOffset(0, 1) == 16, "standard swizzle low bits");
}

static void TestGlyphTexture() {
    const auto original = GlyphFile();
    Require(UiOverlay::IsGlyphTexture(original), "the dump's icon layout must be accepted");
    auto other = original;
    UiOverlay::WriteU32(other, UiOverlay::CtxrDescriptorOffset + 4, 0xc8f00000u);
    Require(!UiOverlay::IsGlyphTexture(other), "another format must be refused");
    Require(!UiOverlay::IsGlyphTexture(std::string_view(original).substr(1)), "another size must be refused");
    auto unsigned_ = original;
    unsigned_[unsigned_.size() - UiOverlay::CtxrFooterBytes] = 'X';
    Require(!UiOverlay::IsGlyphTexture(unsigned_), "a file without the RTXT footer must be refused");

    auto marked = original;
    // A byte no level covers (the gap after level 1's 64x64 texels in the tail block) keeps its value.
    std::set<std::uint32_t> covered;
    for (std::uint32_t level = 1; level < UiOverlay::GlyphLevels; ++level) {
        for (std::uint32_t y = 0; y < UiOverlay::LevelSize(level); ++y) {
            for (std::uint32_t x = 0; x < UiOverlay::LevelSize(level); ++x) covered.insert(UiOverlay::Standard64KbOffset(x + UiOverlay::GlyphTail[level - 1][0], y + UiOverlay::GlyphTail[level - 1][1]));
        }
    }
    std::uint32_t gap = 0;
    while (covered.count(gap) != 0) gap += 4;
    marked[UiOverlay::CtxrHeaderBytes + gap] = 0x5a;
    Require(UiOverlay::GlyphTexture(marked, UiOverlay::Glyph::A)[UiOverlay::CtxrHeaderBytes + gap] == 0x5a, "bytes no level covers keep the original's value");
    const auto replaced = UiOverlay::GlyphTexture(original, UiOverlay::Glyph::A);
    Require(replaced.size() == original.size() && replaced.compare(0, UiOverlay::CtxrHeaderBytes, original, 0, UiOverlay::CtxrHeaderBytes) == 0, "header kept, size unchanged");
    Require(UiOverlay::IsGlyphTexture(replaced), "the replacement keeps the layout");
    const auto texel = [&](std::uint32_t x, std::uint32_t y) { return UiOverlay::ReadU32(replaced, UiOverlay::CtxrHeaderBytes + UiOverlay::GlyphBlockBytes + UiOverlay::Standard64KbOffset(x, y)); };
    Require(texel(0, 0) == 0 && texel(127, 127) == 0, "corners are transparent");
    Require((texel(64, 64) >> 24) == 255 && (texel(64, 64) & 0xffu) < 80, "the disc is dark and opaque");
    Require((texel(64, 42) & 0xffu) > 200, "the A's apex is light");
    Require((texel(64, 11) & 0xffu) > 150 && (texel(64, 11) >> 24) == 255, "the rim is light");
    // Full-colour premultiplied RGBA like the title's icons (not a coverage mask): the alpha channel holds
    // the disc's shape, transparent outside it at every level.
    for (std::uint32_t level = 0; level < 5; ++level) {
        const auto size = UiOverlay::LevelSize(level);
        const auto texels = UiOverlay::RenderGlyph(UiOverlay::Glyph::A, size);
        Require(texels[0] == 0 && texels[size - 1] == 0 && texels[texels.size() - 1] == 0, "corners transparent at every level");
        Require((texels[static_cast<std::size_t>(size / 2) * size + size / 2] >> 24) == 255, "the disc is opaque at every level");
    }
    Require(replaced.compare(0, UiOverlay::CtxrHeaderBytes, std::string(original.substr(0, UiOverlay::CtxrHeaderBytes))) == 0 &&
            UiOverlay::ReadU32(replaced, UiOverlay::CtxrSizeOffset) == UiOverlay::GlyphSurfaceBytes, "the header round-trips");
    // The footer is what t665 lost (the surface was written 12 bytes late, over it): white squares in game.
    Require(replaced.compare(replaced.size() - UiOverlay::CtxrFooterBytes, UiOverlay::CtxrFooterBytes, original, original.size() - UiOverlay::CtxrFooterBytes, UiOverlay::CtxrFooterBytes) == 0,
            "the RTXT footer round-trips");
    for (const auto glyph : {UiOverlay::Glyph::A, UiOverlay::Glyph::B, UiOverlay::Glyph::X, UiOverlay::Glyph::Y, UiOverlay::Glyph::Menu, UiOverlay::Glyph::View}) {
        for (std::uint32_t level = 0; level < UiOverlay::GlyphLevels; ++level) {
            const auto texels = UiOverlay::RenderGlyph(glyph, UiOverlay::LevelSize(level));
            Require(texels.size() == static_cast<std::size_t>(UiOverlay::LevelSize(level)) * UiOverlay::LevelSize(level), "level size");
            for (const auto value : texels) {
                const auto alpha = value >> 24;
                Require((value & 0xffu) <= alpha && ((value >> 8) & 0xffu) <= alpha && ((value >> 16) & 0xffu) <= alpha, "texels are premultiplied");
            }
        }
    }
    Require(UiOverlay::RenderGlyph(UiOverlay::Glyph::B, 128) != UiOverlay::RenderGlyph(UiOverlay::Glyph::X, 128), "glyphs differ");
}

// A prefab: two components, then Root > MenuContents > (Row0, VisualMode > Settings_Int, Row1), Other.
struct PrefabWriter {
    std::string bytes;
    void u32(std::uint32_t value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xffu)); }
    void entity(const std::string& name, std::uint32_t references, std::uint32_t children) {
        bytes.append(16, static_cast<char>(name.size()));
        bytes.push_back(static_cast<char>(name.size()));
        bytes += name;
        u32(8);
        u32(references);
        for (std::uint32_t i = 0; i < references; ++i) {
            bytes.append(16, 'g');
            u32(i);
        }
        u32(children);
    }
};

static std::string Prefab() {
    PrefabWriter writer;
    writer.u32(2);
    writer.u32(0x5894a6c1u);
    writer.bytes.push_back(3);
    writer.bytes += "abc";
    writer.u32(0xe6c07a42u);
    writer.bytes += "\x81\x01";
    writer.bytes.append(129, 'x');
    writer.u32(2);
    writer.entity("Root", 1, 1);
    writer.entity("MenuContents", 0, 3);
    writer.entity("Row0", 2, 0);
    writer.entity("VisualMode", 2, 1);
    writer.entity("Settings_Int", 1, 0);
    writer.entity("Row1", 0, 0);
    writer.entity("Other", 0, 0);
    const auto footer = static_cast<std::uint32_t>(writer.bytes.size());
    writer.bytes += "PRPG";
    writer.u32(UiOverlay::PrefabVersion);
    writer.u32(footer);
    return writer.bytes;
}

static void TestPrefab() {
    const auto file = Prefab();
    std::string error;
    const auto prefab = UiOverlay::ParsePrefab(file, error);
    Require(prefab.has_value() && prefab->roots.size() == 2 && prefab->roots[0].children[0].children.size() == 3, "prefab parses");

    const auto edited = UiOverlay::RemovePrefabEntity(file, "MenuContents", "VisualMode", error);
    Require(edited.has_value(), "the row is removed");
    const auto after = UiOverlay::ParsePrefab(*edited, error);
    Require(after.has_value(), "the edited prefab parses");
    const auto& menu = after->roots[0].children[0];
    Require(menu.children.size() == 2 && menu.children[0].name == "Row0" && menu.children[1].name == "Row1", "the siblings stay in order");
    Require(after->roots[1].name == "Other", "later entities stay");
    const auto& removed = prefab->roots[0].children[0].children[1];
    Require(edited->size() == file.size() - (removed.end - removed.begin), "only the subtree's bytes go");
    Require(edited->compare(0, removed.begin, file, 0, removed.begin) != 0, "the parent's child count changed");

    Require(!UiOverlay::RemovePrefabEntity(file, "MenuContents", "Missing", error), "a missing row is refused");
    Require(!UiOverlay::RemovePrefabEntity(file, "Root", "VisualMode", error), "the parent name must match");
    auto twice = file;
    const auto at = twice.find("Row1");
    twice.replace(at, 4, "Row0");
    Require(UiOverlay::RemovePrefabEntity(twice, "MenuContents", "Row0", error) == std::nullopt, "an ambiguous row is refused");
    auto corrupt = file;
    UiOverlay::WriteU32(corrupt, corrupt.size() - 4, 7);
    Require(!UiOverlay::ParsePrefab(corrupt, error), "a wrong footer offset is refused");
    Require(!UiOverlay::ParsePrefab(std::string_view(file).substr(0, file.size() - 13), error), "a truncated file is refused");
}

static void TestSettings() {
    auto settings = PortSettings::Parse("ButtonIcons = SteamDeck\n", "test.ini");
    Require(settings.warnings.empty() && settings.buttonIcons == PortSettings::ButtonIcons::SteamDeck, "ButtonIcons = steamdeck");
    settings = PortSettings::Parse("ButtonIcons = xbox\n", "test.ini");
    Require(settings.warnings.size() == 1 && settings.buttonIcons == PortSettings::ButtonIcons::PlayStation, "an unknown icon set is refused");
    Require(!settings.Any(), "icons alone need no game arguments");
    PortSettings::ApplyEnvironment(settings, [](const char* name) -> const char* { return std::string_view(name) == "APS5_BUTTON_ICONS" ? "deck" : nullptr; });
    Require(settings.buttonIcons == PortSettings::ButtonIcons::SteamDeck, "APS5_BUTTON_ICONS overrides the file");

    PortSettings::Settings playstation;
    Require(!UiOverlay::ViewPressesTouchPad(playstation, nullptr), "View stays unmapped with PlayStation icons");
    Require(UiOverlay::ViewPressesTouchPad(settings, nullptr) && UiOverlay::ViewPressesTouchPad(settings, ""), "View is the touchpad with Steam Deck icons");
    Require(!UiOverlay::ViewPressesTouchPad(settings, "0") && UiOverlay::ViewPressesTouchPad(playstation, "1"), "APS5_VIEW_AS_TOUCHPAD forces it");
}

// A ctal file: a 3-byte path, then the entries (name, UV quad in the dump's corner order, page).
static std::string Ctal(const std::vector<std::array<std::uint32_t, 6>>& entries) {
    std::string ctal("\x03" "abc", 4);
    ctal += std::string(4, '\0');
    UiOverlay::WriteU32(ctal, 4, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [name, x0, y0, x1, y1, page] : entries) {
        std::string entry(UiOverlay::AtlasEntryBytes, '\0');
        UiOverlay::WriteU32(entry, 0, name);
        const float corners[8] = {x0 / 4096.0f, y0 / 4096.0f, x0 / 4096.0f, y1 / 4096.0f, x1 / 4096.0f, y1 / 4096.0f, x1 / 4096.0f, y0 / 4096.0f};
        std::memcpy(entry.data() + 4, corners, sizeof(corners));
        UiOverlay::WriteU32(entry, 36, page);
        ctal += entry;
    }
    return ctal;
}

static void TestAtlas() {
    const auto ctal = Ctal({{0x11u, 100, 120, 170, 190, 0}, {0x22u, 10, 10, 30, 40, 0}, {0x33u, 0, 0, 70, 70, 7}});
    const auto sprite = UiOverlay::FindAtlasSprite(ctal, 0x11u);
    Require(sprite && sprite->page == 0 && sprite->x == 100 && sprite->y == 120 && sprite->size == 70, "the ctal entry gives the sprite's texels");
    Require(!UiOverlay::FindAtlasSprite(ctal, 0x44u) && !UiOverlay::FindAtlasSprite(ctal, 0x22u) && !UiOverlay::FindAtlasSprite(ctal, 0x33u),
            "missing, non-square and off-page entries are refused");
    Require(!UiOverlay::FindAtlasSprite(std::string_view(ctal).substr(0, ctal.size() - 1), 0x11u), "a truncated ctal is refused");

    // The dump's circled Triangle: page 0 block (19, 23), texel (16, 0) inside it.
    Require(UiOverlay::AtlasTexelOffset(0, 2448, 2944) == 0x41u + 755u * 65536u + UiOverlay::Standard64KbOffset(16, 0), "atlas texel offset");
    Require(UiOverlay::AtlasTexelOffset(1, 0, 0) == 0x41u + UiOverlay::AtlasPageBytes, "pages follow each other");
    const auto offsets = UiOverlay::SpriteBlocks(*sprite);
    Require(offsets.size() == 4 && offsets[0] == UiOverlay::AtlasTexelOffset(0, 0, 0) && offsets[3] == UiOverlay::AtlasTexelOffset(0, 128, 128),
            "a sprite across a block corner touches 4 blocks");

    UiOverlay::AtlasBlocks blocks;
    for (const auto offset : offsets) {
        auto& block = blocks[offset];
        block.resize(UiOverlay::GlyphBlockBytes);
        for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>((i * 7 + offset) & 0xffu);
    }
    const auto before = blocks;
    // A sprite like the title's: transparent corners, grey rings from radius 20, a white face, a black symbol (the redraw makes it a dark disc, light letter).
    std::vector<std::uint32_t> title(70 * 70);
    for (std::uint32_t y = 0; y < 70; ++y) {
        for (std::uint32_t x = 0; x < 70; ++x) {
            const float dx = x + 0.5f - 35.0f, dy = y + 0.5f - 35.0f, radius = std::sqrt(dx * dx + dy * dy);
            const bool symbol = std::abs(std::abs(dx) - 11.0f) < 2.0f && std::abs(dy) < 13.0f;
            title[y * 70 + x] = radius >= 35.0f ? 0u : radius >= 20.0f ? 0xff9e9e9eu : symbol ? 0xff000000u : 0xffffffffu;
        }
    }
    UiOverlay::WriteSprite(blocks, *sprite, title);
    Require(UiOverlay::ReadSprite(blocks, *sprite) == title, "a sprite written into the blocks reads back");
    std::size_t changed = 0;
    for (const auto& [offset, block] : blocks) {
        for (std::uint32_t y = 0; y < 128; ++y) {
            for (std::uint32_t x = 0; x < 128; ++x) {
                const auto at = UiOverlay::Standard64KbOffset(x, y);
                if (block.compare(at, 4, before.at(offset), at, 4) == 0) continue;
                const auto px = static_cast<std::uint32_t>(offset == offsets[1] || offset == offsets[3] ? 128 : 0) + x;
                const auto py = static_cast<std::uint32_t>(offset == offsets[2] || offset == offsets[3] ? 128 : 0) + y;
                Require(px >= 100 && px < 170 && py >= 120 && py < 190, "texels outside the sprite keep their bytes");
                ++changed;
            }
        }
    }
    Require(changed > 0 && changed <= 70 * 70, "only the sprite's texels change");

    const auto redrawn = UiOverlay::RedrawCircledSprite(title, 70, UiOverlay::Glyph::X);
    for (std::uint32_t y = 0; y < 70; ++y) {
        for (std::uint32_t x = 0; x < 70; ++x) {
            const float dx = x + 0.5f - 35.0f, dy = y + 0.5f - 35.0f;
            if (dx * dx + dy * dy >= UiOverlay::AtlasFaceRadius * UiOverlay::AtlasFaceRadius) {
                Require(redrawn[y * 70 + x] == title[y * 70 + x], "the rings and corners keep the title's texels");
            } else {
                Require((redrawn[y * 70 + x] >> 24) == 255, "the face stays opaque");
            }
        }
    }
    Require((redrawn[35 * 70 + 35] & 0xffu) > 200, "the X crosses at the centre (light letter)");
    Require((redrawn[23 * 70 + 35] & 0xffu) < 80, "the face is a dark disc between the X's arms");
    Require((redrawn[35 * 70 + 46] & 0xffu) < 80 && (title[35 * 70 + 46] & 0xffu) == 0, "the title's symbol is gone");
    Require(UiOverlay::RedrawCircledSprite(title, 70, UiOverlay::Glyph::Y) != redrawn, "X and Y differ");

    Require(UiOverlay::SpriteHash({}) == 0xcbf29ce484222325ull && UiOverlay::SpriteHash({0x04030201u}) == 0xbe7a5e775165785dull,
            "the sprite hash is FNV-1a over the bytes in file order");
    Require(UiOverlay::SpriteHash(redrawn) != UiOverlay::SpriteHash(title), "the hash sees the change");

    std::string header(UiOverlay::CtxrHeaderBytes, '\0');
    UiOverlay::WriteU32(header, UiOverlay::CtxrSizeOffset, static_cast<std::uint32_t>(UiOverlay::AtlasPageBytes));
    const std::uint32_t words[] = {0xc8200000u, 0x83ffc3ffu, 0xd0900facu, 4u};
    for (std::size_t i = 0; i < 4; ++i) UiOverlay::WriteU32(header, UiOverlay::CtxrDescriptorOffset + 4 * (i + 1), words[i]);
    const std::string footer("RTXT\x6e\0\0\x01\x41\0\0\x14", UiOverlay::CtxrFooterBytes);
    Require(UiOverlay::IsAtlasPages(header, footer, UiOverlay::AtlasFileBytes) && UiOverlay::AtlasFileBytes == 335544397u, "the dump's atlas pages are accepted");
    Require(!UiOverlay::IsAtlasPages(header, footer, UiOverlay::AtlasFileBytes - 1), "another size is refused");
    auto other = header;
    UiOverlay::WriteU32(other, UiOverlay::CtxrDescriptorOffset + 8, 0x801fc01fu);
    Require(!UiOverlay::IsAtlasPages(other, footer, UiOverlay::AtlasFileBytes), "another descriptor is refused");
    Require(!UiOverlay::IsAtlasPages(header, std::string(UiOverlay::CtxrFooterBytes, '\0'), UiOverlay::AtlasFileBytes), "a file without the footer is refused");
}

static void TestReadPatch() {
    const ReadPatch::Ranges ranges{{10, "abcd"}, {100, "XY"}};
    std::string read = "012345";
    Require(ReadPatch::Apply(ranges, 8, read.data(), read.size()) == 4 && read == "01abcd", "a read over a range's start gets the patch");
    read = "012345";
    Require(ReadPatch::Apply(ranges, 12, read.data(), read.size()) == 2 && read == "cd2345", "a read from inside a range gets its tail");
    read = "0123456789";
    Require(ReadPatch::Apply(ranges, 0, read.data(), read.size()) == 0 && read == "0123456789", "a read before the ranges is untouched");
    read = "01";
    Require(ReadPatch::Apply(ranges, 99, read.data(), read.size()) == 1 && read == "0X", "a read ending inside a range");

    const auto path = std::filesystem::path("patched") / "Atlas.bin";
    Require(!ReadPatchesFor_nid_no_patch(path), "nothing is patched before a patch is added");
    AddReadPatch_nid_no_patch(path, 10, "abcd");
    AddReadPatch_nid_no_patch(path, 100, "XY");
    const auto found = ReadPatchesFor_nid_no_patch(std::filesystem::path("patched") / "." / "Atlas.bin");
    Require(found && found->size() == 2, "the ranges are found by the normalised path");
#ifdef _WIN32
    Require(ReadPatchesFor_nid_no_patch(std::filesystem::path("PATCHED") / "atlas.BIN") != nullptr, "the path's case does not matter on Windows");
#endif
    Require(!ReadPatchesFor_nid_no_patch(std::filesystem::path("patched") / "other.bin"), "other files are not patched");
    TrackReadPatchDescriptor_nid_no_patch(1000, path);
    Require(ReadPatchesForDescriptor_nid_no_patch(1000) == found, "a patched file's descriptor has its ranges");
    TrackReadPatchDescriptor_nid_no_patch(1000, std::filesystem::path("patched") / "other.bin");
    Require(!ReadPatchesForDescriptor_nid_no_patch(1000), "a reused descriptor starts clean");
    TrackReadPatchDescriptor_nid_no_patch(1001, path);
    ForgetReadPatchDescriptor_nid_no_patch(1001);
    Require(!ReadPatchesForDescriptor_nid_no_patch(1001), "a closed descriptor is forgotten");
}

int main() {
    TestTiling();
    TestGlyphTexture();
    TestPrefab();
    TestAtlas();
    TestReadPatch();
    TestSettings();
    std::puts("ui_overlay_tests: ok");
    return 0;
}
