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

// The clamp output modifier on the float forms the game uses (VOP3 f32, with omod, v_cvt_pkrtz_f16_f32,
// VOP3P v_pk_mul_f16): 1.5 -> 1, -0.5 -> 0, 0.25 -> 0.25, and the clamp of v_add_nc_i32 stays a signed
// integer saturate of the raw bits. A NaN clamp input is checked only under APS5_CLAMP_NAN=zero (0) or
// keep (NaN): by default FClamp leaves it to the driver.

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ClampNan;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 8;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

// v_lshlrev_b32 v1, 2, v0; v_lshlrev_b32 v3, 3, v0; buffer_load_dword v4..v7 (a, b, c, d), idxen v1,
// s[0:3]; s_waitcnt; v_mul_f32 v10, v4, v5 clamp; v_max_f32 v11, v4, v4 clamp; v_fma_f32 v12, v4, v5,
// v6 clamp; v_add_f32 v13, v4, v5 clamp mul:2; v_cvt_pkrtz_f16_f32 v14, v4, v5 clamp; v_pk_mul_f16 v15,
// v7, v7 clamp; v_add_nc_i32 v16, v4, v5 clamp; v_mul_f32 v17, v4, v5; buffer_store_dword v10..v17,
// idxen v3, s[4:7]; s_endpgm.
alignas(256) constexpr std::array<std::uint32_t, 44> ClampCode{
    0x34020082, 0x34060083, 0xe0302000, 0x80000401, 0xe0302004, 0x80000501, 0xe0302008, 0x80000601,
    0xe030200c, 0x80000701, 0xbf8c3f70, 0xd508800a, 0x00020b04, 0xd510800b, 0x00020904, 0xd54b800c,
    0x041a0b04, 0xd503800d, 0x08020b04, 0xd52f800e, 0x00020b04, 0xcc10800f, 0x18020f07, 0xd77f8010,
    0x00020b04, 0xd5080011, 0x00020b04, 0xe0702000, 0x80010a03, 0xe0702004, 0x80010b03, 0xe0702008,
    0x80010c03, 0xe070200c, 0x80010d03, 0xe0702010, 0x80010e03, 0xe0702014, 0x80010f03, 0xe0702018,
    0x80011003, 0xe070201c, 0x80011103, 0xbf810000,
};

// nan: the results whose clamp input is NaN, bit n for f32 column n (0-3), bit 4 + 2 * (column - 4) +
// half for the f16 halves of columns 4 and 5.
struct Vector {
    std::uint32_t a, b, c, d;
    std::array<std::uint32_t, Results> expected;
    std::uint32_t nan;
};

constexpr Vector Vectors[] = {
    {0x3fc00000u, 0x3f800000u, 0x00000000u, 0xb8003d00u, {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3c003c00u, 0x34003c00u, 0x7f400000u, 0x3fc00000u}, 0x00u},
    {0xbf000000u, 0x3f800000u, 0x00000000u, 0x34003800u, {0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x3c000000u, 0x2c003400u, 0xfe800000u, 0xbf000000u}, 0x00u},
    {0x3e800000u, 0x3f800000u, 0x00000000u, 0x3a00be00u, {0x3e800000u, 0x3e800000u, 0x3e800000u, 0x3f800000u, 0x3c003400u, 0x38803c00u, 0x7e000000u, 0x3e800000u}, 0x00u},
    {0x7f800000u, 0x00000000u, 0x00000000u, 0x38007e00u, {0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00003c00u, 0x34000000u, 0x7f800000u, 0x7fc00000u}, 0x45u},
    {0x7fc00000u, 0x3f800000u, 0x00000000u, 0xfc007c00u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3c000000u, 0x3c003c00u, 0x7fffffffu, 0x7fc00000u}, 0x1fu},
    {0x40000000u, 0x3f400000u, 0xbe800000u, 0x3c003b00u, {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3a003c00u, 0x3c003a20u, 0x7f400000u, 0x3fc00000u}, 0x00u},
    {0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3c800000u, {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u, 0x38003800u, 0x3c000000u, 0x7e000000u, 0x3e800000u}, 0x00u},
    {0xc0000000u, 0x40400000u, 0x40e00000u, 0x3600b400u, {0x00000000u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x3c000000u, 0x30802c00u, 0x00400000u, 0xc0c00000u}, 0x00u},
    {0x7149f2cau, 0x7149f2cau, 0x00000000u, 0x38004000u, {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3c003c00u, 0x34003c00u, 0x7fffffffu, 0x7f800000u}, 0x00u},
    {0x3f800000u, 0x3f800000u, 0xbf000000u, 0x2c003000u, {0x3f800000u, 0x3f800000u, 0x3f000000u, 0x3f800000u, 0x3c003c00u, 0x1c002400u, 0x7f000000u, 0x3f800000u}, 0x00u},
    {0x3f400000u, 0x3f000000u, 0x3e000000u, 0x80003800u, {0x3ec00000u, 0x3f400000u, 0x3f000000u, 0x3f800000u, 0x38003a00u, 0x00003400u, 0x7e400000u, 0x3ec00000u}, 0x00u},
    {0xff800000u, 0xbf800000u, 0xc0000000u, 0x34003a00u, {0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x2c003880u, 0xbf000000u, 0x7f800000u}, 0x00u},
};

