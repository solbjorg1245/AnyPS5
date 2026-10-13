#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPSCALE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPSCALE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Plugin.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Upscale.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <memory>
#include <optional>
#include <string>

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

// APS5_UPSCALER_PORT's pass (docs/design/dlss.md S2, shaders/UpscaleTemporal.comp): its pipeline and
// samplers, made once per device (VulkanDevice keeps it), and two output-size RGBA16F history images,
// made at the first pass and again when the output size changes, read and written in turn.
class TemporalUpscaler {
public:
    explicit TemporalUpscaler(const Context& context);
    ~TemporalUpscaler();
    TemporalUpscaler(const TemporalUpscaler&) = delete;
    TemporalUpscaler& operator=(const TemporalUpscaler&) = delete;
    // As BilinearUpscaler::Record, reading MainColor, Motion, MainDepth and the last history, writing
    // the next history and the output (a reset when the history images are new).
    std::optional<Upscale::Skip> Record(const Context& context, const Upscale::TemporalPlan& plan);
    struct HistoryImage;

private:
    Context context;
    const FastLayout* layout = nullptr;
    VkSampler linear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::array<std::shared_ptr<HistoryImage>, 2> history;
    // The image the next pass reads.
    std::size_t newest = 0;
    void release() noexcept;
};

// APS5_UPSCALER_PLUGIN's pass (docs/design/dlss.md S4, UpscalerPlugin.h): a loaded plugin's context, made
// at the first pass for its sizes (VulkanDevice keeps it; released with the device, idle), and its
// evaluate recorded into the open batch between two full barriers, kept and marked as the port passes.
class PluginUpscaler {
public:
    // What the Graphics context does not carry.
    struct Device {
        VkInstance instance;
        PFN_vkGetInstanceProcAddr instanceProc;
        std::uint32_t family, apiVersion;
    };
    PluginUpscaler(const Context& context, const Upscale::LoadedPlugin& plugin, const Device& device);
    ~PluginUpscaler();
    PluginUpscaler(const PluginUpscaler&) = delete;
    PluginUpscaler& operator=(const PluginUpscaler&) = delete;
    // As TemporalUpscaler::Record (the plugin's failure: PluginFailed, its error in `detail`).
    std::optional<Upscale::Skip> Record(const Context& context, const Upscale::TemporalPlan& plan, std::string& detail);

private:
    Context context;
    const Upscale::LoadedPlugin& plugin;
    Device device;
    Aps5UpscalerContext* handle = nullptr;
    // The sizes it was made for: the render maxima, the output.
    std::array<std::uint32_t, 4> sizes{};
    bool createFailed = false;
};

}

#endif
