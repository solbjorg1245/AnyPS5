#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP

#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct SourceHandle;
}

namespace AgcDriver {

// Records the guest words a shader's resource analysis reads, as the memory regions its recompile
// request carries. Guest memory is fetched a 4 KiB page at a time; the regions report exactly the
// dwords read, so cache keys built from them do not change with unrelated bytes nearby. A page that
// `pendingWrite` reports as written by recorded GPU work is read word by word instead, so only a
// dword the capture needs waits for that work through the flush hook, not the whole page; a dword
// the query knows to be left unchanged by that work (Driver.cpp's written-element evidence) is read
// raw, without the hook. Debug aid: APS5_NO_WORDWISE_CAPTURE=1 fetches every page whole.
class ShaderMemory {
public:
    // The driver's answer for a range: nothing recorded writes it; recorded work writes it and the
    // read must go through the flush hook; recorded work writes it but leaves it unchanged, so the
    // bytes are read raw; VerifyRaw reads raw and through the hook, counting a difference;
    // KnownValue: recorded work writes it with bytes the driver already knows (a copy HLE's
    // destination, Driver.cpp's known-value ring entries), copied into `known` when the caller
    // passes a span of exactly `bytes` (empty: the answer alone); VerifyKnownValue serves them and
    // reads through the hook too, counting a difference. RawExpected: recorded work writes it but
    // every observation left it unchanged; `known` receives the value the evidence saw last, the
    // bytes are read raw while they still hold it, and another value is read through the hook
    // (waiting for the writer) and reported to the observer as changed, which ends the evidence.
    enum class PendingWrite : std::uint8_t { None, Sync, Raw, VerifyRaw, KnownValue, VerifyKnownValue, RawExpected };
    using PendingWriteQuery = PendingWrite (*)(std::uint64_t address, std::size_t bytes, std::span<std::byte> known);
    // Words served from known values by every capture, and the verified ones and their mismatches
    // (APS5_VERIFY_KNOWN_VALUES=1), for Driver.cpp's [copy] line.
    struct KnownValueCounts {
        std::uint64_t served, verified, mismatches;
    };
    static KnownValueCounts KnownValues();
    // Told, for a dword read that went through the hook, whether the bytes read before the hook's
    // wait were still there after it (the driver's evidence for later raw reads), and the value read.
    using PendingWriteObserver = void (*)(std::uint64_t address, bool unchanged, std::uint32_t value);
    // The calling thread's count of hook syncs that waited for unfinished GPU work: a read is only
    // observed when the count moved across it (against finished work both reads see the GPU's
    // bytes, and "unchanged" would be no evidence). Null observes every read that went through
    // the hook.
    using HookWaitCounter = std::uint64_t (*)();
    // The calling thread's GPU waits so far in milliseconds (Recorder::ThreadWaitedMs), set once
    // by the driver: the [capture] line charges the waits made inside a capture to it.
    using WaitedMsProvider = double (*)();
    static void SetWaitedMsProvider(WaitedMsProvider provider);
    // The driver's description of the GPU work that last wrote a range (Driver::describeWriters),
    // set once; the [capture-stalls] line names the writers of the words captures waited for.
    using WriterDescriber = std::string (*)(std::uint64_t address, std::size_t bytes);
    static void SetWriterDescriber(WriterDescriber describer);
    // Whether recorded GPU work that writes the range has not run yet (its batch is open or in
    // flight with the fence unsignaled): a read through the hook would wait. Set once by the
    // driver; deferPureLeaf defers only such words (a finished writer's word is read cheaply).
    using PendingUnsignaledQuery = bool (*)(std::uint64_t address, std::size_t bytes);
    static void SetPendingUnsignaledQuery(PendingUnsignaledQuery query);
    // Deferred flat slots (see deferPureLeaf in ShaderMemory.cpp): on unless APS5_NO_DEFERRED_FLAT=1.
    static bool DeferredFlatEnabled();
    explicit ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial, PendingWriteQuery pendingWrite = nullptr, PendingWriteObserver observe = nullptr, HookWaitCounter hookWaits = nullptr);
    // Returns what the capture resolved (plan, snapshot, specialization) for
    // ShaderRecompiler::Recompile(request, capture), which then skips its own materialization. One
    // result per call: the draw path captures several stages on one ShaderMemory. With `handle`
    // (the driver's memoized ShaderRecompiler::ResolveSource result) the capture skips the source
    // resolution.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle = nullptr);
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> Regions() const;
    // The page regions read since the previous call (or construction), a word read again
    // included, the initial regions excluded: one stage's own reads on the draw path's shared
    // ShaderMemory (Regions() stays the union). The spans point into the pages, as Regions()'s do.
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> TakeRecentRegions();
    // The driver's source handle memo outcomes, for the [capture] line (APS5_PROFILE_DRAW).
    static void CountHandleMemo(bool hit);
    // The guest address the calling thread's recent capture reads (the last 4096 dwords) found
    // `words` at, consecutively (0: not among them), for reports on a descriptor decoded as garbage;
    // `chain` (optional) receives the reads made just before it as "address=value" pairs.
    static std::uint64_t LocateRecentWords(std::span<const std::uint32_t> words, std::string* chain = nullptr);

private:
    static constexpr std::size_t PageBytes = 4096;
    static constexpr std::size_t PageWords = PageBytes / sizeof(std::uint32_t);

