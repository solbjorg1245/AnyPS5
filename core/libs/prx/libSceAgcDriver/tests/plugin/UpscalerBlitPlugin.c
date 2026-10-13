/* The in-tree sample of the upscaler plugin ABI (docs/design/dlss.md S4): aps5_upscaler_blit.dll, for the
 * unit tests and the GPU lane's end-to-end check of APS5_UPSCALER_PLUGIN. No shaders: an evaluate is a
 * linear vkCmdBlitImage of MainColor's render rectangle over the output, between layout transitions when
 * an image is not in the general layout. Plain C on UpscalerPlugin.h; every Vulkan function comes from
 * the proc addresses given to create, so it does not link the Vulkan loader. */
#include "prx/libSceAgcDriver/Execution/include/UpscalerPlugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Aps5UpscalerContext {
    VkPhysicalDevice physical;
    PFN_vkGetPhysicalDeviceFormatProperties formatProperties;
    PFN_vkCmdPipelineBarrier pipelineBarrier;
    PFN_vkCmdBlitImage blitImage;
    char error[256];
};

static char lastError[256] = "";

static int32_t fail(Aps5UpscalerContext* context, int32_t status, const char* text) {
    char* target = context != NULL ? context->error : lastError;
    snprintf(target, 256, "%s", text);
    return status;
}

static const char* getName(uint32_t* version) {
    if (version != NULL) *version = 1u;
    return "aps5-blit";
}

static int32_t noExtensions(Aps5UpscalerExtensions* out) {
    if (out == NULL || out->struct_size < sizeof(Aps5UpscalerExtensions)) return fail(NULL, APS5_UPSCALER_ERROR, "extension list too small");
    out->required_count = out->optional_count = 0u;
    out->required = out->optional = NULL;
    return APS5_UPSCALER_OK;
}

static int32_t getInstanceExtensions(Aps5UpscalerExtensions* out) {
    return noExtensions(out);
}

static int32_t getDeviceExtensions(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr getInstanceProcAddr, Aps5UpscalerExtensions* out) {
    (void)instance;
    (void)physical;
    (void)getInstanceProcAddr;
    return noExtensions(out);
}

static int32_t create(const Aps5UpscalerCreateInfo* info, Aps5UpscalerContext** out) {
    Aps5UpscalerContext* context;
    if (out == NULL) return fail(NULL, APS5_UPSCALER_ERROR, "no context pointer");
    *out = NULL;
    if (info == NULL || info->struct_size < sizeof(Aps5UpscalerCreateInfo)) return fail(NULL, APS5_UPSCALER_ERROR, "create info too small");
    if (info->instance == VK_NULL_HANDLE || info->device == VK_NULL_HANDLE || info->get_instance_proc_addr == NULL || info->get_device_proc_addr == NULL) return fail(NULL, APS5_UPSCALER_ERROR, "no device");
    context = (Aps5UpscalerContext*)calloc(1, sizeof(*context));
    if (context == NULL) return fail(NULL, APS5_UPSCALER_ERROR, "out of memory");
    context->physical = info->physical_device;
    context->formatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)info->get_instance_proc_addr(info->instance, "vkGetPhysicalDeviceFormatProperties");
    context->pipelineBarrier = (PFN_vkCmdPipelineBarrier)info->get_device_proc_addr(info->device, "vkCmdPipelineBarrier");
    context->blitImage = (PFN_vkCmdBlitImage)info->get_device_proc_addr(info->device, "vkCmdBlitImage");
    if (context->formatProperties == NULL || context->pipelineBarrier == NULL || context->blitImage == NULL) {
        free(context);
        return fail(NULL, APS5_UPSCALER_ERROR, "Vulkan functions missing");
    }
    *out = context;
    return APS5_UPSCALER_OK;
}

/* A transition of `image` between its own layout and `other` (none when the image is in GENERAL, which
 * blits accept). */
static int transition(const Aps5UpscalerImage* image, VkImageLayout other, int toOther, VkImageMemoryBarrier* barrier) {
    VkImageSubresourceRange range = image->subresource;
    if (image->layout == VK_IMAGE_LAYOUT_GENERAL) return 0;
    range.levelCount = range.layerCount = 1u;
    memset(barrier, 0, sizeof(*barrier));
    barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier->oldLayout = toOther ? image->layout : other;
    barrier->newLayout = toOther ? other : image->layout;
    barrier->srcQueueFamilyIndex = barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier->image = image->image;
    barrier->subresourceRange = range;
    return 1;
}

