#include "prx/libc/include/EngineSwitches.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "engine_switches_tests: %s\n", what);
    std::abort();
}

static std::string Joined(const EngineSwitches::Plan& plan) {
    std::string joined;
    for (const auto& arg : plan.args) joined += (joined.empty() ? "" : " ") + arg;
    return joined;
}

static std::string DefaultArgs() {
    std::string joined;
    for (const auto& entry : EngineSwitches::Defaults) joined += (joined.empty() ? "" : " ") + std::string("+") + entry.name + "=" + entry.value;
    return joined;
}

// Unset, empty and 1 = the defaults; 0 = none (the old path).
static void TestSwitch() {
    for (const char* value : {static_cast<const char*>(nullptr), "", " ", "1"}) {
        const auto plan = EngineSwitches::PlanSwitches(value);
        Require(Joined(plan) == DefaultArgs() && plan.warnings.empty(), "defaults");
        Require(plan.line.find("(defaults)") != std::string::npos || plan.args.empty(), "defaults line");
    }
    const auto off = EngineSwitches::PlanSwitches("0");
    Require(off.args.empty() && off.warnings.empty() && off.line.find("APS5_ENGINE_SWITCHES=0") != std::string::npos, "=0 is the old path");
}

// A list replaces the defaults; '+', spaces, ',' and ';' are accepted, a repeated name keeps the last value.
static void TestList() {
    const auto plan = EngineSwitches::PlanSwitches(" +doIndexBufferCulling=false, r_waterEnableAsyncCompute = 0;enableAsyncCompute=false,,doIndexBufferCulling=true ");
    Require(plan.warnings.empty(), "valid list");
    Require(Joined(plan) == "+doIndexBufferCulling=true +r_waterEnableAsyncCompute=0 +enableAsyncCompute=false", "list order and last value wins");
    Require(plan.line.find("(defaults)") == std::string::npos && plan.line.find("+enableAsyncCompute=false") != std::string::npos, "list line");
    const auto values = EngineSwitches::PlanSwitches("r_SkirtDepthBias=-800,r_motionBlurAmount=0.5,r_Wants4K=WantsHigh");
    Require(Joined(values) == "+r_SkirtDepthBias=-800 +r_motionBlurAmount=0.5 +r_Wants4K=WantsHigh", "numbers and words");
    const auto withDefaults = EngineSwitches::PlanSwitches("defaults,r_tessellationEnableAsyncCompute=true");
    Require(Joined(withDefaults) == (DefaultArgs().empty() ? std::string() : DefaultArgs() + " ") + "+r_tessellationEnableAsyncCompute=true", "defaults entry");
}

// The shipped default (s61 PERF-DEFAULTS): unset = doIndexBufferCulling=false and the line shows it; 0 = nothing;
// an explicit list replaces it (doIndexBufferCulling=true turns the cull job back on, another name drops it); the
// "defaults" entry keeps it, and a later entry for the same name wins either way.
static void TestDefaultPrecedence() {
    Require(EngineSwitches::Defaults.size() == 1 && std::string(EngineSwitches::Defaults[0].name) == "doIndexBufferCulling" && std::string(EngineSwitches::Defaults[0].value) == "false", "the cull default");
    const auto unset = EngineSwitches::PlanSwitches(nullptr);
    Require(Joined(unset) == "+doIndexBufferCulling=false" && unset.line.find("(defaults): +doIndexBufferCulling=false") != std::string::npos, "unset applies and shows the default");
    Require(EngineSwitches::PlanSwitches("0").args.empty(), "=0 drops the default");
    const auto on = EngineSwitches::PlanSwitches("doIndexBufferCulling=true");
    Require(Joined(on) == "+doIndexBufferCulling=true" && on.line.find("(defaults)") == std::string::npos && on.line.find("+doIndexBufferCulling=true") != std::string::npos, "an override turns culling back on");
    Require(Joined(EngineSwitches::PlanSwitches("r_dofEnable=false")) == "+r_dofEnable=false", "a list replaces the default");
    Require(Joined(EngineSwitches::PlanSwitches("defaults,doIndexBufferCulling=true")) == "+doIndexBufferCulling=true", "an entry after defaults wins");
    Require(Joined(EngineSwitches::PlanSwitches("doIndexBufferCulling=true,defaults")) == "+doIndexBufferCulling=false", "defaults after an entry wins");
    Require(Joined(EngineSwitches::PlanSwitches("defaults,r_dofEnable=false")) == "+doIndexBufferCulling=false +r_dofEnable=false", "defaults plus another switch");
}

// Bad entries are refused one by one; the rest apply.
static void TestRefused() {
    const auto plan = EngineSwitches::PlanSwitches("noValue,=1,9name=1,a b=1,name=two words,name=x\"y,good=1,dangling=");
    Require(Joined(plan) == "+good=1", "only the valid entry");
    Require(plan.warnings.size() == 7, "one warning per refused entry");
    Require(plan.warnings[0].find("'noValue' refused") != std::string::npos, "warning names the entry");
    const auto none = EngineSwitches::PlanSwitches("bad");
    Require(none.args.empty() && none.line.find("no valid entry") != std::string::npos, "nothing valid");
    Require(EngineSwitches::ValidName("r_ForceGpuWaitForPrevFrame") && EngineSwitches::ValidName("_x1") && !EngineSwitches::ValidName("1x") && !EngineSwitches::ValidName(""), "names");
    Require(EngineSwitches::ValidValue("false") && EngineSwitches::ValidValue("-2.5e+3") && !EngineSwitches::ValidValue("") && !EngineSwitches::ValidValue("a=b"), "values");
}

int main() {
    TestSwitch();
    TestList();
    TestDefaultPrecedence();
    TestRefused();
    std::printf("engine_switches_tests: ok\n");
    return 0;
}
