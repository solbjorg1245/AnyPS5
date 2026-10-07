#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker/WalkProgram.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>

namespace AgcDriver {
namespace {

// Windows and Linux both keep the lowest 64 KiB of the address space unmapped.
constexpr std::uint64_t NullPageBytes = 0x10000;

struct CaptureProfile {
    std::atomic<std::uint64_t> captures{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> pages{0};
    // The whole CaptureResources call: the plan lookup and the materialization are one call.
    std::atomic<std::uint64_t> captureNanoseconds{0};
    // Pages read word by word because recorded GPU work writes them, and the word reads of those
    // pages that waited (a read taking longer than WordWaitNanoseconds went through a hook sync),
    // were read raw on the driver's evidence, or were read both ways (verify) and differed.
    std::atomic<std::uint64_t> pagesWordwise{0};
    std::atomic<std::uint64_t> wordWaits{0};
    // Of the waits, those on a page that is not word-wise (the guest mapped less than the page, so
    // page() fetched nothing and every word goes through the hook): until session 31 these reads
    // were not timed, so the [capture-stalls] line could not see them.
    std::atomic<std::uint64_t> wordWaitsPartial{0};
    std::atomic<std::uint64_t> wordWaitNanoseconds{0};
    std::atomic<std::uint64_t> wordsRaw{0};
    // RawExpected reads that found another value and re-read through the hook.
    std::atomic<std::uint64_t> wordsExpectedChanged{0};
    std::atomic<std::uint64_t> wordsVerified{0};
    std::atomic<std::uint64_t> wordMismatches{0};
    // Words served from the driver's known values (never profile-gated: the [copy] line reads them).
    std::atomic<std::uint64_t> wordsKnown{0};
    std::atomic<std::uint64_t> wordsKnownVerified{0};
    std::atomic<std::uint64_t> wordsKnownMismatches{0};
    // Hook reads not reported to the observer because no GPU wait happened across them.
    std::atomic<std::uint64_t> observationsSkipped{0};
    // Sub-phases of a capture: the source lookup (the stage inputs, the recompiler's key over the
    // code and the plan, timed by CaptureResources itself: ResourceCapture::sourceNanoseconds),
    // whole-page fetches, per-word fetches (their hook waits included) and the GPU waits the flush
    // hook made anywhere inside the capture (Recorder::ThreadWaitedMs). The walk itself is what
    // remains of the capture time after the lookup, the fetches and the specialization
    // (ResourceMaterializer).
    std::atomic<std::uint64_t> resolveNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> wordReadNanoseconds{0};
    std::atomic<std::uint64_t> hookWaitNanoseconds{0};
    // The driver's per-shader source handle memo: captures served from it, and resolves it made.
    std::atomic<std::uint64_t> handleHits{0};
    std::atomic<std::uint64_t> handleMisses{0};
    // The express walk's outcome per capture (ShaderRecompiler::Detail::WalkOutcome) and why the
    // express reader declined: the page is written by recorded GPU work, not mapped whole, a word
    // of a page fetched word by word, or an address the interpreter path rejects.
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(ShaderRecompiler::Detail::WalkOutcome::Count)> expressOutcomes{};
    std::atomic<std::uint64_t> expressPending{0};
    std::atomic<std::uint64_t> expressUnmapped{0};
    std::atomic<std::uint64_t> expressWordwise{0};
    std::atomic<std::uint64_t> expressBoundary{0};
    // Pure flat words left to the GPU (deferPureLeaf).
    std::atomic<std::uint64_t> deferredWords{0};
};

constexpr std::uint64_t WordWaitNanoseconds = 50000;

bool WordwisePages() {
    static const bool wordwise = std::getenv("APS5_NO_WORDWISE_CAPTURE") == nullptr;
    return wordwise;
}

bool CaptureProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::uint64_t NanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

CaptureProfile& CaptureTotals() {
    static CaptureProfile profile;
    return profile;
}

std::atomic<ShaderMemory::WaitedMsProvider> waitedMsProvider{nullptr};
std::atomic<ShaderMemory::WriterDescriber> writerDescriber{nullptr};
std::atomic<ShaderMemory::PendingUnsignaledQuery> pendingUnsignaled{nullptr};

// APS5_PROFILE_DRAW: the word reads of a capture that went through a hook sync (a word of a page
// recorded GPU work writes, read after the GPU wait), kept per capture on the reading thread, and
// their attribution to the captured program: the [capture-stalls] line every 10 s says which
// shader programs' walks wait for the GPU, how long, what the stalled word is to the walk (a
// dword of a captured descriptor, a flat SRT slot, or a value it only derived from: a pointer or
// index) and which GPU program wrote it (the driver's writer table). The design question it
// answers: whether those words could be read on the GPU instead (an address-based build serves
// buffer descriptors only; a stalled T# dword or a pointer the walk follows cannot move there).
// APS5_TRACE_CAPTURE_STALLS=<n> prints the first n stalled captures in full, each stalled word
// with the reads that followed it (what the walk did with the value).
struct CaptureStall {
    std::uint64_t address;
    std::uint32_t before;
    std::uint32_t after;
    std::uint64_t nanoseconds;
};

struct CaptureStallList {
    static constexpr std::size_t Capacity = 8;
    std::array<CaptureStall, Capacity> entries{};
    // Stalls of the capture (the entries hold the first Capacity).
    std::size_t count = 0;
    std::uint64_t nanoseconds = 0;
};

CaptureStallList& ThreadCaptureStalls() {
    thread_local CaptureStallList stalls;
    return stalls;
}

struct StallProgramTotals {
    std::uint64_t captures = 0, words = 0, nanoseconds = 0, maxNanoseconds = 0, changed = 0;
    std::uint64_t sampleAddress = 0;
    std::uint32_t sampleValue = 0;
    std::string sampleKind;
    std::string writers;
};

struct StallStats {
    HostMutex mutex;
    std::map<std::pair<std::uint64_t, int>, StallProgramTotals> byProgram;
    std::uint64_t captures = 0, words = 0, nanoseconds = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> traced{0};
};

StallStats& Stalls() {
    static StallStats stats;
    return stats;
}

std::uint64_t StallTraceLimit() {
    static const std::uint64_t limit = [] {
        const char* text = std::getenv("APS5_TRACE_CAPTURE_STALLS");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    return limit;
}

const char* StageName(ShaderRecompiler::ShaderStage stage) {
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Compute: return "cs";
        case ShaderRecompiler::ShaderStage::Vertex: return "vs";
        case ShaderRecompiler::ShaderStage::TessellationControl: return "hs";
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return "ds";
        case ShaderRecompiler::ShaderStage::Geometry: return "gs";
        case ShaderRecompiler::ShaderStage::Fragment: return "ps";
        case ShaderRecompiler::ShaderStage::Local: return "ls";
        case ShaderRecompiler::ShaderStage::Mesh: return "ms";
    }
    return "?";
}

// What a stalled word is to the walk, by its value: a dword of a captured descriptor (bufN.dM,
// imgN.dM, smpN.dM), a flat SRT slot (flatN), or none of them ("derived": the walk followed it as
// a pointer or index). A zero is left unclassified (too many descriptor dwords are zero).
std::string ClassifyStall(const ShaderRecompiler::ResourceSnapshot& snapshot, std::uint32_t value) {
    if (value == 0) return "zero";
    std::string kind;
    char text[48];
    const auto scan = [&](const char* name, const std::vector<ShaderRecompiler::DescriptorValue>& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            for (std::uint32_t d = 0; d < values[i].dwordCount; ++d) {
                if (values[i].dwords[d] != value) continue;
                std::snprintf(text, sizeof(text), "%s%s%zu.d%u", kind.empty() ? "" : "+", name, i, d);
                kind += text;
                return;
            }
        }
    };
    scan("buf", snapshot.buffers);
    scan("img", snapshot.images);
    scan("smp", snapshot.samplers);
    for (std::size_t i = 0; i < snapshot.flattenedSrt.size(); ++i) {
        if (snapshot.flattenedSrt[i] != value) continue;
        std::snprintf(text, sizeof(text), "%sflat%zu", kind.empty() ? "" : "+", i);
        kind += text;
        break;
    }
    return kind.empty() ? "derived" : kind;
}

void ReportCaptureStalls(StallStats& stats) {
    std::vector<std::pair<const std::pair<std::uint64_t, int>*, const StallProgramTotals*>> hot;
    for (const auto& [key, totals] : stats.byProgram) hot.emplace_back(&key, &totals);
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second->nanoseconds > b.second->nanoseconds; });
    if (hot.size() > 8) hot.resize(8);
    std::string line;
    char text[512];
    std::snprintf(text, sizeof(text), "[capture-stalls] %llu stalled captures, %llu words waited %.0f ms (10 s; %llu of the waits on pages mapped partially); by program (stage code: captures/words/ms, max ms, changed words, sample address=value kind, GPU writers of the sample):", static_cast<unsigned long long>(stats.captures), static_cast<unsigned long long>(stats.words), stats.nanoseconds / 1e6, static_cast<unsigned long long>(CaptureTotals().wordWaitsPartial.exchange(0)));
    line += text;
    for (const auto& [key, totals] : hot) {
        std::snprintf(text, sizeof(text), " [%s 0x%llx: %llu/%llu/%.0f max %.1f changed %llu 0x%llx=%08x %s writers%s]", StageName(static_cast<ShaderRecompiler::ShaderStage>(key->second)), static_cast<unsigned long long>(key->first), static_cast<unsigned long long>(totals->captures), static_cast<unsigned long long>(totals->words), totals->nanoseconds / 1e6, totals->maxNanoseconds / 1e6, static_cast<unsigned long long>(totals->changed), static_cast<unsigned long long>(totals->sampleAddress), totals->sampleValue, totals->sampleKind.c_str(), totals->writers.empty() ? " none" : totals->writers.c_str());
        line += text;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    stats.byProgram.clear();
    stats.captures = stats.words = stats.nanoseconds = 0;
    stats.lastReport = std::chrono::steady_clock::now();
}

// The reads the calling thread made right after the stalled one (what the walk did with the word),
// as " address=value" pairs.
std::string ReadsAfter(std::uint64_t address, std::size_t limit);

void NoteCaptureStalls(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::ResourceCapture& capture, const CaptureStallList& stalls) {
    auto& stats = Stalls();
    const auto shown = std::min(stalls.count, CaptureStallList::Capacity);
    const bool trace = stats.traced.load(std::memory_order_relaxed) < StallTraceLimit() && stats.traced.fetch_add(1, std::memory_order_relaxed) < StallTraceLimit();
    std::string detail;
    std::string firstKind;
    std::uint64_t changed = 0;
    char text[160];
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& stall = stalls.entries[i];
        const auto kind = ClassifyStall(capture.snapshot, stall.after);
        if (i == 0) firstKind = kind;
        if (stall.before != stall.after) ++changed;
        if (!trace) continue;
        std::snprintf(text, sizeof(text), "\n  0x%llx %08x->%08x %s waited %.2f ms; reads after:", static_cast<unsigned long long>(stall.address), stall.before, stall.after, kind.c_str(), stall.nanoseconds / 1e6);
        detail += text;
        detail += ReadsAfter(stall.address, 6);
    }
    const auto describe = writerDescriber.load(std::memory_order_acquire);
    const auto packet = GuestMemory::CurrentPacket();
    std::lock_guard lock(stats.mutex);
    auto& totals = stats.byProgram[{request.shader.codeAddress, static_cast<int>(request.shader.stage)}];
    ++totals.captures;
    totals.words += stalls.count;
    totals.nanoseconds += stalls.nanoseconds;
    totals.maxNanoseconds = std::max(totals.maxNanoseconds, stalls.nanoseconds);
    totals.changed += changed;
    if (shown != 0) {
        totals.sampleAddress = stalls.entries[0].address;
        totals.sampleValue = stalls.entries[0].after;
        totals.sampleKind = firstKind;
        // The writer table is scanned under its own mutex: once per program per interval.
        if (totals.writers.empty() && describe != nullptr) totals.writers = describe(stalls.entries[0].address, sizeof(std::uint32_t));
    }
    ++stats.captures;
    stats.words += stalls.count;
    stats.nanoseconds += stalls.nanoseconds;
    if (trace) std::fprintf(stderr, "[capture-stall] %s 0x%llx in packet 0x%x of queue %u: %zu stalled words waited %.2f ms, writers of the first%s%s\n", StageName(request.shader.stage), static_cast<unsigned long long>(request.shader.codeAddress), packet.opcode, packet.queue, stalls.count, stalls.nanoseconds / 1e6, totals.writers.empty() ? " none" : totals.writers.c_str(), detail.c_str());
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.lastReport > std::chrono::seconds(10)) ReportCaptureStalls(stats);
}

