#include "prx/libc/include/GraphicsModePin.hpp"
#include "prx/libc/include/General.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <system_error>

void InstallGraphicsModePin(const std::function<std::filesystem::path(const char*)>& resolve) {
    const char* value = std::getenv(GraphicsModePin::SwitchName);
    try {
        std::filesystem::path performance;
        bool found = false;
        if (GraphicsModePin::Enabled(value)) {
            performance = resolve(GraphicsModePin::PerformanceGuest);
            std::error_code error;
            found = std::filesystem::is_regular_file(performance, error);
        }
        const auto plan = GraphicsModePin::PlanPin(value, performance, found);
        if (plan.alias) AddPathAliasHost_nid_no_patch(GraphicsModePin::CinematicGuest, plan.target);
        std::fprintf(stderr, "%s\n", plan.line.c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[settings] graphics mode: not pinned (%s): the title's Visual Mode is left live\n", error.what());
    }
    std::fflush(stderr);
}
