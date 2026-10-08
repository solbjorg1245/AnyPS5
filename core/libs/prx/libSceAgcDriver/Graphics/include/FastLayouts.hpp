#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTLAYOUTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTLAYOUTS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

// A push-descriptor layout of the fast path: the set layout (the bindings a ShaderResources build
// makes, flagged VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) and the pipeline layout of
// that set plus the 128-byte push constant range, as Pipeline builds it for a ShaderResources set.
struct FastLayout {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages = 0;
    // Descriptors one push writes (the sum of the binding counts), at most maxPushDescriptors.
    std::uint32_t descriptors = 0;
    // Dense, from 1 in creation order: the push layout id in the fast pipeline key.
    std::uint32_t id = 0;
};

// The fast path's push layouts by ShaderResources layout key (ShaderResources::LayoutKey: binding,
// type, count and stage flags per binding) and push constant stages, created on first use and kept
// for the device's lifetime, so a returned entry and its handles never change (docs/design/
// draw-fastpath.md 2.4, F3a). Thread-safe.
class FastLayouts {
public:
    explicit FastLayouts(const Context& context);
    ~FastLayouts();
    FastLayouts(const FastLayouts&) = delete;
    FastLayouts& operator=(const FastLayouts&) = delete;

    // Null when the device has no push descriptors or the layout needs more than
    // maxPushDescriptors (the draw then takes the old path).
    const FastLayout* Get(std::span<const std::uint32_t> layoutKey, VkShaderStageFlags pushStages);
    // APS5_PROFILE_DRAW counters (cumulative).
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t created = 0;
        std::uint64_t overLimit = 0;
    };
    Stats Counters() const;

private:
    Context context;
    mutable HostMutex mutex;
    std::map<std::vector<std::uint32_t>, std::unique_ptr<FastLayout>> layouts;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> created{0};
    std::atomic<std::uint64_t> overLimit{0};
};

// Records `writes` (one per binding, dstSet ignored) as the set 0 of `layout` (vkCmdPushDescriptorSetKHR).
void PushDescriptors(const Context& context, VkCommandBuffer commands, VkPipelineBindPoint point, const FastLayout& layout, std::span<const VkWriteDescriptorSet> writes);

}

#endif
