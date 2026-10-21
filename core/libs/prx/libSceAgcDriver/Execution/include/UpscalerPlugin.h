/* The upscaler plugin ABI (docs/design/dlss.md S4, APS5_UPSCALER_PLUGIN=1): a DLL built apart from the
 * port (a DLSS bridge from NVIDIA's SDK, an FSR bridge, the in-tree blit sample) upscales in the PS5 Pro
 * seam through these C types and function pointers only.
 * Licence: under which terms a third-party bridge may include this header is the owner's decision
 * (dlss.md Open 1); until then it is the fork's. The fork includes no NVIDIA or AMD header: a bridge
 * includes this file and its vendor's SDK, and the player supplies the built DLL.
 * Rules: plain C, Vulkan's types only. Every struct starts with struct_size (the caller's sizeof), so a
 * later version appends fields and reads only what the other side's size covers. Strings and arrays a
 * plugin returns stay valid until its DLL is unloaded. */
#ifndef APS5_UPSCALER_PLUGIN_H
#define APS5_UPSCALER_PLUGIN_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APS5_UPSCALER_PLUGIN_ABI_VERSION 1u
/* The one exported entry point (PFN_Aps5UpscalerPluginQuery). */
#define APS5_UPSCALER_PLUGIN_QUERY_NAME "APS5_UpscalerPluginQuery"

#if defined(_WIN32)
#define APS5_UPSCALER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define APS5_UPSCALER_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Status codes (int32_t): 0 succeeds; on any other the plugin recorded nothing and last_error says why. */
#define APS5_UPSCALER_OK 0
#define APS5_UPSCALER_ERROR 1
#define APS5_UPSCALER_UNSUPPORTED 2
#define APS5_UPSCALER_ABI_MISMATCH 3

/* Aps5UpscalerCreateInfo.flags and Aps5UpscalerEvaluateInfo.flags. */
#define APS5_UPSCALER_FLAG_DEPTH_INVERTED 0x1u        /* reversed Z: 1.0 is near */
#define APS5_UPSCALER_FLAG_MOTION_RENDER_PIXELS 0x2u  /* motion in render pixels after motion_scale */
#define APS5_UPSCALER_FLAG_HDR 0x4u                   /* color is linear HDR, not tone-mapped */
#define APS5_UPSCALER_FLAG_RESET 0x8u                 /* evaluate only: no history (a cut, new sizes) */

typedef struct Aps5UpscalerContext Aps5UpscalerContext;

/* Extensions a plugin needs: the required ones (any missing turns the plugin off) and the optional ones. */
typedef struct Aps5UpscalerExtensions {
    uint32_t struct_size;
    uint32_t required_count;
    const char* const* required;
    uint32_t optional_count;
    const char* const* optional;
} Aps5UpscalerExtensions;

/* The host's device, after vkCreateDevice. The plugin resolves every Vulkan function through the two
 * proc addresses (it need not link the Vulkan loader). */
typedef struct Aps5UpscalerCreateInfo {
    uint32_t struct_size;
    uint32_t flags;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    uint32_t queue_family;
    uint32_t api_version; /* the instance's VkApplicationInfo.apiVersion */
    uint32_t max_render_width, max_render_height;
    uint32_t output_width, output_height;
} Aps5UpscalerCreateInfo;

/* One image of an evaluate. `layout` is the layout on entry; the plugin leaves the image in it again. */
typedef struct Aps5UpscalerImage {
    uint32_t struct_size;
    VkImage image;
    VkImageView view; /* a 2D view of the subresource below */
    VkFormat format;
    VkImageUsageFlags usage; /* the image's usage flags */
    VkImageSubresourceRange subresource;
    VkExtent2D extent; /* of that mip */
    VkImageLayout layout;
} Aps5UpscalerImage;

/* One frame. The command buffer is outside a render pass; the host records a full memory barrier before
 * and after the call and re-binds its own state afterwards. Color, depth and motion cover render_width x
 * render_height from (0, 0); the output is written over output_width x output_height. */
typedef struct Aps5UpscalerEvaluateInfo {
    uint32_t struct_size;
    uint32_t flags;
    VkCommandBuffer command_buffer;
    Aps5UpscalerImage color, depth, motion, output;
    uint32_t render_width, render_height;
    uint32_t output_width, output_height;
    float jitter_x, jitter_y;             /* render pixels */
    float motion_scale_x, motion_scale_y; /* motion texel * scale = render pixels */
    float exposure, pre_exposure;         /* 1.0 when unknown */
    float frame_delta_ms;                 /* 0 when unknown */
} Aps5UpscalerEvaluateInfo;

typedef struct Aps5UpscalerPluginApi {
    uint32_t struct_size;
    uint32_t abi_version;
    /* The plugin's name, and its own version through `version` (may be null). */
    const char* (*get_name)(uint32_t* version);
    /* Before vkCreateInstance. */
    int32_t (*get_instance_extensions)(Aps5UpscalerExtensions* out);
    /* Before vkCreateDevice, for the physical device the host chose. */
    int32_t (*get_device_extensions)(VkInstance instance, VkPhysicalDevice physical_device, PFN_vkGetInstanceProcAddr get_instance_proc_addr, Aps5UpscalerExtensions* out);
    int32_t (*create)(const Aps5UpscalerCreateInfo* info, Aps5UpscalerContext** out);
    /* Records the frame into info->command_buffer, or records nothing and fails (the host then runs its
     * own upscaler for that frame). */
    int32_t (*evaluate)(Aps5UpscalerContext* context, const Aps5UpscalerEvaluateInfo* info);
    /* The device is idle. */
    void (*release)(Aps5UpscalerContext* context);
    /* Why the last call on `context` (null: the last call without one) failed; never null. */
    const char* (*last_error)(const Aps5UpscalerContext* context);
} Aps5UpscalerPluginApi;

/* The entry point: the host sets out->struct_size to its sizeof and passes its ABI version; the plugin
 * fills what both sizes cover, sets struct_size to what it filled and abi_version to its own, and
 * returns APS5_UPSCALER_ABI_MISMATCH for a version it does not serve. */
typedef int32_t (*PFN_Aps5UpscalerPluginQuery)(uint32_t host_abi_version, Aps5UpscalerPluginApi* out);

#ifdef __cplusplus
}
#endif

#endif