// The calling thread's last capture reads, for ShaderMemory::LocateRecentWords.
struct RecentReads {
    static constexpr std::size_t Count = 4096;
    std::array<std::pair<std::uint64_t, std::uint32_t>, Count> entries{};
    std::size_t next = 0;
};

RecentReads& ThreadRecentReads() {
    thread_local RecentReads reads;
    return reads;
}

double WaitedMs() {
    const auto provider = waitedMsProvider.load(std::memory_order_acquire);
    return provider != nullptr ? provider() : 0.0;
}

std::string ReadsAfter(std::uint64_t address, std::size_t limit) {
    const auto& recent = ThreadRecentReads();
    const auto count = std::min(recent.next, RecentReads::Count);
    std::string text;
    // The newest read of the address, then the reads that followed it.
    for (std::size_t back = 1; back <= count; ++back) {
        const auto index = (recent.next - back) % RecentReads::Count;
        if (recent.entries[index].first != address) continue;
        char item[40];
        for (std::size_t j = 1; j < back && j <= limit; ++j) {
            const auto& [next, value] = recent.entries[(recent.next - back + j) % RecentReads::Count];
            std::snprintf(item, sizeof(item), " %llx=%08x", static_cast<unsigned long long>(next), value);
            text += item;
        }
        break;
    }
    return text.empty() ? " (none)" : text;
}

}

