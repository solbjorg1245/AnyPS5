#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_NATIVEASPECTPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_NATIVEASPECTPATCH_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include "prx/libc/include/PortSettings.hpp"

// Native aspect for Demon's Souls (PPSA01341, NativeAspect = on / APS5_NATIVE_ASPECT=1 with Resolution).
// The title sizes every view's render targets from its tier table (render and display width/height per
// EWantsDisplayMode, FUN_14091ab90) and divides every camera projection by one aspect field of its
// CRenderDisplay (16:9 from CRenderDisplay::Init; the projection's FOV is vertical). On the first
// sceVideoOutOpen (CRenderDisplay::Init opens video out after it set the singleton and the aspect, before
// any view reads the table) the slot's render width and height are rewritten with the planned size
// (PortSettings::PlanNativeAspect; the overlay forces r_Wants4K to that slot). On every flip the aspect
// field is set to Resolution's aspect while the display runs the slot's mode (16:9 in any other mode)
// and the presenter is told to show the 16:9 display buffer at that aspect (it then holds the wider
// image squeezed). Only for the build the layout is known for (PortSettings::SupportedBuildVersion),
// with the table's render half found exactly once in read-only data and the singleton slot inside a
// writable section; the aspect field is only written while it holds 16:9 or the planned value.
// Without any of that nothing is written and the game renders its 16:9 tier, letterboxed as before.
// Part 2 (PortSettings::PlanNativeTargets), also at that first sceVideoOutOpen: CRenderDisplay::SetMode
// loads the slot's shared render size and display size from 8-byte constants the code shares with other
// modes, so its two loads are pointed at a pair of sizes in a page allocated within reach; Init's
// display buffer sizes and the display-size getters are immediates rewritten in place. Every byte is
// checked against the build first; a half whose bytes differ writes nothing and stays 16:9 (part 1).
// Plain Resolution up to 1080p (PortSettings::PlanResolution4kUi, r6) uses the same table rewrite to run
// its tier's render size in the WantsHigh slot (3840x2160 display buffer); the aspect field and the
// presentation are left alone there.
// APS5_PROFILE_DRAW=1 appends the state to the 10 s [timestep] line.
class NativeAspectPatch {
public:
    static NativeAspectPatch& Get();

    // sceVideoOutOpen: rewrites the table once.
    void Open();
    // One flip of the title: keeps the aspect field and the presentation aspect in step with the mode.
    void Flip();
    // ", native aspect ..." for the 10 s digest; empty when off.
    std::string Digest();

private:
    NativeAspectPatch();
    void locate();
    void follow();
    void patchTargets(std::intptr_t delta, bool& display);

    std::mutex mutex;
    PortSettings::NativeAspectPlan plan;
    bool searched = false;
    std::uint32_t* table = nullptr;
    unsigned char* singleton = nullptr;
    std::uint32_t wideBits = 0;
    std::int32_t mode = -1;
    bool presenting = false;
    bool fieldRefused = false;
    bool sharedApplied = false;
    bool displayApplied = false;
    std::uint64_t windowFlips = 0;
    std::uint64_t windowWideFlips = 0;
    std::uint64_t fieldWrites = 0;
};

#endif
