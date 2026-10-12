#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_ENGINESWITCHES_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_ENGINESWITCHES_HPP

#include <array>
#include <string>
#include <string_view>
#include <vector>

// Demon's Souls (PPSA01341) engine switches (H2, s61): the title's own convars, set through the same
// "+name=value" lines its render configs and command line use. The convars are static objects (0xe8
// bytes each, ~560 of them: bool constructor FUN_140460a20, float FUN_1409360b0, int FUN_140441500)
// whose live values sit in one byte table at 0x142f23b40 + a per-convar index; the renderer reads the
// table (doIndexBufferCulling: index 0xcaa, read once by the cull-pass setup FUN_140a1d2d0;
// r_ForceGpuWaitForPrevFrame: 0xce6, read at 0x14091361f and 0x140913920). InstallPortSettingsOverlay
// appends the switches to the render-config copies and the package command line, after the settings'
// own arguments, so they load after the title's lines and win.
// APS5_ENGINE_SWITCHES: unset, empty or 1 = Defaults (doIndexBufferCulling=false); 0 = none (the title's own values, the old path);
// otherwise a list "name=value,name=value" (',' or ';' between entries, a leading '+' allowed) that
// replaces Defaults, with the entry "defaults" to keep them as well. port/reports/s61-h2-engine-switches.md
// and docs/design/engine-switches.md have the inventory and the measurements.
namespace EngineSwitches {

inline constexpr const char* SwitchName = "APS5_ENGINE_SWITCHES";

struct Switch {
    const char* name;
    const char* value;
};

// doIndexBufferCulling=false (s61 H2, t746-t762, same binary, interleaved) makes the title skip its GPU index-buffer
// cull job (XPR_IndexCullObjectsNormal, gated per frame by FUN_140a186b0): queue-0 dispatches 2,112 -> 420 per
// present, GPU busy 135 -> 108 ms, presents 50.3 -> 58.3 per 10 s, picture identical. It was held back while 4 of 9
// such runs died in the job-worker luma-overwrite crash class; that class is fixed (APS5_FIX_KEPT_REUSE, s61
// R-crash), so it is the default since s61 PERF-DEFAULTS. "doIndexBufferCulling=true" (a list replaces the
// defaults) or 0 brings the cull job back. The async-compute switches (enableAsyncCompute, r_water/fog_/particle/
// GI/r_collision*) gave no FPS gain and stay the title's.
inline constexpr std::array<Switch, 1> Defaults{{{"doIndexBufferCulling", "false"}}};

struct Plan {
    // The title arguments, "+name=value" each.
    std::vector<std::string> args;
    // Refused entries (the rest still apply).
    std::vector<std::string> warnings;
    // The one [settings] line.
    std::string line;
};

inline std::string_view TrimSpace(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

// A convar name: a letter or '_' first, then letters, digits and '_'.
inline bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > 96) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!letter(name.front())) return false;
    for (const char c : name) if (!letter(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

// A value: one word of letters, digits, '_', '.', '+' and '-' (true, false, 0.5, -800, WantsHigh).
inline bool ValidValue(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' || c == '-')) return false;
    }
    return true;
}

inline bool Off(const char* value) { return value != nullptr && std::string_view(value) == "0"; }
inline bool UseDefaults(const char* value) { return value == nullptr || TrimSpace(value).empty() || TrimSpace(value) == "1"; }

// Adds name=value, a later entry for the same name replacing an earlier one.
inline void Add(Plan& plan, std::string_view name, std::string_view value) {
    const auto prefix = "+" + std::string(name) + "=";
    for (auto& arg : plan.args) {
        if (arg.starts_with(prefix)) {
            arg = prefix + std::string(value);
            return;
        }
    }
    plan.args.push_back(prefix + std::string(value));
}

inline Plan PlanSwitches(const char* value) {
    Plan plan;
    if (Off(value)) {
        plan.line = "[settings] engine switches: none (APS5_ENGINE_SWITCHES=0): the title's own convar values";
        return plan;
    }
    const bool defaults = UseDefaults(value);
    if (defaults) {
        for (const auto& entry : Defaults) Add(plan, entry.name, entry.value);
    } else {
        std::string_view text(value);
        while (!text.empty()) {
            const auto end = text.find_first_of(",;");
            auto entry = TrimSpace(text.substr(0, end));
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (entry.empty()) continue;
            if (entry == "defaults") {
                for (const auto& item : Defaults) Add(plan, item.name, item.value);
                continue;
            }
            if (entry.front() == '+') entry.remove_prefix(1);
            const auto separator = entry.find('=');
            const auto name = separator == std::string_view::npos ? std::string_view{} : TrimSpace(entry.substr(0, separator));
            const auto setting = separator == std::string_view::npos ? std::string_view{} : TrimSpace(entry.substr(separator + 1));
            if (!ValidName(name) || !ValidValue(setting)) {
                plan.warnings.push_back("[settings] engine switches: '" + std::string(entry) + "' refused (expected name=value, one word each)");
                continue;
            }
            Add(plan, name, setting);
        }
    }
    std::string joined;
    for (const auto& arg : plan.args) joined += (joined.empty() ? "" : " ") + arg;
    if (plan.args.empty() && defaults) plan.line = "[settings] engine switches: none (no defaults; APS5_ENGINE_SWITCHES=name=value,... sets the title's convars)";
    else if (plan.args.empty()) plan.line = "[settings] engine switches: none set (APS5_ENGINE_SWITCHES lists no valid entry)";
    else plan.line = std::string("[settings] engine switches") + (defaults ? " (defaults)" : "") + ": " + joined + " (APS5_ENGINE_SWITCHES=0 = the title's own values)";
    return plan;
}

}

#endif
