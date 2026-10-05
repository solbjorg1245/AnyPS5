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
// Whether a resource of this extent at `address` is a plane of a depth surface whose image holds the
// newest depth (one of another extent there is another resource sharing the memory).
bool DepthSurfaceAt(std::uint64_t address, std::uint32_t width, std::uint32_t height);
// A shader writes the memory of a depth surface of this extent through a storage image: until a draw
// uses the surface again (which takes the written depth over), the storage image holds the newest
// depth and samplers read it.
void NoteDepthSurfaceWrite(std::uint64_t address, std::uint32_t width, std::uint32_t height);
// A buffer fill set guest memory [begin, end) to a pattern whose HTILE words mark every tile cleared
// (ZMASK 0). Where that is the HTILE of a depth surface, the surface was cleared: the AGC toolkit
// clears depth and stencil by filling HTILE with zeros (every tile then holds DB_DEPTH_CLEAR /
// DB_STENCIL_CLEAR), which no draw shows. The slices whose HTILE was filled are cleared to the clear
// values before the surface's next use.
void NoteDepthMetadataClear(std::uint64_t begin, std::uint64_t end);
// Debug aid (APS5_CAPTURE_DIR, see VulkanDevice::CaptureTargets): saves the planes of every depth
// surface (of those with a plane at `addresses`, when given) as
// <prefix>depth_<address>_<width>x<height>_l<slice>.raw and <prefix>stencil_... (u32 width, height,
// VkFormat, then the texels). The GPU must be idle. Returns the number of files written.
std::size_t DumpDepthSurfaces(const Context& context, const std::string& prefix, std::span<const std::uint64_t> addresses = {});
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);

}

#endif