constexpr std::array<const char*, Results> Names{
    "v_mul_f32 clamp", "v_max_f32 clamp", "v_fma_f32 clamp", "v_add_f32 clamp mul:2", "v_cvt_pkrtz_f16_f32 clamp", "v_pk_mul_f16 clamp", "v_add_nc_i32 clamp", "v_mul_f32"};

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

bool IsNan16(std::uint32_t bits) {
    return (bits & 0x7fffu) > 0x7c00u;
}

// A NaN clamp input: anything by default, a NaN with keep, either zero with zero.
bool MatchesNan(std::uint32_t actual, bool half) {
    switch (ShaderRecompiler::ClampNanRule()) {
        case ClampNan::Driver: return true;
        case ClampNan::Keep: return half ? IsNan16(actual) : IsNan32(actual);
        case ClampNan::Zero: break;
    }
    return half ? (actual & 0x7fffu) == 0u : (actual & 0x7fffffffu) == 0u;
}

// Exact, except that any NaN matches a NaN and a clamp may give either zero for 0.
bool MatchesHalf(std::uint32_t actual, std::uint32_t expected, bool nan) {
    if (nan) return MatchesNan(actual, true);
    if (actual == expected) return true;
    if (IsNan16(actual) && IsNan16(expected)) return true;
    return expected == 0u && actual == 0x8000u;
}

bool Matches(std::uint32_t column, std::uint32_t actual, std::uint32_t expected, std::uint32_t nan) {
    if (column == 4u || column == 5u) {
        const std::uint32_t bit = 4u + 2u * (column - 4u);
        return MatchesHalf(actual & 0xffffu, expected & 0xffffu, ((nan >> bit) & 1u) != 0u) &&
            MatchesHalf(actual >> 16u, expected >> 16u, ((nan >> (bit + 1u)) & 1u) != 0u);
    }
    if (column < 4u && ((nan >> column) & 1u) != 0u) return MatchesNan(actual, false);
    if (actual == expected) return true;
    if (column == 6u) return false;
    if (IsNan32(actual) && IsNan32(expected)) return true;
    return column < 4u && expected == 0u && actual == 0x80000000u;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t count) {
    Input.fill(0u);
    for (std::uint32_t lane = 0; lane < count; ++lane) {
        Input[lane * Inputs] = Vectors[lane].a;
        Input[lane * Inputs + 1] = Vectors[lane].b;
        Input[lane * Inputs + 2] = Vectors[lane].c;
        Input[lane * Inputs + 3] = Vectors[lane].d;
    }
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(ClampCode);
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
        for (std::uint32_t column = 0; column < Results; ++column) {
            const auto expected = vector.expected[column];
            const auto actual = Output[lane * Results + column];
            Require(Matches(column, actual, expected, vector.nan), std::string("clamp modifier: vector ") + std::to_string(lane) + " (" + Hex(vector.a) + ", " + Hex(vector.b) + ", " + Hex(vector.c) + ", " + Hex(vector.d) + ") " + Names[column] + " is " + Hex(actual) + ", expected " + Hex(expected));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        constexpr std::uint32_t total = sizeof(Vectors) / sizeof(Vectors[0]);
        static_assert(total <= Threads);
        Run(*device, total);
        Check(total);
        std::puts("clamp modifier tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
