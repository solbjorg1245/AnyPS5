#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_UIOVERLAY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_UIOVERLAY_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prx/libc/include/PortSettings.hpp"

// Demon's Souls (PPSA01341) UI data overlays: files made from the dump's own UI data at the first guest
// path resolve, written to anyps5-overrides/ui (next to the executable, else the per-user folder; the dump
// is never written) and aliased over the title's paths. Both are off unless asked for.
// 1. Button icons (ButtonIcons = steamdeck, APS5_BUTTON_ICONS). Every prompt draws one of the 24 textures
//    ui/textures/buttonicons/_ps5/ps4_*.ctxr ($/ui/textures/buttonicons/****/ps4_*.ctxr in uiglobal.cgpr,
//    uistartmenu.cgpr, uimovies.cgpr, uicharactercreator.cgpr). The title has no other glyph set (no PC or
//    Xbox textures, no platform switch in the UI data or the executable), so the face buttons, Options and
//    Create/touchpad are redrawn here in a Steam Deck style (our own shapes: a dark disc with a light rim and
//    a letter, three bars or two squares) in the original's format; L1/R1/L2/R2/L3/R3, the sticks, the d-pad
//    and the touchpad swipes keep the title's art (the Deck uses the same names). A texture whose header is
//    not the expected 128x128 RGBA8 sRGB 8-level SW_64KB_S surface keeps the original.
// 2. The Visual Mode row (APS5_HIDE_VISUAL_MODE_ROW=1, experimental). Options > Display is the entity
//    tree of ui/scripts/_cmn/uistartmenu.cgpr (SettingsPages/SettingsPageBG/MenuContents: DisplayAdjust,
//    StatSpacer, VisualMode, MotionBlur, FilmGrain, Filters_List, FilterIntensity); the copy drops the
//    VisualMode entity and its subtree. Its components stay in the file, unreferenced. GraphicsModePin
//    already makes the row inert; this removes it from the page.
namespace UiOverlay {

inline constexpr const char* DirectoryName = "ui";
inline constexpr const char* HideVisualModeRowSwitch = "APS5_HIDE_VISUAL_MODE_ROW";
inline constexpr const char* ViewAsTouchPadSwitch = "APS5_VIEW_AS_TOUCHPAD";
// Debug aid: APS5_BUTTON_ICONS_PROBE=<names> (e.g. cross,circle: a SteamDeckGlyphs name without ps4_,
// button_ and .ctxr) writes those textures through the overlay unchanged, to tell a load-path fault from
// a content one.
inline constexpr const char* ButtonIconsProbeSwitch = "APS5_BUTTON_ICONS_PROBE";

inline std::uint32_t ReadU32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}

inline void WriteU32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xffu);
}

// ---- Button icon textures -------------------------------------------------------------------------------

// A ctxr texture: a 77-byte header (the surface size at +0x18, the 8-dword T# at +0x21 with a zero base
// address), then the surface as the GPU reads it. Each button icon is 128x128 R8G8B8A8_SRGB (format 130),
// 8 levels, SW_64KB_S (tile mode 9): the tail block (levels 1-7) first, then level 0, 64 KiB each.
// Texels are premultiplied by alpha.
inline constexpr std::size_t CtxrHeaderBytes = 77;
inline constexpr std::size_t CtxrSizeOffset = 0x18;
inline constexpr std::size_t CtxrDescriptorOffset = 0x21;
inline constexpr std::uint32_t GlyphSize = 128;
inline constexpr std::uint32_t GlyphLevels = 8;
inline constexpr std::size_t GlyphBlockBytes = 65536;
inline constexpr std::size_t GlyphSurfaceBytes = 2 * GlyphBlockBytes;