void ShaderMemory::SetWriterDescriber(WriterDescriber describer) {
    writerDescriber.store(describer, std::memory_order_release);
}

void ShaderMemory::SetPendingUnsignaledQuery(PendingUnsignaledQuery query) {
    pendingUnsignaled.store(query, std::memory_order_release);
}

// Deferred flat slots (SrtRuntime::deferPureLeaf): a pure flat SRT word (one no CPU evaluation
// consumes, IrResourcePlan::pureFlatSlots) on a page that recorded GPU work writes is left to the
// GPU instead of waited for: the walk takes a placeholder, the recompiler lists the slot in
// ResourceSnapshot::deferredFlat, and Draw.cpp (recordDeferredFlat) copies the word on the GPU
// from its host import into the draw's data buffer, behind the work that writes it. Demon's Souls
// uploads its per-frame SRT constants with a compute copy kernel; the captures of the draws that
// follow waited for the GPU backlog at that point (17-28 ms per frame for two draws). Graphics
// stages only until the dispatch path records the copies too. Off under APS5_NO_DEFERRED_FLAT=1,
// and under APS5_VERIFY_EXPRESS=1 (the interpreter's walk alone traces and so alone would defer,
// and the verification compares the two walks' words).
bool ShaderMemory::DeferredFlatEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DEFERRED_FLAT") == nullptr && std::getenv("APS5_VERIFY_EXPRESS") == nullptr;
    return enabled;
}

bool ShaderMemory::deferPureLeaf(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (self.pendingWrite == nullptr || address % sizeof(*value) != 0 || address < NullPageBytes || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) return false;
    // The registered code and header are never GPU-written.
    if (!self.initial.empty()) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin() && address - std::prev(next)->first < std::prev(next)->second.size()) return false;
    }
    const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
    Page* page = self.expressPage != nullptr && self.expressBase == base ? self.expressPage : nullptr;
    if (page == nullptr) {
        const auto found = self.pages.find(base);
        if (found != self.pages.end()) page = &found->second;
    }
    if (page != nullptr) {
        // Fetched whole (nothing recorded writes it), or mapped partially: the ordinary read.
        if (!page->wordwise) return false;
    } else {
        // What page() decides for a new page, so the readers after this one find it word-wise.
        if (!WordwisePages() || !GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) return false;
        if (self.pendingWrite(base, PageBytes, {}) == PendingWrite::None) return false;
        page = &self.pages[base];
        page->wordwise = true;
        ++CaptureTotals().pages;
        ++CaptureTotals().pagesWordwise;
    }
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    // A word already read (another slot, or an earlier stage of the draw) is served as read.
    if (page->valid.test(index)) return false;
    // Only a word the read would wait for: a known value or an evidence-based raw read is cheap,
    // and so is a sync against a writer batch the GPU already finished.
    if (self.pendingWrite(address, sizeof(*value), {}) != PendingWrite::Sync) return false;
    if (const auto unsignaled = pendingUnsignaled.load(std::memory_order_acquire); unsignaled != nullptr && !unsignaled(address, sizeof(*value))) return false;
    self.deferred.push_back(address);
    ++CaptureTotals().deferredWords;
    *value = 0;
    return true;
}

