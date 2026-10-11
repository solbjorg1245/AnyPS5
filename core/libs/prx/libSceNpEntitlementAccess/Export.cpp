#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/AddonContent.hpp"
#include "prx/libc/include/PortSettings.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static constexpr int SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER = static_cast<int>(0x80558003);
static constexpr int SCE_NP_ENTITLEMENT_ACCESS_ERROR_NOT_FOUND = static_cast<int>(0x80558007);
static constexpr int SCE_NP_ERROR_SIGNED_OUT = static_cast<int>(0x80550006);
static constexpr uint32_t SKU_FLAG_FULL = 3;

namespace {

std::filesystem::path EntitlementsPath() {
    if (const char* configured = std::getenv("ANYPS5_ENTITLEMENTS"); configured != nullptr && configured[0] != '\0') return configured;
#ifdef _WIN32
    wchar_t module[MAX_PATH];
    const auto length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length == MAX_PATH) throw std::runtime_error("NpEntitlementAccess: cannot locate the executable");
    return std::filesystem::path(module).parent_path() / "anyps5-entitlements.ini";
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path() / "anyps5-entitlements.ini";
#endif
}

std::vector<std::string> FileLabels() {
    std::vector<std::string> labels;
    const auto path = EntitlementsPath();
    std::ifstream file(path);
    if (!file) {
        if (std::getenv("ANYPS5_ENTITLEMENTS") != nullptr || std::filesystem::exists(path)) throw std::runtime_error("NpEntitlementAccess: cannot read " + path.string());
        return labels;
    }
    std::string line;
    while (std::getline(file, line)) {
        line.erase(std::min(line.find_first_of("#;"), line.size()));
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto label = line.substr(first, line.find_last_not_of(" \t\r") + 1 - first);
        if (label.size() >= sizeof(NpUnifiedEntitlementLabel::data)) throw std::runtime_error("NpEntitlementAccess: entitlement label '" + label + "' in " + path.string() + " is longer than 16 characters");
        labels.push_back(label);
    }
    return labels;
}

// The game's dump folders: GameDir from the settings (the launcher's), and /app0 with the dump its folders
// link to (the release's setup links the dump's folders into app0).
std::vector<std::filesystem::path> GameFolders(const PortSettings::Settings& settings) {
    std::vector<std::filesystem::path> folders;
    if (!settings.gameDir.empty()) folders.push_back(AddonContent::PathFromText(settings.gameDir));
    for (const auto& folder : AddonContent::DumpFolders(ResolvePath_nid_no_patch("/app0"))) folders.push_back(folder);
    return folders;
}

// The add-ons the player owns: the entitlements file's labels and the title's add-ons found in the dump
// (s61 T-deluxe: the Deluxe Edition only from the dump, and not with DeluxeEdition = off). Logged once,
// with the '[settings] deluxe edition' line.
const std::vector<NpEntitlementAccessAddcontEntitlementInfo>& OwnedAddons() {
    static const auto owned = [] {
        const auto& settings = GetPortSettings_nid_no_patch();
        const bool deluxe = settings.deluxeEdition != PortSettings::DeluxeEdition::Off;
        const auto found = AddonContent::Find(GameFolders(settings), AddonContent::PathFromText(settings.addonDir));
        std::vector<std::string> notes;
        const auto labels = AddonContent::AnsweredLabels(FileLabels(), found, deluxe, notes);
        std::vector<NpEntitlementAccessAddcontEntitlementInfo> addons;
        for (const auto& label : labels) {
            NpEntitlementAccessAddcontEntitlementInfo info{};
            std::memcpy(info.entitlement_label.data, label.data(), label.size());
            info.package_type = 3;
            info.download_status = 4;
            addons.push_back(info);
        }
        const auto* entry = AddonContent::FindLabel(found, AddonContent::DeluxeEditionLabel);
        if (entry != nullptr) std::fprintf(stderr, "[settings] deluxe edition: found in the dump (%s: %s), %s\n", PortSettings::PathText(entry->folder).c_str(), entry->reason.c_str(), deluxe ? "enabled" : "turned off (DeluxeEdition = off)");
        else std::fprintf(stderr, "[settings] deluxe edition: not found in the dump, not owned%s\n", deluxe ? "" : " (DeluxeEdition = off)");
        for (const auto& note : notes) std::fprintf(stderr, "[entitlement] %s\n", note.c_str());
        std::string answered;
        for (const auto& label : labels) answered += (answered.empty() ? "" : ", ") + label;
        std::fprintf(stderr, "[entitlement] owned add-ons: %s\n", answered.empty() ? "none" : answered.c_str());
        std::fflush(stderr);
        return addons;
    }();
    return owned;
}

