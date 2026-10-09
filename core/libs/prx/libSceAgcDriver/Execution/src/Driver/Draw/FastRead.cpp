#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint64_t NullPageBytes = 0x10000;
constexpr std::uint64_t ReaderPageBytes = 0x1000;

// Whether a pending write overlaps the word at `address` (in a pending block): the exact ranges of
// the pending-write snapshot, the set the old capture's classifyPendingWrite and the flush hook
// test. It holds every range noted into the open, in-flight and finishing batches, and every
// range that marks a pending block is such a note (Recorder::noteWrite, noteWriteOn); a range
// leaves it when its batch finished, after its completions ran and its blocks cleared (or when the
// batch's fence failed: its work never lands, and the old path's reads stop waiting for it too).
// The snapshot the reader holds is reloaded whenever the publish generation moved since it was
// loaded (the generation moves right after each publish), so a read sees every range published
// before it. A range whose block mark is visible but whose publish is not yet is a note in
// progress on the noting thread (which holds GuestMemory::GpuMutex). Into the open batch it is
// published, fenced and only then submitted, so the word still holds what a read ordered before
// the note sees. Into an in-flight batch (Recorder::noteWriteOn: a completion label appended to a
// submitted batch) the GPU may have stored the word already; the old capture's PendingView loads
// the same snapshot in that window and reads the word raw as well (classifyPendingWrite: no
// overlap, None), so this read gives what the old path gives. The block test alone was stricter
// than the old path there.
bool exactPendingOverlap(FastReader& reader, std::uint64_t address) {
    const auto generation = Graphics::Recorder::PublishGeneration();
    if (!reader.snapshotLoaded || generation != reader.snapshotGeneration) {
        reader.snapshot = Graphics::Recorder::PendingWriteSnapshot();
        reader.snapshotGeneration = generation;
        reader.snapshotLoaded = true;
    }
    return Graphics::Recorder::SnapshotOverlaps(reader.snapshot.get(), address, sizeof(std::uint32_t));
}

// The registered region holding the word, served from its bytes as ShaderMemory serves it; false
// when no region holds it. `boundary`: a region holds the address but not the whole word.
bool readRegion(std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t address, std::uint32_t* value, bool& boundary) {
    for (const auto& region : regions) {
        if (address < region.guestAddress || address - region.guestAddress >= region.bytes.size()) continue;
        const auto offset = static_cast<std::size_t>(address - region.guestAddress);
        if (region.bytes.size() - offset < sizeof(*value)) {
            boundary = true;
            return false;
        }
        std::memcpy(value, region.bytes.data() + offset, sizeof(*value));
        return true;
    }
    return false;
}

// A sampled image's T# words a hit may differ in (the streaming-feedback fields: word 5 bit 25,
// word 6 bits 0-7; DispatchVariant::ignoredBits), which the old path keeps as stored.
std::uint32_t feedbackBits(std::size_t word) {
    if (word % 8 == 5) return 1u << 25u;
    if (word % 8 == 6) return 0xffu;
    return 0;
}

std::atomic<FastPendingQuery> installedQuery{nullptr};

void logServed(std::uint64_t address, std::uint32_t value, FastServedSource source) {
    auto& log = FastServedWords();
    if (log.enabled) log.words.push_back({address, value, source});
}

// This thread's queued labels over the word (FastSrtRead): true with `word` empty when there are
// none, true with the bytes of the last label in queue order when that label covers the whole word
// (and serving labels is on), false (declines) otherwise. The deferred list is the labels in queue
// order; the queued ranges (noted from that list) seen without a list entry over the word decline.
bool queuedLabelWord(const FastReader& reader, std::uint64_t address, std::optional<std::uint32_t>& word) {
    const DeferredLabel* last = nullptr;
    if (reader.labels != nullptr) {
        for (const auto& label : *reader.labels) {
            if (address < label.address + label.size && label.address < address + sizeof(std::uint32_t)) last = &label;
        }
    }
    if (last == nullptr) return !Graphics::Recorder::QueuedLabelOverlapsThisThreadUncounted(address, sizeof(std::uint32_t));
    if (!reader.knownValues || !reader.knownLabels || last->address > address || address + sizeof(std::uint32_t) > last->address + last->size || last->size > last->bytes.size()) return false;
    std::uint32_t value = 0;
    std::memcpy(&value, last->bytes.data() + (address - last->address), sizeof(value));
    word = value;
    return true;
}