// True when `file` is a button icon texture in the layout GlyphSurface writes.
inline bool IsGlyphTexture(std::string_view file) {
    if (file.size() != CtxrHeaderBytes + GlyphSurfaceBytes || ReadU32(file, CtxrSizeOffset) != GlyphSurfaceBytes) return false;
    std::array<std::uint32_t, 8> words{};
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = ReadU32(file, CtxrDescriptorOffset + 4 * i);
    const auto format = (words[1] >> 20u) & 0x1ffu;
    const auto width = (((words[1] >> 30u) & 0x3u) | ((words[2] & 0xfffu) << 2u)) + 1u;
    const auto height = ((words[2] >> 14u) & 0x3fffu) + 1u;
    const auto baseLevel = (words[3] >> 12u) & 0xfu;
    const auto lastLevel = (words[3] >> 16u) & 0xfu;
    const auto tileMode = (words[3] >> 20u) & 0x1fu;
    const auto bcSwizzle = (words[3] >> 25u) & 0x7u;
    const auto type = (words[3] >> 28u) & 0xfu;
    const auto depth = words[4] & 0x1fffu;
    return format == 130u && width == GlyphSize && height == GlyphSize && baseLevel == 0u && lastLevel == GlyphLevels - 1u &&
        tileMode == 9u && bcSwizzle == 0u && type == 9u && depth == 0u;
}

// Byte offset of 4-byte texel (x, y) in a SW_64KB_S block: the driver's TextureDetile.comp standardOffset
// and standard64Extra for 4-byte elements (x, y < 128 at level 0; tail texels add their tail position).
constexpr std::uint32_t Standard64KbOffset(std::uint32_t x, std::uint32_t y) {
    const auto offset = ((y << 4u) & 0x070u) ^ ((y << 5u) & 0x100u) ^ ((y << 6u) & 0x400u) ^ ((x << 2u) & 0x00cu) ^ ((x << 5u) & 0x080u) ^
        ((x << 6u) & 0x200u) ^ ((x << 7u) & 0x800u) ^ ((x << 8u) & 0x2000u) ^ ((x << 9u) & 0x8000u) ^ ((y << 7u) & 0x1000u) ^ ((y << 8u) & 0x4000u);
    return offset & 0xffffu;
}

// Where levels 1-7 sit in the tail block (TextureTiling.cpp kMipTailThin64KB, 4-byte elements).
inline constexpr std::array<std::array<std::uint32_t, 2>, GlyphLevels - 1> GlyphTail{{{64, 0}, {0, 64}, {32, 0}, {0, 32}, {16, 0}, {8, 16}, {0, 24}}};

inline std::uint32_t LevelSize(std::uint32_t level) { return std::max(GlyphSize >> level, 1u); }

// The surface bytes of a mip chain written over `surface` (the original's, so the bytes no level covers
// stay as the title has them): levels[n] holds LevelSize(n)^2 texels row by row, each R, G, B, A from the
// low byte up.
inline std::string GlyphSurface(const std::array<std::vector<std::uint32_t>, GlyphLevels>& levels, std::string surface = std::string(GlyphSurfaceBytes, '\0')) {
    for (std::uint32_t level = 0; level < GlyphLevels; ++level) {
        const auto size = LevelSize(level);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::size_t offset = level == 0 ? GlyphBlockBytes + Standard64KbOffset(x, y) : Standard64KbOffset(x + GlyphTail[level - 1][0], y + GlyphTail[level - 1][1]);
                WriteU32(surface, offset, levels[level][static_cast<std::size_t>(y) * size + x]);
            }
        }
    }
    return surface;
}

enum class Glyph { A, B, X, Y, Menu, View };

struct GlyphFile {
    const char* name;
    Glyph glyph;
};

inline constexpr const char* GlyphGuestDirectory = "/app0/ui/textures/buttonicons/_ps5/";
// Cross -> A, Circle -> B, Square -> X, Triangle -> Y, Options -> Menu, Create and the touchpad -> View.
inline constexpr std::array<GlyphFile, 7> SteamDeckGlyphs{{
    {"ps4_button_cross.ctxr", Glyph::A}, {"ps4_button_circle.ctxr", Glyph::B}, {"ps4_button_square.ctxr", Glyph::X},
    {"ps4_button_triangle.ctxr", Glyph::Y}, {"ps4_options.ctxr", Glyph::Menu}, {"ps4_share.ctxr", Glyph::View},
    {"ps4_touchpad.ctxr", Glyph::View}}};

struct GlyphSegment {
    float ax, ay, bx, by;
};

