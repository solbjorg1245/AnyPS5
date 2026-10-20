#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDASHADER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDASHADER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <span>
#include <vector>

// One 32-bit read per invocation of a lookup test shader, at address + local invocation index * laneStride.
struct BdaTestAccess {
    std::uint64_t address = 0;
    std::uint32_t laneStride = 0;
};

std::vector<std::uint32_t> MakeBdaTestShader(std::uint64_t address, std::uint32_t bits, std::int64_t offset = 0, bool uniform = false);
// A compute shader of `lanes` invocations doing the accesses in order (pc 0x1234 + access index) and storing
// each value at output word lane * accesses + access. `uniform` marks them as reads with a wave-uniform base
// (the lookups of APS5_SPIRV_BDA_UNIFORM=3); `ballot` says whether the target supports subgroup ballots.
std::vector<std::uint32_t> MakeBdaLookupTestShader(std::span<const BdaTestAccess> accesses, std::uint32_t lanes, bool uniform, bool ballot = true);
void RunBdaExecutionTests(const AgcDriver::Graphics::Context& context);
void RunBdaContractTests();

#endif
