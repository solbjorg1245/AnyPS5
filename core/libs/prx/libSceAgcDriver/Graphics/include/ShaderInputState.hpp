#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

ShaderRecompiler::ShaderPixelStageInfo DecodePixelStageInfo(const Registers& context, const std::array<std::uint8_t, 8>& exportMappings);
ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader);
// A guest range DecodeVertexStageInfo read (an attribute word, a vertex V#) with the bytes as read:
// the draw cache validates them by value with the stage's capture (design_cpu_final rule RD).
struct DecodeRead {
    std::uint64_t address;
    std::vector<std::byte> bytes;
};
ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::vector<DecodeRead>* reads = nullptr);
// DecodeVertexStageInfo in two parts for the fast walk (docs/design/draw-fastpath.md section 2.3,
// F2): the header part, memoizable per program (the user words holding the two table pointers and
// the input semantics), and the per-draw part, which reads the attribute words and V#s through
// `read` (false declines the read, and the resolve with it) and builds the same info.
struct VertexFetchPlan {
    struct Semantic {
        std::uint32_t semantic;
        std::int32_t hardwareMapping;
        std::int32_t sizeInElements;
    };
    // False: the program has no vertex attribute table (an empty info).
    bool fetch = false;
    std::uint32_t attribReg = 0;
    std::uint32_t bufferReg = 0;
    std::vector<Semantic> semantics;
};
using VertexFetchRead = bool (*)(void* context, std::uint64_t address, std::uint32_t* value);
VertexFetchPlan DecodeVertexFetchPlan(std::span<const std::byte> header, std::uint64_t headerAddress);
bool ResolveVertexFetch(const VertexFetchPlan& plan, std::span<const std::uint32_t> userData, VertexFetchRead read, void* context, ShaderRecompiler::ShaderVertexStageInfo& info);

}

#endif