void ShaderMemory::SetWaitedMsProvider(WaitedMsProvider provider) {
    waitedMsProvider.store(provider, std::memory_order_release);
}

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> regions, PendingWriteQuery pendingWrite, PendingWriteObserver observe, HookWaitCounter hookWaits) : pendingWrite(pendingWrite), observe(observe), hookWaits(hookWaits) {
    const ShaderRecompiler::RequestMemoryView validated(regions);
    for (const auto& region : regions) {
        initial.emplace(region.guestAddress, region.bytes);
    }
}

ShaderMemory::Page& ShaderMemory::page(std::uint64_t base) {
    const auto found = pages.find(base);
    if (found != pages.end()) return found->second;
    auto& page = pages[base];
    ++CaptureTotals().pages;
    // A page is either mapped whole or read word by word where the guest mapped less than a page,
    // or where recorded GPU work writes into it (a whole-page read would wait for that work).
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) {
        if (WordwisePages() && pendingWrite != nullptr && pendingWrite(base, PageBytes, {}) != PendingWrite::None) {
            page.wordwise = true;
            ++CaptureTotals().pagesWordwise;
            return page;
        }
        const auto started = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        GuestMemory::Read(base, std::as_writable_bytes(std::span(page.words)), sizeof(std::uint32_t));
        if (CaptureProfiled()) CaptureTotals().pageReadNanoseconds += NanosecondsSince(started);
        page.valid.set();
    }
    return page;
}

bool ShaderMemory::read(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        throw std::runtime_error("AGC driver: invalid shader memory read address");
    }
    ++CaptureTotals().reads;
    if (!self.initial.empty()) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            const auto offset = address - previous->first;
            if (offset < previous->second.size()) {
                if (previous->second.size() - offset < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read crosses a snapshot boundary");
                std::memcpy(value, previous->second.data() + offset, sizeof(*value));
                return true;
            }
        }
        if (next != self.initial.end() && next->first - address < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read overlaps a snapshot boundary");
    }
    // The capture follows every scalar load the shader could make, including ones through a null
    // table pointer on paths the shader does not take at run time (Demon's Souls reads [null+0x60]
    // in a compute shader that runs fine on hardware). The null page is never mapped, so such a
    // load reads zero instead of failing the dispatch.
    if (address < NullPageBytes) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) std::fprintf(stderr, "[capture] scalar load from the null page at 0x%llx reads zero\n", static_cast<unsigned long long>(address));
        *value = 0;
        return true;
    }
    auto& page = self.page(address & ~static_cast<std::uint64_t>(PageBytes - 1));
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    if (!page.valid.test(index)) {
        std::uint32_t word = 0;
        auto policy = PendingWrite::Sync;
        if (page.wordwise) {
            policy = self.pendingWrite(address, sizeof(word), std::as_writable_bytes(std::span(&word, 1)));
            if (policy != PendingWrite::None && policy != PendingWrite::Sync && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word))) policy = PendingWrite::Sync;
        }
        // A read is evidence only when the hook waited for unfinished GPU work across it.
        const auto hookWaits = [&] { return self.hookWaits != nullptr ? self.hookWaits() : 0; };
        const auto report = [&](std::uint64_t waitsBefore, bool unchanged, std::uint32_t value) {
            if (self.hookWaits == nullptr || self.hookWaits() != waitsBefore) self.observe(address, unchanged, value);
            else ++CaptureTotals().observationsSkipped;
        };
        const auto fetchStarted = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (policy == PendingWrite::KnownValue || policy == PendingWrite::VerifyKnownValue) {
            // The query stored the known dword in `word`.
            ++CaptureTotals().wordsKnown;
            if (policy == PendingWrite::VerifyKnownValue) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsKnownVerified;
                if (waited != word) ++CaptureTotals().wordsKnownMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word, waited);
                word = waited;
            }
        } else if (policy == PendingWrite::RawExpected) {
            // The query stored the value the evidence saw last in `word`: the bytes still holding
            // it are read raw; another value ends the evidence (the observer is told the word
            // changed) and is read through the hook, which waits for the writer.
            const auto expected = word;
            std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            if (word == expected) {
                ++CaptureTotals().wordsRaw;
            } else {
                ++CaptureTotals().wordsExpectedChanged;
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&word, 1)), alignof(std::uint32_t));
                if (self.observe != nullptr) self.observe(address, false, word);
            }
        } else if (policy == PendingWrite::Raw || policy == PendingWrite::VerifyRaw) {
            std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            ++CaptureTotals().wordsRaw;
            if (policy == PendingWrite::VerifyRaw) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsVerified;
                if (waited != word) ++CaptureTotals().wordMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word, waited);
                word = waited;
            }
        } else {
            // The bytes before the hook's wait are kept for the observer (word-wise pages only: the
            // evidence is about words the GPU writes) and for the stall attribution under
            // APS5_PROFILE_DRAW, which times every hook read (a page the guest mapped partially is
            // read word by word too, and its waits were invisible to [capture-stalls] before).
            const bool accessible = policy == PendingWrite::Sync && GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word));
            const bool observed = page.wordwise && accessible && self.observe != nullptr;
            std::uint32_t before = 0;
            if (accessible) std::memcpy(&before, reinterpret_cast<const void*>(address), sizeof(before));
            const auto waitsBefore = observed ? hookWaits() : 0;
            const bool timed = page.wordwise || CaptureProfiled();
            const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::Read(address, std::as_writable_bytes(std::span(&word, 1)), alignof(std::uint32_t));
            if (timed) {
                const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                if (nanoseconds >= WordWaitNanoseconds) {
                    ++CaptureTotals().wordWaits;
                    if (!page.wordwise) ++CaptureTotals().wordWaitsPartial;
                    CaptureTotals().wordWaitNanoseconds += nanoseconds;
                    if (CaptureProfiled()) {
                        auto& stalls = ThreadCaptureStalls();
                        if (stalls.count < CaptureStallList::Capacity) stalls.entries[stalls.count] = {address, accessible ? before : 0u, word, nanoseconds};
                        ++stalls.count;
                        stalls.nanoseconds += nanoseconds;
                    }
                }
            }
            if (observed) report(waitsBefore, before == word, word);
        }
        if (CaptureProfiled()) CaptureTotals().wordReadNanoseconds += NanosecondsSince(fetchStarted);
        page.words[index] = word;
        page.valid.set(index);
    }
    page.read.set(index);
    page.recent.set(index);
    *value = page.words[index];
    auto& recent = ThreadRecentReads();
    recent.entries[recent.next++ % RecentReads::Count] = {address, *value};
    return true;
}

