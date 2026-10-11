#include "prx/libSceAgcDriver/Graphics/include/FastLayouts.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include <mutex>

namespace AgcDriver::Graphics {

FastLayouts::FastLayouts(const Context& context) : context(context) {
    // Kept for the device's lifetime like the descriptor cache; it must not keep the buffer pool alive.
    this->context.bufferPool.reset();
}

FastLayouts::~FastLayouts() {
    for (const auto& [key, layout] : layouts) {
        context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout->pipeline, nullptr);
        context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, layout->set, nullptr);
    }
}

const FastLayout* FastLayouts::Get(std::span<const std::uint32_t> layoutKey, VkShaderStageFlags pushStages) {
    Require(layoutKey.size() % 4 == 0, "a layout key holds four words per binding");
    if (!context.pushDescriptors) return nullptr;
    std::uint64_t descriptors = 0;
    for (std::size_t i = 2; i < layoutKey.size(); i += 4) descriptors += layoutKey[i];
    if (descriptors > context.maxPushDescriptors) {
        overLimit.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    thread_local std::vector<std::uint32_t> key;
    key.assign(layoutKey.begin(), layoutKey.end());
    key.push_back(pushStages);
    std::lock_guard lock(mutex);
    if (const auto found = layouts.find(key); found != layouts.end()) {
        hits.fetch_add(1, std::memory_order_relaxed);
        return found->second.get();
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(layoutKey.size() / 4);
    for (std::size_t i = 0; i < layoutKey.size(); i += 4) bindings.push_back({layoutKey[i], static_cast<VkDescriptorType>(layoutKey[i + 1]), layoutKey[i + 2], layoutKey[i + 3], nullptr});
    auto layout = std::make_unique<FastLayout>();
    layout->pushStages = pushStages;
    layout->descriptors = static_cast<std::uint32_t>(descriptors);
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setInfo.pBindings = bindings.empty() ? nullptr : bindings.data();
    Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setInfo, nullptr, &layout->set), "vkCreateDescriptorSetLayout push");
    const VkPushConstantRange push{pushStages, 0, PipelinePushConstantBytes};
    VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &layout->set;
    pipelineInfo.pushConstantRangeCount = pushStages != 0 ? 1 : 0;
    pipelineInfo.pPushConstantRanges = pushStages != 0 ? &push : nullptr;
    const auto result = context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineInfo, nullptr, &layout->pipeline);
    if (result != VK_SUCCESS) {
        context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, layout->set, nullptr);
        Check(result, "vkCreatePipelineLayout push");
    }
    layout->id = static_cast<std::uint32_t>(layouts.size() + 1);
    created.fetch_add(1, std::memory_order_relaxed);
    return layouts.emplace(key, std::move(layout)).first->second.get();
}

FastLayouts::Stats FastLayouts::Counters() const {
    return {hits.load(std::memory_order_relaxed), created.load(std::memory_order_relaxed), overLimit.load(std::memory_order_relaxed)};
}

void PushDescriptors(const Context& context, VkCommandBuffer commands, VkPipelineBindPoint point, const FastLayout& layout, std::span<const VkWriteDescriptorSet> writes) {
    if (writes.empty()) return;
    context.Resolved(&DeviceFunctions::cmdPushDescriptorSet, "vkCmdPushDescriptorSetKHR")(commands, point, layout.pipeline, 0, static_cast<std::uint32_t>(writes.size()), writes.data());
}

}
