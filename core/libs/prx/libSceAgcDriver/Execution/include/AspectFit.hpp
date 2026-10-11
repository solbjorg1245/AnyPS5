#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ASPECTFIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ASPECTFIT_HPP

#include <cstdint>

namespace AgcDriver {

struct AspectFitRect {
    std::int32_t x;
    std::int32_t y;
    std::uint32_t width;
    std::uint32_t height;
};

struct AspectFitSize {
    std::uint32_t width;
    std::uint32_t height;
};

// One blit of an image into itself (through a copy): `source` stretched over `destination`, with
// nearest filtering (one edge texel line spread over a margin) or linear.
struct AspectFitBlit {
    AspectFitRect source;
    AspectFitRect destination;
    bool nearest;
};

extern "C" {

double ComputeContainScale_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t boundsWidth, std::uint32_t boundsHeight, bool allowUpscale);
AspectFitSize ComputeContainSize_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t boundsWidth, std::uint32_t boundsHeight, bool allowUpscale);
AspectFitRect ComputeContainRect_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t destinationWidth, std::uint32_t destinationHeight);
std::uint32_t ComputeWidthForHeight_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t height);
std::uint32_t ComputeHeightForWidth_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t width);
// The aspect the flipped image is shown at (libSceVideoOut NativeAspectPatch: the title's 16:9 display
// buffer holds a wider or narrower image squeezed); 0x0 = the buffer's own aspect (the default).
void SetPresentationAspect_nid_postfix(std::uint32_t width, std::uint32_t height);
AspectFitSize PresentationAspect_nid_postfix();
// The destination rect of a width x height flip: contained at the presentation aspect when one is set.
AspectFitRect ComputePresentationRect_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t destinationWidth, std::uint32_t destinationHeight);
// The part of a bufferWidth x bufferHeight buffer that keeps the buffer's own aspect once the buffer
// is presented at presentWidth:presentHeight: a centred full-height column when the presentation is
// wider, a centred full-width band when it is narrower, the whole buffer at the same aspect or with
// no presentation aspect (0x0). Native aspect's UI rect: a 16:9 layout drawn into it keeps its
// proportions on a 21:9 or 16:10 window.
AspectFitRect ComputeUndistortedRect_nid_postfix(std::uint32_t bufferWidth, std::uint32_t bufferHeight, std::uint32_t presentWidth, std::uint32_t presentHeight);
// `rect` (pixels of a bufferWidth x bufferHeight buffer) carried into `into` as the whole buffer maps
// onto it, rounded outwards to whole pixels.
AspectFitRect MapRectInto_nid_postfix(AspectFitRect rect, std::uint32_t bufferWidth, std::uint32_t bufferHeight, AspectFitRect into);
// Native aspect's UI move (APS5_NATIVE_ASPECT_UI_BLIT): the blits that carry a whole width x height
// image into `rect` (a centred column or band, as ComputeUndistortedRect gives: full height or full
// width), linear, then stretch the image's outermost column (row) over each margin the rect leaves,
// nearest, so what the edges hold (nothing, or a full-screen dim) still reaches the window's edges.
// Writes up to three entries of `out`; returns how many (0 when `rect` is the whole image).
std::uint32_t ComputeRectMoveBlits_nid_postfix(std::uint32_t width, std::uint32_t height, AspectFitRect rect, AspectFitBlit* out);

}
}

#endif
