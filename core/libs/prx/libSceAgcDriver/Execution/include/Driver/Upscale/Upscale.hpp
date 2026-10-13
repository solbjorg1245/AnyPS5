#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_UPSCALE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_UPSCALE_HPP

#include "prx/libSceAgcDriver/Execution/include/MfsrPacket.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <variant>

// APS5_MFSR_EXEC=1 (docs/design/dlss.md S1, default off): the PS5 Pro upscaler's hand-over packet
// (MfsrPacket, S0) runs a bilinear stand-in instead of the NOP skip, MainColor's render rectangle
// sampled over the whole output (kRTT index 7, which the post chain reads). The plan is pure (tests);
// Execution/src/Driver/Upscale/Upscale.cpp runs it, Graphics/src/Upscale.cpp records it.
namespace AgcDriver::Upscale {

// Why a pass records nothing (one stderr line per reason): the packet's (PlanBilinear), then the device's.
enum class Skip : std::uint8_t { NoOutput, NoColor, RenderSize, RenderOutsideColor, NoPushDescriptors, ColorFormat, OutputFormat, ColorUnresolved, Failed, Count };

inline const char* SkipName(Skip skip) {
    static constexpr std::array<const char*, static_cast<std::size_t>(Skip::Count)> names{"no output T#", "no MainColor T#", "render size zero or above 16384", "render rectangle larger than MainColor", "no push descriptors", "MainColor format cannot be filtered", "output format has no float storage view", "MainColor T# undecodable or not 2D", "record failed"};
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

// APS5_MFSR_EXEC, read once: on for any value but empty or "0" (ProLitePatch::Enabled's parse).
inline bool Enabled(const char* value) { return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0; }
inline bool Enabled() {
    static const bool enabled = Enabled(std::getenv("APS5_MFSR_EXEC"));
    return enabled;
}

}

#endif