bool ShaderMemory::expressRead(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    auto& totals = CaptureTotals();
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        ++totals.expressBoundary;
        return false;
    }
    ++totals.reads;
    if (!self.initial.empty()) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            const auto offset = address - previous->first;
            if (offset < previous->second.size()) {
                if (previous->second.size() - offset < sizeof(*value)) {
                    ++totals.expressBoundary;
                    return false;
                }
                std::memcpy(value, previous->second.data() + offset, sizeof(*value));
                return true;
            }
        }
        if (next != self.initial.end() && next->first - address < sizeof(*value)) {
            ++totals.expressBoundary;
            return false;
        }
    }
    if (address < NullPageBytes) {
        *value = 0;
        return true;
    }
    const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
    Page* page = self.expressPage;
    if (page == nullptr || self.expressBase != base) {
        const auto found = self.pages.find(base);
        if (found != self.pages.end()) {
            page = &found->second;
        } else {
            if (WordwisePages() && self.pendingWrite != nullptr && self.pendingWrite(base, PageBytes, {}) != PendingWrite::None) {
                ++totals.expressPending;
                return false;
            }
            // What page() does through GuestMemory::Read, without its per-call timing and caller
            // attribution: the flush hook for the page, then a copy checked against the page
            // states (no VirtualQuery). A page not mapped whole stays in the map unfetched, as
            // page() leaves it, for the interpreter's word reads.
            const auto started = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::FlushGpuWrites(base, PageBytes);
            auto& fresh = self.pages[base];
            ++totals.pages;
            const bool whole = GuestMemory::CopyMapped(base, std::as_writable_bytes(std::span(fresh.words))) == GuestMemory::Compare::Equal;
            if (CaptureProfiled()) totals.pageReadNanoseconds += NanosecondsSince(started);
            if (!whole) {
                ++totals.expressUnmapped;
                return false;
            }
            fresh.valid.set();
            page = &fresh;
        }
        self.expressPage = page;
        self.expressBase = base;
    }
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    if (!page->valid.test(index)) {
        ++totals.expressWordwise;
        return false;
    }
    page->read.set(index);
    page->recent.set(index);
    *value = page->words[index];
    auto& recent = ThreadRecentReads();
    recent.entries[recent.next++ % RecentReads::Count] = {address, *value};
    return true;
}

std::uint64_t ShaderMemory::LocateRecentWords(std::span<const std::uint32_t> words, std::string* chain) {
    if (words.empty()) return 0;
    const auto& recent = ThreadRecentReads();
    std::map<std::uint64_t, std::uint32_t> seen;
    const auto count = std::min(recent.next, RecentReads::Count);
    // Oldest first, so a later read of an address wins.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& [address, value] = recent.entries[(recent.next - count + i) % RecentReads::Count];
        seen[address] = value;
    }
    for (std::size_t i = count; i-- > 0;) {
        const auto& [address, value] = recent.entries[(recent.next - count + i) % RecentReads::Count];
        if (value != words[0]) continue;
        bool all = true;
        for (std::size_t word = 1; word < words.size() && all; ++word) {
            const auto found = seen.find(address + word * sizeof(std::uint32_t));
            all = found != seen.end() && found->second == words[word];
        }
        if (!all) continue;
        // The first read of the descriptor's run, then the reads before it.
        auto first = i;
        while (first > 0) {
            const auto previous = recent.entries[(recent.next - count + first - 1) % RecentReads::Count].first;
            if (previous < address || previous >= address + words.size() * sizeof(std::uint32_t)) break;
            --first;
        }
        if (chain != nullptr) {
            char text[48];
            for (std::size_t j = first > 24 ? first - 24 : 0; j < first; ++j) {
                const auto& [before, value] = recent.entries[(recent.next - count + j) % RecentReads::Count];
                std::snprintf(text, sizeof(text), " %llx=%08x", static_cast<unsigned long long>(before), value);
                *chain += text;
            }
        }
        return address;
    }
    return 0;
}

