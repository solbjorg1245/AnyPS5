#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERPRECOMPILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERPRECOMPILE_HPP

#include "Recompiler.hpp"

namespace AgcDriver::DriverDetail {

// APS5_PRECOMPILE=<key list> (ShaderKeyList.hpp): fills the shader disk cache from the key list and
// the game's own files before the first frame, once per process, on the thread that creates the
// first Vulkan device (under the GPU mutex, so no game GPU work runs meanwhile). Unset, the list
// shipped next to the program (shader-keys.txt) is used while the cache is cold for it; "0": off.
void RunShaderPrecompile(const ShaderRecompiler::SpirvTarget& target);

}

#endif
