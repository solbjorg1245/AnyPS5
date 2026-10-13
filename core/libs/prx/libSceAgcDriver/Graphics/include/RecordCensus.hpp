#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDCENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDCENSUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace AgcDriver::Graphics::RecordCensus {

// APS5_RECORD_CENSUS=1 (P-HANDOFF part 2, multi-recorder stage S0; default off, no behaviour change):
// which shared recording structures each draw the recording pipe's thread B records mutates (the 17
// that port/reports/s61-p-scale.md section 1.4 found guarded only by the GPU mutex; the template's
// proof and its write notes apart), and the template (ShaderResources) the draw recorded with. Touch
// sets the structure's bit in the calling thread's mask; B clears it before an item and reads it after
// (RecordPipe.cpp, the [pipe] census line). Nothing but a flag test while the switch is off.
enum Structure : std::size_t {
    OpenBatch,
    CommandPool,
    RecorderLists,
    ActiveRecorder,
    PendingWrites,
    PendingPublish,
    FlushHook,
    LabelTable,
    TransientPool,
    TemplateProof,
    TemplateWrites,
    TemplateShadows,
    TargetState,
    RefreshMemo,
    Framebuffers,
    DccProofs,
    UiProbe,
    RenderPass,
    StructureCount
};

inline constexpr std::array<const char*, StructureCount> StructureNames{"open batch", "command pool", "recorder lists", "active recorder", "pending writes", "pending publish", "flush hook", "label table", "transient pool", "template proof", "template write notes", "template shadows", "target state", "refresh memo", "framebuffers", "dcc proofs", "ui probe", "render pass"};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_RECORD_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct ThreadState {
    std::uint32_t mask = 0;
    const void* templateId = nullptr;
};
static_assert(StructureCount <= 32);

inline ThreadState& Thread() {
    static thread_local ThreadState state;
    return state;
}

inline void Touch(Structure structure) {
    if (Enabled()) Thread().mask |= std::uint32_t{1} << structure;
}

inline void NoteTemplate(const void* id) {
    if (Enabled()) Thread().templateId = id;
}

}

#endif
