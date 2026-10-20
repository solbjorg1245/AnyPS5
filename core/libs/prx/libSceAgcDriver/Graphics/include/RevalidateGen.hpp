#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_REVALIDATEGEN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_REVALIDATEGEN_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics {

// APS5_REVALIDATE_GEN (P-REVALIDATE; default off, the old proof as fallback): the generation gate in
// front of a template's fast proof (ShaderResources::Revalidate). The proof collects each surface
// (a walk, or the collect memo's answer inside the thread's epoch), then asks the pending registry,
// the depth registry, the stamps and the storage cache. A proof on the same thread in the same
// collect epoch would get every collect from the memo (no walk, so no CPU store it did not see
// before), and while the serials below are unchanged its other answers are the last proof's too:
// - epoch: GuestMemory::ThreadCollectEpoch (0: the thread has none; every collect walks);
// - collectMemo: GuestMemory::CollectMemoSerial (an unwatch or a mapping ends the memo's entries);
// - stamps: GuestMemory::StampSerial (no block stamped anywhere: every UnchangedSince holds);
// - pending: StorageTexture::PendingSerial, the template's pendingSerialSeen (0: none);
// - depth: DepthSurfaceSerial, the template's depthSerialSeen.
// The state a template kept at its last plain fast proof against the state now.
struct RevalidateGenState {
    std::uint64_t epoch = 0;
    std::uint64_t collectMemo = 0;
    std::uint64_t stamps = 0;
    std::uint64_t pending = 0;
    std::uint64_t depth = 0;
};

// Skip: nothing moved, the proof is the last one's. SkipCheckStamps: only stamps moved somewhere, so
// the template's own ranges are asked (UnchangedSinceAll) before the skip. The rest name the first
// input that moved, and the proof runs as before.
enum class RevalidateGenStep : std::uint8_t { Skip, SkipCheckStamps, NoMemo, Epoch, Mapping, Pending, Depth, Count };

inline constexpr const char* RevalidateGenStepNames[] = {"skip", "skip after stamps", "no memo", "epoch", "mapping", "pending", "depth"};

constexpr RevalidateGenStep RevalidateGenFor(const RevalidateGenState& kept, const RevalidateGenState& now) {
    if (kept.epoch == 0) return RevalidateGenStep::NoMemo;
    if (now.epoch == 0 || now.epoch != kept.epoch) return RevalidateGenStep::Epoch;
    if (now.collectMemo != kept.collectMemo) return RevalidateGenStep::Mapping;
    if (kept.pending == 0 || now.pending != kept.pending) return RevalidateGenStep::Pending;
    if (now.depth != kept.depth) return RevalidateGenStep::Depth;
    return now.stamps == kept.stamps ? RevalidateGenStep::Skip : RevalidateGenStep::SkipCheckStamps;
}

// Why a skip the serials allowed still ran the proof (the per-element checks of the skip), or none.
enum class RevalidateGenMiss : std::uint8_t { None, Records, Stamps, DepthSurface, Evicted, Keys, Count };

inline constexpr const char* RevalidateGenMissNames[] = {"none", "records", "stamps", "depth surface", "evicted", "keys"};

}

#endif