ShaderMemory::KnownValueCounts ShaderMemory::KnownValues() {
    auto& totals = CaptureTotals();
    return {totals.wordsKnown.load(std::memory_order_relaxed), totals.wordsKnownVerified.load(std::memory_order_relaxed), totals.wordsKnownMismatches.load(std::memory_order_relaxed)};
}

void ShaderMemory::CountHandleMemo(bool hit) {
    if (!CaptureProfiled()) return;
    (hit ? CaptureTotals().handleHits : CaptureTotals().handleMisses).fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle) {
    // The capture's word and page reads (through `read`) are attributed to it ([hooksync], [guestmem]).
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    const bool profile = CaptureProfiled();
    auto& totals = CaptureTotals();
    // The hook's GPU waits made anywhere inside the capture.
    const auto waitedBefore = profile ? WaitedMs() : 0.0;
    const auto started = std::chrono::steady_clock::now();
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = request.context.userData;
    runtime.shaderBase = request.shader.codeAddress;
    runtime.userContext = this;
    runtime.readMemory = &read;
    runtime.readSpecializationMemory = &read;
    runtime.expressRead = &expressRead;
    deferred.clear();
    if (DeferredFlatEnabled() && request.shader.stage != ShaderRecompiler::ShaderStage::Compute) {
        runtime.deferPureLeaf = &deferPureLeaf;
        runtime.deferredReads = &deferred;
    }
    expressPage = nullptr;
    ShaderRecompiler::Detail::NoteWalkOutcome(ShaderRecompiler::Detail::WalkOutcome::NoProgram);
    auto& stalls = ThreadCaptureStalls();
    if (profile) stalls = CaptureStallList{};
    auto capture = handle != nullptr ? ShaderRecompiler::CaptureResources(request, runtime, *handle) : ShaderRecompiler::CaptureResources(request, runtime);
    totals.expressOutcomes[static_cast<std::size_t>(ShaderRecompiler::Detail::LastWalkOutcome())].fetch_add(1, std::memory_order_relaxed);
    if (profile && stalls.count != 0) NoteCaptureStalls(request, *capture, stalls);
    if (profile) {
        totals.captureNanoseconds += NanosecondsSince(started);
        totals.resolveNanoseconds += capture->sourceNanoseconds;
        totals.hookWaitNanoseconds += static_cast<std::uint64_t>((WaitedMs() - waitedBefore) * 1e6);
        std::uint64_t initialBytes = 0;
        for (const auto& [address, bytes] : initial) initialBytes += bytes.size();
        if (++totals.captures % 500 == 0) {
            const auto captures = static_cast<double>(totals.captures.load());
            const auto captureNs = totals.captureNanoseconds.load();
            const auto resolveNs = totals.resolveNanoseconds.load();
            const auto pageNs = totals.pageReadNanoseconds.load();
            const auto wordNs = totals.wordReadNanoseconds.load();
            const auto specializationNs = ShaderRecompiler::ResourceMaterializer::SpecializationNanoseconds();
            const auto hookNs = totals.hookWaitNanoseconds.load();
            const auto expressNs = ShaderRecompiler::Detail::ExpressWalkNanoseconds();
            const auto walkNs = captureNs > resolveNs + pageNs + wordNs + specializationNs + expressNs ? captureNs - resolveNs - pageNs - wordNs - specializationNs - expressNs : 0;
            std::fprintf(stderr, "[capture] %llu captures: %llu word reads, %llu pages fetched (%llu read word by word over pending GPU writes: %llu words waited %.2f s, %llu read raw on evidence, %llu re-read after the word changed, %llu verified with %llu mismatches, %llu served from known values, %llu hook reads not observed: no GPU wait), capture (plan lookup + materialize) %.1f s, %llu KiB registered code this capture; sub-phases (s, us per capture): source resolve %.2f/%.1f, walk %.2f/%.1f, specialization (every materialize) %.2f/%.1f, page reads %.2f/%.1f, word reads %.2f/%.1f, hook GPU waits inside %.2f/%.1f; source handle memo %llu hits, %llu resolves; express walk (program + its reads) %.2f/%.1f: %llu ran, %llu no program, %llu unsupported root, %llu op failed, %llu declined (page pending %llu, unmapped %llu, word of a word-wise page %llu, boundary %llu); %llu flat words left to the GPU\n", static_cast<unsigned long long>(totals.captures.load()), static_cast<unsigned long long>(totals.reads.load()), static_cast<unsigned long long>(totals.pages.load()), static_cast<unsigned long long>(totals.pagesWordwise.load()), static_cast<unsigned long long>(totals.wordWaits.load()), totals.wordWaitNanoseconds.load() / 1e9, static_cast<unsigned long long>(totals.wordsRaw.load()), static_cast<unsigned long long>(totals.wordsExpectedChanged.load()), static_cast<unsigned long long>(totals.wordsVerified.load()), static_cast<unsigned long long>(totals.wordMismatches.load()), static_cast<unsigned long long>(totals.wordsKnown.load()), static_cast<unsigned long long>(totals.observationsSkipped.load()), captureNs / 1e9, static_cast<unsigned long long>(initialBytes / 1024), resolveNs / 1e9, resolveNs / captures / 1e3, walkNs / 1e9, walkNs / captures / 1e3, specializationNs / 1e9, specializationNs / captures / 1e3, pageNs / 1e9, pageNs / captures / 1e3, wordNs / 1e9, wordNs / captures / 1e3, hookNs / 1e9, hookNs / captures / 1e3, static_cast<unsigned long long>(totals.handleHits.load()), static_cast<unsigned long long>(totals.handleMisses.load()), expressNs / 1e9, expressNs / captures / 1e3, static_cast<unsigned long long>(totals.expressOutcomes[0].load()), static_cast<unsigned long long>(totals.expressOutcomes[1].load()), static_cast<unsigned long long>(totals.expressOutcomes[2].load()), static_cast<unsigned long long>(totals.expressOutcomes[3].load()), static_cast<unsigned long long>(totals.expressOutcomes[4].load()), static_cast<unsigned long long>(totals.expressPending.load()), static_cast<unsigned long long>(totals.expressUnmapped.load()), static_cast<unsigned long long>(totals.expressWordwise.load()), static_cast<unsigned long long>(totals.expressBoundary.load()), static_cast<unsigned long long>(totals.deferredWords.load()));
        }
    }
    return capture;
}