// A word an exact pending range overlaps, after the reader's page checks: the old capture's word
// read on a word-wise page (ShaderMemory::read) under the same classification
// (Driver::fastPendingWord, queryPendingWrite's answer for the 4 bytes over the reader's
// snapshot). The answers that read without the flush hook serve; the rest decline "pending block",
// where the capture would wait for the writer. Words inside the last KnownValue range the query
// named reuse its answer while the snapshot generation, the writer push count and the recorder's
// write generation hold (the validation's per-region classification, over a whole range of one
// writer; the write generation moves with a label or store noted inside a range the snapshot
// already covers, which publishes nothing).
bool serveKnown(FastReader& reader, std::uint64_t address, std::uint32_t* value) {
    using Policy = ShaderMemory::PendingWrite;
    if (reader.knownBytes != nullptr && reader.knownBegin <= address && address + sizeof(*value) <= reader.knownEnd && reader.knownGeneration == reader.snapshotGeneration && reader.knownWriters == WrittenBufferPushes().load(std::memory_order_acquire) && reader.knownWrites == Graphics::Recorder::WriteGeneration()) {
        std::memcpy(value, reader.knownBytes->data() + (address - reader.knownBegin), sizeof(*value));
        ++reader.servedKnown;
        logServed(address, *value, FastServedSource::Known);
        return true;
    }
    PendingView view;
    view.snapshot = reader.snapshot;
    view.generation = reader.snapshotGeneration;
    view.loaded = true;
    FastPendingAnswer answer;
    reader.pendingQuery(address, view, answer);
    const auto live = [&] {
        std::uint32_t word = 0;
        std::memcpy(&word, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(word));
        return word;
    };
    switch (answer.policy) {
        case Policy::None:
            // Nothing pending over the word in the classification's view: the capture's hook read
            // finds nothing to wait for (the reader's page checks ran).
            *value = live();
            return true;
        case Policy::KnownValue:
            *value = answer.word;
            ++reader.servedKnown;
            logServed(address, *value, FastServedSource::Known);
            if (answer.rangeBytes != nullptr && answer.rangeBegin <= address && address + sizeof(*value) <= answer.rangeEnd && answer.rangeBytes->size() == answer.rangeEnd - answer.rangeBegin) {
                reader.knownBegin = answer.rangeBegin;
                reader.knownEnd = answer.rangeEnd;
                reader.knownBytes = std::move(answer.rangeBytes);
                reader.knownGeneration = reader.snapshotGeneration;
                reader.knownWriters = answer.writers;
                reader.knownWrites = answer.writes;
            }
            return true;
        case Policy::RawExpected: {
            // Read raw while the word still holds what the evidence saw last; another value is the
            // capture's hook read (it waits, and tells the evidence the word changed).
            if (!reader.knownEvidence) break;
            const auto word = live();
            if (word != answer.word) break;
            *value = word;
            ++reader.servedEvidence;
            logServed(address, *value, FastServedSource::Evidence);
            return true;
        }
        case Policy::Raw:
            if (!reader.knownEvidence) break;
            *value = live();
            ++reader.servedEvidence;
            logServed(address, *value, FastServedSource::Evidence);
            return true;
        default:
            // Sync, VerifyRaw, VerifyKnownValue: the capture reads through the hook.
            break;
    }
    reader.declined = WalkDecline::Pending;
    return false;
}

