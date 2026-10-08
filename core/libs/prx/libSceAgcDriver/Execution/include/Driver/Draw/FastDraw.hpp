#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTDRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_FASTDRAW_HPP

#include "Recompiler.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver::DriverDetail {

// F3b of the draw fast path (docs/design/draw-fastpath.md 2.1-2.8, 2.11), the driver half
// (FastDraw.cpp, Driver::fastDraw): APS5_FAST_DRAW=1 takes a VS+PS (or VS-only) draw on the vertex
// path, direct or GPU-side indirect, through the state memo (F1), the live walk of every stage
// (F2), the variant the walk's specialization selects (PopulateVariant) and Graphics::DrawFast,
// under the GPU mutex after the packet's labels. A decline (Graphics::FastDecline) leaves the draw
// to Driver::draw's path unchanged. APS5_FAST_DRAW_INDIRECT=0 declines indirect draws. Counted
// on the [fastpath] draws line every 10 s under APS5_PROFILE_DRAW. Default off.
bool FastDrawEnabled();
bool FastDrawIndirect();

// APS5_FAST_DRAW_VERIFY=N: every Nth draw the fast path would take is drawn by the old path
// instead and compared: its stages' results with the fast path's (variant identity, bindings word
// by word, push constants, vertex attributes) here, and Graphics::Draw's freshly built set with the
// fast bindings of the same stages (buffer and offset, data words, texture view, sampler; Graphics
// VerifyFastBindings). Counted on the [fastpath] draw verify line; the first differences printed.
// 0 (unset): off.
std::uint32_t FastDrawVerifyEvery();
// Whether this thread's draw holds a comparison armed by the fast path.
bool FastDrawVerifyPending();
// Compares the stages the old path bound (`old`, the draw's program results) with the armed fast
// results; `heuristic`: the old path bound stored results by a heuristic (counted apart).
void VerifyFastDrawStages(std::span<const ShaderRecompiler::RecompileResult* const> old, bool heuristic);
// Ends the draw's comparison (whatever did not run is counted as not compared): Driver::draw's exit.
struct FastDrawVerifyScope {
    FastDrawVerifyScope() = default;
    FastDrawVerifyScope(const FastDrawVerifyScope&) = delete;
    FastDrawVerifyScope& operator=(const FastDrawVerifyScope&) = delete;
    ~FastDrawVerifyScope();
};

}

#endif
