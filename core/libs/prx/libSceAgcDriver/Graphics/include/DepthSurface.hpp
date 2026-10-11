#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TexelStats.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
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
// Moves with every change of what DepthSurfaceAt and DepthSurfaceTexture answer: a surface
// registered or released, or its written depth taken over or noted (session 23). A proof that saw
// the serial and found no surface over its memory need not look again while it is unchanged.
std::uint64_t DepthSurfaceSerial();
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
// Debug aid (APS5_PASS_DUMP, see Execution/include/Driver/PassDump.hpp): reads back the depth plane
// (D16_UNORM or D32_SFLOAT) of array slice `slice` of every depth surface whose depth is at
// `address`, each handed to `consume`. The GPU must be idle. Returns the number of surfaces read.
std::size_t ReadBackDepthSurface(const Context& context, std::uint64_t address, std::uint32_t slice, const std::function<void(const ImageReadback&)>& consume);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);
// Whether DepthSurfaceTexture would answer a lookup of `resource` with `texture` again (the newest
// surface covering it is not overwritten and still hands the texture out), with the lookup's side
// effect: the surface's pending metadata clears are recorded. The fast proof's check of an element
// a depth surface served (ShaderResources::fastRevalidate).
bool DepthSurfaceServes(const Context& context, const GuestTextureResource& resource, const Texture* texture);
// Debug aid (APS5_ASPECT_PROBE=1 with APS5_PROFILE_DRAW=1; Demon's Souls native aspect): notes the
// viewport and scissor of a draw into a depth surface. DepthViewportDigest returns, for the largest depth
// extent drawn into since its last call, the draws and their first four distinct viewport/scissor pairs
// ("; depth viewports ..."), then the display buffer writes below ("; display buffer writes ..."),
// for the 10 s [draw] line; empty when off.
void NoteDepthViewport(const State& state);
std::string DepthViewportDigest();
// The same probe's display buffer writes, to tell the UI draws from the 3D composite and upscale:
// a presented buffer's address is registered (the last four), and every draw whose colour target is
// one of them and every dispatch whose storage image is one of them is listed by program (fragment
// variant id, compute program address), viewport, scissor, blend, extent, count and order within the
// frame (its rank among the frame's display buffer writes and its index among the frame's draws and
// dispatches). NoteDisplayBufferDraw runs for every draw (prepareDrawInputs), AspectProbeDispatch
// around every dispatch (Driver::dispatch), NoteDisplayBufferStorageWrite where a storage image is
// marked written (StorageTexture::MarkDirty), NoteDisplayBufferPresent at every present.
void NoteDisplayBufferPresent(std::uint64_t address);
void NoteDisplayBufferDraw(const State& state, std::span<const CompiledShader> shaders);
void NoteDisplayBufferStorageWrite(std::uint64_t address, std::uint32_t width, std::uint32_t height);
struct AspectProbeDispatch {
    explicit AspectProbeDispatch(std::uint64_t program);
    ~AspectProbeDispatch();
    AspectProbeDispatch(const AspectProbeDispatch&) = delete;
    AspectProbeDispatch& operator=(const AspectProbeDispatch&) = delete;
};

}

#endif