    struct Page {
        std::array<std::uint32_t, PageWords> words{};
        std::bitset<PageWords> valid;
        std::bitset<PageWords> read;
        std::bitset<PageWords> recent;
        bool wordwise = false;
    };

    static bool read(void* context, std::uint64_t address, std::uint32_t* value);
    // The express walk's reader (SrtRuntime::expressRead): a page not fetched yet is copied whole
    // when nothing recorded writes it and it is mapped whole, otherwise the read is declined and
    // the capture falls back to `read`; the words read are recorded in the pages like `read`'s.
    static bool expressRead(void* context, std::uint64_t address, std::uint32_t* value);
    // SrtRuntime::deferPureLeaf: claims a pure flat slot's word on a page recorded GPU work writes
    // (placeholder 0, the address kept in `deferred` for the recompiler) instead of waiting for it.
    static bool deferPureLeaf(void* context, std::uint64_t address, std::uint32_t* value);
    Page& page(std::uint64_t base);

    // Regions given at construction (the registered shader's code and header), referenced as given:
    // the caller keeps them alive for as long as the capture is used.
    std::map<std::uint64_t, std::span<const std::byte>> initial;
    std::map<std::uint64_t, Page> pages;
    // The page the express reader served last (map nodes are stable), reset per capture.
    Page* expressPage = nullptr;
    std::uint64_t expressBase = 0;
    PendingWriteQuery pendingWrite = nullptr;
    PendingWriteObserver observe = nullptr;
    HookWaitCounter hookWaits = nullptr;
    // The current capture's deferred flat words (deferPureLeaf), handed to the recompiler through
    // SrtRuntime::deferredReads.
    std::vector<std::uint64_t> deferred;
};

// The positions, among a dispatch-cache variant's stored words, of the pure flat-SRT leaves a
// capture read (ShaderRecompiler::SrtReadTrace), for a hit that differs only there. `runs` are the
// variant's [begin, end) byte ranges in address order and `words` their dwords in that order;
// `leaves` are (flat offset, address) and `otherReads` the sorted addresses of every other read.
// A leaf is skipped when its address is among the other reads (a walk read aliases the dword,
// counted `aliased`), is not dword-aligned or lies in no run (counted `unmapped`), or its stored
// word differs from the flattened SRT's at that offset (counted `mismatched`). `positions`
// (sorted, may repeat) and `slots` (the flat offset of each) are parallel.
struct DataWordPositionCounts {
    std::uint64_t unmapped = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t aliased = 0;
};
DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots);

