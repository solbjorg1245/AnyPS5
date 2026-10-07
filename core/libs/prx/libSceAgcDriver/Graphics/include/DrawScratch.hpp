#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSCRATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSCRATCH_HPP

#include "prx/libSceAgcDriver/Graphics/include/ScratchLease.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

// The vectors prepareDrawInputs builds for one draw (Draw.cpp), kept with their capacity on the
// drawing thread between draws (ScratchLease). Built fresh, they were ~15-20 of the heap
// allocations a steady-state draw made in the graphics layer: the vertex fetch list and its copy
// plan, the handles and offsets bound with vkCmdBindVertexBuffers, the in-place input ranges, the
// vertex input layout of a draw without a recipe and the validation memo key. The DrawInputs of a
// draw point into these (spans), so the lease is the draw's (Draw, DrawWithRecipe), taken before
// its inputs are prepared and held past their last use.
struct DrawInputScratch {
    std::vector<VertexFetch> fetches;
    VertexCopyPlan plan;
    std::vector<std::size_t> planOrder;
    std::vector<VkBuffer> copyHandles;
    std::vector<VkDeviceSize> copyBases;
    std::vector<VkBuffer> vertexHandles;
    std::vector<VkDeviceSize> vertexOffsets;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> inPlaceRanges;
    VertexInputLayout vertexInput;
    std::vector<std::uint64_t> validationKey;
    unsigned depth = 0;
};

}

#endif
