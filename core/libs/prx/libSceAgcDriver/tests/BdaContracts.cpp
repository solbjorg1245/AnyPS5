#include "BdaShader.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include <array>
#include <string>

namespace {

// Instructions with the opcode (OpVariable: of the storage class) in a SPIR-V module.
std::size_t countInstructions(const std::vector<std::uint32_t>& code, spv::Op opcode, std::uint32_t storage = 0xffffffffu) {
    std::size_t found = 0;
    for (std::size_t offset = 5; offset < code.size();) {
        const auto words = code[offset] >> spv::WordCountShift;
        if (words == 0) break;
        if ((code[offset] & spv::OpCodeMask) == static_cast<std::uint32_t>(opcode) && (storage == 0xffffffffu || (words > 3 && code[offset + 3] == storage))) ++found;
        offset += words;
    }
    return found;
}

template<typename TAction>
void reject(TAction action, const char* expected) {
    try { action(); }
    catch (const std::runtime_error& error) {
        AgcDriver::Graphics::Require(std::string(error.what()).find(expected) != std::string::npos, std::string("unexpected contract error: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("missing contract rejection: ") + expected);
}

}

void RunBdaContractTests() {
    using namespace ShaderRecompiler;
    const std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    const std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    SpirvTargetOptions target{0x00401000u, 0x00010300u, 32, BdaAbi::Version, capabilities, extensions};
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    ValidateBdaTarget(program, target);
    target.bdaAbiVersion = 0;
    reject([&] { ValidateBdaTarget(program, target); }, "ABI version");
    target.bdaAbiVersion = BdaAbi::Version;
    target.supportedCapabilities = {};
    reject([&] { ValidateBdaTarget(program, target); }, "capability");
    target.supportedCapabilities = capabilities;
    target.supportedExtensions = {};
    reject([&] { ValidateBdaTarget(program, target); }, "extension");
    target.supportedExtensions = extensions;
    auto& block = program.CreateBlock();
    block.AppendInstruction(&program.CreateValue(IrOpcode::Barrier, IrType::Void));
    program.BlockOrder().push_back(&block);
    ValidateBdaTarget(program, target);
    AgcDriver::Graphics::Require(!BdaInvocationsMayStop(program), "a BDA program with barriers must keep its invocations running");
    program.Resources().stage = IrShaderStage::TessellationControl;
    reject([&] { ValidateBdaTarget(program, target); }, "barrier-safe");
    program.Resources().stage = IrShaderStage::Compute;
    RecompileRequest request{};
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.fragmentShaderBarycentricEnabled = false;
    const RequestSerializer serializer;
    const auto encoded = serializer.Serialize(request);
    const auto decoded = serializer.Deserialize(encoded);
    AgcDriver::Graphics::Require(decoded.request.target.bdaAbiVersion == BdaAbi::Version && decoded.request.target.supportedCapabilities.size() == capabilities.size() && decoded.request.target.supportedExtensions[1] == extensions[1], "BDA request serialization changed target contract");
    auto invalid = encoded;
    invalid[0] = invalid[0] == 'A' ? 'B' : 'A';
    reject([&] { static_cast<void>(serializer.Deserialize(invalid)); }, "request signature");
    reject([&] { static_cast<void>(serializer.Deserialize("NVNQQWMAAAA=")); }, "serialization version");
    // APS5_SPIRV_BDA_UNIFORM: the Private lookup state of each stage (header check and count from 1, the
    // last range from 2) and the subgroup search of stage 3, which needs subgroup ballots.
    const std::array<BdaTestAccess, 2> accesses{{{0x7fff12300000ULL, 4}, {0x7fff12300040ULL, 0}}};
    for (std::uint32_t stage = 0; stage <= 3; ++stage) {
        SetBdaUniformStageForTests(static_cast<int>(stage));
        for (const bool uniform : {false, true}) {
            const auto code = MakeBdaLookupTestShader(accesses, 32, uniform);
            AgcDriver::Graphics::Require(countInstructions(code, spv::OpVariable, spv::StorageClassPrivate) == (stage == 0 ? 0u : stage == 1 ? 2u : 6u), "BDA lookup state does not match APS5_SPIRV_BDA_UNIFORM stage " + std::to_string(stage));
            AgcDriver::Graphics::Require((countInstructions(code, spv::OpGroupNonUniformBroadcastFirst) != 0) == (stage == 3), "BDA subgroup search does not match APS5_SPIRV_BDA_UNIFORM stage " + std::to_string(stage));
        }
    }
    AgcDriver::Graphics::Require(countInstructions(MakeBdaLookupTestShader(accesses, 32, true, false), spv::OpGroupNonUniformBroadcastFirst) == 0, "BDA subgroup search emitted without subgroup ballots");
    SetBdaUniformStageForTests(-1);
}