// Don't-care bits of a variant's stored words: the texture-streaming feedback fields of each
// sampled-image T# (an element of a GuestImages / SampledImage binding) located among the words,
// its eight words consecutive in address (one run, or adjacent runs across a page). Word 5 bit 25
// is the mip-stats counter enable and word 6 bits 0-7 the counter id (GuestTextureResource.cpp
// decodes both and reports nothing back); the title toggles them per frame. `ignored` receives
// (position, mask) sorted by position, duplicates merged; a T# with a word at a data position is
// left out. Returns how many T#s were located. A null T# (words 0-1 zero) is skipped.
inline constexpr std::uint32_t TsharpWord5IgnoredBits = 1u << 25u;
inline constexpr std::uint32_t TsharpWord6IgnoredBits = 0xffu;
std::size_t IgnoredWordBits(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const ShaderRecompiler::DescriptorBinding> bindings, std::span<const std::uint32_t> dataPositions, std::vector<std::pair<std::uint32_t, std::uint32_t>>& ignored);
// The mask of `position` in a sorted (position, mask) list, 0 when absent.
inline std::uint32_t IgnoredMaskAt(std::span<const std::pair<std::uint32_t, std::uint32_t>> ignored, std::uint32_t position) {
    const auto it = std::lower_bound(ignored.begin(), ignored.end(), position, [](const std::pair<std::uint32_t, std::uint32_t>& entry, std::uint32_t value) { return entry.first < value; });
    return it != ignored.end() && it->first == position ? it->second : 0u;
}
// Whether two word sequences are equal apart from the masked bits (sizes must match).
bool WordsEqualIgnoring(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, std::span<const std::pair<std::uint32_t, std::uint32_t>> ignored);
// Whether two guest descriptors of `binding` are equal apart from the T# don't-care bits of a
// sampled-image binding's elements (any other binding: exactly).
bool SameDescriptorIgnoringTsharpBits(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> left, std::span<const std::uint32_t> right);

// Buffer base slots of a variant's stored words: for each read-only guest-buffer V# (a 4-word
// element of a GuestBuffers binding the shader proves it never stores to) whose base is located
// once among the words (word 0 and the low 16 bits of word 1, two words consecutive in address
// that the walk read: the first one's address among `walkReads`, sorted; none located when
// empty), the positions of those two words with the bits the base occupies. The base alone is
// matched because the shader may patch the other words (stride, record count, format) or build
// the V# from a 64-bit pointer the walk read, whose position then serves. A stage whose stored
// words differ from guest memory only there (within the mask) still matches: a hit refreshes the
// words like data words and writes the masked bits into guestDescriptor[word] of the compiled
// bindings[binding], so the Graphics side's rebased template reads the moved buffer in place. The
// title keeps such a V# in a per-frame block of the flat SRT whose base moves 0x40 per frame
// (~2.3k fragment stage misses per 10 s in Boletaria, [draw-miss] class A). The two words also
// get slots into the FlattenedSrt binding where its words hold the same pair (the pointer the
// shader loaded sits in the flat SRT too; APS5_VERIFY_DATA_HITS caught the stale copy, t272). A V#
// with a base word at a data position, a null one, one located at two positions or whose pair
// the flat SRT holds twice (which copy moved is unknown) yields none; every other descriptor word
// still compares exactly. `slots` is sorted by position (two elements holding one base share its
// positions). Returns how many V#s were located and why the other elements were skipped (a null
// V# counts nowhere).
struct WordPatchSlot {
    std::uint32_t position;
    std::uint32_t binding;
    std::uint32_t word;
    std::uint32_t mask;
};
struct BufferBaseCounts {
    std::uint64_t located = 0;
    // Not proved read-only; not among the words consecutive in address; among them but not read by
    // the walk; located at two positions; a base word at a data position.
    std::uint64_t written = 0, unlocated = 0, unread = 0, ambiguous = 0, data = 0;
};
inline constexpr std::uint32_t VsharpWord1BaseBits = 0xffffu;
BufferBaseCounts BufferBaseWords(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const ShaderRecompiler::DescriptorBinding> bindings, std::span<const std::uint32_t> dataPositions, std::span<const std::uint64_t> walkReads, std::vector<WordPatchSlot>& slots);
// The mask of `position` among sorted patch slots, 0 when absent.
inline std::uint32_t PatchMaskAt(std::span<const WordPatchSlot> slots, std::uint32_t position) {
    const auto it = std::lower_bound(slots.begin(), slots.end(), position, [](const WordPatchSlot& slot, std::uint32_t value) { return slot.position < value; });
    return it != slots.end() && it->position == position ? it->mask : 0u;
}

}

#endif