// The game's queries, the first few of each kind (the answer is fixed for the process).
void LogQuery(const char* what, const char* label, int result, std::uint32_t hits) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) >= 8) return;
    if (label != nullptr) std::fprintf(stderr, "[entitlement] %s %.16s: %s\n", what, label, result == 0 ? "owned" : "not owned");
    else std::fprintf(stderr, "[entitlement] %s: %u owned\n", what, hits);
    std::fflush(stderr);
}

}

extern "C" {

int APS5_VABI sceNpEntitlementAccessGetAddcontEntitlementInfo(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label, NpEntitlementAccessAddcontEntitlementInfo* info) {
    (void)service_label;
    if (!entitlement_label || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    for (const auto& addon : OwnedAddons()) {
        if (std::strncmp(addon.entitlement_label.data, entitlement_label->data, sizeof(entitlement_label->data)) == 0) {
            *info = addon;
            LogQuery("GetAddcontEntitlementInfo", entitlement_label->data, 0, 1);
            return 0;
        }
    }
    LogQuery("GetAddcontEntitlementInfo", entitlement_label->data, SCE_NP_ENTITLEMENT_ACCESS_ERROR_NOT_FOUND, 0);
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NOT_FOUND;
}

int APS5_VABI sceNpEntitlementAccessGetAddcontEntitlementInfoList(uint32_t service_label, NpEntitlementAccessAddcontEntitlementInfo* list, uint32_t list_num, uint32_t* hit_num) {
    (void)service_label;
    if (!hit_num || (!list && list_num != 0)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    const auto& owned = OwnedAddons();
    *hit_num = static_cast<uint32_t>(owned.size());
    std::copy_n(owned.begin(), std::min<std::size_t>(list_num, owned.size()), list);
    LogQuery("GetAddcontEntitlementInfoList", nullptr, 0, *hit_num);
    return 0;
}

int APS5_VABI sceNpEntitlementAccessGetSkuFlag(uint32_t* sku_flag) {
    if (!sku_flag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    *sku_flag = SKU_FLAG_FULL;
    return 0;
}

int APS5_VABI sceNpEntitlementAccessInitialize(const NpEntitlementAccessInitParam* init_param, NpEntitlementAccessBootParam* boot_param) {
    (void)init_param;
    (void)boot_param;
    OwnedAddons();  // the dump is looked at (and the '[settings] deluxe edition' line printed) at boot
    return 0;
}

int APS5_VABI sceNpEntitlementAccessAbortRequest(void) {
 return 0;
}

int APS5_VABI sceNpEntitlementAccessDeleteRequest(void) {
 return 0;
}

int APS5_VABI sceNpEntitlementAccessGenerateTransactionId(void* transaction_id) {
 if (!transaction_id) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
 return SCE_NP_ERROR_SIGNED_OUT;
}

int APS5_VABI sceNpEntitlementAccessPollConsumeEntitlement(void) {
 return SCE_NP_ERROR_SIGNED_OUT;
}

int APS5_VABI sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(void) {
 return SCE_NP_ERROR_SIGNED_OUT;
}

}
