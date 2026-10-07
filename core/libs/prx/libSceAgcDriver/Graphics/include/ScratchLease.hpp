#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SCRATCHLEASE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SCRATCHLEASE_HPP

#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstdlib>
#include <optional>

namespace AgcDriver::Graphics {

// Whether the per-draw scratch objects of the graphics layer (DrawInputScratch, the in-place read
// lists of ShaderResources) are kept with their capacity on the calling thread between draws (the
// default) or built fresh for every use (APS5_NO_GRAPHICS_SCRATCH=1: the old path, one heap
// allocation per vector; the A/B for the [draw] heap line).
inline bool GraphicsScratchEnabled() {
    static const bool enabled = std::getenv("APS5_NO_GRAPHICS_SCRATCH") == nullptr;
    return enabled;
}

// Lends the calling thread its TScratch (one per type, see HostThreadLocal) while no other lease on
// this thread holds it: a nested lease, and every lease while the scratch is disabled, gets a
// private instance on the stack instead, so a holder never sees its vectors reused underneath it.
// TScratch carries `unsigned depth`. Nothing in a scratch outlives its lease by design: the next
// holder resets every member before use.
template<typename TScratch>
class ScratchLease {
public:
    ScratchLease() {
        if (GraphicsScratchEnabled()) {
            auto& shared = HostThreadLocal<TScratch, TScratch>();
            if (shared.depth == 0) scratch = &shared;
        }
        if (scratch == nullptr) scratch = &owned.emplace();
        ++scratch->depth;
    }
    ~ScratchLease() { --scratch->depth; }
    ScratchLease(const ScratchLease&) = delete;
    ScratchLease& operator=(const ScratchLease&) = delete;
    TScratch& operator*() const { return *scratch; }
    TScratch* operator->() const { return scratch; }

private:
    TScratch* scratch = nullptr;
    std::optional<TScratch> owned;
};

}

#endif
