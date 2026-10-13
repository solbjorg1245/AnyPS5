#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_UPSCALE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_UPSCALE_HPP

#include "prx/libSceAgcDriver/Execution/include/MfsrPacket.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <variant>

// APS5_MFSR_EXEC=1 (docs/design/dlss.md S1, default off): the PS5 Pro upscaler's hand-over packet
// (MfsrPacket, S0) runs a bilinear stand-in instead of the NOP skip, MainColor's render rectangle
// sampled over the whole output (kRTT index 7, which the post chain reads). The plan is pure (tests);
// Execution/src/Driver/Upscale/Upscale.cpp runs it, Graphics/src/Upscale.cpp records it.
namespace AgcDriver::Upscale {

// Why a pass records nothing (one stderr line per reason): the packet's (PlanBilinear), then the device's;
// from NoMotion on, why the temporal pass did not (PlanTemporal, then the device's: bilinear instead).
enum class Skip : std::uint8_t { NoOutput, NoColor, RenderSize, RenderOutsideColor, NoPushDescriptors, ColorFormat, OutputFormat, ColorUnresolved, Failed, NoMotion, NoDepth, Parameters, InputUnresolved, History, Count };

inline const char* SkipName(Skip skip) {
    static constexpr std::array<const char*, static_cast<std::size_t>(Skip::Count)> names{"no output T#", "no MainColor T#", "render size zero or above 16384", "render rectangle larger than MainColor", "no push descriptors", "MainColor format cannot be filtered", "output format has no float storage view", "MainColor T# undecodable or not 2D", "record failed", "no Motion T#", "no MainDepth T#", "jitter or MV scale not finite", "Motion or MainDepth T# undecodable or not 2D", "history images could not be made"};
    return names[static_cast<std::size_t>(skip)];
}

inline constexpr std::uint32_t MaxExtent = 16384;
inline constexpr std::uint32_t GroupSize = 8;
// The pass's [gputime] program key (guest programs are keyed by address, 0x3 is the batch, 0x10.. the classes).
inline constexpr std::uint64_t TimingKey = 0x7;

struct Plan {
    MfsrPacket::Descriptor outputWords{}, colorWords{};
    MfsrPacket::ImageSize output{}, color{};
    std::array<std::uint32_t, 2> render{};
    // 8x8 groups over the output.
    std::array<std::uint32_t, 2> groups{};
    // An output texel's normalized MainColor coordinates (UpscaleBilinear.comp): (texel + 0.5) * scale,
    // clamped to [low, high], half a texel inside the render rectangle.
    std::array<float, 2> scale{}, low{}, high{};
};

inline std::variant<Plan, Skip> PlanBilinear(const MfsrPacket::Dispatch& dispatch) {
    if ((dispatch.present & 1u) == 0) return Skip::NoOutput;
    if ((dispatch.present & 2u) == 0) return Skip::NoColor;
    Plan plan;
    plan.outputWords = dispatch.images[0];
    plan.colorWords = dispatch.images[1];
    plan.output = MfsrPacket::DecodeImageSize(plan.outputWords);
    plan.color = MfsrPacket::DecodeImageSize(plan.colorWords);
    plan.render = dispatch.Field<std::array<std::uint32_t, 2>>(MfsrPacket::RenderSizeOffset);
    if (plan.render[0] == 0 || plan.render[1] == 0 || plan.render[0] > MaxExtent || plan.render[1] > MaxExtent) return Skip::RenderSize;
    if (plan.render[0] > plan.color.width || plan.render[1] > plan.color.height) return Skip::RenderOutsideColor;
    const std::array<std::uint32_t, 2> output{plan.output.width, plan.output.height}, color{plan.color.width, plan.color.height};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        plan.groups[axis] = (output[axis] + GroupSize - 1) / GroupSize;
        const auto size = static_cast<float>(color[axis]);
        plan.scale[axis] = static_cast<float>(plan.render[axis]) / static_cast<float>(output[axis]) / size;
        plan.low[axis] = 0.5f / size;
        plan.high[axis] = (static_cast<float>(plan.render[axis]) - 0.5f) / size;
    }
    return plan;
}

