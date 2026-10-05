#ifndef CORE_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVFORMATCONVERSION_HPP
#define CORE_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVFORMATCONVERSION_HPP

#include "SpirvBackend/SpirvMemory/SpirvEmitterState.hpp"
#include <cstdint>

namespace ShaderRecompiler {

bool IsSignedFormatComponent(SpirvFormatComponentType type);
std::uint32_t EmitUFloatToF32Bits(SpirvEmitterState& state, std::uint32_t raw, std::uint32_t bits);
std::uint32_t NormalizeFormatComponent(SpirvEmitterState& state, const SpirvBufferFormatInfo& info, std::uint32_t component, std::uint32_t raw);
// The inverse of NormalizeFormatComponent for typed buffer stores: the lane's 32-bit value (float
// bits for float, normalized and scaled formats) encoded in the component's format, in the low bits.
std::uint32_t EncodeFormatComponent(SpirvEmitterState& state, const SpirvBufferFormatInfo& info, std::uint32_t component, std::uint32_t data);
std::uint32_t EmitTBufferBitcastU32ToI32(SpirvEmitterState& state, std::uint32_t value);

}

#endif
