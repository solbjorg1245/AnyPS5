#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANEEDGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LANEEDGES_HPP

#include "prx/libSceAgcDriver/Graphics/include/LaneCensus.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>

namespace AgcDriver::Graphics {

// APS5_ASYNC_COMPUTE (read once; default 0, off; needs APS5_COMPUTE_QUEUE=1 and its second queue;
// docs/design/fewer-submits.md S4). 1: each lane signals a timeline of its own (LaneHistory turns
// "every batch up to serial s completed" into both lanes' values), and the edges each batch would
// need without the serial chain are computed and counted (LaneEdges) while the S3 chain still
// orders every batch, so the picture cannot change. 2: the edges replace the chain (stage S4b, not
// built yet: runs as 1). With APS5_IMAGE_EDGES=1 (ImageEdgesAsked) they are counted per image too.
inline int AsyncComputeLevel() {
    static const int level = [] {
        const char* value = std::getenv("APS5_ASYNC_COMPUTE");
        if (value == nullptr) return 0;
        if (std::strcmp(value, "1") == 0) return 1;
        return std::strcmp(value, "2") == 0 ? 2 : 0;
    }();
    return level;
}

// APS5_IMAGE_EDGES=1 (read once; default off; on with APS5_ASYNC_COMPUTE=1 only, so with its second
// queue and timeline too; S4c): the commands report the images they read and write (NoteAccess, as
// for APS5_BARRIER_VALIDATE, and Recorder::NoteImageEdge for what those notes leave out), and
// LaneEdges counts beside the coarse wait a fine one: S4a's sources without the image site rule,
// plus the images' RAW, WAW and WAR. Count only: nothing waits for it.
inline bool ImageEdgesAsked() {
    static const bool asked = [] {
        const char* value = std::getenv("APS5_IMAGE_EDGES");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return asked;
}

constexpr std::size_t LaneIndex(Lane lane) { return static_cast<std::size_t>(lane); }
constexpr Lane OtherLane(Lane lane) { return lane == Lane::C ? Lane::G : Lane::C; }

// A timeline per lane: a lane's batches signal it with their serials, in serial order, so a value
// v on it means every batch of that lane up to v completed (a signal covers every earlier command
// of its queue). "Every batch up to s" is then s's own lane at s and the other lane at its newest
// serial below s (0: nothing to wait for). Kept for the newest Size serials (the recorder keeps it
// under its completed-ring mutex); an older serial gets min(s, the lane's newest serial) on both
// lanes: the first batch of a lane at or after s covers s there, so such a wait is longer, never
// short. Callers pass submitted serials.
class LaneHistory {
public:
    static constexpr std::size_t Size = 1024;

    void Submitted(Lane lane, std::uint64_t serial) {
        auto& entry = ring[serial % Size];
        entry.serial = serial;
        entry.lane = lane;
        entry.other = newest[LaneIndex(OtherLane(lane))];
        newest[LaneIndex(lane)] = serial;
    }
    // The value per lane (by LaneIndex) that covers every batch up to `serial`.
    std::array<std::uint64_t, 2> ValuesFor(std::uint64_t serial) const {
        std::array<std::uint64_t, 2> values{};
        if (serial == 0) return values;
        if (const auto& entry = ring[serial % Size]; entry.serial == serial) {
            values[LaneIndex(entry.lane)] = serial;
            values[LaneIndex(OtherLane(entry.lane))] = entry.other;
            return values;
        }
        for (std::size_t lane = 0; lane < values.size(); ++lane) values[lane] = std::min(serial, newest[lane]);
        return values;
    }

private:
    struct Entry {
        std::uint64_t serial = 0;
        std::uint64_t other = 0;
        Lane lane = Lane::G;
    };
    std::array<Entry, Size> ring{};
    std::array<std::uint64_t, 2> newest{};
};

// The waits the batches of the two queues would need without the serial chain (GPU-free; the
// recorder keeps one under GuestMemory::GpuMutex). A batch of lane L needs the other lane's batch S:
// - label: a wait of L took a label value S stores (Recorder::NoteLabelEdge);
// - RAW, WAW, WAR: L reads a range S writes, writes a range S writes, writes a range S reads (the
//   noted ranges in 64 KiB blocks; a range of more than MaxBlocks blocks meets the other lane's
//   newest access of its kind anywhere, and marks every block);
// - producer: S recorded commands whose writes no range notes (copies, uploads and write-backs,
//   unit shadows, copy-backs, key stores, fills, unnamed sites, deferred copies);
// - image: S drew or dispatched (render targets, depth, storage images: a batch of the other lane
//   may sample them with no noted read);
// - shadow: L took a staged shadow the other lane registered (Recorder::NoteShadowTake): the other
//   lane's newest batch at L's submit.
// A need above the other lane's newest submitted batch is clamped to it (a wait never names a value
// nothing was submitted to signal); one on a retired batch (finished, completions run) is dropped.
// Sticky per lane: a wait for value V covers every later batch of the lane (a wait covers every
// later command of its queue), so only a need above the lane's last wait makes a wait.
// APS5_IMAGE_EDGES (EnableImages): a fine wait per batch beside it, by the same rules, from the
// needs above but the image site rule (its producers are Submit's `fineProducer`: a dispatch that
// kept the queued copy-backs is none), and from the images the batches read and write (ImageAccess,
// by handle value): image RAW, WAW, WAR. A handle value an image's successor reuses keeps the old
// marks: such a mark only adds a wait, it never hides one.
class LaneEdges {
public:
    enum class Source : std::uint8_t { Label = 0, Raw, Waw, War, Producer, Image, Shadow, Count };
    static constexpr std::size_t SourceCount = static_cast<std::size_t>(Source::Count);
    // The fine wait's sources: Label to War as Source's, then producer, shadow, the images'.
    enum class FineSource : std::uint8_t { Label = 0, Raw, Waw, War, Producer, Shadow, ImageRaw, ImageWaw, ImageWar, Count };
    static constexpr std::size_t FineSourceCount = static_cast<std::size_t>(FineSource::Count);
    static constexpr unsigned BlockShift = 16;
    static constexpr std::uint64_t MaxBlocks = 1024;
    struct Wait {
        // The other lane's timeline value (0: none), and the sources that asked for more than the
        // lane's last wait (a bit per Source; per FineSource in a fine wait).
        std::uint64_t serial = 0;
        std::uint32_t sources = 0;
    };

    // Batch `serial` of `lane` reads or writes [begin, end).
    void Access(Lane lane, std::uint64_t serial, std::uint64_t begin, std::uint64_t end, bool write) {
        if (end <= begin) return;
        const auto self = LaneIndex(lane);
        const auto other = LaneIndex(OtherLane(lane));
        const auto meet = [&](const Marks& marks) {
            need(lane, marks.write[other], write ? Source::Waw : Source::Raw);
            if (write) need(lane, marks.read[other], Source::War);
        };
        const auto mark = [&](Marks& marks) {
            auto& value = write ? marks.write[self] : marks.read[self];
            value = std::max(value, serial);
        };
        const auto first = begin >> BlockShift;
        const auto last = (end - 1) >> BlockShift;
        if (last - first >= MaxBlocks) {
            meet(anywhere);
            mark(wide);
        } else {
            meet(wide);
            for (auto block = first; block <= last; ++block) {
                auto& marks = blocks[block];
                meet(marks);
                mark(marks);
            }
        }
        mark(anywhere);
    }
    // An edge of `source` from lane `lane`'s next batch to the other lane's batch `serial`, or to
    // the other lane's newest submitted batch.
    void Need(Lane lane, std::uint64_t serial, Source source) { need(lane, serial, source); }
    void NeedNewest(Lane lane, Source source) { need(lane, submitted[LaneIndex(OtherLane(lane))], source); }
    // APS5_IMAGE_EDGES: keeps the fine needs and the image table too.
    void EnableImages() { fine = true; }
    // Batch `serial` of `lane` reads or writes image `image` (its handle value; EnableImages first).
    void ImageAccess(Lane lane, std::uint64_t serial, std::uint64_t image, bool write) {
        if (!fine) return;
        const auto self = LaneIndex(lane);
        const auto other = LaneIndex(OtherLane(lane));
        auto& marks = images[image];
        needFine(lane, marks.write[other], write ? FineSource::ImageWaw : FineSource::ImageRaw);
        if (write) needFine(lane, marks.read[other], FineSource::ImageWar);
        auto& value = write ? marks.write[self] : marks.read[self];
        value = std::max(value, serial);
    }
    // Batch `serial` of `lane` is submitted (`producer`, `image`: it recorded such commands;
    // `fineProducer`: producing ones by the fine rule): its wait, then the needs it leaves the other
    // lane's next batch. `fineWait`, when given, receives the fine wait (EnableImages).
    Wait Submit(Lane lane, std::uint64_t serial, bool producer, bool image, bool fineProducer = false, Wait* fineWait = nullptr) {
        const auto self = LaneIndex(lane);
        const auto wait = take(needs[self], waited[self]);
        if (fine) {
            const auto fineTaken = take(fineNeeds[self], fineWaited[self]);
            if (fineWait != nullptr) *fineWait = fineTaken;
        }
        submitted[self] = std::max(submitted[self], serial);
        if (producer) need(OtherLane(lane), serial, Source::Producer);
        if (image) need(OtherLane(lane), serial, Source::Image);
        if (fineProducer) needFine(OtherLane(lane), serial, FineSource::Producer);
        return wait;
    }
    // Every batch up to `serial` retired. The block and image tables drop the entries only retired
    // batches marked once they doubled since their last prune.
    void Retired(std::uint64_t serial) {
        retired = std::max(retired, serial);
        prune(blocks, pruneAt, PruneMinimum);
        prune(images, imagePruneAt, ImagePruneMinimum);
    }
    std::size_t Blocks() const { return blocks.size(); }
    std::size_t Images() const { return images.size(); }

private:
    // The newest batch per lane (by LaneIndex) that wrote, and that read, a block (an image).
    struct Marks {
        std::array<std::uint64_t, 2> write{};
        std::array<std::uint64_t, 2> read{};
    };
    static constexpr std::size_t PruneMinimum = std::size_t{1} << 16;
    static constexpr std::size_t ImagePruneMinimum = std::size_t{1} << 12;

    // A lane's needs above its last wait (`last`) and past the retired batches make its wait, which
    // becomes the last; every need is taken.
    template <std::size_t Count>
    Wait take(std::array<std::uint64_t, Count>& lane, std::uint64_t& last) {
        Wait wait;
        for (std::size_t source = 0; source < Count; ++source) {
            if (lane[source] > last && lane[source] > retired) {
                wait.serial = std::max(wait.serial, lane[source]);
                wait.sources |= 1u << source;
            }
            lane[source] = 0;
        }
        if (wait.serial != 0) last = wait.serial;
        return wait;
    }
    void need(Lane lane, std::uint64_t serial, Source source) {
        serial = std::min(serial, submitted[LaneIndex(OtherLane(lane))]);
        if (serial == 0 || serial <= retired) return;
        auto& value = needs[LaneIndex(lane)][static_cast<std::size_t>(source)];
        value = std::max(value, serial);
        // The fine wait takes every source but the site rules (its producers come from Submit).
        if (fine && source != Source::Producer && source != Source::Image) needFine(lane, serial, source == Source::Shadow ? FineSource::Shadow : static_cast<FineSource>(source));
    }
    void needFine(Lane lane, std::uint64_t serial, FineSource source) {
        serial = std::min(serial, submitted[LaneIndex(OtherLane(lane))]);
        if (!fine || serial == 0 || serial <= retired) return;
        auto& value = fineNeeds[LaneIndex(lane)][static_cast<std::size_t>(source)];
        value = std::max(value, serial);
    }
    // Drops the entries only retired batches marked, once `table` holds `at` of them.
    void prune(std::unordered_map<std::uint64_t, Marks>& table, std::size_t& at, std::size_t minimum) {
        if (table.size() < at) return;
        for (auto it = table.begin(); it != table.end();) {
            const auto& marks = it->second;
            const bool live = std::max({marks.write[0], marks.write[1], marks.read[0], marks.read[1]}) > retired;
            it = live ? std::next(it) : table.erase(it);
        }
        at = std::max(minimum, table.size() * 2);
    }

    std::unordered_map<std::uint64_t, Marks> blocks;
    // APS5_IMAGE_EDGES: the marks by image handle value (and dispatch template address).
    std::unordered_map<std::uint64_t, Marks> images;
    // Accesses over more than MaxBlocks blocks (they mark every block), and the newest access of
    // each kind per lane anywhere (what such an access meets).
    Marks wide;
    Marks anywhere;
    std::array<std::array<std::uint64_t, SourceCount>, 2> needs{};
    std::array<std::uint64_t, 2> waited{};
    // APS5_IMAGE_EDGES: the fine needs and the last fine wait per lane.
    std::array<std::array<std::uint64_t, FineSourceCount>, 2> fineNeeds{};
    std::array<std::uint64_t, 2> fineWaited{};
    std::array<std::uint64_t, 2> submitted{};
    std::uint64_t retired = 0;
    std::size_t pruneAt = PruneMinimum;
    std::size_t imagePruneAt = ImagePruneMinimum;
    bool fine = false;
};

}

#endif