WalkMismatch mismatchOf(ShaderRecompiler::DescriptorRole role) {
    using Role = ShaderRecompiler::DescriptorRole;
    switch (role) {
        case Role::GuestBuffers: return WalkMismatch::Buffer;
        case Role::GuestImages: return WalkMismatch::Image;
        case Role::GuestSamplers: return WalkMismatch::Sampler;
        case Role::FlattenedSrt: return WalkMismatch::Flat;
        case Role::ShaderData: return WalkMismatch::Data;
        default: return WalkMismatch::Other;
    }
}

}

bool FastPendingExact() {
    static const bool exact = [] {
        const char* value = std::getenv("APS5_FAST_PENDING_BLOCKS");
        return value == nullptr || std::strcmp(value, "0") == 0;
    }();
    return exact;
}

bool FastSrtRead(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& reader = *static_cast<FastReader*>(context);
    ++reader.reads;
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        reader.declined = WalkDecline::Boundary;
        return false;
    }
    bool boundary = false;
    for (const auto& program : reader.programs) {
        if (readRegion(program.memory, address, value, boundary)) return true;
        if (boundary) break;
    }
    if (!boundary && readRegion(reader.regions, address, value, boundary)) return true;
    if (boundary) {
        reader.declined = WalkDecline::Boundary;
        return false;
    }
    if (address < NullPageBytes) {
        *value = 0;
        return true;
    }
    // The 64 KiB block is the prefilter; a word in a pending block that no pending range overlaps
    // reads as the old capture reads it (raw). One a range overlaps is served as the capture
    // serves it (serveKnown, after the checks below) or declines.
    bool pending = false;
    if (Graphics::Recorder::BlockPending(address)) {
        ++reader.pendingInBlocks;
        if (!reader.exactPending || exactPendingOverlap(reader, address)) {
            if (!reader.exactPending || !reader.knownValues || reader.pendingQuery == nullptr) {
                reader.declined = WalkDecline::Pending;
                return false;
            }
            pending = true;
        } else {
            ++reader.pendingPassed;
        }
    }
    // A label of this thread not recorded yet: the old capture reads its word through the flush
    // hook, which records the thread's labels (all of them, in queue order) and waits for them, or
    // the old path captures again after recording one over its reads
    // (recordQueuedLabelsAfterCapture). Either way the word then holds the last label's bytes,
    // recorded after every write pending over it (so the label decides a pending word too); the
    // packet records the labels before its draw or dispatch. Anything short of the last label
    // covering the word declines. The uncounted query: the [labels] line's "capture pages read
    // word-wise over a queued label" counts the old capture's page queries only.
    std::optional<std::uint32_t> labelWord;
    if (!queuedLabelWord(reader, address, labelWord)) {
        reader.declined = WalkDecline::QueuedLabel;
        return false;
    }
    const auto page = address & ~(ReaderPageBytes - 1);
    if (page != reader.page) {
        // The old capture's page read runs the flush hook (GuestMemory::FlushGpuWrites), which
        // stores the storage-image results pending over the page (pending or flushing images) and
        // publishes its unit shadows first; this reader has none, so such a page is the old path's
        // (the hook-free checks VariantValidation makes).
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{page, page + ReaderPageBytes}}};
        if (Graphics::StorageTexture::AnyPendingOverlaps(range) || Graphics::AnyShadowedOverlaps(page, ReaderPageBytes)) {
            reader.declined = WalkDecline::PendingStorage;
            return false;
        }
        bool queried = false;
        const bool readable = GuestMemory::ReadableWord(address, &queried);
        if (queried) ++reader.queries;
        if (!readable) {
            reader.declined = WalkDecline::Unmapped;
            return false;
        }
        reader.page = page;
    }
    if (labelWord) {
        *value = *labelWord;
        ++reader.servedLabels;
        logServed(address, *value, FastServedSource::Label);
        return true;
    }
    if (pending) return serveKnown(reader, address, value);
    std::memcpy(value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(*value));
    return true;
}

