#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::uint64_t NullPageBytes = 0x10000;
constexpr std::uint64_t ReaderPageBytes = 0x1000;

bool pendingProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profiled;
}

std::atomic<std::uint64_t> readsInPendingBlocks{0};
std::atomic<std::uint64_t> readsPastPendingBlocks{0};

// Whether a pending write overlaps the word at `address` (in a pending block): the exact ranges of
// the pending-write snapshot, the set the old capture's classifyPendingWrite and the flush hook
// test. It holds every range noted into the open, in-flight and finishing batches, and every
// range that marks a pending block is such a note (Recorder::noteWrite, noteWriteOn); a range
// leaves it when its batch finished, after its completions ran and its blocks cleared (or when the
// batch's fence failed: its work never lands, and the old path's reads stop waiting for it too).
// The snapshot the reader holds is reloaded whenever the publish generation moved since it was
// loaded (the generation moves right after each publish), so a read sees every range published
// before it. A range whose block mark is visible but whose publish is not yet is the noting
// thread's open note: it is published, fenced and only then submitted, so the word still holds
// what a read ordered before the note sees (the block test alone gives no more: a read just
// before the mark reads the word raw too).
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

FastPendingReads FastPendingReadTotals() {
    return {readsInPendingBlocks.load(std::memory_order_relaxed), readsPastPendingBlocks.load(std::memory_order_relaxed)};
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
    // reads as the old capture reads it (raw).
    if (Graphics::Recorder::BlockPending(address)) {
        const bool overlaps = !reader.exactPending || exactPendingOverlap(reader, address);
        if (pendingProfiled()) {
            readsInPendingBlocks.fetch_add(1, std::memory_order_relaxed);
            if (!overlaps) readsPastPendingBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        if (overlaps) {
            reader.declined = WalkDecline::Pending;
            return false;
        }
        ++reader.pendingPassed;
    }
    if (Graphics::Recorder::QueuedLabelOverlapsThisThread(address, sizeof(*value))) {
        reader.declined = WalkDecline::QueuedLabel;
        return false;
    }
    // A deferred label is written only when the packet records its labels: the old path captures
    // again after recording one over its reads (recordQueuedLabelsAfterCapture).
    if (reader.labels != nullptr) {
        for (const auto& label : *reader.labels) {
            if (address < label.address + label.size && label.address < address + sizeof(*value)) {
                reader.declined = WalkDecline::QueuedLabel;
                return false;
            }
        }
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
    std::memcpy(value, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(*value));
    return true;
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