// The strokes of a glyph in 128-unit space (round caps, StrokeWidth wide); arcs become short segments.
inline std::vector<GlyphSegment> GlyphStrokes(Glyph glyph) {
    std::vector<GlyphSegment> strokes;
    const auto arc = [&strokes](float cx, float cy, float radius, float fromDegrees, float toDegrees) {
        constexpr int steps = 12;
        constexpr float radians = 3.14159265358979f / 180.0f;
        for (int i = 0; i < steps; ++i) {
            const float a0 = (fromDegrees + (toDegrees - fromDegrees) * static_cast<float>(i) / steps) * radians;
            const float a1 = (fromDegrees + (toDegrees - fromDegrees) * static_cast<float>(i + 1) / steps) * radians;
            strokes.push_back({cx + radius * std::cos(a0), cy + radius * std::sin(a0), cx + radius * std::cos(a1), cy + radius * std::sin(a1)});
        }
    };
    switch (glyph) {
        case Glyph::A: strokes = {{64, 37, 45, 91}, {64, 37, 83, 91}, {52, 73, 76, 73}}; break;
        case Glyph::X: strokes = {{46, 38, 82, 90}, {82, 38, 46, 90}}; break;
        case Glyph::Y: strokes = {{46, 38, 64, 63}, {82, 38, 64, 63}, {64, 63, 64, 90}}; break;
        case Glyph::B:
            strokes = {{50, 38, 50, 90}, {50, 38, 65, 38}, {50, 63, 66, 63}, {50, 90, 67, 90}};
            arc(65.0f, 50.5f, 12.5f, -90.0f, 90.0f);
            arc(67.0f, 76.5f, 13.5f, -90.0f, 90.0f);
            break;
        case Glyph::Menu:
        case Glyph::View: break;
    }
    return strokes;
}

inline constexpr float StrokeWidth = 10.5f;

struct GlyphColor {
    float r, g, b, a;
};

// The colour at a point of the 128-unit glyph (sRGB-coded, straight alpha): a light rim (radius 51-55)
// around a dark disc shaded top to bottom, the symbol in near-white.
inline GlyphColor GlyphSample(Glyph glyph, const std::vector<GlyphSegment>& strokes, float x, float y) {
    constexpr GlyphColor clear{0.0f, 0.0f, 0.0f, 0.0f};
    constexpr GlyphColor rim{0.84f, 0.84f, 0.84f, 1.0f};
    constexpr GlyphColor ink{0.95f, 0.95f, 0.95f, 1.0f};
    const float dx = x - 64.0f;
    const float dy = y - 64.0f;
    const float radius2 = dx * dx + dy * dy;
    if (radius2 > 55.0f * 55.0f) return clear;
    if (radius2 > 51.0f * 51.0f) return rim;
    const float t = std::clamp((y - 13.0f) / 102.0f, 0.0f, 1.0f);
    const GlyphColor body{0.20f - 0.08f * t, 0.21f - 0.08f * t, 0.23f - 0.08f * t, 1.0f};
    const auto inside = [x, y](float x0, float y0, float x1, float y1) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; };
    switch (glyph) {
        case Glyph::Menu:
            for (const float bar : {48.0f, 64.0f, 80.0f}) {
                if (std::abs(y - bar) <= 3.75f && x >= 42.0f && x <= 86.0f) return ink;
            }
            return body;
        case Glyph::View:
            // The front square (lower right) hides the back one (upper left) where they overlap.
            if (inside(54.0f, 54.0f, 86.0f, 86.0f)) return inside(59.0f, 59.0f, 81.0f, 81.0f) ? body : ink;
            if (inside(42.0f, 42.0f, 74.0f, 74.0f) && !inside(47.0f, 47.0f, 69.0f, 69.0f)) return ink;
            return body;
        default: break;
    }
    constexpr float half2 = StrokeWidth * StrokeWidth / 4.0f;
    for (const auto& stroke : strokes) {
        const float sx = stroke.bx - stroke.ax;
        const float sy = stroke.by - stroke.ay;
        const float length2 = sx * sx + sy * sy;
        const float along = length2 == 0.0f ? 0.0f : std::clamp(((x - stroke.ax) * sx + (y - stroke.ay) * sy) / length2, 0.0f, 1.0f);
        const float qx = stroke.ax + along * sx - x;
        const float qy = stroke.ay + along * sy - y;
        if (qx * qx + qy * qy <= half2) return ink;
    }
    return body;
}