bool FastKnownValues() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FAST_KNOWN_VALUES");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool FastKnownEvidence() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FAST_KNOWN_EVIDENCE");
        return FastKnownValues() && (value == nullptr || std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

bool FastKnownLabels() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FAST_KNOWN_LABELS");
        return FastKnownValues() && (value == nullptr || std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

void SetFastPendingQuery(FastPendingQuery query) {
    installedQuery.store(query, std::memory_order_release);
}

FastPendingQuery InstalledFastPendingQuery() {
    return installedQuery.load(std::memory_order_acquire);
}

FastServedLog& FastServedWords() {
    static thread_local FastServedLog log;
    return log;
}

FastServedLogScope::FastServedLogScope(bool enable) : previous(FastServedWords().enabled) {
    auto& log = FastServedWords();
    if (enable) {
        log.words.clear();
        log.enabled = true;
    }
}

FastServedLogScope::~FastServedLogScope() {
    FastServedWords().enabled = previous;
}

void CheckServedWords(std::span<const FastServedWord> words, const char* what, FastServedCheck& check) {
    static std::atomic<std::uint32_t> printed{0};
    for (const auto& served : words) {
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(served.address)), sizeof(std::uint32_t))) {
            ++check.unreadable;
            continue;
        }
        // Through the flush hook: this thread's labels over the word are recorded, the recorded
        // work writing it is waited for, the storage results over it are stored.
        std::uint32_t final = 0;
        GuestMemory::Read(served.address, std::as_writable_bytes(std::span(&final, 1)), alignof(std::uint32_t));
        ++check.compared;
        if (final == served.value) continue;
        ++check.mismatched[static_cast<std::size_t>(served.source)];
        if (printed.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[fastpath] %s: the word at 0x%llx served from %s as %08x holds %08x once its pending writes landed\n", what, static_cast<unsigned long long>(served.address), FastServedSourceNames[static_cast<std::size_t>(served.source)], served.value, final);
    }
}

std::uint64_t CompareServedWords(std::span<const FastServedWord> words, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& unread) {
    std::uint64_t differing = 0;
    for (const auto& served : words) {
        std::uint32_t old = 0;
        bool boundary = false;
        if (!readRegion(regions, served.address, &old, boundary)) {
            ++unread;
            continue;
        }
        if (old != served.value) ++differing;
    }
    return differing;
}

std::string FastPendingServedText(const FastPendingReads& reads) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::string switches;
    if (!FastKnownValues()) switches = " (APS5_FAST_KNOWN_VALUES=0)";
    else if (!FastKnownEvidence() || !FastKnownLabels()) switches = std::string(" (") + (!FastKnownEvidence() ? "APS5_FAST_KNOWN_EVIDENCE=0" : "") + (!FastKnownEvidence() && !FastKnownLabels() ? ", " : "") + (!FastKnownLabels() ? "APS5_FAST_KNOWN_LABELS=0" : "") + ")";
    char text[256];
    std::snprintf(text, sizeof(text), "; pending words served from known values %llu, on write evidence %llu; queued-label words served %llu%s", count(reads.known), count(reads.evidence), count(reads.labels), switches.c_str());
    return text;
}

WalkDecline WalkDeclineOf(ShaderRecompiler::WalkStatus status, const FastReader& reader) {
    using ShaderRecompiler::WalkStatus;
    switch (status) {
        case WalkStatus::NoSource: return WalkDecline::NoSource;
        case WalkStatus::IncompletePlan: return WalkDecline::IncompletePlan;
        case WalkStatus::NoProgram: return WalkDecline::NoProgram;
        case WalkStatus::Bindless: return WalkDecline::Bindless;
        case WalkStatus::UnsupportedRoot: return WalkDecline::UnsupportedRoot;
        case WalkStatus::OpFailed: return WalkDecline::OpFailed;
        default: return reader.declined.value_or(WalkDecline::Failed);
    }
}

