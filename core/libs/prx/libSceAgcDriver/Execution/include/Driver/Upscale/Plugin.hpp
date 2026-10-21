#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_PLUGIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_UPSCALE_PLUGIN_HPP

#include "prx/libSceAgcDriver/Execution/include/UpscalerPlugin.h"
#include "prx/libc/include/PortSettings.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// APS5_UPSCALER_PLUGIN=1 (docs/design/dlss.md S4, default off): an upscaler DLL the player gives
// (UpscalerPlugin = <path>, APS5_UPSCALER_PLUGIN_PATH) runs instead of the port's temporal pass when
// Upscaler = fsr or dlss under APS5_UPSCALER_SETTINGS=1 (UpscalerPlugin.h). Any failure: one
// "[upscale] plugin off" line and the port's passes, as without it (invariant 7).
namespace AgcDriver::Upscale {

inline constexpr std::uint64_t PluginTimingKey = 0x9;
inline constexpr const char* PluginSwitchName = "APS5_UPSCALER_PLUGIN";
// APS5_UPSCALER_PLUGIN, read once (on for any value but empty or "0").
inline bool PluginSwitch() {
    static const bool on = PortSettings::SwitchOn(std::getenv(PluginSwitchName));
    return on;
}

// Why the plugin is not loaded (nullptr: it is to be; "" silently, the switch is off).
inline const char* PluginRefusal(bool switchOn, std::string_view path, bool settingsActive, PortSettings::Upscaler backend) {
    if (!switchOn) return "";
    if (path.empty()) return "no path (UpscalerPlugin = <path> or APS5_UPSCALER_PLUGIN_PATH)";
    if (!settingsActive || (backend != PortSettings::Upscaler::Fsr && backend != PortSettings::Upscaler::Dlss)) return "Upscaler is not fsr or dlss under APS5_UPSCALER_SETTINGS=1";
    return nullptr;
}

// Which pass a frame tries first: the plugin when it is to be loaded and its device is ready, else the
// port's (temporal, else bilinear), as today.
enum class Backend : std::uint8_t { Port, Plugin };
inline Backend ChooseBackend(bool switchOn, std::string_view path, bool settingsActive, PortSettings::Upscaler backend, bool ready) {
    return PluginRefusal(switchOn, path, settingsActive, backend) == nullptr && ready ? Backend::Plugin : Backend::Port;
}

// A plugin's wanted extensions against those the implementation offers: the ones to add (not already
// in `enabled`), and the missing ones (a required one among them turns the plugin off).
struct ExtensionMerge {
    std::vector<const char*> added;
    std::vector<std::string> missingRequired, missingOptional;
};
inline ExtensionMerge MergeExtensions(const Aps5UpscalerExtensions& wanted, std::span<const VkExtensionProperties> offered, std::span<const char* const> enabled) {
    ExtensionMerge merge;
    const auto take = [&](const char* const* names, std::uint32_t count, std::vector<std::string>& missing) {
        for (std::uint32_t index = 0; names != nullptr && index < count; ++index) {
            const char* name = names[index];
            if (name == nullptr) continue;
            const auto same = [&](const char* other) { return std::strcmp(other, name) == 0; };
            if (std::any_of(enabled.begin(), enabled.end(), same) || std::any_of(merge.added.begin(), merge.added.end(), same)) continue;
            if (std::any_of(offered.begin(), offered.end(), [&](const VkExtensionProperties& item) { return same(item.extensionName); })) merge.added.push_back(name);
            else missing.emplace_back(name);
        }
    };
    take(wanted.required, wanted.required_count, merge.missingRequired);
    take(wanted.optional, wanted.optional_count, merge.missingOptional);
    return merge;
}

// A loaded plugin: its library (unloaded with the object) and the api it filled.
struct LoadedPlugin {
    void* library = nullptr;
    Aps5UpscalerPluginApi api{};
    std::string name, path;
    std::uint32_t version = 0;
    LoadedPlugin() = default;
    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;
    ~LoadedPlugin();
};

// Loads `path` (UTF-8), resolves `entry` and checks the ABI: the plugin, or why not.
std::variant<std::shared_ptr<LoadedPlugin>, std::string> LoadPlugin(const std::string& path, const char* entry = APS5_UPSCALER_PLUGIN_QUERY_NAME, std::uint32_t hostAbiVersion = APS5_UPSCALER_PLUGIN_ABI_VERSION);

// The process's plugin, read and loaded once (the first VulkanDevice, ahead of vkCreateInstance) with its
// one line; null when off.
const LoadedPlugin* ActivePlugin();
// The plugin's last error on `context` (null: outside one), never null.
inline const char* PluginError(const LoadedPlugin& plugin, const Aps5UpscalerContext* context) {
    const char* error = plugin.api.last_error(context);
    return error != nullptr && error[0] != '\0' ? error : "no reason given";
}
// Adds to `enabled` the plugin's extensions (`status` and `wanted` from its call) that `offered` has;
// false, with one "plugin off" line, when its call failed or a required one is not offered (`what`:
// "instance" or "device").
bool AddPluginExtensions(const LoadedPlugin& plugin, const char* what, std::int32_t status, const Aps5UpscalerExtensions& wanted, std::span<const VkExtensionProperties> offered, std::vector<const char*>& enabled);
// "a, b" for the log.
inline std::string JoinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& name : names) joined += (joined.empty() ? "" : ", ") + name;
    return joined;
}

}

#endif
