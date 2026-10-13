#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPSCALE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPSCALE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Upscale.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <optional>

namespace AgcDriver::Graphics {

struct FastLayout;

// APS5_MFSR_EXEC's pass (docs/design/dlss.md S1, shaders/UpscaleBilinear.comp): its pipeline and
// sampler, made once per device (VulkanDevice keeps it) on a push layout of FastLayouts.
class BilinearUpscaler {
public:
    explicit BilinearUpscaler(const Context& context);
    ~BilinearUpscaler();
    BilinearUpscaler(const BilinearUpscaler&) = delete;
    BilinearUpscaler& operator=(const BilinearUpscaler&) = delete;
    // Records `plan` into the open batch as a fast dispatch records (GuestMemory::GpuMutex held):
    // MainColor sampled, every output texel written through a storage view, the output marked dirty.
    // Nullopt once recorded, else why nothing was; throws what the lookups throw.
    std::optional<Upscale::Skip> Record(const Context& context, const Upscale::Plan& plan);

private:
    Context context;
    const FastLayout* layout = nullptr;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    void release() noexcept;
};

}

#endif