namespace {

// Calls emit(first, last) for every run [first, last) of set bits. Clear bits are skipped a machine
// word at a time (libstdc++'s _Find_first/_Find_next): a capture reads a few dozen words of each
// 1024-word page, and testing every bit of every page twice per stage capture was a tenth of the
// draw thread.
template<std::size_t Bits, typename Emit>
void forEachRun(const std::bitset<Bits>& bits, Emit&& emit) {
    for (auto first = bits._Find_first(); first < Bits;) {
        auto last = first + 1;
        while (last < Bits && bits.test(last)) ++last;
        emit(first, last);
        first = last < Bits ? bits._Find_next(last) : Bits;
    }
}

}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(initial.size() + pages.size());
    auto next = initial.begin();
    // Both maps are ordered by address and never overlap, so a merge keeps the result sorted.
    for (const auto& [base, page] : pages) {
        while (next != initial.end() && next->first < base) {
            result.push_back({next->first, next->second});
            ++next;
        }
        forEachRun(page.read, [&](std::size_t first, std::size_t last) { result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, last - first))}); });
    }
    for (; next != initial.end(); ++next) result.push_back({next->first, next->second});
    return result;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::TakeRecentRegions() {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    for (auto& [base, page] : pages) {
        if (page.recent.none()) continue;
        forEachRun(page.recent, [&](std::size_t first, std::size_t last) { result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, last - first))}); });
        page.recent.reset();
    }
    return result;
}

DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots) {
    DataWordPositionCounts counts;
    positions.clear();
    slots.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& [slot, address] : leaves) {
        if ((address & 3u) != 0) {
            ++counts.unmapped;
            continue;
        }
        if (std::binary_search(otherReads.begin(), otherReads.end(), address)) {
            ++counts.aliased;
            continue;
        }
        auto run = std::upper_bound(runs.begin(), runs.end(), address, [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& candidate) { return value < candidate.first; });
        if (run == runs.begin() || address + sizeof(std::uint32_t) > (run - 1)->second) {
            ++counts.unmapped;
            continue;
        }
        --run;
        const auto position = prefix[static_cast<std::size_t>(run - runs.begin())] + static_cast<std::size_t>((address - run->first) / sizeof(std::uint32_t));
        if (position >= words.size() || slot >= flattenedSrt.size() || words[position] != flattenedSrt[slot]) {
            ++counts.mismatched;
            continue;
        }
        found.emplace_back(static_cast<std::uint32_t>(position), slot);
    }
    std::sort(found.begin(), found.end());
    positions.reserve(found.size());
    slots.reserve(found.size());
    for (const auto& [position, slot] : found) {
        positions.push_back(position);
        slots.push_back(slot);
    }
    return counts;
}

std::size_t IgnoredWordBits(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const ShaderRecompiler::DescriptorBinding> bindings, std::span<const std::uint32_t> dataPositions, std::vector<std::pair<std::uint32_t, std::uint32_t>>& ignored) {
    constexpr std::size_t TsharpWords = 8;
    ignored.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    // The guest address of a position (positions index the runs' words in order).
    const auto addressOf = [&](std::size_t position) {
        const auto run = static_cast<std::size_t>(std::upper_bound(prefix.begin(), prefix.end(), position) - prefix.begin()) - 1;
        return runs[run].first + static_cast<std::uint64_t>(position - prefix[run]) * sizeof(std::uint32_t);
    };
    std::size_t located = 0;
    for (const auto& binding : bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::SampledImage) continue;
        const auto& descriptor = binding.guestDescriptor;
        for (std::size_t element = 0; element + TsharpWords <= descriptor.size(); element += TsharpWords) {
            const auto* tsharp = descriptor.data() + element;
            if (tsharp[0] == 0 && tsharp[1] == 0) continue;
            for (std::size_t position = 0; position + TsharpWords <= words.size(); ++position) {
                if (words[position] != tsharp[0] || !std::equal(tsharp, tsharp + TsharpWords, words.begin() + static_cast<std::ptrdiff_t>(position))) continue;
                if (addressOf(position + TsharpWords - 1) != addressOf(position) + (TsharpWords - 1) * sizeof(std::uint32_t)) continue;
                const auto word5 = static_cast<std::uint32_t>(position + 5);
                const auto word6 = static_cast<std::uint32_t>(position + 6);
                if (std::binary_search(dataPositions.begin(), dataPositions.end(), word5) || std::binary_search(dataPositions.begin(), dataPositions.end(), word6)) continue;
                ignored.emplace_back(word5, TsharpWord5IgnoredBits);
                ignored.emplace_back(word6, TsharpWord6IgnoredBits);
                ++located;
            }
        }
    }
    std::sort(ignored.begin(), ignored.end());
    std::size_t kept = 0;
    for (std::size_t i = 0; i < ignored.size(); ++i) {
        if (kept != 0 && ignored[kept - 1].first == ignored[i].first) ignored[kept - 1].second |= ignored[i].second;
        else ignored[kept++] = ignored[i];
    }
    ignored.resize(kept);
    return located;
}

