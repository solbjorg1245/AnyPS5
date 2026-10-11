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

}
}

#endif