// APS5_MFSR_EXEC: on for any value but empty or "0" (ProLitePatch::Enabled's parse).
inline bool Enabled(const char* value) { return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0; }
// APS5_MFSR_EXEC, or Upscaler = port under APS5_UPSCALER_SETTINGS (libc PortSettings::PlanUpscalerChain),
// read once (Upscale.cpp).
bool Enabled();

// APS5_UPSCALER_PORT=1 (docs/design/dlss.md S2, default off, with APS5_MFSR_EXEC): the port's temporal
// upscaler (Graphics::TemporalUpscaler, UpscaleTemporal.comp) instead of the bilinear stand-in, which
// still runs whenever the temporal pass is skipped. Its [gputime] key; the current frame's share.
inline constexpr std::uint64_t PortTimingKey = 0x8;
inline constexpr float PortCurrentWeight = 0.1f;

// APS5_UPSCALER_PORT_SIGNS, read once (the conventions are unverified until S0's run): x and y flip the
// MV scale's x and y, j and k the jitter's, z takes the smallest depth as the nearest. Default: the MV
// scale as given, the jitter as stored (y already negated), reversed Z.
struct Signs {
    std::array<float, 2> motion{1.0f, 1.0f}, jitter{1.0f, 1.0f};
    bool smallestNearest = false;
    // The letters taken, in "xyjkz" order, for the log ("" for none).
    std::array<char, 6> letters{};
};

inline Signs ParseSigns(const char* value) {
    Signs signs;
    constexpr std::string_view letters = "xyjkz";
    std::size_t taken = 0;
    for (std::size_t index = 0; index < letters.size(); ++index) {
        if (value == nullptr || std::strchr(value, letters[index]) == nullptr) continue;
        if (index < 2) signs.motion[index] = -1.0f;
        else if (index < 4) signs.jitter[index - 2] = -1.0f;
        else signs.smallestNearest = true;
        signs.letters[taken++] = letters[index];
    }
    return signs;
}

// What the history images hold: the sizes of the last temporal pass (zeros before the first, and after
// a frame it skipped).
struct History {
    std::array<std::uint32_t, 2> output{}, render{};
    bool operator==(const History&) const = default;
};

struct TemporalPlan {
    // PlanBilinear's: the output and MainColor, the render rectangle, the groups.
    Plan base;
    MfsrPacket::Descriptor motionWords{}, depthWords{};
    // Render pixels, the signs applied: where a sample lies (texel + 0.5 - jitter) and Motion's scale.
    std::array<float, 2> jitter{}, motionScale{};
    // The packet's reset bit, a first frame, or sizes other than the history's: the history is not read.
    bool reset = false, smallestNearest = false;
    History next;
};

inline std::variant<TemporalPlan, Skip> PlanTemporal(const MfsrPacket::Dispatch& dispatch, const History& previous, const Signs& signs) {
    const auto planned = PlanBilinear(dispatch);
    if (const auto* skip = std::get_if<Skip>(&planned)) return *skip;
    if ((dispatch.present & 4u) == 0) return Skip::NoMotion;
    if ((dispatch.present & 8u) == 0) return Skip::NoDepth;
    TemporalPlan plan;
    plan.base = std::get<Plan>(planned);
    plan.motionWords = dispatch.images[2];
    plan.depthWords = dispatch.images[3];
    const auto jitter = dispatch.Field<std::array<float, 2>>(MfsrPacket::JitterOffset);
    const auto scale = dispatch.Field<std::array<float, 2>>(MfsrPacket::MotionScaleOffset);
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (!std::isfinite(jitter[axis]) || !std::isfinite(scale[axis])) return Skip::Parameters;
        plan.jitter[axis] = jitter[axis] * signs.jitter[axis];
        plan.motionScale[axis] = scale[axis] * signs.motion[axis];
    }
    plan.smallestNearest = signs.smallestNearest;
    plan.next = {{plan.base.output.width, plan.base.output.height}, plan.base.render};
    plan.reset = dispatch.Field<std::uint8_t>(MfsrPacket::ResetOffset) != 0 || !(previous == plan.next);
    return plan;
}

// APS5_UPSCALER_PORT, or the settings' upscaler as for Enabled().
bool PortEnabled();
inline const Signs& PortSigns() {
    static const Signs signs = ParseSigns(std::getenv("APS5_UPSCALER_PORT_SIGNS"));
    return signs;
}

}

#endif