static int32_t evaluate(Aps5UpscalerContext* context, const Aps5UpscalerEvaluateInfo* info) {
    VkFormatProperties source, target;
    VkImageMemoryBarrier barriers[2];
    VkImageBlit region;
    uint32_t count;
    if (context == NULL) return fail(NULL, APS5_UPSCALER_ERROR, "no context");
    if (info == NULL || info->struct_size < sizeof(Aps5UpscalerEvaluateInfo) || info->color.struct_size < sizeof(Aps5UpscalerImage) || info->output.struct_size < sizeof(Aps5UpscalerImage)) {
        return fail(context, APS5_UPSCALER_ERROR, "evaluate info too small");
    }
    /* Checked before anything is recorded: a failure leaves the command buffer as it was. */
    context->formatProperties(context->physical, info->color.format, &source);
    context->formatProperties(context->physical, info->output.format, &target);
    if ((source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 || (source.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) return fail(context, APS5_UPSCALER_UNSUPPORTED, "color format cannot be a linear blit source");
    if ((target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) return fail(context, APS5_UPSCALER_UNSUPPORTED, "output format cannot be a blit destination");
    if ((info->color.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || (info->output.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) return fail(context, APS5_UPSCALER_UNSUPPORTED, "images lack the transfer usage");
    if (info->render_width == 0u || info->render_height == 0u || info->render_width > info->color.extent.width || info->render_height > info->color.extent.height) return fail(context, APS5_UPSCALER_ERROR, "render rectangle outside the color image");
    memset(&region, 0, sizeof(region));
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = info->color.subresource.baseMipLevel;
    region.srcSubresource.baseArrayLayer = info->color.subresource.baseArrayLayer;
    region.srcSubresource.layerCount = 1u;
    region.srcOffsets[1].x = (int32_t)info->render_width;
    region.srcOffsets[1].y = (int32_t)info->render_height;
    region.srcOffsets[1].z = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel = info->output.subresource.baseMipLevel;
    region.dstSubresource.baseArrayLayer = info->output.subresource.baseArrayLayer;
    region.dstSubresource.layerCount = 1u;
    region.dstOffsets[1].x = (int32_t)info->output_width;
    region.dstOffsets[1].y = (int32_t)info->output_height;
    region.dstOffsets[1].z = 1;
    count = (uint32_t)transition(&info->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, &barriers[0]);
    count += (uint32_t)transition(&info->output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &barriers[count]);
    if (count != 0u) context->pipelineBarrier(info->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, count, barriers);
    context->blitImage(info->command_buffer, info->color.image, info->color.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, info->output.image,
        info->output.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region, VK_FILTER_LINEAR);
    /* Back to the layouts the host keeps the images in. */
    count = (uint32_t)transition(&info->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, &barriers[0]);
    count += (uint32_t)transition(&info->output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, &barriers[count]);
    if (count != 0u) context->pipelineBarrier(info->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, count, barriers);
    return APS5_UPSCALER_OK;
}

static void release(Aps5UpscalerContext* context) {
    free(context);
}

static const char* lastErrorOf(const Aps5UpscalerContext* context) {
    return context != NULL ? context->error : lastError;
}

APS5_UPSCALER_PLUGIN_EXPORT int32_t APS5_UpscalerPluginQuery(uint32_t hostAbiVersion, Aps5UpscalerPluginApi* out) {
    Aps5UpscalerPluginApi api;
    uint32_t size;
    if (out == NULL) return fail(NULL, APS5_UPSCALER_ERROR, "no api pointer");
    if (hostAbiVersion != APS5_UPSCALER_PLUGIN_ABI_VERSION) {
        out->abi_version = APS5_UPSCALER_PLUGIN_ABI_VERSION;
        return fail(NULL, APS5_UPSCALER_ABI_MISMATCH, "ABI version mismatch");
    }
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = APS5_UPSCALER_PLUGIN_ABI_VERSION;
    api.get_name = getName;
    api.get_instance_extensions = getInstanceExtensions;
    api.get_device_extensions = getDeviceExtensions;
    api.create = create;
    api.evaluate = evaluate;
    api.release = release;
    api.last_error = lastErrorOf;
    size = out->struct_size < api.struct_size ? out->struct_size : api.struct_size;
    memcpy(out, &api, size);
    out->struct_size = size;
    return APS5_UPSCALER_OK;
}
