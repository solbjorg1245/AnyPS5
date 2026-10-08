#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERMERGE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERMERGE_HPP

#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {

// One global memory barrier (vkCmdPipelineBarrier with a single VkMemoryBarrier), as the dispatch
// path records them (VulkanDevice::recordDispatch, GuestBufferMemory's staging copy passes).
struct MemoryBarrierMasks {
    VkPipelineStageFlags sourceStages;
    VkPipelineStageFlags destinationStages;
    VkAccessFlags sourceAccess;
    VkAccessFlags destinationAccess;
};

// Two barriers recorded back to back, nothing between them, as one: the union of both sides. Every
// earlier command is before either and every later one after either, so its scopes contain both
// barriers' and it orders and makes visible all they did; it may order more (the second's source
// stages before the first's destination stages), never less.
constexpr MemoryBarrierMasks MergeAdjacentBarriers(const MemoryBarrierMasks& first, const MemoryBarrierMasks& second) {
    return {first.sourceStages | second.sourceStages, first.destinationStages | second.destinationStages, first.sourceAccess | second.sourceAccess, first.destinationAccess | second.destinationAccess};
}

// Whether `outer`, recorded where `inner` was, does at least what `inner` did: every stage and
// access of `inner`, on both sides, is `outer`'s too.
constexpr bool BarrierContains(const MemoryBarrierMasks& outer, const MemoryBarrierMasks& inner) {
    return (outer.sourceStages & inner.sourceStages) == inner.sourceStages && (outer.destinationStages & inner.destinationStages) == inner.destinationStages && (outer.sourceAccess & inner.sourceAccess) == inner.sourceAccess && (outer.destinationAccess & inner.destinationAccess) == inner.destinationAccess;
}

// The recorder's coverage (Recorder::Commands): `covered` is the destination access mask of the
// last recorded trailing barrier with ALL_COMMANDS as its destination stage (0: none, or something
// was recorded after it), every write recorded before it then being available and visible to those
// accesses of any later stage, and every earlier command ordered before every later stage. A
// leading barrier that makes earlier writes visible to `needed` and orders earlier work before its
// own commands adds nothing after such a barrier.
constexpr bool LeadingBarrierCovered(VkAccessFlags covered, VkAccessFlags needed) {
    return covered != 0 && (covered & needed) == needed;
}

// The barriers around a dispatch (VulkanDevice::recordDispatch): the indirect read's (the group
// counts were stored by earlier recorded work or the host), the leading one (results of earlier
// recorded work visible to the dispatch) and the trailing one (its own visible to everything
// after, the coverage it marks).
inline constexpr MemoryBarrierMasks IndirectArgumentsBarrier{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
inline constexpr MemoryBarrierMasks DispatchLeadingBarrier{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
inline constexpr MemoryBarrierMasks DispatchTrailingBarrier{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
// What a staging copy pass's leading barrier (copy-in, copy-back: GuestBufferMemory) must find
// covered to be left out: its copies read their sources and write their destinations.
inline constexpr VkAccessFlags StagingCopyAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

}

#endif
