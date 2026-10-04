#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace AgcDriver::Graphics {

class Texture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
void ClearDepthSurfaces(VkDevice device);
bool DepthSurfaceAt(std::uint64_t address);
// Debug aid (APS5_CAPTURE_DIR, see VulkanDevice::CaptureTargets): saves every depth surface's planes
// as <prefix>depth_<address>_<width>x<height>.raw and <prefix>stencil_... (u32 width, height,
// VkFormat, then the texels). The GPU must be idle. Returns the number of files written.
std::size_t DumpDepthSurfaces(const Context& context, const std::string& prefix);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

}

#endif