std::uint32_t CompareWalkedResults(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, WalkDifference& first, std::uint64_t& feedbackOnly, std::uint64_t& flatFeedback, std::uint64_t& deferredSkipped, bool deferredMismatch) {
    std::uint32_t kinds = 0;
    const auto note = [&](WalkMismatch kind, std::size_t binding, std::size_t word, std::uint32_t oldWord, std::uint32_t walkedWord) {
        if (kinds == 0) first = {kind, binding, word, oldWord, walkedWord};
        kinds |= 1u << static_cast<unsigned>(kind);
    };
    if (old.variantId != walked.variantId) note(WalkMismatch::Variant, 0, 0, static_cast<std::uint32_t>(old.variantId), static_cast<std::uint32_t>(walked.variantId));
    if (old.bindings.size() != walked.bindings.size()) {
        note(WalkMismatch::Layout, old.bindings.size(), 0, 0, static_cast<std::uint32_t>(walked.bindings.size()));
        return kinds;
    }
    for (std::size_t index = 0; index < old.bindings.size(); ++index) {
        const auto& left = old.bindings[index];
        const auto& right = walked.bindings[index];
        if (left.kind != right.kind || left.role != right.role || left.descriptorSet != right.descriptorSet || left.binding != right.binding || left.count != right.count || left.guestDescriptor.size() != right.guestDescriptor.size()) {
            note(WalkMismatch::Layout, index, 0, 0, 0);
            continue;
        }
        // Walked results have no deferredWords (WalkResources clears deferPureLeaf), so only the
        // old side's are looked at (a `right.deferredWords.empty()` term would always hold;
        // tests/Submit.cpp pins both sides).
        if (deferredMismatch && !left.deferredWords.empty()) {
            const std::size_t word = left.deferredWords.front().first;
            const bool inside = word < left.guestDescriptor.size();
            note(WalkMismatch::Deferred, index, word, inside ? left.guestDescriptor[word] : 0, inside ? right.guestDescriptor[word] : 0);
            continue;
        }
        const bool image = left.role == ShaderRecompiler::DescriptorRole::GuestImages && left.guestDescriptor.size() % 8 == 0;
        bool feedback = false;
        for (std::size_t word = 0; word < left.guestDescriptor.size(); ++word) {
            const auto a = left.guestDescriptor[word];
            const auto b = right.guestDescriptor[word];
            if (a == b) continue;
            // A word the old capture left to the GPU holds a placeholder there.
            if (left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt && std::any_of(left.deferredWords.begin(), left.deferredWords.end(), [&](const auto& deferred) { return deferred.first == word; })) {
                ++deferredSkipped;
                continue;
            }
            // The flat copy of a sampled T# word the old stage compare accepted through its
            // don't-care bits (FlatTsharpFeedbackCopy): the hit binds the stored word, the walk the
            // live one (t387: every flat mismatch, plain and heuristic, was this).
            if (left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt && FlatTsharpFeedbackCopy(old.bindings, walked.bindings, a, b)) {
                ++flatFeedback;
                continue;
            }
            if (image && ((a ^ b) & ~feedbackBits(word)) == 0) {
                feedback = true;
                continue;
            }
            note(mismatchOf(left.role), index, word, a, b);
            break;
        }
        if (feedback) ++feedbackOnly;
    }
    if (old.pushConstants != walked.pushConstants) {
        std::size_t at = 0;
        while (at < old.pushConstants.size() && at < walked.pushConstants.size() && old.pushConstants[at] == walked.pushConstants[at]) ++at;
        note(WalkMismatch::Push, 0, at, 0, 0);
    }
    return kinds;
}

std::uint32_t CompareWalkedResult(const ShaderRecompiler::RecompileResult& old, const ShaderRecompiler::RecompileResult& walked, bool strictDeferred) {
    WalkDifference first;
    std::uint64_t feedbackOnly = 0;
    std::uint64_t flatFeedback = 0;
    std::uint64_t deferredSkipped = 0;
    return CompareWalkedResults(old, walked, first, feedbackOnly, flatFeedback, deferredSkipped, strictDeferred);
}

std::span<const char* const> FastWalkMismatchNames() {
    return WalkMismatchNames;
}

}
