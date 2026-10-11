#include "prx/libc/include/UiOverlay.hpp"

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

int main() {
    TestTiling();
    TestGlyphTexture();
    TestPrefab();
    TestSettings();
    std::puts("ui_overlay_tests: ok");
    return 0;
}