BufferBaseCounts BufferBaseWords(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const ShaderRecompiler::DescriptorBinding> bindings, std::span<const std::uint32_t> dataPositions, std::span<const std::uint64_t> walkReads, std::vector<WordPatchSlot>& slots) {
    constexpr std::size_t VsharpWords = 4;
    constexpr std::size_t BaseWords = 2;
    BufferBaseCounts counts;
    slots.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    const auto addressOf = [&](std::size_t position) {
        const auto run = static_cast<std::size_t>(std::upper_bound(prefix.begin(), prefix.end(), position) - prefix.begin()) - 1;
        return runs[run].first + static_cast<std::uint64_t>(position - prefix[run]) * sizeof(std::uint32_t);
    };
    const auto dataPosition = [&](std::size_t position) { return std::binary_search(dataPositions.begin(), dataPositions.end(), static_cast<std::uint32_t>(position)); };
    std::size_t flat = bindings.size();
    for (std::size_t b = 0; b < bindings.size(); ++b) {
        if (bindings[b].role == ShaderRecompiler::DescriptorRole::FlattenedSrt) {
            flat = b;
            break;
        }
    }
    for (std::size_t b = 0; b < bindings.size(); ++b) {
        const auto& binding = bindings[b];
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        const auto& descriptor = binding.guestDescriptor;
        for (std::size_t element = 0; (element + 1) * VsharpWords <= descriptor.size(); ++element) {
            const auto* vsharp = descriptor.data() + element * VsharpWords;
            if (vsharp[0] == 0 && (vsharp[1] & VsharpWord1BaseBits) == 0) continue;
            // An element beyond bufferWritten is not proved read-only.
            if (element >= binding.bufferWritten.size() || binding.bufferWritten[element]) {
                ++counts.written;
                continue;
            }
            std::size_t found = words.size();
            bool ambiguous = false, unread = false;
            for (std::size_t position = 0; position + BaseWords <= words.size() && !ambiguous; ++position) {
                if (words[position] != vsharp[0] || ((words[position + 1] ^ vsharp[1]) & VsharpWord1BaseBits) != 0) continue;
                const auto address = addressOf(position);
                if (addressOf(position + BaseWords - 1) != address + (BaseWords - 1) * sizeof(std::uint32_t)) continue;
                if (!std::binary_search(walkReads.begin(), walkReads.end(), address)) {
                    unread = true;
                    continue;
                }
                if (found != words.size()) ambiguous = true;
                found = position;
            }
            // The pair's copies among the flat SRT words: patched with it, or ambiguous.
            std::size_t flatSlot = 0, flatCopies = 0;
            if (!ambiguous && found != words.size() && flat < bindings.size()) {
                const auto& flatWords = bindings[flat].guestDescriptor;
                for (std::size_t s = 0; s + 1 < flatWords.size(); ++s) {
                    if (flatWords[s] != words[found] || flatWords[s + 1] != words[found + 1]) continue;
                    flatSlot = s;
                    ++flatCopies;
                }
                if (flatCopies > 1) ambiguous = true;
            }
            if (ambiguous) ++counts.ambiguous;
            else if (found == words.size()) ++(unread ? counts.unread : counts.unlocated);
            else if (dataPosition(found) || dataPosition(found + 1)) ++counts.data;
            else {
                slots.push_back({static_cast<std::uint32_t>(found), static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(element * VsharpWords), 0xffffffffu});
                slots.push_back({static_cast<std::uint32_t>(found + 1), static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(element * VsharpWords + 1), VsharpWord1BaseBits});
                if (flatCopies == 1) {
                    slots.push_back({static_cast<std::uint32_t>(found), static_cast<std::uint32_t>(flat), static_cast<std::uint32_t>(flatSlot), 0xffffffffu});
                    slots.push_back({static_cast<std::uint32_t>(found + 1), static_cast<std::uint32_t>(flat), static_cast<std::uint32_t>(flatSlot + 1), VsharpWord1BaseBits});
                }
                ++counts.located;
            }
        }
    }
    std::sort(slots.begin(), slots.end(), [](const WordPatchSlot& a, const WordPatchSlot& c) { return a.position != c.position ? a.position < c.position : a.binding != c.binding ? a.binding < c.binding : a.word < c.word; });
    return counts;
}

bool WordsEqualIgnoring(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, std::span<const std::pair<std::uint32_t, std::uint32_t>> ignored) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        if (((a[i] ^ b[i]) & ~IgnoredMaskAt(ignored, static_cast<std::uint32_t>(i))) != 0) return false;
    }
    return true;
}

bool SameDescriptorIgnoringTsharpBits(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> left, std::span<const std::uint32_t> right) {
    if (left.size() != right.size()) return false;
    const bool sampled = binding.role == ShaderRecompiler::DescriptorRole::GuestImages && binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] == right[i]) continue;
        const auto word = i % 8;
        const auto mask = !sampled ? 0u : word == 5 ? TsharpWord5IgnoredBits : word == 6 ? TsharpWord6IgnoredBits : 0u;
        if (((left[i] ^ right[i]) & ~mask) != 0) return false;
    }
    return true;
}

}
