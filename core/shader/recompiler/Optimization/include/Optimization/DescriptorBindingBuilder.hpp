#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DESCRIPTORBINDINGBUILDER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/BindingAllocator.hpp"
#include "Recompiler.hpp"

namespace ShaderRecompiler {

class DescriptorBindingBuilder {
public:
    void Populate(BindingAllocationResult& allocation, const IrProgram& program, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads) const;
    void Populate(BindingAllocationResult& allocation, const ShaderInfo& info, IrShaderStage stage, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads) const;
    // Populate's bindings and push constants for `layout`, written in place: `bindings` is resized
    // to the layout and each element reset, so the vectors (and every binding's vectors) keep their
    // storage across calls (the fast paths' per-thread results, PopulateVariant). The content is
    // Populate's, by the same code; `shaderData` is scratch. A throw leaves the outputs unspecified.
    void PopulateInto(const IrBindingLayout& layout, const ShaderInfo& info, IrShaderStage stage, std::uint32_t userDataBase, const ResourceSnapshot& snapshot, const std::array<std::uint32_t, 3>& partialThreads, std::vector<DescriptorBinding>& bindings, std::vector<std::byte>& pushConstants, std::vector<std::uint32_t>& shaderData) const;
};

}

#endif
