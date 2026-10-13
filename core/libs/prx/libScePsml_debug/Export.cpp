#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/ProLitePatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/MfsrPacket.hpp"

// PS5 Pro machine-learning upscaler (MFSR). Only reachable when IsPro answers true, which this runtime
// never does unless Pro-lite (APS5_MFSR_PROBE=1, ProLitePatch.hpp) patched the renderer's one call; then
// the ten calls the title makes (FUN_140a622f0 InitSystem, FUN_140a628c0 InitContext, FUN_140a63730 mip
// bias, FUN_141e83ac0 dispatchMfsr, FUN_140a62e30 release) are emulated, every other stub still throws.
// Arguments: SysV registers at those call sites. dispatchMfsr gets one NOP packet (MfsrPacket.hpp) that
// the driver skips, so the kRTT index 7 output stays unwritten (docs/design/dlss.md S0: diagnostic only).
namespace PsmlMfsr {

// {size, alignment, count}: 64 KiB the title allocates and never reads.
struct Requirement { std::uint64_t size, alignment, count; };
struct Context { std::uint32_t width, height; };
// What dispatchMfsr hands the packet call: {DCB cursor, n}.
struct PacketSpan { std::uint32_t* cursor; std::uint32_t words; };

unsigned char sharedResources;

// Without Pro-lite the calls throw as before.
void probe(const char* function) {
    if (!MfsrProbeActive_nid_no_patch()) NotImplemented_nid_no_patch(function);
}

// The title does not zero its output first (InitSystem 0x140a6243c): every field is written.
void require(Requirement* requirement) { *requirement = {0x10000, 0x10000, 1}; }

template<typename T> T read(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

// The first dispatch's images, then the 10 s row (dispatchMfsr runs on the render thread only).
struct Digest {
    std::chrono::steady_clock::time_point start;
    std::uint64_t dispatches = 0, resets = 0, total = 0;
    std::array<float, 2> low{}, high{};

    void add(const AgcDriver::MfsrPacket::Dispatch& dispatch) {
        using namespace AgcDriver::MfsrPacket;
        const auto now = std::chrono::steady_clock::now();
        const auto jitter = dispatch.Field<std::array<float, 2>>(JitterOffset);
        for (int axis = 0; axis < 2; ++axis) {
            low[axis] = dispatches == 0 ? jitter[axis] : std::min(low[axis], jitter[axis]);
            high[axis] = dispatches == 0 ? jitter[axis] : std::max(high[axis], jitter[axis]);
        }
        if (dispatches++ == 0) start = now;
        if (dispatch.Field<std::uint8_t>(ResetOffset) != 0) ++resets;
        std::array<ImageSize, 4> sizes{};
        for (std::size_t index = 0; index < sizes.size(); ++index) sizes[index] = DecodeImageSize(dispatch.images[index]);
        const auto& output = sizes[0];
        if (total++ == 0) {
            const auto scale = dispatch.Field<std::array<float, 2>>(MotionScaleOffset);
            std::fprintf(stderr, "[mfsr] first dispatch: %zu dwords, pointers 0x%x, color %ux%u format %u, motion %ux%u format %u, depth %ux%u format %u, MV scale (%.1f, %.1f), +0x118 %.3f\n",
                Words, dispatch.present, sizes[1].width, sizes[1].height, sizes[1].format, sizes[2].width, sizes[2].height, sizes[2].format, sizes[3].width, sizes[3].height,
                sizes[3].format, scale[0], scale[1], dispatch.Field<float>(0x118));
        }
        if (now - start < std::chrono::seconds(10)) return;
        const auto render = dispatch.Field<std::array<std::uint32_t, 2>>(RenderSizeOffset);
        std::fprintf(stderr, "[mfsr] %llu dispatches, render %ux%u, output %ux%u format %u, jitter (%.3f, %.3f) .. (%.3f, %.3f), %llu resets\n",
            static_cast<unsigned long long>(dispatches), render[0], render[1], output.width, output.height, output.format, low[0], low[1], high[0], high[1],
            static_cast<unsigned long long>(resets));
        dispatches = 0;
        resets = 0;
    }
} digest;

}

using namespace PsmlMfsr;
using namespace AgcDriver;

extern "C" {

// scePsmlMfsrGetSharedResourcesInitRequirement(rdi requirement, rsi parameters), InitSystem 0x140a6243c.
APS5_EXPORT("+2KpvixvL6E", scePsmlUnknown__P2KpvixvL6E);
int APS5_VABI scePsmlUnknown__P2KpvixvL6E(Requirement* requirement, const void*) {
 probe(__func__);
 require(requirement);
 return 0;
}

// scePsmlMfsrInit(), InitSystem 0x140a623aa.
APS5_EXPORT("3WVD91e12ZQ", scePsmlUnknown_3WVD91e12ZQ);
int APS5_VABI scePsmlUnknown_3WVD91e12ZQ() {
 probe(__func__);
 return 0;
}

// scePsmlMfsrGetDispatchMfsrPacketSizeInDwords(rdi context, rsi uint32 n), dispatchMfsr 0x141e83aeb.
APS5_EXPORT("AHalTX9wFZY", scePsmlUnknown_AHalTX9wFZY);
int APS5_VABI scePsmlUnknown_AHalTX9wFZY(Context*, std::uint32_t* words) {
 probe(__func__);
 *words = static_cast<std::uint32_t>(MfsrPacket::Words);
 return 0;
}

APS5_EXPORT("ArakEpzsZo0", scePsmlUnknown_ArakEpzsZo0);
int APS5_VABI scePsmlUnknown_ArakEpzsZo0() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

APS5_EXPORT("FSGaTQze0UY", scePsmlUnknown_FSGaTQze0UY);
int APS5_VABI scePsmlUnknown_FSGaTQze0UY() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// scePsmlMfsrGetMipmapBias(rdi context, rsi float bias, edx width, ecx height), FUN_140a63730 with the
// pair at [job+0x50] (FUN_1409f4970; the gate's, probably the render size): log2(width / output width).
APS5_EXPORT("GHna9-DvnUk", scePsmlUnknown_GHna9_MDvnUk);
int APS5_VABI scePsmlUnknown_GHna9_MDvnUk(const Context* context, float* bias, std::uint32_t width, std::uint32_t) {
 probe(__func__);
 *bias = context != nullptr && context->width != 0 && width != 0 ? std::log2(static_cast<float>(width) / static_cast<float>(context->width)) : 0.0f;
 return 0;
}

APS5_EXPORT("GJY0MvuTcs8", scePsmlUnknown_GJY0MvuTcs8);
int APS5_VABI scePsmlUnknown_GJY0MvuTcs8() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// scePsmlMfsrReleaseContext(rdi context), FUN_140a62e30.
APS5_EXPORT("JaLBe0P3jSU", scePsmlUnknown_JaLBe0P3jSU);
int APS5_VABI scePsmlUnknown_JaLBe0P3jSU(Context* context) {
 probe(__func__);
 delete context;
 return 0;
}

APS5_EXPORT("LXq+6mIxpCw", scePsmlUnknown_LXq_P6mIxpCw);
int APS5_VABI scePsmlUnknown_LXq_P6mIxpCw() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

APS5_EXPORT("RUNLFro+qok", scePsmlUnknown_RUNLFro_Pqok);
int APS5_VABI scePsmlUnknown_RUNLFro_Pqok() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// scePsmlMfsrCreateSharedResources(rdi handle out, rsi parameters: +0x08 {address, size} blocks, +0x10
// their count), InitSystem 0x140a62811.
APS5_EXPORT("eWoKNeB6V-k", scePsmlUnknown_eWoKNeB6V_Mk);
int APS5_VABI scePsmlUnknown_eWoKNeB6V_Mk(void** handle, const void* parameters) {
 probe(__func__);
 *handle = &sharedResources;
 const auto* blocks = read<const std::uint64_t*>(parameters, 0x08);
 std::fprintf(stderr, "[mfsr] shared resources: %llu block(s), first %llu bytes at 0x%llx\n", static_cast<unsigned long long>(read<std::uint64_t>(parameters, 0x10)),
     static_cast<unsigned long long>(blocks != nullptr ? blocks[1] : 0), static_cast<unsigned long long>(blocks != nullptr ? blocks[0] : 0));
 return 0;
}

APS5_EXPORT("gxv3i+MTEzU", scePsmlUnknown_gxv3i_PMTEzU);
int APS5_VABI scePsmlUnknown_gxv3i_PMTEzU() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// scePsmlMfsrReleaseSharedResources(rdi handle), FUN_140a62e30.
APS5_EXPORT("jEevBXmagOQ", scePsmlUnknown_jEevBXmagOQ);
int APS5_VABI scePsmlUnknown_jEevBXmagOQ(void*) {
 probe(__func__);
 return 0;
}

// scePsmlMfsrCreateContext1100(rdi context out, rsi parameters: +0x08 {address, size} blocks, +0x10 their
// count, +0x18/+0x1a u16 maximum output size (3840x2160 from FUN_140a621a0), +0x38 the shared resources),
// InitContext 0x140a62d73.
int APS5_VABI scePsmlMfsrCreateContext1100(Context** context, const void* parameters) {
 probe(__func__);
 *context = new Context{read<std::uint16_t>(parameters, 0x18), read<std::uint16_t>(parameters, 0x1a)};
 std::fprintf(stderr, "[mfsr] context %ux%u, %llu buffer block(s), shared resources %s\n", (*context)->width, (*context)->height,
     static_cast<unsigned long long>(read<std::uint64_t>(parameters, 0x10)), read<const void*>(parameters, 0x38) == &sharedResources ? "ours" : "unknown");
 std::fflush(stderr);
 return 0;
}

// scePsmlMfsrGetContextBufferRequirement1100(rdi requirement, rsi the context parameters), InitContext
// 0x140a62987.
int APS5_VABI scePsmlMfsrGetContextBufferRequirement1100(Requirement* requirement, const void*) {
 probe(__func__);
 require(requirement);
 return 0;
}

// scePsmlMfsrGetDispatchMfsrPacket1100(rdi context, rsi {DCB cursor, n}, rdx the 0x158-byte block),
// dispatchMfsr 0x141e83b6a; the title advances the cursor by n dwords only when this returns 0.
int APS5_VABI scePsmlMfsrGetDispatchMfsrPacket1100(Context*, const PacketSpan* span, const void* block) {
 probe(__func__);
 if (span == nullptr || span->cursor == nullptr || span->words != MfsrPacket::Words || block == nullptr) return static_cast<int>(0x8a810006u);
 const auto dispatch = MfsrPacket::Capture(block);
 MfsrPacket::Encode(dispatch, std::span<std::uint32_t, MfsrPacket::Words>(span->cursor, MfsrPacket::Words));
 digest.add(dispatch);
 return 0;
}

}
