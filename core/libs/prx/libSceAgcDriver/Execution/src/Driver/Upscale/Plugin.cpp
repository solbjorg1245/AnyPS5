#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Plugin.hpp"
#include <SDL_loadso.h>
#include <SDL_error.h>
#include <cstdio>

namespace AgcDriver::Upscale {

LoadedPlugin::~LoadedPlugin() {
    if (library != nullptr) SDL_UnloadObject(library);
}

std::variant<std::shared_ptr<LoadedPlugin>, std::string> LoadPlugin(const std::string& path, const char* entry, std::uint32_t hostAbiVersion) {
    // SDL_LoadObject takes UTF-8 (LoadLibraryW on Windows), as the Vulkan loader's load does.
    auto plugin = std::make_shared<LoadedPlugin>();
    plugin->path = path;
    plugin->library = SDL_LoadObject(path.c_str());
    if (plugin->library == nullptr) return std::string("cannot load ") + path + ": " + SDL_GetError();
    const auto query = reinterpret_cast<PFN_Aps5UpscalerPluginQuery>(SDL_LoadFunction(plugin->library, entry));
    if (query == nullptr) return std::string("no ") + entry + " in " + path;
    auto& api = plugin->api;
    api.struct_size = sizeof(api);
    const auto status = query(hostAbiVersion, &api);
    if (status == APS5_UPSCALER_ABI_MISMATCH || (status == APS5_UPSCALER_OK && api.abi_version != hostAbiVersion)) {
        return "ABI version " + std::to_string(api.abi_version) + " (the port's " + std::to_string(hostAbiVersion) + ")";
    }
    if (status != APS5_UPSCALER_OK) return "query failed (status " + std::to_string(status) + ")";
    if (api.struct_size < sizeof(api)) return "api of " + std::to_string(api.struct_size) + " bytes (the port reads " + std::to_string(sizeof(api)) + ")";
    if (api.get_name == nullptr || api.get_instance_extensions == nullptr || api.get_device_extensions == nullptr || api.create == nullptr || api.evaluate == nullptr || api.release == nullptr || api.last_error == nullptr) {
        return std::string("api with a null function");
    }
    const char* name = api.get_name(&plugin->version);
    plugin->name = name != nullptr && name[0] != '\0' ? name : "unnamed";
    return plugin;
}

const LoadedPlugin* ActivePlugin() {
    // Kept for the process: a device torn down at exit still calls its release.
    static const auto* plugin = [] {
        auto* kept = new std::shared_ptr<LoadedPlugin>();
        if (!PluginSwitch()) return kept;
        const auto& settings = GetPortSettings_nid_no_patch();
        if (const char* refusal = PluginRefusal(true, settings.upscalerPlugin, PortSettings::UpscalerActive(settings), settings.upscalerBackend)) {
            std::fprintf(stderr, "[upscale] plugin off: %s\n", refusal);
            return kept;
        }
        auto loaded = LoadPlugin(settings.upscalerPlugin);
        if (const auto* reason = std::get_if<std::string>(&loaded)) {
            std::fprintf(stderr, "[upscale] plugin off: %s\n", reason->c_str());
            return kept;
        }
        *kept = std::get<std::shared_ptr<LoadedPlugin>>(loaded);
        std::fprintf(stderr, "[upscale] plugin %s v%u (%s)\n", (*kept)->name.c_str(), (*kept)->version, (*kept)->path.c_str());
        return kept;
    }();
    return plugin->get();
}

bool AddPluginExtensions(const LoadedPlugin& plugin, const char* what, std::int32_t status, const Aps5UpscalerExtensions& wanted, std::span<const VkExtensionProperties> offered, std::vector<const char*>& enabled) {
    if (status != APS5_UPSCALER_OK) {
        std::fprintf(stderr, "[upscale] plugin off: its %s extensions: %s\n", what, PluginError(plugin, nullptr));
        return false;
    }
    const auto merge = MergeExtensions(wanted, offered, enabled);
    if (!merge.missingRequired.empty()) {
        std::fprintf(stderr, "[upscale] plugin off: %s extensions not offered: %s\n", what, JoinNames(merge.missingRequired).c_str());
        return false;
    }
    if (!merge.missingOptional.empty()) std::fprintf(stderr, "[upscale] plugin: optional %s extensions not offered: %s\n", what, JoinNames(merge.missingOptional).c_str());
    enabled.insert(enabled.end(), merge.added.begin(), merge.added.end());
    return true;
}

}