// One level of a glyph, `size` texels square, 4x4 samples per texel, premultiplied by alpha.
inline std::vector<std::uint32_t> RenderGlyph(Glyph glyph, std::uint32_t size) {
    constexpr int samples = 4;
    const auto strokes = GlyphStrokes(glyph);
    const float scale = static_cast<float>(GlyphSize) / static_cast<float>(size);
    std::vector<std::uint32_t> texels(static_cast<std::size_t>(size) * size);
    for (std::uint32_t py = 0; py < size; ++py) {
        for (std::uint32_t px = 0; px < size; ++px) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const float gx = (static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / samples) * scale;
                    const float gy = (static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / samples) * scale;
                    const auto color = GlyphSample(glyph, strokes, gx, gy);
                    r += color.r * color.a;
                    g += color.g * color.a;
                    b += color.b * color.a;
                    a += color.a;
                }
            }
            const auto byte = [](float sum) { return static_cast<std::uint32_t>(std::clamp(sum / (samples * samples) * 255.0f + 0.5f, 0.0f, 255.0f)); };
            texels[static_cast<std::size_t>(py) * size + px] = byte(r) | (byte(g) << 8u) | (byte(b) << 16u) | (byte(a) << 24u);
        }
    }
    return texels;
}

// The replacement file: the original's header (checked by IsGlyphTexture) and its surface with the glyph's
// levels written in.
inline std::string GlyphTexture(std::string_view original, Glyph glyph) {
    std::array<std::vector<std::uint32_t>, GlyphLevels> levels;
    for (std::uint32_t level = 0; level < GlyphLevels; ++level) levels[level] = RenderGlyph(glyph, LevelSize(level));
    return std::string(original.substr(0, CtxrHeaderBytes)) + GlyphSurface(levels, std::string(original.substr(CtxrHeaderBytes)));
}

// The Steam Deck's View button (SDL Back; no PlayStation button reaches it otherwise, and a DualSense's
// Create reports the same) presses the touchpad while the prompts show Steam Deck icons, which draw the
// touchpad as View. APS5_VIEW_AS_TOUCHPAD=1 or =0 forces it either way.
inline bool ViewPressesTouchPad(const PortSettings::Settings& settings, const char* switchValue) {
    if (switchValue != nullptr && switchValue[0] != '\0') return std::string_view(switchValue) != "0";
    return settings.buttonIcons == PortSettings::ButtonIcons::SteamDeck;
}

// ---- Compiled prefabs (.cgpr) -----------------------------------------------------------------------------

// A compiled prefab: u32 component count; the components (u32 type hash, LEB128 size, payload); the entity
// tree (u32 root count, then per entity a 16-byte GUID, u8 name length, the name, u32 flags, u32 component
// count, that many (16-byte GUID, u32 component index), u32 child count, the children depth first); then
// the footer "PRPG", u32 version (134), u32 offset of the footer. Entities name their components by index,
// so an entity can go without touching the components.
struct PrefabEntity {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t childCountOffset = 0;
    std::string name;
    std::vector<PrefabEntity> children;
};

struct Prefab {
    std::size_t rootCountOffset = 0;
    std::size_t footer = 0;
    std::vector<PrefabEntity> roots;
};

inline constexpr std::uint32_t PrefabVersion = 134;

