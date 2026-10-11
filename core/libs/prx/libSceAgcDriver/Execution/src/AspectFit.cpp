#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace AgcDriver {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AspectFit: ") + reason);
}

std::atomic<std::uint64_t> presentationAspect{0};

std::uint32_t roundToExtent(double value) {
    require(value >= 0.0 && value <= static_cast<double>(std::numeric_limits<std::uint32_t>::max()), "computed extent is out of range");
    return static_cast<std::uint32_t>(std::lround(value));
}

}
extern "C" {

double ComputeContainScale_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t boundsWidth, std::uint32_t boundsHeight, bool allowUpscale) {
    require(sourceWidth != 0 && sourceHeight != 0, "source extent must be non-zero");
    require(boundsWidth != 0 && boundsHeight != 0, "bounds extent must be non-zero");
    const auto scale = std::min(static_cast<double>(boundsWidth) / static_cast<double>(sourceWidth), static_cast<double>(boundsHeight) / static_cast<double>(sourceHeight));
    return allowUpscale ? scale : std::min(scale, 1.0);
}

AspectFitSize ComputeContainSize_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t boundsWidth, std::uint32_t boundsHeight, bool allowUpscale) {
    const auto scale = ComputeContainScale_nid_postfix(sourceWidth, sourceHeight, boundsWidth, boundsHeight, allowUpscale);
    return AspectFitSize{std::max<std::uint32_t>(1, roundToExtent(sourceWidth * scale)), std::max<std::uint32_t>(1, roundToExtent(sourceHeight * scale))};
}

AspectFitRect ComputeContainRect_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t destinationWidth, std::uint32_t destinationHeight) {
    const auto size = ComputeContainSize_nid_postfix(sourceWidth, sourceHeight, destinationWidth, destinationHeight, true);
    const auto x = (static_cast<std::int64_t>(destinationWidth) - static_cast<std::int64_t>(size.width)) / 2;
    const auto y = (static_cast<std::int64_t>(destinationHeight) - static_cast<std::int64_t>(size.height)) / 2;
    return AspectFitRect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), size.width, size.height};
}

std::uint32_t ComputeWidthForHeight_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t height) {
    require(sourceWidth != 0 && sourceHeight != 0, "source extent must be non-zero");
    return std::max<std::uint32_t>(1, roundToExtent(static_cast<double>(height) * sourceWidth / sourceHeight));
}

std::uint32_t ComputeHeightForWidth_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t width) {
    require(sourceWidth != 0 && sourceHeight != 0, "source extent must be non-zero");
    return std::max<std::uint32_t>(1, roundToExtent(static_cast<double>(width) * sourceHeight / sourceWidth));
}

void SetPresentationAspect_nid_postfix(std::uint32_t width, std::uint32_t height) {
    presentationAspect.store(width == 0 || height == 0 ? 0 : (static_cast<std::uint64_t>(width) << 32) | height, std::memory_order_relaxed);
}

AspectFitSize PresentationAspect_nid_postfix() {
    const auto aspect = presentationAspect.load(std::memory_order_relaxed);
    return AspectFitSize{static_cast<std::uint32_t>(aspect >> 32), static_cast<std::uint32_t>(aspect)};
}

AspectFitRect ComputePresentationRect_nid_postfix(std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::uint32_t destinationWidth, std::uint32_t destinationHeight) {
    const auto aspect = PresentationAspect_nid_postfix();
    if (aspect.width == 0 || aspect.height == 0) return ComputeContainRect_nid_postfix(sourceWidth, sourceHeight, destinationWidth, destinationHeight);
    return ComputeContainRect_nid_postfix(aspect.width, aspect.height, destinationWidth, destinationHeight);
}

AspectFitRect ComputeUndistortedRect_nid_postfix(std::uint32_t bufferWidth, std::uint32_t bufferHeight, std::uint32_t presentWidth, std::uint32_t presentHeight) {
    require(bufferWidth != 0 && bufferHeight != 0, "buffer extent must be non-zero");
    if (presentWidth == 0 || presentHeight == 0) return AspectFitRect{0, 0, bufferWidth, bufferHeight};
    // How much wider than tall the presenter draws a buffer pixel.
    const auto stretch = (static_cast<double>(presentWidth) * bufferHeight) / (static_cast<double>(presentHeight) * bufferWidth);
    if (stretch >= 1.0) {
        const auto width = std::clamp<std::uint32_t>(roundToExtent(bufferWidth / stretch), 1, bufferWidth);
        return AspectFitRect{static_cast<std::int32_t>((bufferWidth - width) / 2), 0, width, bufferHeight};
    }
    const auto height = std::clamp<std::uint32_t>(roundToExtent(bufferHeight * stretch), 1, bufferHeight);
    return AspectFitRect{0, static_cast<std::int32_t>((bufferHeight - height) / 2), bufferWidth, height};
}

AspectFitRect MapRectInto_nid_postfix(AspectFitRect rect, std::uint32_t bufferWidth, std::uint32_t bufferHeight, AspectFitRect into) {
    require(bufferWidth != 0 && bufferHeight != 0, "buffer extent must be non-zero");
    const auto scaleX = static_cast<double>(into.width) / bufferWidth;
    const auto scaleY = static_cast<double>(into.height) / bufferHeight;
    const auto left = std::floor(into.x + rect.x * scaleX);
    const auto top = std::floor(into.y + rect.y * scaleY);
    const auto right = std::ceil(into.x + (static_cast<double>(rect.x) + rect.width) * scaleX);
    const auto bottom = std::ceil(into.y + (static_cast<double>(rect.y) + rect.height) * scaleY);
    return AspectFitRect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top), roundToExtent(right - left), roundToExtent(bottom - top)};
}

std::uint32_t ComputeRectMoveBlits_nid_postfix(std::uint32_t width, std::uint32_t height, AspectFitRect rect, AspectFitBlit* out) {
    require(width != 0 && height != 0 && out != nullptr, "move blits need an image and an output");
    if (rect.x == 0 && rect.y == 0 && rect.width == width && rect.height == height) return 0;
    require(rect.x >= 0 && rect.y >= 0 && rect.width != 0 && rect.height != 0 && static_cast<std::uint64_t>(rect.x) + rect.width <= width && static_cast<std::uint64_t>(rect.y) + rect.height <= height, "move rect must lie inside the image");
    require(rect.height == height || rect.width == width, "move rect must be a full-height column or a full-width band");
    const auto left = static_cast<std::uint32_t>(rect.x);
    const auto top = static_cast<std::uint32_t>(rect.y);
    const auto right = width - left - rect.width;
    const auto bottom = height - top - rect.height;
    std::uint32_t count = 0;
    out[count++] = AspectFitBlit{AspectFitRect{0, 0, width, height}, rect, false};
    if (rect.height == height) {
        if (left != 0) out[count++] = AspectFitBlit{AspectFitRect{0, 0, 1, height}, AspectFitRect{0, 0, left, height}, true};
        if (right != 0) out[count++] = AspectFitBlit{AspectFitRect{static_cast<std::int32_t>(width - 1), 0, 1, height}, AspectFitRect{static_cast<std::int32_t>(left + rect.width), 0, right, height}, true};
    } else {
        if (top != 0) out[count++] = AspectFitBlit{AspectFitRect{0, 0, width, 1}, AspectFitRect{0, 0, width, top}, true};
        if (bottom != 0) out[count++] = AspectFitBlit{AspectFitRect{0, static_cast<std::int32_t>(height - 1), width, 1}, AspectFitRect{0, static_cast<std::int32_t>(top + rect.height), width, bottom}, true};
    }
    return count;
}

}
}
