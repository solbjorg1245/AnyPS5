#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEREFRESHFIELD_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLEREFRESHFIELD_HPP

#include <cmath>
#include <cstdint>
#include <optional>
#include "prx/libSceVideoOut/include/TitleRefreshTable.hpp"

// APS5_TIMESTEP_FIELD (TimestepPatch): what one flip does with the title's refresh field (CRenderDisplay+0x28,
// the float FUN_140b380e0 divides the frame step by), given the TargetFramesPerSecond enum at +0x30 and the
// refresh table's 60 and 30 FPS entries. Enum 1 (60) and 0 (Unlocked: the constant 59.94, one flip per vblank
// too) take the 60 FPS entry, 2 (30) the 30 FPS entry; 3 (20) and anything else are left alone. Until the
// field is known it must hold one of the title's refreshes (59.94, 29.97, 19.98, within 0.01; also 119.88 when the
// table is patch_game.py --fps120's, APS5_TABLE_COMPAT) with the enum 0-3, else the layout is not the expected one
// (refused: never written); once known, another value is the title's own write at a mode change (FUN_14091b3a0
// copies the table's entry), replaced like any other.
namespace TitleRefreshField {

enum class Action { Write, AlreadySet, OtherMode, Refuse };

struct Step {
    Action action = Action::OtherMode;
    // The entry of the enum's mode (Write: the value to store).
    float value = 0.0f;
    // The field held another value than the known one: the title rewrote it at a mode change.
    bool titleRewrote = false;
};

inline bool TitleRefresh(float value, bool patched = false) {
    return std::fabs(value - 59.94f) <= 0.01f || std::fabs(value - 29.97f) <= 0.01f || std::fabs(value - 19.98f) <= 0.01f
        || (patched && std::fabs(value - TitleRefreshTable::PatchedRefresh) <= 0.01f);
}

// `known`: the field's value after the last decision (ours after a write, else the value seen); none before
// the first. `patched`: the refresh table is in patch_game.py --fps120's form.
inline Step Decide(std::int32_t mode, float current, std::optional<float> known, float rate60, float rate30, bool patched = false) {
    if (!known && (mode < 0 || mode > 3 || !TitleRefresh(current, patched))) return {Action::Refuse, 0.0f, false};
    Step step;
    step.titleRewrote = known && *known != current;
    if (mode < 0 || mode > 2) return step;
    step.value = mode == 2 ? rate30 : rate60;
    step.action = step.value == current ? Action::AlreadySet : Action::Write;
    return step;
}

}

#endif