// Parses `file`; nullopt (and `error`) when it is not a whole prefab of the known version.
inline std::optional<Prefab> ParsePrefab(std::string_view file, std::string& error) {
    const auto fail = [&error](std::string text) -> std::optional<Prefab> { error = std::move(text); return std::nullopt; };
    if (file.size() < 16 || file.substr(file.size() - 12, 4) != "PRPG") return fail("no PRPG footer");
    Prefab prefab;
    prefab.footer = file.size() - 12;
    if (ReadU32(file, prefab.footer + 4) != PrefabVersion) return fail("prefab version " + std::to_string(ReadU32(file, prefab.footer + 4)));
    if (ReadU32(file, prefab.footer + 8) != prefab.footer) return fail("footer offset mismatch");
    std::size_t at = 0;
    const auto need = [&](std::size_t bytes) { return at + bytes <= prefab.footer; };
    if (!need(4)) return fail("truncated");
    const auto components = ReadU32(file, at);
    at += 4;
    for (std::uint32_t i = 0; i < components; ++i) {
        if (!need(5)) return fail("truncated component " + std::to_string(i));
        at += 4;
        std::uint64_t size = 0;
        for (unsigned shift = 0;; shift += 7) {
            if (!need(1) || shift > 28) return fail("bad component size " + std::to_string(i));
            const auto byte = static_cast<unsigned char>(file[at++]);
            size |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
            if (byte < 0x80u) break;
        }
        if (!need(size)) return fail("truncated component " + std::to_string(i));
        at += size;
    }
    const std::function<bool(PrefabEntity&, int)> entity = [&](PrefabEntity& node, int depth) {
        if (depth > 64 || !need(17)) return false;
        node.begin = at;
        const auto length = static_cast<unsigned char>(file[at + 16]);
        at += 17;
        if (!need(length + 8u)) return false;
        node.name = std::string(file.substr(at, length));
        at += length + 4;
        const auto references = ReadU32(file, at);
        at += 4;
        if (references > (prefab.footer - at) / 20) return false;
        at += static_cast<std::size_t>(references) * 20;
        if (!need(4)) return false;
        node.childCountOffset = at;
        const auto children = ReadU32(file, at);
        at += 4;
        if (children > (prefab.footer - at) / 29) return false;
        node.children.resize(children);
        for (auto& child : node.children) if (!entity(child, depth + 1)) return false;
        node.end = at;
        return true;
    };
    if (!need(4)) return fail("no entity tree");
    prefab.rootCountOffset = at;
    const auto roots = ReadU32(file, at);
    at += 4;
    if (roots > (prefab.footer - at) / 29) return fail("bad root count");
    prefab.roots.resize(roots);
    for (auto& root : prefab.roots) if (!entity(root, 0)) return fail("bad entity at " + std::to_string(at));
    if (at != prefab.footer) return fail("entity tree ends at " + std::to_string(at) + ", footer at " + std::to_string(prefab.footer));
    return prefab;
}

// `file` without the one entity named `name` whose parent is named `parent` (and its subtree): the
// parent's child count drops by one and the footer moves. nullopt (and `error`) unless exactly one matches.
inline std::optional<std::string> RemovePrefabEntity(std::string_view file, std::string_view parent, std::string_view name, std::string& error) {
    const auto prefab = ParsePrefab(file, error);
    if (!prefab) return std::nullopt;
    std::vector<std::pair<const PrefabEntity*, const PrefabEntity*>> matches;
    const std::function<void(const PrefabEntity&)> visit = [&](const PrefabEntity& node) {
        for (const auto& child : node.children) {
            if (node.name == parent && child.name == name) matches.emplace_back(&node, &child);
            visit(child);
        }
    };
    for (const auto& root : prefab->roots) visit(root);
    if (matches.size() != 1) {
        error = std::to_string(matches.size()) + " entities " + std::string(parent) + "/" + std::string(name);
        return std::nullopt;
    }
    const auto& [owner, removed] = matches.front();
    std::string result(file.substr(0, removed->begin));
    result += file.substr(removed->end);
    WriteU32(result, owner->childCountOffset, ReadU32(file, owner->childCountOffset) - 1u);
    const auto footer = prefab->footer - (removed->end - removed->begin);
    WriteU32(result, footer + 8, static_cast<std::uint32_t>(footer));
    return result;
}

inline constexpr const char* StartMenuGuest = "/app0/ui/scripts/_cmn/uistartmenu.cgpr";

}

// Writes the overlays the settings ask for and aliases them; called after InstallPortSettingsOverlay.
void InstallUiOverlay(const std::function<std::filesystem::path(const char*)>& resolve);

#endif
