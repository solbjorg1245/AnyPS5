#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

// The DX9 ("legacy") float rules (Recompiler.hpp): v_mad_legacy_f32 gives c for 0 * Inf + c and
// 0 * NaN + c, and v_mul_legacy_f32 stays as it was. The clamp of a NaN product is checked only
// under APS5_CLAMP_NAN=zero (0) or keep (NaN): by default FClamp leaves it to the driver.

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 4;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

// v_lshlrev_b32 v1, 2, v0; buffer_load_dword v4..v6 (a, b, c), idxen v1, s[0:3]; s_waitcnt;
// v_mad_legacy_f32 v10, v4, v5, v6; v_mul_f32 v11, v4, v5 clamp; v_mul_legacy_f32 v12, v4, v5;
// buffer_store_dword v10..v12, idxen v1, s[4:7]; s_endpgm.
alignas(256) constexpr std::array<std::uint32_t, 20> LegacyCode{
    0x34020082, 0xe0302000, 0x80000401, 0xe0302004, 0x80000501, 0xe0302008, 0x80000601, 0xbf8c3f70,
    0xd540000a, 0x041a0b04, 0xd508800b, 0x00020b04, 0x0e180b04, 0xe0702000, 0x80010a01, 0xe0702004,
    0x80010b01, 0xe0702008, 0x80010c01, 0xbf810000,
};

struct Vector {
    std::uint32_t a, b, c;
    std::array<std::uint32_t, 3> expected;
};

constexpr Vector Vectors[] = {
    {0x00000000u, 0x7f800000u, 0x3f800000u, {0x3f800000u, 0x00000000u, 0x00000000u}},
    {0x7f800000u, 0x00000000u, 0x3f800000u, {0x3f800000u, 0x00000000u, 0x00000000u}},
    {0x80000000u, 0x7fc00000u, 0x40000000u, {0x40000000u, 0x00000000u, 0x00000000u}},
    {0x00000000u, 0xff800000u, 0xc0a00000u, {0xc0a00000u, 0x00000000u, 0x00000000u}},
    {0x00000000u, 0x00000000u, 0x80000000u, {0x00000000u, 0x00000000u, 0x00000000u}},
    {0x00000000u, 0x7fc00000u, 0x7fc00000u, {0x7fc00000u, 0x00000000u, 0x00000000u}},
    {0x40000000u, 0x40400000u, 0x3f800000u, {0x40e00000u, 0x3f800000u, 0x40c00000u}},
    {0x7f800000u, 0x40000000u, 0x3f800000u, {0x7f800000u, 0x3f800000u, 0x7f800000u}},
    {0x7fc00000u, 0x40000000u, 0x3f800000u, {0x7fc00000u, 0x00000000u, 0x7fc00000u}},
    {0x3f000000u, 0x3f000000u, 0x00000000u, {0x3e800000u, 0x3e800000u, 0x3e800000u}},
    {0xc0000000u, 0x40400000u, 0x00000000u, {0xc0c00000u, 0x00000000u, 0xc0c00000u}},
    {0x7f800000u, 0x7f800000u, 0xff800000u, {0x7fc00000u, 0x3f800000u, 0x7f800000u}},
};

constexpr std::array<const char*, 3> Names{"v_mad_legacy_f32", "v_mul_f32 clamp", "v_mul_legacy_f32"};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x01016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

bool IsNan32(std::uint32_t bits) {
    return (bits & 0x7fffffffu) > 0x7f800000u;
}

// Any NaN matches a NaN; the clamp of a negative or -0 product may give either zero.
bool Matches(std::uint32_t column, std::uint32_t actual, std::uint32_t expected, bool nanProduct) {
    if (column == 1u && nanProduct) {
        switch (ShaderRecompiler::ClampNanRule()) {
            case ShaderRecompiler::ClampNan::Driver: return true;
            case ShaderRecompiler::ClampNan::Keep: return IsNan32(actual);
            case ShaderRecompiler::ClampNan::Zero: break;
        }
    }
    if (actual == expected) return true;
    if (IsNan32(actual) && IsNan32(expected)) return true;
    return column == 1u && expected == 0u && actual == 0x80000000u;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t count) {
    Input.fill(0u);
    for (std::uint32_t lane = 0; lane < count; ++lane) {
        Input[lane * Inputs] = Vectors[lane].a;
        Input[lane * Inputs + 1] = Vectors[lane].b;
        Input[lane * Inputs + 2] = Vectors[lane].c;
    }
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(LegacyCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint32_t count) {
    for (std::uint32_t lane = 0; lane < count; ++lane) {
        const auto& vector = Vectors[lane];
        const bool nanProduct = IsNan32(vector.a) || IsNan32(vector.b) || ((vector.a & 0x7fffffffu) == 0u && (vector.b & 0x7fffffffu) == 0x7f800000u) ||
            ((vector.b & 0x7fffffffu) == 0u && (vector.a & 0x7fffffffu) == 0x7f800000u);
        for (std::uint32_t column = 0; column < Names.size(); ++column) {
            const auto expected = vector.expected[column];
            const auto actual = Output[lane * Results + column];
            Require(Matches(column, actual, expected, nanProduct), std::string("legacy float: vector ") + std::to_string(lane) + " (" + Hex(vector.a) + ", " + Hex(vector.b) + ", " + Hex(vector.c) + ") " + Names[column] + " is " + Hex(actual) + ", expected " + Hex(expected));
        }
    }
}

}

int main() {
    try {
        if (!ShaderRecompiler::LegacyMadRule()) {
            std::puts("legacy float tests skipped (APS5_NO_LEGACY_MAD_RULE set)");
            return 0;
        }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        constexpr std::uint32_t total = sizeof(Vectors) / sizeof(Vectors[0]);
        static_assert(total <= Threads);
        Run(*device, total);
        Check(total);
        std::puts("legacy float tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
