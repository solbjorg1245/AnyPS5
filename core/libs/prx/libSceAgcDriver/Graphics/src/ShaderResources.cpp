#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ScratchLease.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libc/include/General.hpp"
#include "Optimization/include/Optimization/ShaderStageInputInfo.hpp"
#include "Optimization/include/Optimization/ResourceMaterializer.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include "prx/libc/include/GuestAllocations.hpp"

namespace AgcDriver::Graphics {
namespace {

bool overlap(std::uint64_t first, std::size_t firstSize, std::uint64_t second, std::size_t secondSize) {
    return first < second + secondSize && second < first + firstSize;
}

VkComponentSwizzle ComponentSwizzleFor(std::uint8_t dstSel) {
    switch (dstSel) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        case 7: return VK_COMPONENT_SWIZZLE_A;
        default: throw std::runtime_error("AGC graphics: guest texture descriptor has an invalid destination channel selector " + std::to_string(dstSel));
    }
}

// Sampled textures are reused across draws and dispatches while their guest bytes are unchanged; a
// byte copy of the guest surface validates each reuse, so CPU or GPU writes to it force a re-upload.
// The key is everything a lookup matches: the device, the eight descriptor words and the swizzle.
struct TextureKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    std::array<std::uint32_t, 4> components;
    bool depthCompare = false;
    bool operator==(const TextureKey&) const = default;
};

std::uint64_t hashWords(std::uint64_t hash, const std::uint32_t* words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
    return hash;
}

struct TextureKeyHash {
    std::size_t operator()(const TextureKey& key) const noexcept {
        auto hash = 14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device);
        hash = hashWords(hash, key.words.data(), key.words.size()) ^ static_cast<std::uint64_t>(key.depthCompare);
        return static_cast<std::size_t>(hashWords(hash, key.components.data(), key.components.size()));
    }
};

// The sampled-image T#s' texture-streaming feedback fields (word 5 bit 25, word 6 bits 0-7; see
// IgnoredWordBits in ShaderMemory.hpp) are left out of the key: the title toggles them per frame,
// which keyed one surface's view twice and re-snapshotted it on the second. APS5_NO_TSHARP_MASK=1
// keys on the words as given.
bool TsharpKeyMask() {
    static const bool mask = std::getenv("APS5_NO_TSHARP_MASK") == nullptr;
    return mask;
}

TextureKey MakeTextureKey(VkDevice device, std::span<const std::uint32_t> words, VkComponentMapping components, bool depthCompare = false) {
    TextureKey key{device, {}, {static_cast<std::uint32_t>(components.r), static_cast<std::uint32_t>(components.g), static_cast<std::uint32_t>(components.b), static_cast<std::uint32_t>(components.a)}};
    key.depthCompare = depthCompare;
    std::copy(words.begin(), words.end(), key.words.begin());
    if (TsharpKeyMask() && words.size() == 8) {
        key.words[5] &= ~TsharpWord5IgnoredBits;
        key.words[6] &= ~TsharpWord6IgnoredBits;
    }
    return key;
}

struct CachedTexture {
    TextureKey key;
    std::uint64_t address;
    // The guest bytes the snapshot was made from, shared by entries that view the same image (see
    // Texture::SharesImageWith); empty for a view of a storage image.
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::shared_ptr<Texture> texture;
    // A fast-cleared surface is cached as its clear texels; it stays valid while the keys are unchanged.
    DccKeys keys = DccKeys::Uncompressed;
    // Write generation the snapshot is known current at (see GuestMemory::CollectWrites).
    std::uint64_t generation = 0;
    // A texture viewing a storage image on the GPU has no snapshot; it stays valid while that image
    // is still its surface's cached image and nothing wrote the guest memory since it matched.
    std::shared_ptr<StorageTexture> source;
    std::uint64_t sourceVersion = 0;
    std::uint64_t accounted = 0;
};

// Entries in use order (front = most recent) with a hash index by key: a lookup is O(1) and the
// eviction takes the back. APS5_NO_TEXTURE_HASH=1 finds entries by scanning the list (the index is
// still kept), the linear lookup of before.
struct TextureCache {
    HostMutex mutex;
    std::list<CachedTexture> entries;
    std::unordered_map<TextureKey, std::list<CachedTexture>::iterator, TextureKeyHash> index;
    // Per surface address, the newest entry whose snapshot image other keys of the surface may view.
    std::unordered_map<std::uint64_t, std::list<CachedTexture>::iterator> surfaces;
    std::uint64_t bytes = 0;
    // Of `bytes`, the entries of LargeTextureBytes and more (APS5_TEXTURE_LARGE_MIB bounds them).
    std::uint64_t largeBytes = 0;
};

constexpr std::uint64_t LargeTextureBytes = 64ull << 20u;

TextureCache& Textures() {
    static TextureCache cache;
    return cache;
}

bool TextureHashEnabled() {
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_HASH") != nullptr;
    return !disabled;
}

std::list<CachedTexture>::iterator findTexture(TextureCache& cache, const TextureKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

void eraseTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.bytes -= it->accounted;
    if (it->accounted >= LargeTextureBytes) cache.largeBytes -= it->accounted;
    cache.index.erase(it->key);
    if (const auto surface = cache.surfaces.find(it->address); surface != cache.surfaces.end() && surface->second == it) cache.surfaces.erase(surface);
    cache.entries.erase(it);
}

// Moves an entry to the front (most recently used).
void touchTexture(TextureCache& cache, std::list<CachedTexture>::iterator it) {
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
}

// APS5_PROFILE_DRAW: what the sampled-texture and storage-image lookups did, printed as [textures]
// every 10 s (cumulative). Storage-sourced sampled textures need no CPU read; snapshots do, and a
// pending read is a snapshot compare or read made while recorded work still wrote the surface (the
// flush hook may find that work already signaled, so this bounds the hook syncs from above; the
// [hooksync] line attributes the actual waits).
struct TextureCounters {
    std::atomic<std::uint64_t> fromStorage{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> pendingReads{0};
    std::atomic<std::uint64_t> storageFallbacks{0};
    std::atomic<std::uint64_t> records{0};
    std::atomic<std::uint64_t> fastHits{0};
    std::atomic<std::uint64_t> fastMisses{0};
    std::atomic<std::uint64_t> storageHits{0};
    std::atomic<std::uint64_t> storageCreated{0};
    // Storage images a Revalidate refreshed directly instead of through the lookups (T1, see
    // ShaderResources::refreshOwnObjects).
    std::atomic<std::uint64_t> ownRefreshes{0};
    // Entries dropped to stay inside the byte budget, and entries a lookup replaced because the surface
    // changed (memory, keys, source): which of the two makes the snapshots.
    std::atomic<std::uint64_t> budgetEvictions{0};
    // Their accounted bytes, and how many of them were 64 MiB or more.
    std::atomic<std::uint64_t> evictedBytes{0};
    std::atomic<std::uint64_t> largeEvictions{0};
    std::atomic<std::uint64_t> replaced{0};
    std::atomic<std::uint64_t> cachedBytes{0};
    std::atomic<std::uint64_t> sameSurface{0};
    std::atomic<std::uint64_t> sharedImages{0};
    std::atomic<std::int64_t> lastReport{0};
};

TextureCounters& TextureCounts() {
    static TextureCounters counters;
    return counters;
}

// Sampled elements the driver could not decode and bound as null (see decodeBoundSampled).
std::atomic<std::uint64_t> sampledNullBound{0};

void reportTextureCounters() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& counters = TextureCounts();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto count = [](const std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.load(std::memory_order_relaxed)); };
    std::fprintf(stderr, "[textures] sampled created: %llu from storage images, %llu snapshots (%llu snapshot reads over recorded writes inside cachedTexture, %llu storage-path fallbacks); stage-A records %llu: %llu fast hits, %llu full lookups; storage images %llu hits, %llu created, %llu own-object refreshes; cache %llu MiB, %llu budget evictions (%llu MiB, %llu of 64 MiB or more), %llu replaced, %llu made beside another key of the surface, %llu views of a shared image\n", count(counters.fromStorage), count(counters.snapshots), count(counters.pendingReads), count(counters.storageFallbacks), count(counters.records), count(counters.fastHits), count(counters.fastMisses), count(counters.storageHits), count(counters.storageCreated), count(counters.ownRefreshes), count(counters.cachedBytes) >> 20u, count(counters.budgetEvictions), count(counters.evictedBytes) >> 20u, count(counters.largeEvictions), count(counters.replaced), count(counters.sameSurface), count(counters.sharedImages));
    // The image elements bound as null over these 10 s instead of dropping their draws (see
    // ShaderRecompiler::NullBoundImage and decodeBoundSampled); each was a thrown draw before.
    using ShaderRecompiler::NullBoundImage;
    using ShaderRecompiler::ResourceMaterializer;
    const auto taken = [](NullBoundImage reason) { return static_cast<unsigned long long>(ResourceMaterializer::TakeNullBound(reason)); };
    const auto direct = taken(NullBoundImage::Undecodable);
    const auto tableBits = taken(NullBoundImage::TableUndecodable);
    const auto tableUnreadable = taken(NullBoundImage::TableUnreadable);
    std::fprintf(stderr, "[draws] undecodable image elements bound as null (10 s%s): capture %llu (T# words 5-6), table entries %llu (words 5-6) + %llu (unreadable), driver decode %llu\n", ResourceMaterializer::NullUndecodable() ? "" : ", off: APS5_NO_NULL_UNDECODABLE", direct, tableBits, tableUnreadable, static_cast<unsigned long long>(sampledNullBound.exchange(0, std::memory_order_relaxed)));
}

// What the sampled-texture lookups on this thread proved their returned objects current against,
// taken by object into the ShaderResources being built or revalidated (captureValidation), which
// then repeats the proof from write stamps on its next Revalidate. The lookups run inside one build
// on one thread; records nobody takes (a build that threw, a resident target looked up outside a
// build, a thread that never captures) are dropped by the next capture or by the bound below.
struct LookupRecord {
    const void* object;
    GuestTextureResource resource;
    std::uint64_t bytes;
    DccKeys keys;
    std::uint64_t generation;
    const StorageTexture* source;
    // Served by a depth surface (cachedTexture's DepthSurfaceTexture answer).
    bool depth = false;
};

thread_local std::vector<LookupRecord> lookupLog;
// APS5_VERIFY_FAST_PROOFS=8: set around the walk beside an accepted proof, so captureValidation
// diffs only those records (see VerifyFastProofsMode).
thread_local bool diffRecordsNow = false;

void logLookup(const LookupRecord& record) {
    // The bound drops the oldest half, never everything: one build's lookups (a few dozen at most)
    // are the newest records, and a capture must find them even when the bound trips mid-build.
    constexpr std::size_t bound = 1024;
    if (lookupLog.size() >= bound) lookupLog.erase(lookupLog.begin(), lookupLog.begin() + bound / 2);
    lookupLog.push_back(record);
}

std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip, std::uint64_t guestBytes = 0);

bool MetadataMoved(const StorageTexture& image, const GuestTextureResource& resource) {
    return resource.dccAddress != 0 && image.Descriptor().dccAddress != resource.dccAddress && !image.ServesKeysAt(resource.dccAddress);
}

// Whether a sampled texture over `resource` can be a view of the surface's cached storage image
// instead of a CPU snapshot (see cachedTexture): the format has a storage form and is not block
// compressed, and the surface lives in host-imported memory, where the image uploads and refreshes
// GPU-direct (a snapshot of such a surface reads and compares its bytes on the CPU, waiting for
// the recorded work that wrote them). APS5_NO_SAMPLED_FROM_STORAGE=1 keeps every sampled texture a
// snapshot as before.
bool SampledFromStorageEligible(const Context& context, std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes) {
    static const bool disabled = std::getenv("APS5_NO_SAMPLED_FROM_STORAGE") != nullptr;
    if (disabled || IsBlockCompressed(format) || !StorageFormatAvailable(context, format)) return false;
    return HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
}

bool SampledFromStorageEligible(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    return SampledFromStorageEligible(context, resource.format, resource.baseAddress, guestBytes);
}

// Whether a sampled texture over a fast-cleared, storage-eligible surface views the cleared storage
// image (see cachedTexture). APS5_NO_CLEARED_VIEW=1 keeps such surfaces snapshots, as before.
bool ClearedViewEnabled() {
    static const bool disabled = std::getenv("APS5_NO_CLEARED_VIEW") != nullptr;
    return !disabled;
}

// Surfaces whose storage image could not be made (no writable committed pages, say): remembered
// so the sampled lookup does not throw and fall back on every use. A failure is keyed by the surface
// (address and size: the heap reuses addresses) and holds only while the guest mappings are what
// they were when it failed (the allocation generation): pages committed later, or another surface
// at the address, get a fresh attempt.
struct StorageFailures {
    HostMutex mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> generations;
};

StorageFailures& StorageFailed() {
    static StorageFailures failures;
    return failures;
}

// The cached storage image for a sampled descriptor, or null when it cannot be made (then the
// snapshot path serves the descriptor, as before). Every null return counts as a fallback.
std::shared_ptr<StorageTexture> sampledStorageSource(const Context& context, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    auto& counters = TextureCounts();
    auto& failures = StorageFailed();
    const auto surface = std::make_pair(resource.baseAddress, guestBytes);
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    {
        std::lock_guard lock(failures.mutex);
        if (const auto it = failures.generations.find(surface); it != failures.generations.end()) {
            if (it->second == generation) {
                counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            failures.generations.erase(it);
        }
    }
    try {
        return cachedStorageTexture(context, {}, resource, 0, guestBytes);
    } catch (const std::exception& error) {
        counters.storageFallbacks.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(failures.mutex);
        if (failures.generations.size() < 4096 && failures.generations.emplace(surface, generation).second) std::fprintf(stderr, "[textures] sampled texture 0x%llx (%ux%u format %u) keeps the snapshot path: %s\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, error.what());
        return nullptr;
    }
}

// `guestBytes` is the surface size when the caller described the surface already (0: described here).
void ReportUndecodedTexture(std::span<const std::uint32_t> words, const char* reason) {
    static HostMutex mutex;
    static std::set<std::vector<std::uint32_t>> seen;
    {
        std::lock_guard lock(mutex);
        if (seen.size() >= 64 || !seen.emplace(words.begin(), words.end()).second) return;
    }
    std::string chain;
    const auto source = ShaderMemory::LocateRecentWords(words, &chain);
    std::string text;
    char word[16];
    for (const auto value : words) {
        std::snprintf(word, sizeof(word), " %08x", value);
        text += word;
    }
    std::fprintf(stderr, "[gpu] undecodable T#%s read at 0x%llx (%s); reads before:%s\n", text.c_str(), static_cast<unsigned long long>(source), reason, chain.c_str());
}

// A null T# (base address 0: the game's own, or one the recompiler rejected as no descriptor, see
// validImageDescriptor) reads zeros on hardware and drops stores. It binds a zeroed 1x1 linear
// RGBA8 surface of the binding's shape in driver memory: one for sampled reads and one for storage,
// so stores never reach what sampled reads see.
bool NullTextureWords(std::span<const std::uint32_t> words) {
    return words.size() >= 2 && words[0] == 0 && (words[1] & 0xffu) == 0;
}

std::array<std::uint32_t, 8> NullTextureDescriptor(std::optional<ShaderRecompiler::DescriptorImageShape> shape, bool storage) {
    alignas(256) static std::array<std::byte, 256> sampledZeros{};
    alignas(256) static std::array<std::byte, 256> storageZeros{};
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(storage ? storageZeros.data() : sampledZeros.data()));
    std::uint32_t type = 9;  // 2D
    if (shape == ShaderRecompiler::DescriptorImageShape::Image1D) type = 8;
    else if (shape == ShaderRecompiler::DescriptorImageShape::Image2DArray) type = 13;
    else if (shape == ShaderRecompiler::DescriptorImageShape::Image3D) type = 10;
    constexpr auto rgba8 = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format8_8_8_8UNorm);
    return {static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (rgba8 << 20u), 0u, 0xfacu | (type << 28u), 0u, 0u, 0u, 0u};
}

// A sampled element as the build bound it (resolveImageBinding): a null T# and one the driver
// cannot decode take the null texture's words (`words` then names `nullWords`). Walks repeating
// the build's lookups (Revalidate) decode through this so they meet the build's objects: decoding
// the guest words, they threw on such an element and dropped the draw. APS5_NO_NULL_UNDECODABLE=1
// decodes the guest words as before.
GuestTextureResource decodeBoundSampled(std::span<const std::uint32_t>& words, std::array<std::uint32_t, 8>& nullWords, std::optional<ShaderRecompiler::DescriptorImageShape> shape) {
    if (!ShaderRecompiler::ResourceMaterializer::NullUndecodable()) return DecodeTextureResource(words);
    if (!NullTextureWords(words)) {
        try {
            return DecodeTextureResource(words);
        } catch (const std::exception&) {
            // Bound as null by the build.
        }
    }
    nullWords = NullTextureDescriptor(shape, false);
    words = nullWords;
    return DecodeTextureResource(words);
}

std::shared_ptr<Texture> cachedTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components, std::uint64_t guestBytes = 0, bool depthCompare = false) {
    CaptureTrace::Log("sampled-lookup address=%llx width=%u height=%u dcc=%llx", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned long long>(resource.dccAddress));
    if (auto depth = DepthSurfaceTexture(context, words, resource, components)) {
        // Recorded so the fast proof covers the element (DepthSurfaceServes) instead of finding no
        // record and leaving the object to the full walk on every call. APS5_NO_DEPTH_RECORDS=1
        // leaves it unrecorded as before.
        static const bool record = std::getenv("APS5_NO_DEPTH_RECORDS") == nullptr;
        if (record) logLookup({depth.get(), resource, 0, DccKeys::Uncompressed, 0, nullptr, true});
        return depth;
    }
    const auto depthBitsWidth = words.size() >= 4 ? ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) : 0u;
    if (depthBitsWidth == 32u) {
        char text[160];
        std::snprintf(text, sizeof(text), "AGC graphics: 32-bit integer read of the depth-layout texture 0x%llx, which is no depth surface drawn with, is not implemented", static_cast<unsigned long long>(resource.baseAddress));
        throw std::runtime_error(text);
    }
    constexpr auto unorm16 = static_cast<std::uint32_t>(ShaderRecompiler::IrBufferFormat::Format16UNorm);
    if (depthBitsWidth == 16u && resource.format != unorm16) {
        auto normalized = resource;
        normalized.format = unorm16;
        return cachedTexture(context, words, normalized, components, guestBytes, depthCompare);
    }
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    const bool profile = LookupOutcomes::Profiled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto scanKeys = [&] {
        const auto scanStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto keys = TextureClearKeys(resource, guestBytes);
        if (profile && resource.dccAddress != 0) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
        return keys;
    };
    if (guestBytes == 0) guestBytes = DescribeSurface(resource).guestBytes;
    // No surface outgrows the console's 16 GiB: a larger size is a descriptor decoded wrongly (or
    // garbage), whose CPU snapshot below would fail to allocate. The draw is skipped with the words
    // that describe it (0x2128000000 bytes recurred in some Boletaria runs).
    if (guestBytes > (16ull << 30u)) {
        char text[768];
        int length = std::snprintf(text, sizeof(text), "AGC graphics: texture 0x%llx describes 0x%llx bytes (%ux%u depth/last array %u base array %u, mips %u base %u last %u, format %u, tile %d, dim %d); words",
                                   static_cast<unsigned long long>(resource.baseAddress), static_cast<unsigned long long>(guestBytes), resource.width, resource.height, resource.depthOrLastArray, resource.baseArray,
                                   resource.mipCount, resource.baseLevel, resource.lastLevel, resource.format, static_cast<int>(resource.tileMode), static_cast<int>(resource.dimension));
        for (std::size_t i = 0; i < words.size() && length > 0 && length < static_cast<int>(sizeof(text)) - 10; ++i) length += std::snprintf(text + length, sizeof(text) - length, " %08x", words[i]);
        // Where this thread's capture read the words, and what that memory holds now.
        std::string chain;
        const auto source = ShaderMemory::LocateRecentWords(words, &chain);
        if (length > 0 && length < static_cast<int>(sizeof(text)) - 40) {
            if (source == 0) length += std::snprintf(text + length, sizeof(text) - length, "; not among this thread's recent capture reads");
            else {
                length += std::snprintf(text + length, sizeof(text) - length, "; read at 0x%llx, now", static_cast<unsigned long long>(source));
                std::array<std::uint32_t, 8> now{};
                const auto bytes = std::min(words.size(), now.size()) * sizeof(std::uint32_t);
                if (GuestMemory::Accessible(reinterpret_cast<const void*>(source), bytes)) {
                    std::memcpy(now.data(), reinterpret_cast<const void*>(source), bytes);
                    for (std::size_t i = 0; i < bytes / sizeof(std::uint32_t) && length > 0 && length < static_cast<int>(sizeof(text)) - 10; ++i) length += std::snprintf(text + length, sizeof(text) - length, " %08x", now[i]);
                }
            }
        }
        if (!chain.empty()) std::fprintf(stderr, "[gpu] capture reads before texture 0x%llx's descriptor at 0x%llx:%s\n", static_cast<unsigned long long>(resource.baseAddress), static_cast<unsigned long long>(source), chain.c_str());
        throw std::runtime_error(text);
    }
    auto& counters = TextureCounts();
    const auto address = resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(guestBytes);
    // A storage image whose results for this surface (or for a mip chain containing it) are still on
    // the GPU supplies the texture by a view of it; anything else needs those results in guest
    // memory first.
    auto source = StorageTexture::FindPending(address, guestBytes);
    if (source != nullptr && (depthCompare || !Texture::CanCopyFrom(*source, resource) || MetadataMoved(*source, resource))) source.reset();
    std::optional<DccKeys> keys;
    bool clearThroughKeys = false;
    if (source != nullptr && resource.dccAddress != 0 && IsDccClear(source->FilledKeys())) {
        keys = scanKeys();
        if (*keys == source->FilledKeys()) {
            std::array<std::byte, 16> probe{};
            if (!FillDccClear(ResolveTextureFormat(resource.format), *keys, resource.dccAlphaOnMsb, probe)) {
                char text[256];
                std::snprintf(text, sizeof(text), "AGC graphics: sampled texture 0x%llx (%ux%u format %u, dcc 0x%llx) reads %s DCC keys filled over its pending image, a clear value the format has no encoding for", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
                throw std::runtime_error(text);
            }
            static const bool traceKeys = std::getenv("APS5_TRACE_DCC_KEYS") != nullptr;
            if (traceKeys) std::fprintf(stderr, "[dcc-keys] sampled 0x%llx through keys 0x%llx reads the %s fill, not the pending image\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(resource.dccAddress), DccKeysName(*keys));
            source.reset();
            clearThroughKeys = true;
        }
    }

    // Otherwise a surface in host-imported memory is viewed through its cached storage image (made
    // here when there is none): its refresh after a CPU or GPU write is a GPU-direct detile from the
    // import, recorded behind the producer, so no bytes are read or compared on the CPU and nothing
    // waits for the producer. A fast-cleared surface (keys) is viewed only through an image whose own
    // descriptor carries the DCC address (below); it stays a snapshot otherwise, its texels not read.
    if (!depthCompare && source == nullptr && !clearThroughKeys && SampledFromStorageEligible(context, resource, guestBytes)) {

        keys = scanKeys();
        if (*keys == DccKeys::Uncompressed) {
            source = sampledStorageSource(context, resource, guestBytes);
        } else if (ClearedViewEnabled() && StorageClearAvailable(context, resource.format, *keys)) {
            // A fast-cleared surface is viewed as well, through its image cleared on the GPU (the
            // lookup's Refresh, see StorageTexture::upload), when the image's own descriptor names
            // the same DCC metadata so the refresh sees the keys. A snapshot of the clear texels
            // (filled and uploaded on the CPU, then replaced by a view once results are pending: two
            // 4K uploads per clear at the movie stage) serves the surface otherwise, as before.
            auto candidate = sampledStorageSource(context, resource, guestBytes);
            if (candidate != nullptr && candidate->Descriptor().dccAddress == resource.dccAddress) source = std::move(candidate);
        }
        // The lookup's Refresh may have flushed another image's results over the memory (which
        // marks the keys uncompressed): a surface that stays a snapshot re-reads them behind that
        // flush, as it does behind its own. APS5_NO_KEYS_RESCAN=1 keeps the keys scanned above.
        static const bool rescan = std::getenv("APS5_NO_KEYS_RESCAN") == nullptr;
        if (rescan && source == nullptr) keys.reset();
    }
    // Pages of the surface written since they were last collected are stamped now, before any
    // UnchangedSince (here and on a cache hit) looks at them: the checks only compare stamped blocks,
    // so a write the game made since the last walk is invisible until a collect stamps its page. The
    // walk is memoized per packet, so a texture viewed from a storage image repeats it for free.
    auto generation = GuestMemory::CollectWrites(address, bytes);
    if (source != nullptr && !GuestMemory::UnchangedSince(address, bytes, source->Generation())) {
        // The CPU wrote the memory while results were pending: Refresh merges them the usual way.
        source->Refresh();
    }
    // Results stored to guest memory just now are stamped newer than `generation`; a snapshot read
    // after them is current at the generation of a second (memoized) collect. The store marks the
    // surface's DCC keys uncompressed, so the keys are read after it.
    const auto flushStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (source == nullptr && !clearThroughKeys && StorageTexture::FlushPending(address, bytes, nullptr, "sampled texture")) {
        if (profile) LookupOutcomes::Add(LookupOutcomes::PendingFlush, flushStart);
        generation = GuestMemory::CollectWrites(address, bytes);
        keys.reset();
    }
    if (!keys.has_value()) keys = scanKeys();
    if (disabled) {
        if (source != nullptr) return std::make_shared<Texture>(context, source, resource, components);
        std::vector<std::byte> snapshot(bytes);
        ReadTextureSurface(resource, *keys, snapshot);
        return std::make_shared<Texture>(context, *context.detiler, resource, components, snapshot, depthCompare);
    }
    auto& cache = Textures();
    const auto key = MakeTextureKey(context.device, words, components, depthCompare);
    std::lock_guard lock(cache.mutex);
    if (auto it = findTexture(cache, key); it != cache.entries.end()) {
        if (it->source != nullptr) {
            // The view follows the storage image, whatever the GPU wrote to it since; guest memory
            // written meanwhile is taken in by refreshing the image. A fast clear the image cannot
            // see (keys, and no pending results to prefer) ends the view: a snapshot holds the clear.
            if ((source == nullptr || source == it->source) && (source != nullptr || *keys == DccKeys::Uncompressed) && StorageImageServesKeys(*it->source, resource.dccAddress)) {
                if (!GuestMemory::UnchangedSince(address, bytes, it->source->Generation())) it->source->Refresh();
                it->keys = *keys;
                touchTexture(cache, it);
                logLookup({it->texture.get(), resource, guestBytes, *keys, 0, it->source.get()});
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(*keys != DccKeys::Uncompressed ? LookupOutcomes::SampledHitClearedView : LookupOutcomes::SampledHitView, start);
                return it->texture;
            }
        } else if (source == nullptr && it->bytes != nullptr && it->bytes->size() == guestBytes && it->keys == *keys) {
            // Unwritten pages need no comparison, nor do blocks nobody stamped since the snapshot;
            // partially resident textures compare only committed pages. The compare goes through
            // the flush hook: it waits for recorded work over the surface (counted, and named for
            // the [hooksync] line).
            const auto equalsCommitted = [&] {
                if (Recorder::SnapshotWriteOverlaps(address, bytes)) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
                return GuestMemory::EqualsCommittedSince(address, *it->bytes, it->generation);
            };
            if (*keys != DccKeys::Uncompressed || GuestMemory::UnchangedSince(address, it->bytes->size(), it->generation) || equalsCommitted()) {
                // Debug aid: APS5_VERIFY_TEXTURE_HITS=<n> compares every n-th snapshot hit with guest
                // memory and names the surfaces whose bytes changed unseen by the write stamps.
                static const unsigned verifyEvery = [] { const char* v = std::getenv("APS5_VERIFY_TEXTURE_HITS"); return v != nullptr ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u; }();
                static std::atomic<std::uint64_t> verifyCount{0};
                if (verifyEvery != 0 && *keys == DccKeys::Uncompressed && verifyCount.fetch_add(1, std::memory_order_relaxed) % verifyEvery == 0) {
                    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
                    if (!GuestMemory::EqualsCommitted(address, *it->bytes)) {
                        static std::atomic<std::uint32_t> stale{0};
                        if (stale.fetch_add(1, std::memory_order_relaxed) < 300) {
                            // Where it differs: the first differing byte, how many 4 KiB pages
                            // differ, and the first page's mapping and tracker stamps.
                            std::vector<std::byte> current(it->bytes->size());
                            GuestMemory::ReadCommitted(address, current);
                            std::size_t first = current.size();
                            std::size_t pages = 0;
                            for (std::size_t page = 0; page < current.size(); page += 4096) {
                                const auto length = std::min<std::size_t>(4096, current.size() - page);
                                if (std::memcmp(current.data() + page, it->bytes->data() + page, length) == 0) continue;
                                ++pages;
                                if (first == current.size()) {
                                    first = page;
                                    while (first < page + length && current[first] == (*it->bytes)[first]) ++first;
                                }
                            }
                            const bool unchanged = GuestMemory::UnchangedSince(address, it->bytes->size(), it->generation);
                            std::fprintf(stderr, "[texture-stale] 0x%llx+0x%zx %ux%u format %u tile %d mips %u: snapshot differs from guest memory although unstamped since generation %llu (now %llu, unchanged %d); first difference +0x%zx, %zu pages differ; %s\n", static_cast<unsigned long long>(address), it->bytes->size(), resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.mipCount, static_cast<unsigned long long>(it->generation), static_cast<unsigned long long>(generation), unchanged ? 1 : 0, first, pages, first < current.size() ? GuestMemory::DescribePage(address + first).c_str() : "no difference on re-read");
                        }
                    }
                }
                it->generation = generation;
                touchTexture(cache, it);
                logLookup({it->texture.get(), resource, guestBytes, *keys, generation, nullptr});
                reportTextureCounters();
                if (profile) LookupOutcomes::Add(LookupOutcomes::SampledHitSnapshot, start);
                return it->texture;
            }
        }
        eraseTexture(cache, it);
        counters.replaced.fetch_add(1, std::memory_order_relaxed);
    }
    CachedTexture entry{key, address, nullptr, nullptr, *keys, generation};
    // A view of a storage image owns no texels (the storage cache accounts for the image): charging it
    // the surface's size let a few views of a large array (each mip or slice view is its own key)
    // evict every snapshot. APS5_ACCOUNT_VIEWS=1 charges views fully as before.
    static const bool accountViews = std::getenv("APS5_ACCOUNT_VIEWS") != nullptr;
    entry.accounted = source != nullptr && !accountViews ? std::min<std::uint64_t>(guestBytes, 1ull << 20u) : guestBytes;
    // Another key of a surface already snapshotted (a mip or slice view, another swizzle, a streaming
    // texture's moved base level) views that image while its snapshot is current, instead of reading
    // and uploading the surface again. It is charged a share of the surface: the image lives as long
    // as any view of it. APS5_NO_SHARED_TEXTURE_IMAGES=1 snapshots every key.
    static const bool shareImages = std::getenv("APS5_NO_SHARED_TEXTURE_IMAGES") == nullptr;
    bool shared = false;
    if (source == nullptr && shareImages && *keys == DccKeys::Uncompressed) {
        if (const auto found = cache.surfaces.find(address); found != cache.surfaces.end()) {
            auto& base = *found->second;
            if (base.bytes != nullptr && base.bytes->size() == guestBytes && base.keys == *keys && base.texture->SharesImageWith(resource, depthCompare)) {
                const auto current = [&] {
                    if (GuestMemory::UnchangedSince(address, base.bytes->size(), base.generation)) return true;
                    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::TextureCompare);
                    return GuestMemory::EqualsCommittedSince(address, *base.bytes, base.generation);
                };
                if (current()) {
                    base.generation = generation;
                    entry.bytes = base.bytes;
                    entry.texture = std::make_shared<Texture>(context, std::shared_ptr<const Texture>(base.texture), resource, components);
                    entry.accounted = guestBytes / 8u;
                    counters.sharedImages.fetch_add(1, std::memory_order_relaxed);
                    shared = true;
                }
            }
        }
    }
    if (shared) {
    } else if (source != nullptr) {
        entry.source = source;
        entry.sourceVersion = source->Version();
        entry.texture = std::make_shared<Texture>(context, source, resource, components);
        counters.fromStorage.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Snapshot before the upload so a write racing with it is caught by the next comparison.
        if (*keys == DccKeys::Uncompressed && Recorder::SnapshotWriteOverlaps(address, bytes)) counters.pendingReads.fetch_add(1, std::memory_order_relaxed);
        auto snapshot = std::make_shared<std::vector<std::byte>>(bytes);
        ReadTextureSurface(resource, *keys, *snapshot);
        static const bool traceTextures = std::getenv("APS5_TRACE_TEXTURES") != nullptr;
        if (traceTextures) {
            std::size_t nonzero = 0;
            for (std::size_t i = 0; i < snapshot->size(); i += 64) nonzero += (*snapshot)[i] != std::byte{0};
            std::fprintf(stderr, "[texture] 0x%llx %ux%u format %u tile %d: %zu of %zu sampled bytes nonzero\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), nonzero, snapshot->size() / 64);
        }
        entry.texture = std::make_shared<Texture>(context, *context.detiler, resource, components, *snapshot, depthCompare);
        entry.bytes = std::move(snapshot);
        counters.snapshots.fetch_add(1, std::memory_order_relaxed);
    }
    // The budget counts guest bytes of the cached surfaces (each also holds a GPU image and, for a
    // snapshot, a CPU copy). APS5_TEXTURE_CACHE_MIB sets it. Default 6 GiB: a Boletaria frame samples
    // more than 2 GiB, and at 2 GiB the LRU cycled (~3900 evictions and ~15 GiB re-uploaded per 10 s,
    // half the draw thread's time); 6 GiB of entries took ~10 GiB of video memory in total.
    static const std::uint64_t budget = [] {
        const char* value = std::getenv("APS5_TEXTURE_CACHE_MIB");
        const auto mib = value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
        return (mib != 0 ? mib : 6144ull) << 20u;
    }();
    while (!cache.entries.empty() && cache.bytes + entry.accounted > budget) {
        const auto victim = std::prev(cache.entries.end());
        counters.evictedBytes.fetch_add(victim->accounted, std::memory_order_relaxed);
        if (victim->accounted >= (64ull << 20u)) counters.largeEvictions.fetch_add(1, std::memory_order_relaxed);
        eraseTexture(cache, victim);
        counters.budgetEvictions.fetch_add(1, std::memory_order_relaxed);
    }
    // Debug aid (bisecting budget-dependent bugs by surface size): APS5_TEXTURE_LARGE_MIB=<n> keeps
    // the entries of 64 MiB and more within n MiB of their own, least recently used first.
    static const std::uint64_t largeBudget = [] {
        const char* value = std::getenv("APS5_TEXTURE_LARGE_MIB");
        return value != nullptr ? std::strtoull(value, nullptr, 10) << 20u : 0ull;
    }();
    if (largeBudget != 0 && entry.accounted >= LargeTextureBytes) {
        while (cache.largeBytes != 0 && cache.largeBytes + entry.accounted > largeBudget) {
            auto victim = std::prev(cache.entries.end());
            while (victim->accounted < LargeTextureBytes && victim != cache.entries.begin()) --victim;
            if (victim->accounted < LargeTextureBytes) break;
            counters.evictedBytes.fetch_add(victim->accounted, std::memory_order_relaxed);
            counters.largeEvictions.fetch_add(1, std::memory_order_relaxed);
            eraseTexture(cache, victim);
            counters.budgetEvictions.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (entry.accounted >= LargeTextureBytes) cache.largeBytes += entry.accounted;
    cache.bytes += entry.accounted;
    counters.cachedBytes.store(cache.bytes, std::memory_order_relaxed);
    auto texture = entry.texture;
    if (profile) {
        // An entry made while another one of the same surface is cached under different descriptor
        // words: the key churns (APS5_TRACE_TEXTURE_KEYS=1 names the words that differ).
        static const bool traceKeys = std::getenv("APS5_TRACE_TEXTURE_KEYS") != nullptr;
        static std::atomic<std::uint32_t> traced{0};
        for (const auto& other : cache.entries) {
            if (other.address != address) continue;
            counters.sameSurface.fetch_add(1, std::memory_order_relaxed);
            if (traceKeys && traced.fetch_add(1, std::memory_order_relaxed) < 200) {
                char diff[256] = "";
                std::size_t at = 0;
                for (std::size_t w = 0; w < other.key.words.size() && at < sizeof(diff); ++w) {
                    if (other.key.words[w] != entry.key.words[w]) at += static_cast<std::size_t>(std::snprintf(diff + at, sizeof(diff) - at, " w%zu %08x->%08x", w, other.key.words[w], entry.key.words[w]));
                }
                std::fprintf(stderr, "[texture-keys] 0x%llx %ux%u: another key cached (%s%s%s)\n", static_cast<unsigned long long>(address), resource.width, resource.height, diff, other.key.components != entry.key.components ? " swizzle" : "", other.key.depthCompare != entry.key.depthCompare ? " depth-compare" : "");
            }
            break;
        }
    }
    logLookup({texture.get(), resource, guestBytes, *keys, generation, source.get()});
    const bool ownsSnapshot = !shared && source == nullptr;
    cache.entries.push_front(std::move(entry));
    cache.index[key] = cache.entries.begin();
    if (ownsSnapshot) cache.surfaces[address] = cache.entries.begin();
    reportTextureCounters();
    if (profile) LookupOutcomes::Add(source != nullptr ? LookupOutcomes::SampledMadeView : LookupOutcomes::SampledMadeSnapshot, start);
    return texture;
}

// Storage images stay on the GPU between dispatches: while guest memory still holds what an image was
// last uploaded from or written back as, its next use skips the upload and detile.
struct StorageKey {
    VkDevice device;
    std::array<std::uint32_t, 8> words;
    bool operator==(const StorageKey&) const = default;
};

struct StorageKeyHash {
    std::size_t operator()(const StorageKey& key) const noexcept {
        return static_cast<std::size_t>(hashWords(14695981039346656037ull ^ reinterpret_cast<std::uintptr_t>(key.device), key.words.data(), key.words.size()));
    }
};

struct CachedStorageTexture {
    StorageKey key;
    std::uint32_t mip;
    std::shared_ptr<StorageTexture> texture;
};

// As TextureCache: use order with a hash index by key, plus one by image for StorageImageCached.
struct StorageTextureCache {
    HostMutex mutex;
    std::list<CachedStorageTexture> entries;
    std::unordered_map<StorageKey, std::list<CachedStorageTexture>::iterator, StorageKeyHash> index;
    std::unordered_map<const StorageTexture*, std::list<CachedStorageTexture>::iterator> byImage;
    std::uint64_t bytes = 0;
};

StorageTextureCache& StorageTextures() {
    static StorageTextureCache cache;
    return cache;
}

std::list<CachedStorageTexture>::iterator findStorage(StorageTextureCache& cache, const StorageKey& key) {
    if (TextureHashEnabled()) {
        const auto found = cache.index.find(key);
        return found == cache.index.end() ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key == key) return it;
    }
    return cache.entries.end();
}

std::list<CachedStorageTexture>::iterator findStorageByImage(StorageTextureCache& cache, VkDevice device, const StorageTexture* image) {
    if (TextureHashEnabled()) {
        const auto found = cache.byImage.find(image);
        return found == cache.byImage.end() || found->second->key.device != device ? cache.entries.end() : found->second;
    }
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        if (it->key.device == device && it->texture.get() == image) return it;
    }
    return cache.entries.end();
}

// Evicts an entry: its pending results go to guest memory first (the image may die with the entry).
void evictStorage(StorageTextureCache& cache, std::list<CachedStorageTexture>::iterator it) {
    it->texture->SetCached(false);
    it->texture->Flush();
    cache.bytes -= it->texture->GuestBytes();
    cache.index.erase(it->key);
    cache.byImage.erase(it->texture.get());
    cache.entries.erase(it);
}

// Storage images are shared by every descriptor of one surface (address, extent, layers, format, tile
// mode): the image holds the whole mip chain, and render targets in the same memory attach to it.
std::array<std::uint32_t, 8> SurfaceKey(const Context& context, const GuestTextureResource& resource) {
    // Guest formats that store in the same Vulkan format share the image (views carry the difference).
    return {static_cast<std::uint32_t>(resource.baseAddress), static_cast<std::uint32_t>(resource.baseAddress >> 32u), resource.width, resource.height, (resource.depthOrLastArray << 16u) | (resource.mipCount & 0xffffu), (static_cast<std::uint32_t>(resource.tileMode) << 12u) | (static_cast<std::uint32_t>(resource.dimension) << 20u), resource.baseArray, static_cast<std::uint32_t>(StorageFormatForGuest(context, resource.format))};
}

// `guestBytes` is the surface size when the caller described the surface already (0: described here).
std::shared_ptr<StorageTexture> cachedStorageTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip, std::uint64_t guestBytes) {
    if (DepthSurfaceAt(resource.baseAddress, resource.width, resource.height)) {
        char text[112];
        std::snprintf(text, sizeof(text), "AGC graphics: storage image access to depth/stencil surface 0x%llx is not implemented", static_cast<unsigned long long>(resource.baseAddress));
        throw std::runtime_error(text);
    }
    static const bool disabled = std::getenv("APS5_NO_TEXTURE_CACHE") != nullptr;
    if (disabled) return std::make_shared<StorageTexture>(context, *context.detiler, resource, mip);
    static_cast<void>(words);
    const bool profile = LookupOutcomes::Profiled();
    auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto& counters = TextureCounts();
    const StorageKey key{context.device, SurfaceKey(context, resource)};
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    auto it = findStorage(cache, key);
    if (it != cache.entries.end() && MetadataMoved(*it->texture, resource)) {
        evictStorage(cache, it);
        it = cache.entries.end();
    }
    if (it != cache.entries.end()) {
        it->texture->Refresh();
        cache.entries.splice(cache.entries.begin(), cache.entries, it);
        counters.storageHits.fetch_add(1, std::memory_order_relaxed);
        if (profile) LookupOutcomes::Add(LookupOutcomes::StorageHit, start);
        return it->texture;
    }
    // Results other images hold over this memory reach it before the new image reads it: a hit's
    // Refresh flushes them, the constructor's upload does not, and a GPU-direct upload reads the
    // import buffer without the flush hook. The flush is recorded ahead of the upload in the batch.
    if (guestBytes == 0) guestBytes = DescribeSurface(resource).guestBytes;
    if (StorageTexture::FlushPending(resource.baseAddress, static_cast<std::size_t>(guestBytes), nullptr, "storage image creation", PublishScope::None) && profile) start = LookupOutcomes::Add(LookupOutcomes::PendingFlush, start);
    CachedStorageTexture entry{key, mip, std::make_shared<StorageTexture>(context, *context.detiler, resource, mip)};
    // The constructor's upload may have recorded into the open batch (a GPU clear, a direct
    // detile) before the image could keep itself (no weak_from_this yet): the batch keeps it here,
    // so an eviction or a failed view before it ran cannot destroy a referenced image.
    // APS5_NO_KEEP_NEW_STORAGE=1 leaves the image to its cache entry alone, as before.
    static const bool keepNew = std::getenv("APS5_NO_KEEP_NEW_STORAGE") == nullptr;
    if (auto* recorder = Recorder::Active(); keepNew && recorder != nullptr && GuestMemory::GpuMutex().HeldByThisThread() && recorder->Recording()) recorder->Keep(entry.texture);
    constexpr std::uint64_t budget = 2048ull << 20u;
    while (!cache.entries.empty() && cache.bytes + entry.texture->GuestBytes() > budget) evictStorage(cache, std::prev(cache.entries.end()));
    cache.bytes += entry.texture->GuestBytes();
    auto texture = entry.texture;
    cache.entries.push_front(std::move(entry));
    cache.index[key] = cache.entries.begin();
    cache.byImage[texture.get()] = cache.entries.begin();
    texture->SetCached(true);
    counters.storageCreated.fetch_add(1, std::memory_order_relaxed);
    if (profile) LookupOutcomes::Add(LookupOutcomes::StorageMade, start);
    return texture;
}

}

void FlushCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot flush textures without a Vulkan device");
    GuestMemory::AssertGpuLockHeld("FlushCachedTextures");
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    for (const auto& entry : cache.entries) {
        if (entry.key.device == device) entry.texture->Flush();
    }
}

void ClearCachedTextures(VkDevice device) {
    Require(device != VK_NULL_HANDLE, "cannot clear textures without a Vulkan device");
    auto& sampled = Textures();
    {
        std::lock_guard lock(sampled.mutex);
        for (auto it = sampled.entries.begin(); it != sampled.entries.end();) {
            if (it->key.device == device) eraseTexture(sampled, it++);
            else ++it;
        }
    }
    auto& storage = StorageTextures();
    std::lock_guard lock(storage.mutex);
    for (auto it = storage.entries.begin(); it != storage.entries.end();) {
        if (it->key.device != device) {
            ++it;
            continue;
        }
        it->texture->SetCached(false);
        storage.bytes -= it->texture->GuestBytes();
        storage.index.erase(it->key);
        storage.byImage.erase(it->texture.get());
        it = storage.entries.erase(it);
    }
}

bool StorageImageCached(const Context& context, const StorageTexture* image) {
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    const auto it = findStorageByImage(cache, context.device, image);
    if (it == cache.entries.end()) return false;
    cache.entries.splice(cache.entries.begin(), cache.entries, it);
    return true;
}

namespace {

// StorageImageCached for several images under one acquisition of the cache mutex.
bool StorageImagesCached(const Context& context, std::span<const StorageTexture* const> images) {
    if (images.empty()) return true;
    auto& cache = StorageTextures();
    std::lock_guard lock(cache.mutex);
    for (const auto* image : images) {
        const auto it = findStorageByImage(cache, context.device, image);
        if (it == cache.entries.end()) return false;
        cache.entries.splice(cache.entries.begin(), cache.entries, it);
    }
    return true;
}

}

std::shared_ptr<StorageTexture> CachedStorageSurface(const Context& context, const GuestTextureResource& resource) {
    return cachedStorageTexture(context, {}, resource, 0);
}

bool StorageImageServesKeys(const StorageTexture& image, std::uint64_t dccAddress) {
    GuestTextureResource resource{};
    resource.dccAddress = dccAddress;
    return !MetadataMoved(image, resource);
}

namespace {

const char* roleName(ShaderRecompiler::DescriptorRole role) {
    switch (role) {
        case ShaderRecompiler::DescriptorRole::GuestBuffers: return "GuestBuffers";
        case ShaderRecompiler::DescriptorRole::GuestImages: return "GuestImages";
        case ShaderRecompiler::DescriptorRole::GuestSamplers: return "GuestSamplers";
        case ShaderRecompiler::DescriptorRole::Gds: return "Gds";
        case ShaderRecompiler::DescriptorRole::BdaPagetable: return "BdaPagetable";
        case ShaderRecompiler::DescriptorRole::FaultBuffer: return "FaultBuffer";
        case ShaderRecompiler::DescriptorRole::FlattenedSrt: return "FlattenedSrt";
        case ShaderRecompiler::DescriptorRole::ShaderData: return "ShaderData";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor role");
}

const char* kindName(ShaderRecompiler::DescriptorKind kind) {
    switch (kind) {
        case ShaderRecompiler::DescriptorKind::UniformBuffer: return "UniformBuffer";
        case ShaderRecompiler::DescriptorKind::StorageBuffer: return "StorageBuffer";
        case ShaderRecompiler::DescriptorKind::UniformTexelBuffer: return "UniformTexelBuffer";
        case ShaderRecompiler::DescriptorKind::StorageTexelBuffer: return "StorageTexelBuffer";
        case ShaderRecompiler::DescriptorKind::SampledImage: return "SampledImage";
        case ShaderRecompiler::DescriptorKind::StorageImage: return "StorageImage";
        case ShaderRecompiler::DescriptorKind::Sampler: return "Sampler";
    }
    throw std::runtime_error("AGC graphics: unknown descriptor kind");
}

// Consecutive identical storage descriptors address successive mips of one texture (dynamic-mip
// storage writes), so the second and later ones share the first one's image lookup.
// APS5_NO_STORAGE_DEDUPE=1 looks each one up (and refreshes the image) separately as before.
bool StorageDedupeEnabled() {
    static const bool disabled = std::getenv("APS5_NO_STORAGE_DEDUPE") != nullptr;
    return !disabled;
}

// Whether storage element `element` of `binding` repeats the previous element's eight words.
bool SameAsPreviousStorageElement(const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t element) {
    if (element == 0) return false;
    const auto words = binding.guestDescriptor.begin() + static_cast<std::size_t>(element) * 8u;
    return std::equal(words, words + 8, words - 8);
}

}

ShaderResources::ShaderResources(const Context& context, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes) : ShaderResources(context, std::array<CompiledShader, 2>{{{ShaderRecompiler::ShaderStage::Vertex, &vertex, 0}, {ShaderRecompiler::ShaderStage::Fragment, &fragment, static_cast<std::uint32_t>(vertex.pushConstants.size())}}}, target, indexAddress, indexBytes) {}

ShaderResources::ShaderResources(const Context& context, std::span<const CompiledShader> shaders, const ColorTarget& target, std::uint64_t indexAddress, std::size_t indexBytes, std::span<const GuestMemorySnapshot> snapshots) : context(context), guestMemory(context) {
    prepareAddressBindings(shaders, snapshots);
    build(shaders, &target, indexAddress, indexBytes);
}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots) : ShaderResources(context, compute, snapshots, false) {}

namespace {

// Whether a compute stage maps registered guest memory through BDA tables (see prepareAddressBindings).
bool UsesAddressTables(const CompiledShader& compute) {
    Require(compute.program != nullptr, "missing compiled shader");
    return std::any_of(compute.program->bindings.begin(), compute.program->bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
}

}

ShaderResources::ShaderResources(const Context& context, const CompiledShader& compute, std::span<const GuestMemorySnapshot> snapshots, bool deferred) : context(context), guestMemory(context), deferredCompute(compute), deferredSnapshots(snapshots) {
    Require(compute.stage == ShaderRecompiler::ShaderStage::Compute, "compute resources require a compute shader");
    // Every use of a compute build is a recorded dispatch that calls MarkGpuWrites, which staged
    // buffers need (a synchronous draw's use would not).
    guestMemory.AllowDeviceStaging();
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (!deferred) {
        prepareAddressBindings(shaders, snapshots);
        build(shaders, nullptr, 0, 0);
        forgetDeferredInputs();
        return;
    }
    // Address-based shaders pin and mirror registered memory in prepareAddressBindings (reconciling
    // imports retires buffers to the recorder, refreshing mirrors waits for recorded work): that is
    // device-lock work, so their whole build waits for Complete(). Without tables the call only
    // checks the bindings.
    lockedBuild = UsesAddressTables(compute);
    if (lockedBuild) return;
    unlockedPrepare = true;
    prepareAddressBindings(shaders, snapshots);
    buildPrepare(shaders, nullptr, 0, 0);
}

void ShaderResources::Complete() {
    Require(!completed, "shader resources were already completed");
    const std::span<const CompiledShader> shaders(&deferredCompute, 1);
    if (lockedBuild) {
        prepareAddressBindings(shaders, deferredSnapshots);
        buildPrepare(shaders, nullptr, 0, 0);
    }
    buildComplete();
    forgetDeferredInputs();
}

void ShaderResources::forgetDeferredInputs() {
    // The compiled shader and the captured regions belong to the dispatch, which returns while this
    // object lives on in the resource cache and the recorder: nothing may reach for them after the
    // build, so they are dropped rather than left dangling.
    deferredCompute = {};
    deferredSnapshots = {};
}

void ShaderResources::build(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes) {
    buildPrepare(shaders, target, indexAddress, indexBytes);
    buildComplete();
}

namespace {

// APS5_PROFILE_DRAW: the [resources] phase totals. Each thread accumulates its builds' phases in
// arrays of its own and merges them into the shared totals every 1000 of its builds (and when it
// ends), so no build takes the shared mutex per phase; the totals lag by up to 999 builds per
// worker.
constexpr std::size_t BuildPhaseCount = static_cast<std::size_t>(ShaderResources::BuildPhase::Count);
constexpr std::array<const char*, BuildPhaseCount> BuildPhaseNames{"bindings", "precollect", "guest memory upload", "descriptors", "stage A", "images", "bda", "stage B"};

struct BuildProfile {
    HostMutex mutex;
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
};

BuildProfile& Builds() {
    static BuildProfile profile;
    return profile;
}

bool BuildProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

struct ThreadBuildProfile {
    std::array<double, BuildPhaseCount> ms{};
    std::uint64_t builds = 0;
    ~ThreadBuildProfile() { merge(); }
    void merge() {
        auto& profile = Builds();
        std::lock_guard lock(profile.mutex);
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) profile.ms[i] += ms[i];
        profile.builds += builds;
        ms = {};
        if (builds == 0) return;
        builds = 0;
        std::string report;
        for (std::size_t i = 0; i < BuildPhaseCount; ++i) report += " " + std::string(BuildPhaseNames[i]) + "=" + std::to_string(static_cast<long long>(profile.ms[i])) + "ms";
        std::fprintf(stderr, "[resources] %llu builds, phase totals:%s\n", static_cast<unsigned long long>(profile.builds), report.c_str());
    }
};

ThreadBuildProfile& ThreadBuilds() {
    thread_local ThreadBuildProfile profile;
    return profile;
}

void addBuildPhase(ShaderResources::BuildPhase which, double ms) {
    ThreadBuilds().ms[static_cast<std::size_t>(which)] += ms;
}

// APS5_PROFILE_DRAW: guest buffer elements bound by descriptor (addGuestBuffer) and how many the
// recompiler proved read-only, plus the pending-write notes their uses skipped (MarkGpuWrites),
// printed as [buffers] every 10 s from MarkGpuWrites. Cumulative.
struct BufferWriteCounters {
    std::atomic<std::uint64_t> elements{0};
    std::atomic<std::uint64_t> readOnly{0};
    std::atomic<std::uint64_t> notesSkipped{0};
    std::atomic<std::int64_t> lastReport{0};
};

BufferWriteCounters& BufferWrites() {
    static BufferWriteCounters counters;
    return counters;
}

}

double ShaderResources::phase(BuildPhase which) {
    if (!BuildProfiled()) return 0.0;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(now - phaseStart).count();
    addBuildPhase(which, ms);
    if (ms > 50) std::fprintf(stderr, "[resources] %s took %.0f ms (%zu textures, %zu storage images, %zu buffers, bda %d)\n", BuildPhaseNames[static_cast<std::size_t>(which)], ms, textures.size(), storageTextures.size(), allocations.size(), usesBda ? 1 : 0);
    phaseStart = now;
    return ms;
}

void ShaderResources::buildPrepare(std::span<const CompiledShader> shaders, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes) {
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    if (BuildProfiled()) {
        auto& profile = ThreadBuilds();
        if (++profile.builds % 1000 == 0) profile.merge();
    }
    try {
        Require(!shaders.empty() && context.limits.maxBoundDescriptorSets >= 1, "shader descriptor set exceeds device limits");
        std::set<std::uint32_t> occupied;
        for (const auto& shader : shaders) {
            Require(shader.program != nullptr, "missing compiled shader");
            const VkShaderStageFlags flags = VulkanStage(shader.stage);
            std::uint64_t stageDescriptors = 0;
            std::vector<std::size_t> offsetsInData;
            std::int64_t shaderData = -1;
            for (const auto& binding : shader.program->bindings) {
                Require(binding.descriptorSet == 0, "unexpected descriptor set: every shader resource must use descriptor set zero");
                Require(occupied.insert(binding.binding).second, "duplicate shader binding");
                const bool addressRole = binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable || binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer;
                const bool bufferRole = addressRole || binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers || binding.role == ShaderRecompiler::DescriptorRole::ShaderData || binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
                const bool imageRole = binding.role == ShaderRecompiler::DescriptorRole::GuestImages || binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers;
                if (imageRole) {
                    addImageBinding(binding, flags);
                    continue;
                }
                if (!bufferRole) Require(false, std::string("unsupported descriptor role ") + roleName(binding.role));
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role) + ": only StorageBuffer is supported");
                Require(!binding.readOnly, "read-only descriptors are unsupported because the recompiler emits no NonWritable decoration");
                Require(binding.count != 0, "empty descriptor binding");
                stageDescriptors += binding.count;
                storageBuffers += binding.count;
                Require(stageDescriptors <= context.limits.maxPerStageDescriptorStorageBuffers && stageDescriptors <= context.limits.maxPerStageResources, "shader descriptors exceed per-stage limits");
                Binding item{{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, flags, nullptr}, {}};
                if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
                    Require(binding.guestDescriptor.size() == static_cast<std::uint64_t>(binding.count) * 4, "guest buffer descriptor must contain four DWORDs per array element");
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        // An element the recompiler did not classify (a producer without the
                        // vector) counts as written, like imageWritten below.
                        const bool written = element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                        const bool atomic = element < binding.bufferAtomic.size() && binding.bufferAtomic[element];
                        const auto index = addGuestBuffer(std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4), target, indexAddress, indexBytes, written, atomic);
                        allocations[index].sourceShader = static_cast<std::int32_t>(&shader - shaders.data());
                        allocations[index].sourceBinding = static_cast<std::uint32_t>(&binding - shader.program->bindings.data());
                        allocations[index].sourceElement = element;
                        const auto& push = shader.program->pushConstants;
                        if (!push.empty()) {
                            const auto position = shader.program->memoryOffsetDword * 4u + element;
                            Require(position < push.size(), "guest buffer offset lies outside the shader's push constants");
                            allocations[index].pushByte = static_cast<std::int32_t>(shader.pushConstantOffset + position);
                        } else {
                            allocations[index].dataByte = shader.program->memoryOffsetDword * 4u + element;
                            offsetsInData.push_back(index);
                        }
                        item.allocations.push_back(index);
                    }
                } else if (addressRole) {
                    item.allocations.push_back(allocations.size());
                    allocations.push_back({0, 0, false, nullptr, binding.role});
                } else {
                    Require(binding.count == 1, "shader data and flattened SRT descriptors must not be arrays");
                    Require(!binding.guestDescriptor.empty(), "empty shader data descriptor");
                    item.allocations.push_back(addDataBuffer(binding.guestDescriptor));
                    allocations[item.allocations.back()].sourceShader = static_cast<std::int32_t>(&shader - shaders.data());
                    allocations[item.allocations.back()].sourceBinding = static_cast<std::uint32_t>(&binding - shader.program->bindings.data());
                    if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) shaderData = static_cast<std::int64_t>(item.allocations.back());
                }
                bindings.push_back(std::move(item));
            }
            for (const auto index : offsetsInData) allocations[index].dataAllocation = shaderData;
        }
        Require(storageBuffers <= context.limits.maxDescriptorSetStorageBuffers, "pipeline descriptors exceed device limits");
        timing.bindingsMs = phase(BuildPhase::Bindings);
        // For every build, locked ones included: their stage B then takes the fast path too, and the
        // collects cost the same wherever they run.
        if (precollectImages()) phase(BuildPhase::Precollect);
        guestMemory.UploadPrepare(usesBda);
        timing.uploadMs = phase(BuildPhase::Upload);
        std::vector<VkDescriptorSetLayoutBinding> description;
        for (const auto& binding : bindings) {
            description.push_back(binding.layout);
            layoutKey.insert(layoutKey.end(), {binding.layout.binding, static_cast<std::uint32_t>(binding.layout.descriptorType), binding.layout.descriptorCount, binding.layout.stageFlags});
        }
        static const bool noLayoutCache = std::getenv("APS5_NO_LAYOUT_CACHE") != nullptr;
        static const bool noPoolCache = std::getenv("APS5_NO_POOL_CACHE") != nullptr;
        if (context.descriptorCache != nullptr && !noLayoutCache) {
            _layout = context.descriptorCache->Layout(layoutKey, description);
        } else {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<std::uint32_t>(description.size());
            info.pBindings = description.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &_layout), "vkCreateDescriptorSetLayout");
            ownsLayout = true;
        }
        if (!bindings.empty()) {
            // The set is sized from the plan: every image element becomes exactly one descriptor
            // when stage B looks it up.
            std::vector<VkDescriptorPoolSize> sizes;
            if (storageBuffers != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(storageBuffers)});
            if (plannedSampledImages != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, plannedSampledImages});
            if (plannedStorageImages != 0) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, plannedStorageImages});
            if (!samplers.empty()) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(samplers.size())});
            if (context.descriptorCache != nullptr && !noPoolCache) {
                const auto allocated = context.descriptorCache->Allocate(_layout, sizes);
                _set = allocated.set;
                cachePool = allocated.pool;
            }
            if (_set == VK_NULL_HANDLE) {
                VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                poolInfo.maxSets = 1;
                poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
                poolInfo.pPoolSizes = sizes.data();
                Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
                VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocation.descriptorPool = pool;
                allocation.descriptorSetCount = 1;
                allocation.pSetLayouts = &_layout;
                Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &_set), "vkAllocateDescriptorSets");
            }
        }
        timing.descriptorsMs = phase(BuildPhase::Descriptors);
        if (BuildProfiled()) {
            timing.prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            addBuildPhase(BuildPhase::StageA, timing.prepareMs);
        }
    } catch (...) {
        release();
        throw;
    }
}

void ShaderResources::buildComplete() {
    const auto stageStart = std::chrono::steady_clock::now();
    phaseStart = stageStart;
    try {
        // The lookups run in plan order: Revalidate walks the bindings the same way, and consecutive
        // storage elements of one mip chain share the previous element's image.
        for (const auto& deferred : deferredImages) resolveImageBinding(*deferred.binding, bindings[deferred.index]);
        deferredImages.clear();
        // The records served their purpose: each holds the cache entry's objects as of stage A,
        // which would otherwise keep a replaced texture or an evicted storage image (and its device
        // memory) alive, outside the caches' budgets, for as long as this object is cached.
        imageRecords.clear();
        imageRecords.shrink_to_fit();
        nextImageRecord = 0;
        Require(textures.size() == plannedSampledImages && storageTextures.size() == plannedStorageImages, "image lookups disagree with the descriptor plan");
        timing.bindingsMs += phase(BuildPhase::Images);
        guestMemory.UploadFinish(usesBda);
        timing.uploadMs += phase(BuildPhase::Upload);
        if (usesBda) bda = std::make_unique<BdaResources>(context, guestMemory);
        else if (usesFaultBuffer) bda = std::make_unique<BdaResources>(context);
        phase(BuildPhase::Bda);
        if (_set != VK_NULL_HANDLE) {
            // One update call for the whole set: the info arrays are sized up front so every write's
            // pointer into them stays valid until the call.
            std::size_t bufferCount = 0;
            std::size_t imageCount = 0;
            for (const auto& binding : bindings) {
                bufferCount += binding.allocations.size();
                imageCount += binding.imageAllocations.size();
            }
            std::vector<VkDescriptorBufferInfo> buffers;
            std::vector<VkDescriptorImageInfo> images;
            buffers.reserve(bufferCount);
            images.reserve(imageCount);
            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(bindings.size());
            for (const auto& binding : bindings) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = _set;
                write.dstBinding = binding.layout.binding;
                write.descriptorCount = binding.layout.descriptorCount;
                write.descriptorType = binding.layout.descriptorType;
                switch (binding.layout.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        write.pBufferInfo = buffers.data() + buffers.size();
                        for (const auto index : binding.allocations) buffers.push_back(descriptor(allocations[index]));
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, textureFirstLayer[index] ? textures[index]->FirstLayerView() : textures[index]->View(), textures[index]->Layout()});
                        break;
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({VK_NULL_HANDLE, storageFirstLayer[index] ? storageTextures[index]->FirstLayerView(storageMips[index]) : storageTextures[index]->View(storageMips[index]), VK_IMAGE_LAYOUT_GENERAL});
                        break;
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        write.pImageInfo = images.data() + images.size();
                        for (const auto index : binding.imageAllocations) images.push_back({samplers[index]->Handle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                        break;
                    default: throw std::runtime_error("AGC graphics: ShaderResources encountered an unknown descriptor type while writing the descriptor set");
                }
                writes.push_back(write);
            }
            context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
            if (CheckStaleImports()) {
                boundBuffers.clear();
                for (const auto& info : buffers) boundBuffers.push_back(info.buffer);
            }
        }
        for (const auto& allocation : allocations) {
            if (allocation.adjustment == 0) continue;
            if (allocation.pushByte >= 0) {
                pushPatches.emplace_back(static_cast<std::uint32_t>(allocation.pushByte), allocation.adjustment);
                continue;
            }
            Require(allocation.dataAllocation >= 0, "a guest buffer off the storage buffer offset alignment in a shader without shader data is not implemented");
            auto& data = allocations[static_cast<std::size_t>(allocation.dataAllocation)];
            Require(data.buffer != nullptr && allocation.dataByte < data.size, "guest buffer offset lies outside the shader's data buffer");
            data.buffer->Bytes()[allocation.dataByte] = static_cast<std::byte>(allocation.adjustment);
            dataPatches.push_back({static_cast<std::size_t>(allocation.dataAllocation), allocation.dataByte, allocation.adjustment});
        }
        timing.descriptorsMs += phase(BuildPhase::Descriptors);
        noteReusable();
        completed = true;
        if (BuildProfiled()) {
            timing.completeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
            addBuildPhase(BuildPhase::StageB, timing.completeMs);
            reportDescriptorCaches();
        }
    } catch (...) {
        release();
        throw;
    }
}

// Whether this build can serve later dispatches of the same content (see ContentKey): nothing to do
// once the GPU completed (no BDA, no copied written buffers, no lease) and every guest buffer bound
// in place through a host import (or staged in device memory from one, see
// GuestBufferMemory::DirectRegions) whose identity is recorded for Revalidate.
namespace {

// APS5_NO_TEMPLATE_DATA_REFRESH=1: compute keys keep the ShaderData/FlattenedSrt words and no
// template refreshes its data buffers (see ShaderResources::ContentKey).
bool TemplateDataRefresh() {
    static const bool enabled = std::getenv("APS5_NO_TEMPLATE_DATA_REFRESH") == nullptr;
    return enabled;
}

bool DataRole(ShaderRecompiler::DescriptorRole role) {
    return role == ShaderRecompiler::DescriptorRole::ShaderData || role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
}

// The largest data buffer vkCmdUpdateBuffer refreshes; a template with a bigger one is not reused.
constexpr std::size_t MaxRefreshBytes = 65536;

// FNV-1a over one data buffer's words, its count first (DataWordsHash).
constexpr std::uint64_t FnvOffset = 14695981039346656037ull;
constexpr std::uint64_t FnvPrime = 1099511628211ull;
void mixDataWords(std::uint64_t& hash, std::span<const std::uint32_t> words) {
    hash = (hash ^ static_cast<std::uint64_t>(words.size())) * FnvPrime;
    for (const auto word : words) hash = (hash ^ word) * FnvPrime;
}

// ResourceCache::Find and Touch calls (the [rescache] line).
std::atomic<std::uint64_t> resourceCacheFinds{0};
std::atomic<std::uint64_t> resourceCacheTouches{0};

}

void ShaderResources::noteReusable() {
    // Taken whether or not the object turns out reusable: the lookups' records are this build's.
    captureValidation();
    reusable = false;
    directRegions.clear();
    // A lease template (see LeaseTemplate): the cached space alone, no copied writes, data buffers
    // a hit can refresh; set at the build only (a rearmed use keeps it). On by default with the
    // rebased lease templates (session 41, t328/t329: the same-key ones alone measured no gain,
    // t322-t324: most address-based keys differ in their V# bases per dispatch); APS5_NO_LEASE_REBASE=1
    // turns both off, APS5_LEASE_REUSE=1 then keeps the same-key templates.
    static const bool leaseReuse = std::getenv("APS5_LEASE_REUSE") != nullptr || (std::getenv("APS5_NO_LEASE_REBASE") == nullptr && TemplateDataRefresh());
    if (leaseReuse && !leaseTemplate && bda != nullptr && HoldsLease() && guestMemory.LeaseShape() == 0 && !guestMemory.HasCopiedWrites() && guestMemory.SpaceSerial() != 0 && !(TemplateDataRefresh() && std::any_of(allocations.begin(), allocations.end(), [](const Allocation& allocation) { return allocation.buffer != nullptr && !allocation.guest && allocation.size > MaxRefreshBytes; }))) {
        leaseTemplate = true;
        leaseSerial = guestMemory.SpaceSerial();
    }
    if (NeedsCompletion() || HoldsLease()) return;
    if (TemplateDataRefresh() && std::any_of(allocations.begin(), allocations.end(), [](const Allocation& allocation) { return allocation.buffer != nullptr && !allocation.guest && allocation.size > MaxRefreshBytes; })) return;
    const auto regions = guestMemory.DirectRegions();
    if (!regions.has_value()) return;
    for (const auto& [begin, end] : *regions) {
        // No reconcile here: the upload just took these imports, and the set is about to be recorded
        // against them.
        auto serial = HostImportSerial(context, begin, static_cast<std::size_t>(end - begin), false);
        // Read-only image mirrors (exe ranges) are as stable as imports; their serials have the top
        // bit set, so the two spaces never collide.
        if (serial == 0) serial = ImageMirrorSerial(context, begin, static_cast<std::size_t>(end - begin));
        if (serial == 0) return;
        directRegions.push_back({begin, end, serial});
    }
    reusable = true;
}

// APS5_PROFILE_DRAW: the per-device descriptor caches' counters, every 10 s.
void ShaderResources::reportDescriptorCaches() const {
    static HostMutex reportMutex;
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(reportMutex);
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto descriptors = context.descriptorCache != nullptr ? context.descriptorCache->Counters() : DescriptorCache::Stats{};
    const auto samplerHits = context.samplerCache != nullptr ? context.samplerCache->Hits() : 0;
    const auto samplerMisses = context.samplerCache != nullptr ? context.samplerCache->Misses() : 0;
    std::fprintf(stderr, "[descriptors] layouts %llu hits / %llu created, sets %llu from %llu pools, samplers %llu hits / %llu created\n", static_cast<unsigned long long>(descriptors.layoutHits), static_cast<unsigned long long>(descriptors.layoutMisses), static_cast<unsigned long long>(descriptors.sets), static_cast<unsigned long long>(descriptors.pools), static_cast<unsigned long long>(samplerHits), static_cast<unsigned long long>(samplerMisses));
}

std::vector<std::uint32_t> ShaderResources::ContentKey(const CompiledShader& shader, bool dataWords, bool rebaseReadOnly) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    std::vector<std::uint32_t> key;
    key.reserve(8 + program.bindings.size() * 12);
    key.push_back(dataWords ? 1u : 0u);
    key.push_back(static_cast<std::uint32_t>(shader.stage));
    key.push_back(static_cast<std::uint32_t>(program.variantId));
    key.push_back(static_cast<std::uint32_t>(program.variantId >> 32u));
    key.push_back(static_cast<std::uint32_t>(program.bindings.size()));
    const auto packBits = [&](const std::vector<bool>& bits) {
        key.push_back(static_cast<std::uint32_t>(bits.size()));
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) word |= 1u << (i % 32u);
            if (i % 32u == 31u || i + 1 == bits.size()) {
                key.push_back(word);
                word = 0;
            }
        }
    };
    for (const auto& binding : program.bindings) {
        key.insert(key.end(), {static_cast<std::uint32_t>(binding.kind), static_cast<std::uint32_t>(binding.role), binding.descriptorSet, binding.binding, binding.count, binding.readOnly ? 1u : 0u, binding.imageShape.has_value() ? static_cast<std::uint32_t>(*binding.imageShape) + 1u : 0u, static_cast<std::uint32_t>(binding.guestDescriptor.size())});
        if (rebaseReadOnly && binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            const auto start = key.size();
            key.insert(key.end(), binding.guestDescriptor.begin(), binding.guestDescriptor.end());
            for (std::size_t offset = 0; offset + 4 <= binding.guestDescriptor.size(); offset += 4) {
                const auto element = offset / 4;
                const bool written = element >= binding.bufferWritten.size() || binding.bufferWritten[element];
                if (written) continue;
                auto* words = key.data() + start + offset;
                const bool null = words[0] == 0 && (words[1] & 0xffffu) == 0;
                words[0] = null ? 0u : 1u;
                words[1] &= ~0xffffu;
                // The size too: the draw's descriptor copy takes the snapshot's own range.
                words[2] = words[2] == 0 ? 0u : 1u;
            }
        } else if ((dataWords && !rebaseReadOnly) || !DataRole(binding.role)) {
            // Rebased draw keys leave the data words out as well: a hit binds a data buffer copy
            // holding the draw's own words (PrepareDrawBindings).
            key.insert(key.end(), binding.guestDescriptor.begin(), binding.guestDescriptor.end());
        }
        packBits(binding.imageWritten);
        packBits(binding.samplerDepthCompare);
        packBits(binding.imageDepthCompare);
        // Read-only elements are bound without a write set: an object built for one written set
        // must not serve a build with another (the variant implies it, this makes it explicit).
        packBits(binding.bufferWritten);
    }
    return key;
}

namespace {

// APS5_PROFILE_DRAW: Revalidate outcomes and time, printed as [rescache] every 10 s next to the
// device's hit/miss line: failed = the object could not be reused (whichever image path it took);
// of the reused ones, fast = every image proved current from stamps, full = the lookups were repeated.
// Why the fast path left an object to the full walk (the "fast-fail by reason" counts).
using FastFail = ShaderResources::FastFail;
constexpr std::array<const char*, static_cast<std::size_t>(FastFail::Count)> FastFailNames{"no record", "collect", "pending image", "evicted image", "memory changed", "keys", "cleared view", "storage keys", "depth surface"};
using OwnRefreshFallback = ShaderResources::OwnRefreshFallback;
constexpr std::array<const char*, static_cast<std::size_t>(OwnRefreshFallback::Count)> OwnRefreshFallbackNames{"disabled", "snapshot texture", "cleared view", "foreign view", "surface key", "not imported", "uncached", "re-run failed"};

// APS5_TRACE_WALKS=1 (diagnostic, session 23): a fast proof that failed and was left to the full
// walk prints, for the first 2 cases per variant, fail reason and walk outcome, the reason, the
// surfaces a Pending failure found pending (their records, the image pending over each right now,
// the T1 fallback and the re-run's reason), and what the walk returned: the same objects (its new
// records) or another object (the element, its old record and the object that replaced it), with
// a 10 s count per template stage, reason and outcome. Diagnosis of the resource-cache bound
// (PROGRESS sessions 21-23).
bool TracePendingWalk() {
    static const bool enabled = std::getenv("APS5_TRACE_WALKS") != nullptr;
    return enabled;
}

struct RevalidateProfile {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> nanoseconds{0};
    std::atomic<std::uint64_t> fast{0};
    std::atomic<std::uint64_t> full{0};
    std::atomic<std::uint64_t> failed{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fastFails{};
    // Epoch gate: registry scans skipped because the pending serial was unchanged, and import
    // serial loops skipped because the table's identity was unchanged.
    std::atomic<std::uint64_t> serialSkips{0};
    std::atomic<std::uint64_t> importSkips{0};
    // Elements with DCC keys the fast path accepted through the images' key proofs.
    std::atomic<std::uint64_t> keysProven{0};
    std::atomic<std::uint64_t> storageKeysProven{0};
    // The 'pending image' fast-fails by what overlapped: a surface with an own object (the query
    // excepts it, so the overlapping image is foreign to the surface) or a snapshot texture (no own
    // object); and the fails whose overlapping surfaces all have an own object (the T1 trim of
    // design_cpu_final applies: flush the foreign image, refresh the own object, no full walk).
    std::atomic<std::uint64_t> pendingForeign{0};
    std::atomic<std::uint64_t> pendingSnapshot{0};
    std::atomic<std::uint64_t> pendingT1Eligible{0};
    // The full walks by the reason of the fast proof that preceded them (fastFails also counts the
    // Pending failures the own-object refresh resolved), the Pending failures it resolved (with the
    // objects it refreshed: storage images, view sources) and those it left to the walk by reason;
    // view surfaces a foreign image overlapped that the proof accepted because FindPending names
    // their own source; surfaces a foreign image was still pending over after their own object's
    // refresh (its alias, a straddling image) that the re-run accepted; proofs the verify switch
    // checked against the full walk.
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FastFail::Count)> fullByReason{};
    std::atomic<std::uint64_t> ownRefreshed{0};
    std::atomic<std::uint64_t> ownStorageRefreshes{0};
    std::atomic<std::uint64_t> ownViewRefreshes{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(OwnRefreshFallback::Count)> ownFallbacks{};
    std::atomic<std::uint64_t> ownSourceViews{0};
    // Elements a depth surface served, proved by DepthSurfaceServes; time of the calls that walked.
    std::atomic<std::uint64_t> depthRecords{0};
    std::atomic<std::uint64_t> fullNanoseconds{0};
    // One proved fastRevalidate in 64 per thread, timed by phase (setup, sampled textures, storage
    // images, pending scan, stamp queries, cache touch + record moves) with its element counts.
    std::atomic<std::uint64_t> phaseSamples{0};
    std::array<std::atomic<std::uint64_t>, 6> phaseNanoseconds{};
    std::atomic<std::uint64_t> phaseTextures{0};
    std::atomic<std::uint64_t> phaseStorage{0};
    std::atomic<std::uint64_t> phaseKeyed{0};
    std::atomic<std::uint64_t> phasePending{0};
    // One fast-proved Revalidate in 64 per thread, timed around the proof: entry to the proof, the
    // proof, the checks after it, (2) the imports, (3) the staging copies.
    std::atomic<std::uint64_t> outerSamples{0};
    std::array<std::atomic<std::uint64_t>, 5> outerNanoseconds{};
    std::atomic<std::uint64_t> refreshedOverlaps{0};
    std::atomic<std::uint64_t> proofsVerified{0};
    std::atomic<std::int64_t> lastReport{0};
};

RevalidateProfile& Revalidations() {
    static RevalidateProfile profile;
    return profile;
}

void countFastFail(FastFail reason) {
    if (BuildProfiled()) Revalidations().fastFails[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countPendingFail(std::span<const StorageTexture::PendingQuery> pending) {
    if (!BuildProfiled()) return;
    auto& profile = Revalidations();
    bool eligible = true;
    for (const auto& query : pending) {
        if (!query.overlaps) continue;
        if (query.except != nullptr) {
            profile.pendingForeign.fetch_add(1, std::memory_order_relaxed);
        } else {
            profile.pendingSnapshot.fetch_add(1, std::memory_order_relaxed);
            eligible = false;
        }
    }
    if (eligible) profile.pendingT1Eligible.fetch_add(1, std::memory_order_relaxed);
}

void countKeysProven(bool storage) {
    if (BuildProfiled()) (storage ? Revalidations().storageKeysProven : Revalidations().keysProven).fetch_add(1, std::memory_order_relaxed);
}

void countFullWalk(FastFail reason) {
    if (BuildProfiled() && reason != FastFail::Count) Revalidations().fullByReason[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countOwnRefreshFallback(OwnRefreshFallback reason) {
    if (BuildProfiled()) Revalidations().ownFallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void countOwnRefresh(bool storage) {
    TextureCounts().ownRefreshes.fetch_add(1, std::memory_order_relaxed);
    if (BuildProfiled()) (storage ? Revalidations().ownStorageRefreshes : Revalidations().ownViewRefreshes).fetch_add(1, std::memory_order_relaxed);
}

// APS5_NO_OWN_IMAGE_REFRESH=1: a Pending failure of the fast proof takes the full walk, as before
// the own-object refresh (T1).
bool OwnImageRefreshEnabled() {
    static const bool disabled = std::getenv("APS5_NO_OWN_IMAGE_REFRESH") != nullptr;
    return !disabled;
}

// APS5_VERIFY_PROOFS=1: after every own-object refresh the full walk runs beside the proof and the
// process aborts when it would return another object or upload again (a different decision).
bool VerifyProofs() {
    static const bool enabled = std::getenv("APS5_VERIFY_PROOFS") != nullptr;
    return enabled;
}

// APS5_VERIFY_FAST_PROOFS=1 (diagnostic, session 21): after every fast proof that succeeds the full
// walk runs beside it; when the walk would return another object or upload again, the first stale
// element is named on stderr (the first two cases per variant, with the template's resources), the case is
// counted every 10 s and the proof fails, so the template is rebuilt. Found because templates
// that survive across frames (APS5_RESOURCE_CACHE_ENTRIES=4096) doubled the game's visible set
// (t231) while APS5_NO_FAST_REVALIDATE=1 did not (t235).
// APS5_VERIFY_FAST_PROOFS=2..6 (diagnostic, session 22): the walk beside the proof keeps the
// game's visible set right while its verdict never differs in gameplay (t237), so one class of
// the walk's side effects runs beside every accepted fast proof, without a verdict, to find the
// one the fast path lacks: 2 = the sampled-texture lookups only, 3 = the storage-image lookups
// only, 4 = only the pending flush over every snapshot's memory (the sampled lookup's), 5 = only
// Refresh of every storage image and view source, 6 = no side effect, a spin of
// APS5_FAST_PROOF_SPIN_US microseconds (default 20) in place of the walk's time, 7 = the whole
// walk in binding order (sampled and storage lookups interleaved) without the record update
// (captureValidation) and without a verdict.
unsigned VerifyFastProofsMode() {
    static const unsigned mode = [] {
        const char* value = std::getenv("APS5_VERIFY_FAST_PROOFS");
        if (value == nullptr) return 0u;
        const auto parsed = std::strtoul(value, nullptr, 10);
        return parsed >= 2 && parsed <= 8 ? static_cast<unsigned>(parsed) : 1u;
    }();
    return mode;
}

// Modes 1 and 8 walk with a verdict; 8 also diffs the walk's records against the fast path's
// (captureValidation) and prints the differing fields every 10 s.
bool VerifyFastProofs() {
    const auto mode = VerifyFastProofsMode();
    return mode == 1 || mode == 8;
}

// APS5_NO_SERIAL_MEMO=1 (diagnostic, session 22): the fast proof scans the pending registry on
// every call, as if its serial had moved since the last proof (the epoch gate's memo off, the
// per-element proofs kept).
bool SerialMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_SERIAL_MEMO") == nullptr;
    return enabled;
}

// The fast proof notes a written storage image on the depth surfaces covering its memory
// (NoteDepthSurfaceWrite), as the build and the full walk do: without the note a surface
// sampled after a compute pass overwrote it served its own stale depth (DepthSurfaceTexture),
// which made the game's GPU occlusion culling conservative and doubled the draws per frame
// once compute templates survived across frames (APS5_RESOURCE_CACHE_ENTRIES=4096, t231;
// t235/t237 with the walk did not). APS5_NO_FAST_DEPTH_NOTE=1 skips the note as before.
// The fast proof fails a sampled surface a depth surface now covers (session 23, see
// fastRevalidate). APS5_NO_FAST_DEPTH_CHECK=1 restores the proof without it.
bool FastDepthCheck() {
    static const bool enabled = std::getenv("APS5_NO_FAST_DEPTH_CHECK") == nullptr;
    return enabled;
}

bool FastDepthNote() {
    static const bool enabled = std::getenv("APS5_NO_FAST_DEPTH_NOTE") == nullptr;
    return enabled;
}

// Whether a sampled surface's keys are the very keys of the image the view follows (the same
// metadata, extent, format and alpha placement: the same scan), so the image's proof serves it.
bool SameKeySurface(const StorageTexture* source, const GuestTextureResource& resource, std::uint64_t guestBytes) {
    if (source == nullptr) return false;
    const auto& own = source->Descriptor();
    return own.dccAddress == resource.dccAddress && source->GuestBytes() == guestBytes && own.format == resource.format && own.dccAlphaOnMsb == resource.dccAlphaOnMsb;
}

void countRevalidate(bool fast, bool ok, std::chrono::steady_clock::time_point start) {
    auto& profile = Revalidations();
    const auto now = std::chrono::steady_clock::now();
    profile.calls.fetch_add(1, std::memory_order_relaxed);
    profile.nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), std::memory_order_relaxed);
    (!ok ? profile.failed : fast ? profile.fast : profile.full).fetch_add(1, std::memory_order_relaxed);
    if (!fast) profile.fullNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count()), std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto last = profile.lastReport.load();
    if (nowMs - last < 10000 || !profile.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto byReason = [](const auto& counts, const auto& names) {
        std::string reasons;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto count = counts[i].load(std::memory_order_relaxed);
            if (count == 0) continue;
            reasons += " " + std::string(names[i]) + " " + std::to_string(count);
        }
        return reasons;
    };
    const auto reasons = byReason(profile.fastFails, FastFailNames);
    const auto fullReasons = byReason(profile.fullByReason, FastFailNames);
    const auto fallbacks = byReason(profile.ownFallbacks, OwnRefreshFallbackNames);
    std::fprintf(stderr, "[rescache] revalidate %llu calls %.1f ms: fast %llu, full %llu, failed %llu; fast-fail by reason:%s; full walks: T1 refreshed %llu (%llu storage images, %llu view sources, %llu views served by their own pending source, %llu overlaps left by the refresh accepted), full by reason:%s, T1 fallback by reason:%s, proofs verified %llu; fast-fail pending image: own 0 (the query excepts the own object), foreign %llu, snapshot %llu, T1 eligible %llu; keys proven: %llu sampled, %llu storage; epoch gate: %llu registry scans skipped, %llu import loops skipped\n", static_cast<unsigned long long>(profile.calls.load()), profile.nanoseconds.load() / 1e6, static_cast<unsigned long long>(profile.fast.load()), static_cast<unsigned long long>(profile.full.load()), static_cast<unsigned long long>(profile.failed.load()), reasons.c_str(), static_cast<unsigned long long>(profile.ownRefreshed.load()), static_cast<unsigned long long>(profile.ownStorageRefreshes.load()), static_cast<unsigned long long>(profile.ownViewRefreshes.load()), static_cast<unsigned long long>(profile.ownSourceViews.load()), static_cast<unsigned long long>(profile.refreshedOverlaps.load()), fullReasons.c_str(), fallbacks.c_str(), static_cast<unsigned long long>(profile.proofsVerified.load()), static_cast<unsigned long long>(profile.pendingForeign.load()), static_cast<unsigned long long>(profile.pendingSnapshot.load()), static_cast<unsigned long long>(profile.pendingT1Eligible.load()), static_cast<unsigned long long>(profile.keysProven.load()), static_cast<unsigned long long>(profile.storageKeysProven.load()), static_cast<unsigned long long>(profile.serialSkips.load()), static_cast<unsigned long long>(profile.importSkips.load()));
    std::fprintf(stderr, "[rescache] revalidate split: calls that walked or failed %.1f ms (fast proofs the rest); depth-served elements proven %llu\n", profile.fullNanoseconds.load() / 1e6, static_cast<unsigned long long>(profile.depthRecords.load()));
    if (const auto samples = profile.phaseSamples.load(); samples != 0) {
        const auto us = [&](std::size_t phase) { return profile.phaseNanoseconds[phase].load() / 1e3 / static_cast<double>(samples); };
        const auto per = [&](const std::atomic<std::uint64_t>& count) { return static_cast<double>(count.load()) / static_cast<double>(samples); };
        std::fprintf(stderr, "[rescache] fast proof phases (%llu sampled proofs, us each): setup %.2f, textures %.2f, storage %.2f, pending scan %.2f, stamps %.2f, cache+records %.2f; per proof %.1f textures, %.1f storage images, %.1f keyed, %.1f pending queries\n", static_cast<unsigned long long>(samples), us(0), us(1), us(2), us(3), us(4), us(5), per(profile.phaseTextures), per(profile.phaseStorage), per(profile.phaseKeyed), per(profile.phasePending));
    }
    if (const auto samples = profile.outerSamples.load(); samples != 0) {
        const auto us = [&](std::size_t phase) { return profile.outerNanoseconds[phase].load() / 1e3 / static_cast<double>(samples); };
        std::fprintf(stderr, "[rescache] fast revalidate around the proof (%llu sampled, us each): entry %.2f, proof %.2f, after the proof %.2f, imports %.2f, staging copies %.2f\n", static_cast<unsigned long long>(samples), us(0), us(1), us(2), us(3), us(4));
    }
}

// APS5_NO_EPOCH_REVALIDATE=1: the per-element registry, cache and stamp checks and the per-region
// flush and import lookups of every Revalidate, as before the epoch gate.
bool EpochRevalidate() {
    static const bool enabled = std::getenv("APS5_NO_EPOCH_REVALIDATE") == nullptr;
    return enabled;
}

// APS5_NO_PROOF_SCRATCH=1: one thread_local per scratch object, as before the shared one.
template <typename T, int Slot>
T& SeparateLocal() {
    thread_local T value{};
    return value;
}

}

// The proofs' scratch in one thread_local: MinGW's thread_local is emulated (one
// __emutls_get_address call, plus the init guard's, per object a function touches), and a
// fast-proved Revalidate touched ten of them per call.
struct ShaderResources::ProofScratch {
    std::uint32_t outerTick = 0;
    std::uint32_t phaseTick = 0;
    std::vector<PendingOverlap> overlapping;
    std::vector<PendingOverlap> refreshed;
    std::vector<GuestMemory::UnchangedQuery> queries;
    std::vector<StorageTexture::PendingQuery> pending;
    // The element each pending query stands for, in query order.
    std::vector<PendingOverlap> owners;
    std::vector<const StorageTexture*> images;
    std::vector<DccKeys> scannedKeys;
    std::vector<StorageTexture::PendingQuery> regions;
};

ShaderResources::ProofScratch* ShaderResources::proofScratch() {
    static const bool separate = std::getenv("APS5_NO_PROOF_SCRATCH") != nullptr;
    if (separate) return nullptr;
    thread_local ProofScratch scratch;
    return &scratch;
}

// Turns the lookups' records into this object's per-texture validation records (see
// ValidatedSurface); the last record of an object is the freshest.
void ShaderResources::captureValidation() {
    const auto record = [](const void* object) -> const LookupRecord* {
        for (auto it = lookupLog.rbegin(); it != lookupLog.rend(); ++it) {
            if (it->object == object) return &*it;
        }
        return nullptr;
    };
    // APS5_VERIFY_FAST_PROOFS=8: the records the walk makes against the ones the fast path kept
    // (a proof moves generation and keys itself), per field, printed every 10 s with the first case.
    const bool diffRecords = diffRecordsNow;
    thread_local std::vector<ValidatedSurface> previous;
    if (diffRecords) previous = validatedTextures;
    validatedTextures.assign(textures.size(), {});
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (const auto* found = record(textures[i].get())) validatedTextures[i] = {found->resource, found->bytes, found->keys, found->generation, 0, found->source, true, found->depth};
    }
    lookupLog.clear();
    if (diffRecords && previous.size() == validatedTextures.size()) {
        static std::atomic<std::uint64_t> compared{0}, keysDiff{0}, sourceDiff{0}, bytesDiff{0}, addressDiff{0}, genOlder{0}, genNewer{0}, validDiff{0};
        static HostMutex caseMutex;
        static std::string firstCases;
        static std::atomic<long long> lastReport{0};
        for (std::size_t i = 0; i < previous.size(); ++i) {
            const auto& was = previous[i];
            const auto& now = validatedTextures[i];
            if (!was.valid || !now.valid) {
                if (was.valid != now.valid) validDiff.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            compared.fetch_add(1, std::memory_order_relaxed);
            const char* field = nullptr;
            if (was.keys != now.keys) { keysDiff.fetch_add(1, std::memory_order_relaxed); field = "keys"; }
            else if (was.source != now.source) { sourceDiff.fetch_add(1, std::memory_order_relaxed); field = "source"; }
            else if (was.bytes != now.bytes) { bytesDiff.fetch_add(1, std::memory_order_relaxed); field = "bytes"; }
            else if (was.resource.baseAddress != now.resource.baseAddress) { addressDiff.fetch_add(1, std::memory_order_relaxed); field = "address"; }
            else if (now.source == nullptr && now.generation < was.generation) { genOlder.fetch_add(1, std::memory_order_relaxed); field = "generation older"; }
            else if (now.source == nullptr && now.generation > was.generation) { genNewer.fetch_add(1, std::memory_order_relaxed); field = "generation newer"; }
            if (field != nullptr) {
                std::lock_guard caseLock(caseMutex);
                // One case per surface (the first 24 surfaces), with the collected generation
                // the proof saw, so the writer between the proof and the walk can be named.
                static std::set<std::uint64_t> casedSurfaces;
                if (casedSurfaces.size() < 24 && casedSurfaces.insert(now.resource.baseAddress).second) {
                    char text[320];
                    std::snprintf(text, sizeof(text), " [%s: 0x%llx+0x%llx %ux%u f%u t%d mips %u dcc 0x%llx: keys %s -> %s, source %p -> %p, generation %llu (proof collected %llu) -> %llu]", field, static_cast<unsigned long long>(now.resource.baseAddress), static_cast<unsigned long long>(now.bytes), now.resource.width, now.resource.height, static_cast<unsigned>(now.resource.format), static_cast<int>(now.resource.tileMode), now.resource.mipCount, static_cast<unsigned long long>(now.resource.dccAddress), DccKeysName(was.keys), DccKeysName(now.keys), static_cast<const void*>(was.source), static_cast<const void*>(now.source), static_cast<unsigned long long>(was.generation), static_cast<unsigned long long>(was.collected), static_cast<unsigned long long>(now.generation));
                    firstCases += text;
                }
            }
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = lastReport.load();
        if (nowMs - last >= 10000 && lastReport.compare_exchange_strong(last, nowMs)) {
            std::lock_guard caseLock(caseMutex);
            std::fprintf(stderr, "[rescache] walk records vs fast records: %llu compared, keys %llu, source %llu, bytes %llu, address %llu, generation older %llu, newer %llu, validity %llu; first cases:%s\n", static_cast<unsigned long long>(compared.load()), static_cast<unsigned long long>(keysDiff.load()), static_cast<unsigned long long>(sourceDiff.load()), static_cast<unsigned long long>(bytesDiff.load()), static_cast<unsigned long long>(addressDiff.load()), static_cast<unsigned long long>(genOlder.load()), static_cast<unsigned long long>(genNewer.load()), static_cast<unsigned long long>(validDiff.load()), firstCases.c_str());
        }
    }
}

// Proves every texture and storage image still current without repeating its lookup: exactly the
// "unchanged" branches of cachedTexture and StorageTexture::Refresh, evaluated from write stamps,
// the pending-results registry and the DCC keys. Per element: collect first (stamps come only from
// collects, and the generation a record moves to must predate the checks, so a write landing between
// them is stamped newer: the rule of Driver's dispatch cache), then no other image may have results
// pending over the memory (a lookup would flush them into it), a storage image must still be the
// cache's image of its surface, the memory must be unchanged since the object's content matched it,
// and the DCC keys must be what the content was made under whenever the surface has any (a fast
// clear touches only the keys). Anything else, including a failed collect (memory outside the arena,
// uncommitted pages), is left to the full walk.
// The epoch gate: while the pending registry's serial is what this object's last proof saw
// (pendingSerialSeen), the registry is what it was then (every change bumps the serial under the
// registry mutex), so the pending-image checks are skipped; otherwise one scan answers them for
// every surface at once. The stamp checks go through one tracker lock, the cache checks through
// the images' cached flags and one touch of the cache.
bool ShaderResources::fastRevalidate(std::uint64_t serialBefore, std::span<const PendingOverlap> refreshed, FastFail& reason, std::vector<PendingOverlap>& overlapping, bool& accepted) {
    reason = FastFail::Count;
    overlapping.clear();
    accepted = false;
    if (!EpochRevalidate()) return fastRevalidateEach();
    auto* const scratch = proofScratch();
    auto& phaseTick = scratch != nullptr ? scratch->phaseTick : SeparateLocal<std::uint32_t, 0>();
    const bool timed = BuildProfiled() && (++phaseTick & 63u) == 0;
    std::array<std::chrono::steady_clock::time_point, 7> marks{};
    std::uint64_t keyed = 0;
    const auto mark = [&](std::size_t phase) {
        if (timed) marks[phase] = std::chrono::steady_clock::now();
    };
    mark(0);
    const auto fail = [&reason](FastFail why) {
        reason = why;
        countFastFail(why);
        return false;
    };
    if (validatedTextures.size() != textures.size()) return fail(FastFail::NoRecord);
    // A depth surface drawn at a sampled surface since its record was made: the lookup would serve
    // the surface's depth image in place of the snapshot or view (DepthSurfaceTexture is
    // cachedTexture's first answer), and nothing below would notice, since depth draws stamp no
    // guest memory, register no pending image and touch no DCC keys. The record exists only because
    // the build's lookup found no such surface, so the scan repeats only once the depth registry's
    // serial moved since the last proof. (Session 23: the half-size depth 0x2ef270000 a Boletaria
    // compute pass downsamples into its occlusion pyramid was snapshotted as zeros during the load;
    // with compute templates surviving across frames the proof kept the snapshot and the game drew
    // twice as much.)
    const auto depthSerial = DepthSurfaceSerial();
    const bool depthCheck = FastDepthCheck() && depthSerial != depthSerialSeen;
    const bool unchanged = SerialMemoEnabled() && pendingSerialSeen != 0 && pendingSerialSeen == serialBefore;
    const bool keyProofs = KeyFastPath();
    auto& queries = scratch != nullptr ? scratch->queries : SeparateLocal<std::vector<GuestMemory::UnchangedQuery>, 0>();
    auto& pending = scratch != nullptr ? scratch->pending : SeparateLocal<std::vector<StorageTexture::PendingQuery>, 0>();
    // The element each pending query stands for, in query order.
    auto& owners = scratch != nullptr ? scratch->owners : SeparateLocal<std::vector<PendingOverlap>, 0>();
    auto& images = scratch != nullptr ? scratch->images : SeparateLocal<std::vector<const StorageTexture*>, 0>();
    auto& scannedKeys = scratch != nullptr ? scratch->scannedKeys : SeparateLocal<std::vector<DccKeys>, 0>();
    queries.clear();
    pending.clear();
    owners.clear();
    images.clear();
    scannedKeys.assign(textures.size(), DccKeys::Uncompressed);
    const auto query = [&](std::uint64_t begin, std::uint64_t end, const StorageTexture* except, const StorageTexture* identity, PendingOverlap owner) {
        pending.push_back({begin, end, except, identity, false});
        owners.push_back(owner);
    };
    mark(1);
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid) return fail(FastFail::NoRecord);
        if (surface.depth) {
            // The lookup answers from the depth surface before any memory or key check: proved
            // when it would hand out the same texture (no guest memory is involved).
            if (!DepthSurfaceServes(context, surface.resource, textures[i].get())) return fail(FastFail::DepthSurface);
            if (BuildProfiled()) Revalidations().depthRecords.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        if (depthCheck && DepthSurfaceAt(address, surface.resource.width, surface.resource.height)) return fail(FastFail::DepthSurface);
        surface.collected = GuestMemory::CollectWrites(address, bytes);
        if (surface.collected == 0) return fail(FastFail::Collect);
        const auto* source = surface.source;
        if (timed && surface.resource.dccAddress != 0) ++keyed;
        if (!keyProofs) {
            // Without proofs the keys are compared with the record's (a cleared view's identity
            // below is the record's too): the scan repeats on every call, as before.
            if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = surface.keys;
            if (!unchanged) query(address, address + bytes, source, source != nullptr && surface.keys != DccKeys::Uncompressed ? source : nullptr, {i, false, source != nullptr && surface.keys == DccKeys::Uncompressed});
        } else if (source != nullptr) {
            // The view follows its image, so it stays valid while the image would Refresh as
            // unchanged: the image's own keys must be what its content was uploaded under (its
            // proof answers without a scan while they are unstamped), and its memory unchanged
            // since its generation (the query below). The surface's own keys come from the same
            // proof when they are the image's, else from the texture's.
            const auto& own = source->Descriptor();
            const auto sourceKeys = own.dccAddress != 0 ? ProvedClearKeys(own, source->GuestBytes(), source->KeyProof()) : DccKeys::Uncompressed;
            if (sourceKeys != source->UploadedKeys()) return fail(FastFail::Keys);
            const auto keys = surface.resource.dccAddress == 0 ? DccKeys::Uncompressed : SameKeySurface(source, surface.resource, surface.bytes) ? sourceKeys : ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof());
            if (surface.resource.dccAddress != 0 && surface.resource.dccAddress != own.dccAddress && (keys != DccKeys::Uncompressed || !StorageImageServesKeys(*source, surface.resource.dccAddress))) return fail(FastFail::Keys);
            scannedKeys[i] = keys;
            // A view of a fast-cleared surface stays one only while its image still has results
            // pending over it (cachedTexture's hit rule), unless the image's own descriptor names
            // the surface's metadata (the lookup views the cleared image then). The identity is
            // asked whatever the registry's serial did: the keys may have moved since the last
            // proof while the registry did not.
            if (keys != DccKeys::Uncompressed && !(own.dccAddress == surface.resource.dccAddress && ClearedViewEnabled() && StorageClearAvailable(context, surface.resource.format, keys))) query(address, address + bytes, source, source, {i, false, false});
            else if (!unchanged) query(address, address + bytes, source, nullptr, {i, false, keys == DccKeys::Uncompressed});
            if (surface.resource.dccAddress != 0 || own.dccAddress != 0) countKeysProven(false);
        } else {
            // A snapshot holds its clear texels or the guest bytes: the keys must be what it was made under.
            const auto keys = surface.resource.dccAddress != 0 ? ProvedClearKeys(surface.resource, surface.bytes, textures[i]->KeyProof()) : DccKeys::Uncompressed;
            if (keys != surface.keys) return fail(FastFail::Keys);
            scannedKeys[i] = keys;
            if (!unchanged) query(address, address + bytes, nullptr, nullptr, {i, false, false});
            if (surface.resource.dccAddress != 0) countKeysProven(false);
        }
        if (source != nullptr) {
            if (!source->Cached()) return fail(FastFail::Evicted);
            queries.push_back({address, bytes, source->Generation()});
            images.push_back(source);
        } else {
            queries.push_back({address, bytes, surface.generation});
        }
    }
    mark(2);
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (storageTextures[i] != nullptr && (i >= storageKeys.size() || !StorageImageServesKeys(*storageTextures[i], storageKeys[i]))) return fail(FastFail::StorageKeys);
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return fail(FastFail::NoRecord);
        const auto& own = image->Descriptor();
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        // A written image overwrites the depth surfaces covering its memory (see FastDepthNote).
        if (i < storageWritten.size() && storageWritten[i] && FastDepthNote()) NoteDepthSurfaceWrite(address, own.width, own.height);
        if (GuestMemory::CollectWrites(address, bytes) == 0) return fail(FastFail::Collect);
        if (own.dccAddress != 0) {
            // Refresh's unchanged branch (its key compare against the keys the content was
            // uploaded under, through the image's proof; the memory query below), minus its
            // byte-compare fallback; the image's generation is left where it is.
            if (!keyProofs || ProvedClearKeys(own, bytes, image->KeyProof()) != image->UploadedKeys()) return fail(FastFail::StorageKeys);
            countKeysProven(true);
        }
        if (!unchanged) query(address, address + bytes, image, nullptr, {i, true, false});
        if (!image->Cached()) return fail(FastFail::Evicted);
        queries.push_back({address, bytes, image->Generation()});
        images.push_back(image);
    }
    mark(3);
    const auto pendingQueries = pending.size();
    if (!pending.empty()) {
        if (!StorageTexture::ScanPending(pending)) return fail(FastFail::ClearedView);
        // A foreign image over a view under uncompressed keys is no failure while FindPending
        // names the view's own source: the lookup views the source (cachedTexture's hit rule)
        // and touches nothing else. A surface whose own object T1 refreshed in this Revalidate
        // (`refreshed`) is no failure either: a storage element's lookup is that Refresh and
        // returns the image whatever stays registered over it; a view's lookup hits on the source
        // when FindPending names it, and views the cached image of its key (the source, proved by
        // refreshOwnObjects) when FindPending names nothing. Every other overlap is the full
        // walk's, or T1's (Revalidate).
        const auto refreshedOwner = [&](const PendingOverlap& owner) {
            return std::find_if(refreshed.begin(), refreshed.end(), [&](const PendingOverlap& entry) { return entry.element == owner.element && entry.storage == owner.storage; });
        };
        for (std::size_t k = 0; k < pending.size(); ++k) {
            if (!pending[k].overlaps) continue;
            if (OwnImageRefreshEnabled() && owners[k].viewUncompressed && pending[k].found == pending[k].except) {
                if (BuildProfiled()) Revalidations().ownSourceViews.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            if (const auto entry = refreshedOwner(owners[k]); entry != refreshed.end() && (entry->storage || pending[k].found == pending[k].except || (pending[k].found == nullptr && entry->sourceEligible))) {
                if (BuildProfiled()) Revalidations().refreshedOverlaps.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
                continue;
            }
            overlapping.push_back(owners[k]);
        }
        if (!overlapping.empty()) {
            countPendingFail(pending);
            return fail(FastFail::Pending);
        }
    } else if (unchanged && BuildProfiled()) {
        Revalidations().serialSkips.fetch_add(1, std::memory_order_relaxed);
    }
    mark(4);
    if (!GuestMemory::UnchangedSinceAll(queries)) return fail(FastFail::Changed);
    mark(5);
    if (!StorageImagesCached(context, images)) return fail(FastFail::Evicted);
    for (const auto* image : images) image->NoteProved();
    for (std::size_t i = 0; i < validatedTextures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (surface.depth) continue;
        if (surface.source == nullptr) surface.generation = surface.collected;
        surface.keys = scannedKeys[i];
    }
    depthSerialSeen = depthSerial;
    if (timed) {
        mark(6);
        auto& profile = Revalidations();
        // Phase 0 (setup) runs from the first mark to the loops; the rest between marks.
        for (std::size_t phase = 0; phase < 6; ++phase) profile.phaseNanoseconds[phase].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(marks[phase + 1] - marks[phase]).count()), std::memory_order_relaxed);
        profile.phaseSamples.fetch_add(1, std::memory_order_relaxed);
        profile.phaseTextures.fetch_add(textures.size(), std::memory_order_relaxed);
        profile.phaseStorage.fetch_add(storageTextures.size(), std::memory_order_relaxed);
        profile.phaseKeyed.fetch_add(keyed, std::memory_order_relaxed);
        profile.phasePending.fetch_add(pendingQueries, std::memory_order_relaxed);
    }
    return true;
}

bool ShaderResources::fastRevalidateEach() {
    if (validatedTextures.size() != textures.size()) return false;
    for (std::size_t i = 0; i < textures.size(); ++i) {
        auto& surface = validatedTextures[i];
        if (!surface.valid || surface.depth) return false;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        surface.collected = GuestMemory::CollectWrites(address, bytes);
        if (surface.collected == 0) return false;
        if (PendingStorageOverlaps(address, bytes, surface.source)) return false;
        if (FastDepthCheck() && DepthSurfaceAt(address, surface.resource.width, surface.resource.height)) return false;
        if (surface.source != nullptr && surface.resource.dccAddress != 0 && surface.resource.dccAddress != surface.source->Descriptor().dccAddress) return false;
        if (surface.source != nullptr) {
            // The view follows the image: it needs the image current with guest memory, as the
            // lookup's Refresh would make it.
            if (!StorageImageCached(context, surface.source) || !GuestMemory::UnchangedSince(address, bytes, surface.source->Generation())) return false;
        } else if (!GuestMemory::UnchangedSince(address, bytes, surface.generation)) {
            return false;
        }
        if (surface.resource.dccAddress != 0 && TextureClearKeys(surface.resource, surface.bytes) != surface.keys) return false;
        // A view made under fast-clear keys stays one only while its image still has results pending
        // over the surface (the lookup prefers them to the clear); once they are flushed the clear
        // the image cannot see wins and the lookup makes a snapshot (cachedTexture's hit rule).
        if (surface.source != nullptr && surface.keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, surface.bytes).get() != surface.source) return false;
    }
    for (std::size_t i = 0; i < storageTextures.size(); ++i) {
        if (storageTextures[i] != nullptr && (i >= storageKeys.size() || (storageKeys[i] != 0 && storageKeys[i] != storageTextures[i]->Descriptor().dccAddress))) return false;
        // Consecutive elements of one mip chain share the image (see addStorageImageBinding).
        if (i != 0 && storageTextures[i] == storageTextures[i - 1]) continue;
        const auto* image = storageTextures[i].get();
        if (image == nullptr) return false;
        // StorageTexture::Refresh checks the image's own surface, not the element's descriptor (one
        // image serves every descriptor of its memory, with or without a DCC address), and compares
        // its keys with the keys it was uploaded under, which only the image knows (they move to
        // Uncompressed on every write-back; a fast clear after it puts the key bytes back to what a
        // record saw while the image holds last frame's results), so a surface with keys is left to
        // the full walk. Without a DCC address both sides of that compare are Uncompressed whatever
        // happened, and the memory checks below are Refresh's.
        const auto& own = image->Descriptor();
        if (own.dccAddress != 0) return false;
        const auto address = own.baseAddress;
        const auto bytes = static_cast<std::size_t>(image->GuestBytes());
        if (GuestMemory::CollectWrites(address, bytes) == 0) return false;
        if (PendingStorageOverlaps(address, bytes, image)) return false;
        if (!StorageImageCached(context, image)) return false;
        if (!GuestMemory::UnchangedSince(address, bytes, image->Generation())) return false;
    }
    // Every check passed: snapshot textures are current at the collects issued before them. Storage
    // images keep their own generation: nothing was stamped above it, so the blocks a later write
    // stamps are the same set either way.
    for (auto& surface : validatedTextures) {
        if (surface.source == nullptr) surface.generation = surface.collected;
    }
    return true;
}

ShaderResources::OwnRefreshFallback ShaderResources::refreshOwnObjects(std::span<const CompiledShader> shaders, std::span<PendingOverlap> overlapping) {
    const auto listed = [&](std::size_t element, bool storage) {
        return std::find_if(overlapping.begin(), overlapping.end(), [&](const PendingOverlap& overlap) { return overlap.element == element && overlap.storage == storage; });
    };
    // A sampled view's lookup (cachedTexture): the pending image FindPending serves the surface
    // by, if it fits the view, else the cached storage image of the surface's key, refreshed; the
    // view stays the lookup's answer only when that image is its own source. `sourceEligible` is
    // recorded whichever branch answers: a later refresh of this call may flush the source, and
    // the re-run then judges the surface by the other branch.
    const auto refreshView = [&](std::size_t i, PendingOverlap& overlap) {
        const auto& surface = validatedTextures[i];
        const auto& source = textures[i]->SharedStorageSource();
        if (source == nullptr || source.get() != surface.source) return OwnRefreshFallback::Snapshot;
        if (!source->Cached()) return OwnRefreshFallback::Uncached;
        if (!StorageImageServesKeys(*source, surface.resource.dccAddress)) return OwnRefreshFallback::Keys;
        const auto address = surface.resource.baseAddress;
        const auto bytes = static_cast<std::size_t>(surface.bytes);
        const bool imported = SampledFromStorageEligible(context, surface.resource, surface.bytes);
        overlap.sourceEligible = imported && SurfaceKey(context, source->Descriptor()) == SurfaceKey(context, surface.resource);
        auto found = StorageTexture::FindPending(address, surface.bytes);
        if (found != nullptr && !Texture::CanCopyFrom(*found, surface.resource)) found.reset();
        if (found != nullptr) {
            if (found != source) return OwnRefreshFallback::ForeignView;
            if (!GuestMemory::UnchangedSince(address, bytes, source->Generation())) {
                countOwnRefresh(false);
                source->Refresh();
            }
            return OwnRefreshFallback::Count;
        }
        if (!imported) return OwnRefreshFallback::NotImported;
        if (!overlap.sourceEligible) return OwnRefreshFallback::SurfaceKey;
        countOwnRefresh(false);
        // The cache's image of the key is this source while it is cached (an entry is replaced
        // only after its image left the cache), so the lookup would refresh and return it.
        source->Refresh();
        return OwnRefreshFallback::Count;
    };
    // A storage element's lookup (cachedStorageTexture): the cached image of its surface key,
    // refreshed; the key is the element's words', which mapped to this image at the build.
    const auto refreshStorage = [&](std::size_t i) {
        auto& image = storageTextures[i];
        if (image == nullptr || !image->Cached()) return OwnRefreshFallback::Uncached;
        if (i >= storageKeys.size() || !StorageImageServesKeys(*image, storageKeys[i])) return OwnRefreshFallback::Keys;
        countOwnRefresh(true);
        image->Refresh();
        return OwnRefreshFallback::Count;
    };
    std::size_t textureIndex = 0;
    std::size_t storageIndex = 0;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) return OwnRefreshFallback::Uncached;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
            if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element, ++textureIndex) {
                    if (textureIndex >= textures.size()) return OwnRefreshFallback::Uncached;
                    const auto overlap = listed(textureIndex, false);
                    if (overlap == overlapping.end()) continue;
                    if (const auto fallback = refreshView(textureIndex, *overlap); fallback != OwnRefreshFallback::Count) return fallback;
                }
            } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                for (std::uint32_t element = 0; element < binding.count; ++element, ++storageIndex) {
                    if (storageIndex >= storageTextures.size()) return OwnRefreshFallback::Uncached;
                    if (listed(storageIndex, true) == overlapping.end()) continue;
                    if (const auto fallback = refreshStorage(storageIndex); fallback != OwnRefreshFallback::Count) return fallback;
                }
            }
        }
    }
    return OwnRefreshFallback::Count;
}

bool ShaderResources::Revalidate(std::span<const CompiledShader> shaders, ProofReport* report) {
    if (report != nullptr) *report = {ProofPath::Full, ProofFailure::Other};
    if (!reusable || shaders.empty()) return false;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    // APS5_NO_FAST_REVALIDATE=1 always repeats the lookups.
    static const bool noFast = std::getenv("APS5_NO_FAST_REVALIDATE") != nullptr;
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto* const scratch = proofScratch();
    auto& outerTick = scratch != nullptr ? scratch->outerTick : SeparateLocal<std::uint32_t, 1>();
    const bool outerTimed = profile && (++outerTick & 63u) == 0;
    std::array<std::chrono::steady_clock::time_point, 5> outerMarks{};
    const auto outerMark = [&](std::size_t at) {
        if (outerTimed) outerMarks[at] = std::chrono::steady_clock::now();
    };
    const auto finish = [&](bool fast, bool ok, ProofFailure failure = ProofFailure::Other) {
        if (outerTimed && fast && ok) {
            // Marks: 0 before the proof, 1 after it, 2 before the imports, 3 before the staging
            // copies, 4 = now; unset marks (a path that skipped them) are taken as the next one.
            const auto now = std::chrono::steady_clock::now();
            auto& counts = Revalidations();
            auto previous = start;
            for (std::size_t at = 0; at <= 4; ++at) {
                auto mark = at == 4 ? now : outerMarks[at];
                if (mark == std::chrono::steady_clock::time_point{}) mark = previous;
                counts.outerNanoseconds[at].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(mark - previous).count()), std::memory_order_relaxed);
                previous = mark;
            }
            counts.outerSamples.fetch_add(1, std::memory_order_relaxed);
        }
        if (profile) countRevalidate(fast, ok, start);
        if (report != nullptr) report->failure = ok ? ProofFailure::None : failure;
        return ok;
    };
    // Loaded before any check (the memo rule of fastRevalidate): kept as the memo only when the
    // registry did not move through the whole proof, flushes of this call included.
    const auto serialBefore = StorageTexture::PendingSerial();
    // (1) Textures and storage images, from stamps when every element allows it (fastRevalidate),
    // else by the same lookups as the build (which refresh or replace them as guest memory changed):
    // they must hand back the very objects the set's views belong to. The build appended them stage
    // by stage, binding by binding, so the walk repeats that order.
    // The first element the full walk would replace (APS5_VERIFY_FAST_PROOFS).
    char walkMismatch[200] = {};
    // APS5_TRACE_WALKS: the object the walk's lookup returned for that element, and the element.
    char walkReplacement[360] = {};
    std::size_t walkMismatchIndex = static_cast<std::size_t>(-1);
    bool walkMismatchStorage = false;
    const auto describeReplacement = [&](const Texture* texture, const StorageTexture* image) {
        const void* object = texture != nullptr ? static_cast<const void*>(texture) : static_cast<const void*>(image);
        const LookupRecord* record = nullptr;
        for (auto it = lookupLog.rbegin(); it != lookupLog.rend(); ++it) {
            if (it->object == object) { record = &*it; break; }
        }
        if (texture != nullptr) {
            if (const auto* source = texture->StorageSource(); source != nullptr) {
                const auto& own = source->Descriptor();
                std::snprintf(walkReplacement, sizeof(walkReplacement), " -> texture %p: view of image %p 0x%llx+0x%llx %ux%u f%u dcc 0x%llx gen %llu ver %llu", static_cast<const void*>(texture), static_cast<const void*>(source), static_cast<unsigned long long>(own.baseAddress), static_cast<unsigned long long>(source->GuestBytes()), own.width, own.height, static_cast<unsigned>(own.format), static_cast<unsigned long long>(own.dccAddress), static_cast<unsigned long long>(source->Generation()), static_cast<unsigned long long>(source->Version()));
            } else {
                std::snprintf(walkReplacement, sizeof(walkReplacement), " -> texture %p: snapshot", static_cast<const void*>(texture));
            }
        } else if (image != nullptr) {
            const auto& own = image->Descriptor();
            std::snprintf(walkReplacement, sizeof(walkReplacement), " -> image %p 0x%llx+0x%llx %ux%u f%u dcc 0x%llx gen %llu ver %llu", static_cast<const void*>(image), static_cast<unsigned long long>(own.baseAddress), static_cast<unsigned long long>(image->GuestBytes()), own.width, own.height, static_cast<unsigned>(own.format), static_cast<unsigned long long>(own.dccAddress), static_cast<unsigned long long>(image->Generation()), static_cast<unsigned long long>(image->Version()));
        } else {
            std::snprintf(walkReplacement, sizeof(walkReplacement), " -> none");
        }
        if (record != nullptr) {
            const auto used = std::strlen(walkReplacement);
            std::snprintf(walkReplacement + used, sizeof(walkReplacement) - used, " (record keys %s gen %llu source %p)", DccKeysName(record->keys), static_cast<unsigned long long>(record->generation), static_cast<const void*>(record->source));
        }
    };
    // Mode 7 of APS5_VERIFY_FAST_PROOFS walks without moving the records.
    bool recordsAfterWalk = true;
    const auto fullWalk = [&] {
        lookupLog.clear();
        std::size_t textureIndex = 0;
        std::size_t storageIndex = 0;
        for (const auto& shader : shaders) {
            if (shader.program == nullptr) return false;
            for (const auto& binding : shader.program->bindings) {
                if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
                if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                    const auto elementWords = binding.count != 0 ? binding.guestDescriptor.size() / binding.count : 0;
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
                        std::array<std::uint32_t, 8> nullWords{};
                        const auto resource = decodeBoundSampled(words, nullWords, binding.imageShape);
                        const VkComponentMapping components{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
                        const auto found = textureIndex < textures.size() ? cachedTexture(context, words, resource, components, 0, !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element)) : decltype(textures)::value_type{};
                        if (textureIndex >= textures.size() || found != textures[textureIndex]) {
                            std::snprintf(walkMismatch, sizeof(walkMismatch), "sampled texture %zu of %zu (binding %u element %u) 0x%llx %ux%u format %u dcc 0x%llx", textureIndex, textures.size(), binding.binding, element, static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned>(resource.format), static_cast<unsigned long long>(resource.dccAddress));
                            if (TracePendingWalk()) {
                                walkMismatchIndex = textureIndex;
                                walkMismatchStorage = false;
                                describeReplacement(found.get(), nullptr);
                            }
                            return false;
                        }
                        ++textureIndex;
                    }
                } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                    for (std::uint32_t element = 0; element < binding.count; ++element) {
                        const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                        if (storageIndex >= storageTextures.size()) {
                            std::snprintf(walkMismatch, sizeof(walkMismatch), "storage image %zu beyond the %zu recorded", storageIndex, storageTextures.size());
                            return false;
                        }
                        const auto resource = DecodeTextureResource(words);
                        if (storageIndex < storageWritten.size() && storageWritten[storageIndex]) NoteDepthSurfaceWrite(resource.baseAddress, resource.width, resource.height);
                        std::shared_ptr<StorageTexture> expected;
                        if (SameAsPreviousStorageElement(binding, element) && StorageDedupeEnabled()) expected = storageTextures[storageIndex - 1];
                        else expected = cachedStorageTexture(context, words, resource, storageMips[storageIndex]);
                        if (expected != storageTextures[storageIndex]) {
                            std::snprintf(walkMismatch, sizeof(walkMismatch), "storage image %zu of %zu (binding %u element %u) 0x%llx %ux%u format %u dcc 0x%llx%s", storageIndex, storageTextures.size(), binding.binding, element, static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, static_cast<unsigned>(resource.format), static_cast<unsigned long long>(resource.dccAddress), storageIndex < storageWritten.size() && storageWritten[storageIndex] ? " written" : "");
                            if (TracePendingWalk()) {
                                walkMismatchIndex = storageIndex;
                                walkMismatchStorage = true;
                                describeReplacement(nullptr, expected.get());
                            }
                            return false;
                        }
                        ++storageIndex;
                    }
                }
            }
        }
        if (textureIndex != textures.size() || storageIndex != storageTextures.size()) {
            std::snprintf(walkMismatch, sizeof(walkMismatch), "element counts %zu/%zu textures, %zu/%zu storage images", textureIndex, textures.size(), storageIndex, storageTextures.size());
            return false;
        }
        // The walk proved every object current again: the records move to what it proved, or one
        // spurious stamp (a label sharing a 64 KiB block with a surface's edge) would keep this
        // object on the full walk for good.
        if (recordsAfterWalk) captureValidation();
        return true;
    };
    auto& overlapping = scratch != nullptr ? scratch->overlapping : SeparateLocal<std::vector<PendingOverlap>, 1>();
    auto& refreshed = scratch != nullptr ? scratch->refreshed : SeparateLocal<std::vector<PendingOverlap>, 2>();
    refreshed.clear();
    FastFail reason = FastFail::Count;
    bool accepted = false;
    outerMark(0);
    bool fast = !noFast && fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);
    outerMark(1);
    // T1 (design_cpu_final M3, rule RT1): a Pending failure whose overlapping images are foreign
    // to the surfaces is resolved by the own objects' refresh (what the walk's lookups would do to
    // them) and the fast proof run again, which is then authoritative (it accepts what stays
    // pending over the refreshed surfaces, see fastRevalidate); a fallback, or a second failure,
    // takes the full walk as before.
    bool ownRefreshed = false;
    auto fallback = OwnRefreshFallback::Count;
    // APS5_TRACE_PENDING_WALK: the surfaces the first proof found pending and their records, before
    // T1 and the walk move them.
    thread_local std::vector<PendingOverlap> tracedOverlaps;
    thread_local std::vector<ValidatedSurface> tracedRecords;
    const bool tracePending = TracePendingWalk() && !fast;
    if (tracePending) {
        tracedOverlaps = overlapping;
        tracedRecords = validatedTextures;
    }
    if (!fast && !overlapping.empty()) {
        if (!OwnImageRefreshEnabled()) fallback = OwnRefreshFallback::Disabled;
        else {
            refreshed = overlapping;
            fallback = refreshOwnObjects(shaders, refreshed);
        }
        if (fallback == OwnRefreshFallback::Count) {
            fast = fastRevalidate(serialBefore, refreshed, reason, overlapping, accepted);
            if (fast) ownRefreshed = true;
            else fallback = OwnRefreshFallback::Rerun;
        }
        if (fallback != OwnRefreshFallback::Count) countOwnRefreshFallback(fallback);
    }
    if (report != nullptr) report->path = !fast ? ProofPath::Full : ownRefreshed ? ProofPath::OwnRefreshed : ProofPath::Fast;
    // APS5_TRACE_PENDING_WALK (see TracePendingWalk).
    const auto tracePendingCase = [&](bool walked) {
        constexpr std::size_t reasons = static_cast<std::size_t>(FastFail::Count) + 1;
        static std::atomic<std::uint64_t> traced[2][reasons][2];
        static HostMutex traceMutex;
        static std::unordered_map<std::uint64_t, unsigned> tracedByVariant[reasons][2];
        static std::atomic<long long> lastTrace{0};
        const std::size_t group = shaders.front().stage == ShaderRecompiler::ShaderStage::Compute ? 0 : 1;
        const std::size_t reasonIndex = std::min<std::size_t>(static_cast<std::size_t>(reason), static_cast<std::size_t>(FastFail::Count));
        const char* reasonName = reason == FastFail::Count ? "no fast proof" : FastFailNames[static_cast<std::size_t>(reason)];
        traced[group][reasonIndex][walked ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        const auto variantId = shaders.front().program != nullptr ? shaders.front().program->variantId : 0;
        bool print = false;
        {
            std::lock_guard traceLock(traceMutex);
            auto& byVariant = tracedByVariant[reasonIndex][walked ? 1 : 0];
            auto found = byVariant.find(variantId);
            if (found == byVariant.end() && byVariant.size() < 64) found = byVariant.emplace(variantId, 0u).first;
            if (found != byVariant.end() && found->second < 2) {
                ++found->second;
                print = true;
            }
        }
        if (print) {
            std::string text;
            char item[520];
            for (const auto& overlap : tracedOverlaps) {
                const bool still = std::find_if(overlapping.begin(), overlapping.end(), [&](const PendingOverlap& entry) { return entry.element == overlap.element && entry.storage == overlap.storage; }) != overlapping.end();
                std::uint64_t address = 0;
                std::uint64_t bytes = 0;
                const StorageTexture* own = nullptr;
                if (overlap.storage) {
                    const auto* image = overlap.element < storageTextures.size() ? storageTextures[overlap.element].get() : nullptr;
                    if (image == nullptr) continue;
                    const auto& desc = image->Descriptor();
                    address = desc.baseAddress;
                    bytes = image->GuestBytes();
                    own = image;
                    std::snprintf(item, sizeof(item), " ; storage %zu%s: image %p 0x%llx+0x%llx %ux%u f%u dcc 0x%llx gen %llu ver %llu", overlap.element, still ? " (still pending after T1)" : "", static_cast<const void*>(image), static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), desc.width, desc.height, static_cast<unsigned>(desc.format), static_cast<unsigned long long>(desc.dccAddress), static_cast<unsigned long long>(image->Generation()), static_cast<unsigned long long>(image->Version()));
                } else {
                    if (overlap.element >= tracedRecords.size()) continue;
                    const auto& record = tracedRecords[overlap.element];
                    address = record.resource.baseAddress;
                    bytes = record.bytes;
                    own = record.source;
                    char sourceText[160] = "snapshot";
                    if (record.source != nullptr) {
                        const auto& desc = record.source->Descriptor();
                        std::snprintf(sourceText, sizeof(sourceText), "view of image %p 0x%llx %ux%u f%u gen %llu ver %llu", static_cast<const void*>(record.source), static_cast<unsigned long long>(desc.baseAddress), desc.width, desc.height, static_cast<unsigned>(desc.format), static_cast<unsigned long long>(record.source->Generation()), static_cast<unsigned long long>(record.source->Version()));
                    }
                    std::snprintf(item, sizeof(item), " ; sampled %zu%s%s%s: 0x%llx+0x%llx %ux%u f%u t%d mips %u dcc 0x%llx keys %s gen %llu collected %llu, %s", overlap.element, overlap.viewUncompressed ? " (view uncompressed)" : "", overlap.sourceEligible ? " (source eligible)" : "", still ? " (still pending after T1)" : "", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), record.resource.width, record.resource.height, static_cast<unsigned>(record.resource.format), static_cast<int>(record.resource.tileMode), record.resource.mipCount, static_cast<unsigned long long>(record.resource.dccAddress), DccKeysName(record.keys), static_cast<unsigned long long>(record.generation), static_cast<unsigned long long>(record.collected), sourceText);
                    text += item;
                    if (walked && overlap.element < validatedTextures.size()) {
                        const auto& now = validatedTextures[overlap.element];
                        std::snprintf(item, sizeof(item), " | walk record: %s keys %s gen %llu source %p", now.valid ? "valid" : "INVALID", DccKeysName(now.keys), static_cast<unsigned long long>(now.generation), static_cast<const void*>(now.source));
                    } else {
                        item[0] = 0;
                    }
                }
                text += item;
                StorageTexture::PendingQuery query{address, address + bytes, own, nullptr, false};
                StorageTexture::ScanPending(std::span<StorageTexture::PendingQuery>(&query, 1));
                if (query.found != nullptr) {
                    const auto& desc = query.found->Descriptor();
                    std::snprintf(item, sizeof(item), " | pending now: image %p 0x%llx+0x%llx %ux%u f%u t%d mips %u dcc 0x%llx gen %llu ver %llu%s", static_cast<const void*>(query.found), static_cast<unsigned long long>(desc.baseAddress), static_cast<unsigned long long>(query.found->GuestBytes()), desc.width, desc.height, static_cast<unsigned>(desc.format), static_cast<int>(desc.tileMode), desc.mipCount, static_cast<unsigned long long>(desc.dccAddress), static_cast<unsigned long long>(query.found->Generation()), static_cast<unsigned long long>(query.found->Version()), query.overlaps ? "" : " (found but no overlap)");
                } else {
                    std::snprintf(item, sizeof(item), " | pending now: none%s", query.overlaps ? " (a store in progress overlaps)" : "");
                }
                text += item;
            }
            char oldText[400] = {};
            if (!walked && !walkMismatchStorage && walkMismatchIndex < tracedRecords.size()) {
                const auto& record = tracedRecords[walkMismatchIndex];
                char sourceText[160] = "snapshot";
                if (record.source != nullptr) {
                    const auto& desc = record.source->Descriptor();
                    std::snprintf(sourceText, sizeof(sourceText), "view of image %p 0x%llx %ux%u f%u gen %llu ver %llu", static_cast<const void*>(record.source), static_cast<unsigned long long>(desc.baseAddress), desc.width, desc.height, static_cast<unsigned>(desc.format), static_cast<unsigned long long>(record.source->Generation()), static_cast<unsigned long long>(record.source->Version()));
                }
                std::snprintf(oldText, sizeof(oldText), " [old record: %s 0x%llx+0x%llx %ux%u f%u t%d mips %u dcc 0x%llx keys %s gen %llu collected %llu, %s]", record.valid ? "valid" : "INVALID", static_cast<unsigned long long>(record.resource.baseAddress), static_cast<unsigned long long>(record.bytes), record.resource.width, record.resource.height, static_cast<unsigned>(record.resource.format), static_cast<int>(record.resource.tileMode), record.resource.mipCount, static_cast<unsigned long long>(record.resource.dccAddress), DccKeysName(record.keys), static_cast<unsigned long long>(record.generation), static_cast<unsigned long long>(record.collected), sourceText);
            }
            std::fprintf(stderr, "[rescache] full walk (%s; stage %u variant 0x%llx, %zu textures %zu storage; %zu pending, %zu after T1; T1 %s): %s%s%s%s%s\n", reasonName, static_cast<unsigned>(shaders.front().stage), static_cast<unsigned long long>(variantId), textures.size(), storageTextures.size(), tracedOverlaps.size(), overlapping.size(), tracedOverlaps.empty() ? "n/a" : fallback == OwnRefreshFallback::Count ? "resolved" : OwnRefreshFallbackNames[static_cast<std::size_t>(fallback)], walked ? "SAME objects" : "REBUILD at ", walked ? "" : walkMismatch, walked ? "" : walkReplacement, oldText, text.c_str());
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = lastTrace.load();
        if (nowMs - last >= 10000 && lastTrace.compare_exchange_strong(last, nowMs)) {
            std::string line;
            char item[96];
            for (std::size_t g = 0; g < 2; ++g) {
                line += g == 0 ? " compute:" : " | graphics:";
                for (std::size_t r = 0; r < reasons; ++r) {
                    const auto same = traced[g][r][1].load(std::memory_order_relaxed);
                    const auto rebuild = traced[g][r][0].load(std::memory_order_relaxed);
                    if (same == 0 && rebuild == 0) continue;
                    std::snprintf(item, sizeof(item), " %s %llu/%llu", r == static_cast<std::size_t>(FastFail::Count) ? "no fast proof" : FastFailNames[r], static_cast<unsigned long long>(same), static_cast<unsigned long long>(rebuild));
                    line += item;
                }
            }
            std::fprintf(stderr, "[rescache] full walks by reason (cumulative, same/rebuild):%s\n", line.c_str());
        }
    };
    if (!fast) {
        countFullWalk(reason);
        const bool walked = fullWalk();
        if (tracePending) tracePendingCase(walked);
        if (!walked) {
            const auto failure = [&] {
                switch (reason) {
                    case FastFail::Pending: return ProofFailure::Pending;
                    case FastFail::Evicted: return ProofFailure::Evicted;
                    case FastFail::Changed: return ProofFailure::Changed;
                    case FastFail::Keys: case FastFail::ClearedView: case FastFail::StorageKeys: return ProofFailure::Keys;
                    default: return ProofFailure::Other;
                }
            }();
            return finish(false, false, failure);
        }
    } else if (ownRefreshed || accepted) {
        if (profile && ownRefreshed) Revalidations().ownRefreshed.fetch_add(1, std::memory_order_relaxed);
        if (VerifyProofs()) {
            // The walk after the refresh, or beside a proof that accepted a foreign overlap, must
            // find every object in place and current: another object, or an upload (a moved
            // content version), is a decision the proof got wrong.
            thread_local std::vector<std::pair<const StorageTexture*, std::uint64_t>> versions;
            versions.clear();
            for (const auto& surface : validatedTextures) {
                if (surface.source != nullptr) versions.emplace_back(surface.source, surface.source->Version());
            }
            for (const auto& image : storageTextures) versions.emplace_back(image.get(), image->Version());
            const bool same = fullWalk();
            const auto moved = std::find_if(versions.begin(), versions.end(), [](const auto& entry) { return entry.first->Version() != entry.second; });
            if (!same || moved != versions.end()) {
                std::fprintf(stderr, "[rescache] APS5_VERIFY_PROOFS: the T1 proof (%s) disagrees with the full walk (%s)\n", ownRefreshed ? "own-object refresh" : "accepted overlap", !same ? "another object" : "an upload");
                std::fflush(stderr);
                std::abort();
            }
            if (profile) Revalidations().proofsVerified.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (fast && VerifyFastProofs()) {
        // The walk beside the proof: another object, or an upload (a moved content version), is a
        // decision the fast proof got wrong (APS5_VERIFY_FAST_PROOFS, see VerifyFastProofs).
        thread_local std::vector<std::pair<const StorageTexture*, std::uint64_t>> fastVersions;
        fastVersions.clear();
        for (const auto& surface : validatedTextures) {
            if (surface.source != nullptr) fastVersions.emplace_back(surface.source, surface.source->Version());
        }
        for (const auto& image : storageTextures) fastVersions.emplace_back(image.get(), image->Version());
        walkMismatch[0] = 0;
        // What the walk's lookups did beside the proof (session 22): this thread's lookup outcomes
        // and the texture-cache counters before and after, summed per template stage (compute /
        // graphics) and printed with the 10 s line, to name the side effect the fast path lacks.
        static std::atomic<std::uint64_t> walkKinds[2][LookupOutcomes::Count];
        static std::atomic<std::uint64_t> walkCacheDeltas[2][4];
        const std::size_t walkGroup = shaders.front().stage == ShaderRecompiler::ShaderStage::Compute ? 0 : 1;
        auto& walkOutcomes = ThreadLookupOutcomes();
        const auto kindsBefore = walkOutcomes.counts;
        auto& walkCounters = TextureCounts();
        const std::uint64_t cacheBefore[4] = {walkCounters.replaced.load(std::memory_order_relaxed), walkCounters.storageCreated.load(std::memory_order_relaxed), walkCounters.snapshots.load(std::memory_order_relaxed), walkCounters.fromStorage.load(std::memory_order_relaxed)};
        diffRecordsNow = VerifyFastProofsMode() == 8;
        const bool same = fullWalk();
        diffRecordsNow = false;
        for (std::size_t k = 0; k < LookupOutcomes::Count; ++k) {
            if (walkOutcomes.counts[k] != kindsBefore[k]) walkKinds[walkGroup][k].fetch_add(walkOutcomes.counts[k] - kindsBefore[k], std::memory_order_relaxed);
        }
        const std::uint64_t cacheAfter[4] = {walkCounters.replaced.load(std::memory_order_relaxed), walkCounters.storageCreated.load(std::memory_order_relaxed), walkCounters.snapshots.load(std::memory_order_relaxed), walkCounters.fromStorage.load(std::memory_order_relaxed)};
        for (std::size_t k = 0; k < 4; ++k) {
            if (cacheAfter[k] != cacheBefore[k]) walkCacheDeltas[walkGroup][k].fetch_add(cacheAfter[k] - cacheBefore[k], std::memory_order_relaxed);
        }
        const auto moved = std::find_if(fastVersions.begin(), fastVersions.end(), [](const auto& entry) { return entry.first->Version() != entry.second; });
        static std::atomic<std::uint64_t> verified{0};
        static std::atomic<std::uint64_t> wrong{0};
        // The first two cases of each variant (64 variants at most), so a case that appears only
        // in gameplay is printed after the intro's.
        static HostMutex printedMutex;
        static std::unordered_map<std::uint64_t, unsigned> printedByVariant;
        static std::atomic<long long> lastReport{0};
        verified.fetch_add(1, std::memory_order_relaxed);
        const bool stale = !same || moved != fastVersions.end();
        if (stale) {
            wrong.fetch_add(1, std::memory_order_relaxed);
            const auto variantId = shaders.front().program != nullptr ? shaders.front().program->variantId : 0;
            bool print = false;
            {
                std::lock_guard printedLock(printedMutex);
                auto found = printedByVariant.find(variantId);
                if (found == printedByVariant.end() && printedByVariant.size() < 64) found = printedByVariant.emplace(variantId, 0u).first;
                if (found != printedByVariant.end() && found->second < 2) {
                    ++found->second;
                    print = true;
                }
            }
            if (print) {
                char movedText[160] = {};
                if (same && moved != fastVersions.end()) {
                    const auto& own = moved->first->Descriptor();
                    std::snprintf(movedText, sizeof(movedText), "image 0x%llx %ux%u format %u re-uploaded (version %llu -> %llu)", static_cast<unsigned long long>(own.baseAddress), own.width, own.height, static_cast<unsigned>(own.format), static_cast<unsigned long long>(moved->second), static_cast<unsigned long long>(moved->first->Version()));
                }
                std::fprintf(stderr, "[rescache] fast proof wrong (%s path, stage %u variant 0x%llx, %zu textures %zu storage images): %s%s | %s\n", ownRefreshed ? "T1" : accepted ? "accepted-overlap" : "fast", static_cast<unsigned>(shaders.front().stage), static_cast<unsigned long long>(shaders.front().program != nullptr ? shaders.front().program->variantId : 0), textures.size(), storageTextures.size(), same ? "" : walkMismatch, movedText, Describe().c_str());
            }
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = lastReport.load();
        if (nowMs - last >= 10000 && lastReport.compare_exchange_strong(last, nowMs)) {
            std::fprintf(stderr, "[rescache] fast proofs verified %llu, wrong %llu\n", static_cast<unsigned long long>(verified.load()), static_cast<unsigned long long>(wrong.load()));
            std::string kinds;
            char item[128];
            for (std::size_t k = 0; k < LookupOutcomes::Count; ++k) {
                const auto compute = walkKinds[0][k].load(std::memory_order_relaxed);
                const auto graphics = walkKinds[1][k].load(std::memory_order_relaxed);
                if (compute == 0 && graphics == 0) continue;
                std::snprintf(item, sizeof(item), " %s %llu/%llu", LookupOutcomes::Name(static_cast<LookupOutcomes::Kind>(k)), static_cast<unsigned long long>(compute), static_cast<unsigned long long>(graphics));
                kinds += item;
            }
            static const char* const cacheNames[4] = {"replaced", "storage created", "snapshots made", "views made"};
            for (std::size_t k = 0; k < 4; ++k) {
                std::snprintf(item, sizeof(item), " | %s %llu/%llu", cacheNames[k], static_cast<unsigned long long>(walkCacheDeltas[0][k].load(std::memory_order_relaxed)), static_cast<unsigned long long>(walkCacheDeltas[1][k].load(std::memory_order_relaxed)));
                kinds += item;
            }
            std::fprintf(stderr, "[rescache] walks beside proofs (compute/graphics, cumulative):%s\n", kinds.c_str());
        }
        if (stale) return finish(false, false, ProofFailure::Other);
    } else if (fast && VerifyFastProofsMode() >= 2) {
        // One class of the walk's side effects beside the accepted proof, no verdict (see
        // VerifyFastProofsMode).
        const auto mode = VerifyFastProofsMode();
        if (mode == 7) {
            recordsAfterWalk = false;
            fullWalk();
            recordsAfterWalk = true;
        } else if (mode == 6) {
            static const long long spinUs = [] { const char* v = std::getenv("APS5_FAST_PROOF_SPIN_US"); return v != nullptr ? std::strtoll(v, nullptr, 10) : 20ll; }();
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs);
            while (std::chrono::steady_clock::now() < until) {}
        } else if (mode == 4) {
            for (const auto& surface : validatedTextures) {
                if (surface.valid && surface.source == nullptr) StorageTexture::FlushPending(surface.resource.baseAddress, static_cast<std::size_t>(surface.bytes), nullptr, "sampled texture");
            }
        } else if (mode == 5) {
            for (const auto& texture : textures) {
                if (texture == nullptr) continue;
                if (const auto& source = texture->SharedStorageSource(); source != nullptr) source->Refresh();
            }
            for (const auto& image : storageTextures) {
                if (image != nullptr) image->Refresh();
            }
        } else {
            lookupLog.clear();
            std::size_t storageIndex = 0;
            for (const auto& shader : shaders) {
                if (shader.program == nullptr) break;
                for (const auto& binding : shader.program->bindings) {
                    if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages) continue;
                    if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
                        if (mode != 2) continue;
                        const auto elementWords = binding.count != 0 ? binding.guestDescriptor.size() / binding.count : 0;
                        for (std::uint32_t element = 0; element < binding.count; ++element) {
                            auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
                            std::array<std::uint32_t, 8> nullWords{};
                            const auto resource = decodeBoundSampled(words, nullWords, binding.imageShape);
                            const VkComponentMapping components{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
                            cachedTexture(context, words, resource, components, 0, !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element));
                        }
                    } else if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
                        for (std::uint32_t element = 0; element < binding.count; ++element, ++storageIndex) {
                            if (mode != 3 || storageIndex >= storageTextures.size()) continue;
                            if (SameAsPreviousStorageElement(binding, element) && StorageDedupeEnabled()) continue;
                            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
                            cachedStorageTexture(context, words, DecodeTextureResource(words), storageMips[storageIndex]);
                        }
                    }
                }
            }
            lookupLog.clear();
        }
    }
    // (2) The imported buffers the set reads in place: results of storage images pending in them go
    // to guest memory first (as an upload does), then each import must still be the one the set was
    // written against. This comes last because the lookups and flushes above can reconcile imports
    // themselves; nothing else touches them between here and the dispatch being recorded. Under the
    // epoch gate the flush runs only for regions one registry scan finds pending images over (none
    // while the serial is the memo's), and the serial loop only while the import table's identity
    // moved since the last proof (a retire bumps its epoch, a registry change its generation).
    outerMark(2);
    const auto serialLoop = [&] {
        for (const auto& region : directRegions) {
            auto serial = HostImportSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin), true);
            if (serial == 0) serial = ImageMirrorSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin));
            if (serial != region.serial) return false;
        }
        return true;
    };
    if (EpochRevalidate()) {
        if (!directRegions.empty() && !(pendingSerialSeen != 0 && pendingSerialSeen == StorageTexture::PendingSerial())) {
            auto& regions = scratch != nullptr ? scratch->regions : SeparateLocal<std::vector<StorageTexture::PendingQuery>, 1>();
            regions.clear();
            for (const auto& region : directRegions) regions.push_back({region.begin, region.end, nullptr, nullptr, false});
            StorageTexture::ScanPending(regions);
            // A region over a unit shadow's fresh results (a retile bumped the serial) is
            // published by the flush, as one over a pending image is stored.
            for (const auto& region : regions) {
                if (region.overlaps || AnyShadowedOverlaps(region.begin, static_cast<std::size_t>(region.end - region.begin))) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
            }
        }
        if (HostImportsUnchanged(context, importsProof)) {
            if (profile) Revalidations().importSkips.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
            importsProof = HostImportsIdentity(context);
        }
    } else {
        for (const auto& region : directRegions) StorageTexture::FlushPending(region.begin, static_cast<std::size_t>(region.end - region.begin), nullptr, "imported buffer region");
        if (!serialLoop()) return finish(fast, false, ProofFailure::Imports);
    }
    // (3) Buffers staged in device memory (GuestBufferMemory::AllowDeviceStaging) are copied in
    // from their imports anew for this use, after the flushes above and before the work is
    // recorded; a failure to record leaves the object unusable for this dispatch, not the batch.
    outerMark(3);
    if (auto* recorder = Recorder::Active(); recorder != nullptr) {
        try {
            guestMemory.RecordStagingCopies(*recorder);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[resources] staging copies of a reused build failed: %s\n", error.what());
            return finish(fast, false);
        }
    }
    pendingSerialSeen = EpochRevalidate() && StorageTexture::PendingSerial() == serialBefore ? serialBefore : 0;
    return finish(fast, true);
}

namespace {

// APS5_PROFILE_DRAW: what makes content keys miss. On a miss the key is compared with the last key
// seen for the same shader variant(s) and the first differing word is charged to its binding's role
// (and to its index within the V# or T# element), printed as [rescache] every 10 s: it says whether
// V# bases of ring allocations, SRT words or image descriptors churn.
struct ChurnCounts {
    std::array<std::uint64_t, 8> roles{};
    std::array<std::uint64_t, 4> bufferWords{};
    std::array<std::uint64_t, 8> imageWords{};
    std::uint64_t firstSeen = 0;
    std::uint64_t sameKey = 0;
    std::uint64_t layout = 0;
    std::uint64_t drawWords = 0;
};

// Dispatch keys and draw keys are counted apart: what churns a draw's key decides the draw steps.
struct ChurnProfile {
    HostMutex mutex;
    std::unordered_map<std::uint64_t, ResourceCache::Key> lastByVariant;
    ChurnCounts dispatch;
    ChurnCounts draws;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

ChurnProfile& Churn() {
    static ChurnProfile profile;
    return profile;
}

// Charges word `diff` of `key` to the binding of the ContentKey that starts at `at` (the layout of
// ShaderResources::ContentKey), moving `at` past that key; false when `diff` lies beyond it.
bool chargeShaderKey(ChurnCounts& profile, const ResourceCache::Key& key, std::size_t& at, std::size_t diff) {
    if (at + 5 > key.size() || diff < at + 5) {
        ++profile.layout;
        return true;
    }
    const bool dataWords = key[at] != 0;
    const auto bindings = key[at + 4];
    at += 5;
    for (std::uint32_t binding = 0; binding < bindings; ++binding) {
        if (at + 8 > key.size() || diff < at + 8) {
            ++profile.layout;
            return true;
        }
        const auto kind = key[at];
        const auto role = key[at + 1];
        const auto descriptorWords = dataWords || !DataRole(static_cast<ShaderRecompiler::DescriptorRole>(role)) ? key[at + 7] : 0u;
        at += 8;
        if (diff < at + descriptorWords) {
            if (role < profile.roles.size()) ++profile.roles[role];
            const auto index = diff - at;
            if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) ++profile.bufferWords[index % 4];
            else if (role == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::GuestImages) && (kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::SampledImage) || kind == static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::StorageImage))) ++profile.imageWords[index % 8];
            return true;
        }
        at += descriptorWords;
        for (int flags = 0; flags < 4; ++flags) {
            if (at >= key.size()) {
                ++profile.layout;
                return true;
            }
            const auto words = 1 + (static_cast<std::size_t>(key[at]) + 31) / 32;
            if (diff < at + words) {
                ++profile.layout;
                return true;
            }
            at += words;
        }
    }
    return false;
}

}

std::shared_ptr<ShaderResources> ResourceCache::Find(const Key& key) {
    resourceCacheFinds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) {
        noteMiss(key);
        return nullptr;
    }
    entries.splice(entries.begin(), entries, found->second);
    return found->second->second;
}

void ResourceCache::noteMiss(const Key& key) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile || key.size() < 5) return;
    // The variant(s) the key belongs to: a compute key starts with the data-words flag, the stage
    // and its variant, a draw key with a marker, the device and its stages' keys (Draw.cpp
    // DrawResourceKey).
    const bool draw = key[0] == 0xffffffffu;
    std::uint64_t variants = 0;
    if (draw) {
        std::size_t at = 4;
        for (std::uint32_t stage = 0; stage < key[3] && at + 4 < key.size(); ++stage) {
            variants = variants * 1000003ull ^ (key[at + 3] | (static_cast<std::uint64_t>(key[at + 4]) << 32u));
            at += 1 + key[at];
        }
    } else {
        variants = key[2] | (static_cast<std::uint64_t>(key[3]) << 32u);
    }
    auto& churn = Churn();
    std::lock_guard lock(churn.mutex);
    auto& counts = draw ? churn.draws : churn.dispatch;
    const auto found = churn.lastByVariant.find(variants);
    if (found == churn.lastByVariant.end()) {
        ++counts.firstSeen;
        churn.lastByVariant.emplace(variants, key);
    } else {
        const auto& previous = found->second;
        const auto common = std::min(key.size(), previous.size());
        std::size_t diff = 0;
        while (diff < common && key[diff] == previous[diff]) ++diff;
        if (diff == common && key.size() == previous.size()) {
            ++counts.sameKey;
        } else if (draw) {
            std::size_t at = 4;
            bool charged = false;
            for (std::uint32_t stage = 0; stage < key[3] && at < key.size() && !charged; ++stage) {
                ++at;
                charged = chargeShaderKey(counts, key, at, diff);
            }
            if (!charged) ++counts.drawWords;
        } else {
            std::size_t at = 0;
            if (!chargeShaderKey(counts, key, at, diff)) ++counts.layout;
        }
        found->second = key;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - churn.lastReport < std::chrono::seconds(10)) return;
    churn.lastReport = now;
    const auto report = [](const char* what, const ChurnCounts& churn) {
        std::string line = std::string("[rescache] miss churn (") + what + "), first differing word by role:";
        char item[256];
        for (std::size_t role = 0; role < churn.roles.size(); ++role) {
            if (churn.roles[role] == 0) continue;
            std::snprintf(item, sizeof(item), " %s %llu", roleName(static_cast<ShaderRecompiler::DescriptorRole>(role)), static_cast<unsigned long long>(churn.roles[role]));
            line += item;
            if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestBuffers)) {
                std::snprintf(item, sizeof(item), " (V# word %llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.bufferWords[0]), static_cast<unsigned long long>(churn.bufferWords[1]), static_cast<unsigned long long>(churn.bufferWords[2]), static_cast<unsigned long long>(churn.bufferWords[3]));
                line += item;
            } else if (role == static_cast<std::size_t>(ShaderRecompiler::DescriptorRole::GuestImages)) {
                std::snprintf(item, sizeof(item), " (T# word %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu)", static_cast<unsigned long long>(churn.imageWords[0]), static_cast<unsigned long long>(churn.imageWords[1]), static_cast<unsigned long long>(churn.imageWords[2]), static_cast<unsigned long long>(churn.imageWords[3]), static_cast<unsigned long long>(churn.imageWords[4]), static_cast<unsigned long long>(churn.imageWords[5]), static_cast<unsigned long long>(churn.imageWords[6]), static_cast<unsigned long long>(churn.imageWords[7]));
                line += item;
            }
        }
        std::snprintf(item, sizeof(item), "; layout %llu, draw target/index %llu, same key %llu (not inserted or evicted), first seen %llu", static_cast<unsigned long long>(churn.layout), static_cast<unsigned long long>(churn.drawWords), static_cast<unsigned long long>(churn.sameKey), static_cast<unsigned long long>(churn.firstSeen));
        line += item;
        std::fprintf(stderr, "%s\n", line.c_str());
    };
    report("dispatch", churn.dispatch);
    report("draws", churn.draws);
}

void ResourceCache::Insert(const Key& key, std::shared_ptr<ShaderResources> resources, std::vector<std::shared_ptr<ShaderResources>>* evicted) {
    std::lock_guard lock(mutex);
    if (const auto found = index.find(key); found != index.end()) {
        if (evicted != nullptr) evicted->push_back(std::move(found->second->second));
        entries.erase(found->second);
        index.erase(found);
    }
    entries.emplace_front(key, std::move(resources));
    index.emplace(key, entries.begin());
    // Entries pin their textures and storage images past the texture caches' budgets, so the bound
    // stays modest: APS5_RESOURCE_CACHE_ENTRIES (default 4096) covers several frames of distinct
    // dispatch and draw content. Below the working set it thrashes (Boletaria at 1024: 11.3k
    // draw-template builds per 10 s at 66-78 us, most of them templates evicted under the same key;
    // 4096 entries leave 3.4k). Compute templates then survive across frames, which exposed the fast
    // proof's missing depth-surface check (fixed in session 23, FastFail::DepthSurface; PROGRESS
    // sessions 21-23): at 4096 the game drew twice as much until then.
    // With the rebased lease templates (default, see noteReusable) the default is 16384: at 4096 they
    // were evicted before their key recurred (t327: 8k rebased finds per 10 s found none held, 3k
    // rearms; t328 at 16384: 5.8k rearms). 16384 alone changed nothing (t329).
    static const std::size_t capacity = [] {
        const auto fallback = std::getenv("APS5_NO_LEASE_REBASE") == nullptr && TemplateDataRefresh() ? 16384ull : 4096ull;
        const char* value = std::getenv("APS5_RESOURCE_CACHE_ENTRIES");
        const auto parsed = value ? std::strtoull(value, nullptr, 10) : fallback;
        return static_cast<std::size_t>(parsed != 0 ? parsed : fallback);
    }();
    while (entries.size() > capacity) {
        if (evicted != nullptr) evicted->push_back(std::move(entries.back().second));
        index.erase(entries.back().first);
        entries.pop_back();
    }
}

void ResourceCache::Remove(const Key& key, const ShaderResources* object) {
    std::lock_guard lock(mutex);
    if (object != nullptr) {
        const auto found = index.find(key);
        if (found == index.end() || found->second->second.get() != object) return;
    }
    erase(key);
}

bool ResourceCache::Touch(const Key& key) {
    resourceCacheTouches.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    const auto found = index.find(key);
    if (found == index.end()) return false;
    entries.splice(entries.begin(), entries, found->second);
    return true;
}

std::uint64_t ResourceCache::Finds() {
    return resourceCacheFinds.load(std::memory_order_relaxed);
}

std::uint64_t ResourceCache::Touches() {
    return resourceCacheTouches.load(std::memory_order_relaxed);
}

void ResourceCache::Clear() {
    std::lock_guard lock(mutex);
    index.clear();
    entries.clear();
}

std::size_t ResourceCache::Size() const {
    std::lock_guard lock(mutex);
    return entries.size();
}

void ResourceCache::erase(const Key& key) {
    const auto found = index.find(key);
    if (found == index.end()) return;
    entries.erase(found->second);
    index.erase(found);
}

ResourceCache& SharedResourceCache() {
    static auto* cache = new ResourceCache();
    return *cache;
}

DescriptorCache::DescriptorCache(const Context& context) : context(context), destroyLayout(context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")), destroyPool(context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")), freeSets(context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")) {}

DescriptorCache::~DescriptorCache() {
    for (const auto pool : pools) destroyPool(context.device, pool, nullptr);
    for (const auto& [key, layout] : layouts) destroyLayout(context.device, layout, nullptr);
}

VkDescriptorSetLayout DescriptorCache::Layout(std::span<const std::uint32_t> key, std::span<const VkDescriptorSetLayoutBinding> bindings) {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> keyCopy(key.begin(), key.end());
    if (const auto found = layouts.find(keyCopy); found != layouts.end()) {
        ++stats.layoutHits;
        return found->second;
    }
    ++stats.layoutMisses;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
    layouts.emplace(std::move(keyCopy), layout);
    return layout;
}

namespace {
// What one chain pool holds; a set needing more of any type gets a dedicated pool.
constexpr std::uint32_t ChainPoolSets = 1024;
constexpr std::array<VkDescriptorPoolSize, 4> ChainPoolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512}}};
}

DescriptorCache::SetAllocation DescriptorCache::Allocate(VkDescriptorSetLayout layout, std::span<const VkDescriptorPoolSize> sizes) {
    for (const auto& size : sizes) {
        const auto capacity = std::find_if(ChainPoolSizes.begin(), ChainPoolSizes.end(), [&](const auto& item) { return item.type == size.type; });
        if (capacity == ChainPoolSizes.end() || size.descriptorCount > capacity->descriptorCount) return {};
    }
    std::lock_guard lock(mutex);
    const auto allocate = context.Resolved(&DeviceFunctions::allocateDescriptorSets, "vkAllocateDescriptorSets");
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &layout;
    // Newest pool first: it has the most room; a full or fragmented pool is left for its sets to
    // drain and tried again later.
    for (auto it = pools.rbegin(); it != pools.rend(); ++it) {
        allocation.descriptorPool = *it;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto result = allocate(context.device, &allocation, &set);
        if (result == VK_SUCCESS) {
            ++stats.sets;
            return {set, *it};
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) Check(result, "vkAllocateDescriptorSets");
    }
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = ChainPoolSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(ChainPoolSizes.size());
    poolInfo.pPoolSizes = ChainPoolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    pools.push_back(pool);
    ++stats.pools;
    allocation.descriptorPool = pool;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(allocate(context.device, &allocation, &set), "vkAllocateDescriptorSets");
    ++stats.sets;
    return {set, pool};
}

void DescriptorCache::Free(const SetAllocation& allocation) noexcept {
    if (allocation.set == VK_NULL_HANDLE || allocation.pool == VK_NULL_HANDLE) return;
    std::lock_guard lock(mutex);
    static_cast<void>(freeSets(context.device, allocation.pool, 1, &allocation.set));
}

DescriptorCache::Stats DescriptorCache::Counters() const {
    std::lock_guard lock(mutex);
    return stats;
}

std::size_t ShaderResources::addGuestBuffer(std::span<const std::uint32_t> words, const ColorTarget* target, std::uint64_t indexAddress, std::size_t indexBytes, bool written, bool atomic) {
    Require(words.size() == 4, "buffer descriptor must contain four DWORDs");
    Require((words[1] & 0x40000000u) == 0, "buffer descriptor has reserved bits set");
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
    Require(descriptor.Type() == 0u, "buffer descriptor uses an unsupported type");
    const auto address = descriptor.Base48();
    const auto byteSize = descriptor.GetSize();
    if (byteSize == 0 || address == 0) {
        allocations.push_back({0, EmptyBufferBytes, false, nullptr, ShaderRecompiler::DescriptorRole::GuestBuffers, false});
        return allocations.size() - 1;
    }
    Require(byteSize <= context.limits.maxStorageBufferRange, "shader buffer exceeds descriptor range limit");
    Require(byteSize <= std::numeric_limits<std::size_t>::max(), "shader buffer size exceeds host address space");
    const auto size = static_cast<std::size_t>(byteSize);
    Require(target == nullptr || !overlap(address, size, target->address, target->bytes), "shader buffer aliases the render target");
    // APS5_ALL_BUFFERS_WRITTEN=1: every element is noted as written, as before bufferWritten existed.
    static const bool allWritten = std::getenv("APS5_ALL_BUFFERS_WRITTEN") != nullptr;
    Require(!written || !overlap(address, size, indexAddress, indexBytes), "writable shader buffer aliases the index buffer");
    written = written || allWritten;
    if (written) guestMemory.AddWritable(address, size, atomic);
    else {
        guestMemory.AddReadable(address, size);
        ++readOnlyBuffers;
    }
    if (BuildProfiled()) {
        auto& counters = BufferWrites();
        counters.elements.fetch_add(1, std::memory_order_relaxed);
        if (!written) counters.readOnly.fetch_add(1, std::memory_order_relaxed);
    }
    allocations.push_back({address, size, true, nullptr, ShaderRecompiler::DescriptorRole::ShaderData, written});
    return allocations.size() - 1;
}

std::string ShaderResources::Describe() const {
    const auto sample = [](std::uint64_t address, std::uint64_t bytes) {
        // Heaps bound whole can include uncommitted pages; those ranges are not sampled.
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) return -1.0;
        std::size_t nonzero = 0;
        std::size_t samples = 0;
        const auto* data = reinterpret_cast<const std::uint32_t*>(address);
        const auto words = bytes / 4;
        const auto step = std::max<std::uint64_t>(1, words / 4096);
        for (std::uint64_t i = 0; i < words; i += step, ++samples) nonzero += data[i] != 0;
        return samples == 0 ? 0.0 : static_cast<double>(nonzero) / samples;
    };
    std::string text;
    char line[160];
    for (const auto& range : describedRanges) {
        std::snprintf(line, sizeof(line), " %s 0x%llx+0x%llx(%ux%u f%u t%d) nz=%.2f", range.kind, static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), range.width, range.height, range.format, range.tileMode, sample(range.address, range.bytes));
        text += line;
        if (range.dccAddress != 0) {
            const auto keys = ReadDccKeys(range.dccAddress, range.bytes);
            std::snprintf(line, sizeof(line), " dcc=%s@0x%llx", DccKeysName(keys), static_cast<unsigned long long>(range.dccAddress));
            text += line;
            if (keys == DccKeys::Mixed) {
                // Where the keys change: first key, how many leading keys match it, and the next key.
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(range.dccAddress);
                const auto count = static_cast<std::size_t>(range.bytes / 256u);
                std::size_t run = 1;
                while (run < count && bytes[run] == bytes[0]) ++run;
                std::snprintf(line, sizeof(line), "(%02x x%zu then %02x of %zu)", bytes[0], run, run < count ? bytes[run] : 0u, count);
                text += line;
            }
        }
    }
    // APS5_TRACE_DISPATCH_IO=2 also lists the words of small buffers (constants).
    static const bool words = [] { const char* value = std::getenv("APS5_TRACE_DISPATCH_IO"); return value != nullptr && value[0] == '2'; }();
    const auto appendWords = [&](const std::uint32_t* data, std::size_t bytes) {
        if (!words || bytes > 0x200) return;
        text += " [";
        for (std::size_t i = 0; i < bytes / 4; ++i) {
            char word[12];
            std::snprintf(word, sizeof(word), "%s%08x", i == 0 ? "" : " ", data[i]);
            text += word;
        }
        text += "]";
    };
    for (const auto& allocation : allocations) {
        if (allocation.guest) {
            std::snprintf(line, sizeof(line), " buffer%s 0x%llx+0x%zx nz=%.2f", allocation.written ? "" : "(ro)", static_cast<unsigned long long>(allocation.address), allocation.size, sample(allocation.address, allocation.size));
            text += line;
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(allocation.address), allocation.size)) appendWords(reinterpret_cast<const std::uint32_t*>(allocation.address), allocation.size);
        } else if (allocation.buffer) {
            const auto bytes = allocation.buffer->Bytes();
            std::snprintf(line, sizeof(line), " data+0x%zx nz=%.2f", allocation.size, sample(reinterpret_cast<std::uint64_t>(bytes.data()), allocation.size));
            text += line;
            appendWords(reinterpret_cast<const std::uint32_t*>(bytes.data()), allocation.size);
        }
    }
    return text;
}

std::size_t ShaderResources::addDataBuffer(std::span<const std::uint32_t> words) {
    const auto size = words.size() * sizeof(std::uint32_t);
    Require(size <= context.limits.maxStorageBufferRange, "shader data buffer exceeds descriptor range limit");
    const bool refreshable = TemplateDataRefresh() && size <= MaxRefreshBytes;
    auto buffer = std::make_unique<Buffer>(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (refreshable ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0u));
    std::memcpy(buffer->Bytes().data(), words.data(), size);
    Allocation allocation{0, size, false, std::move(buffer)};
    if (refreshable) allocation.dataWords.assign(words.begin(), words.end());
    allocations.push_back(std::move(allocation));
    mixDataWords(dataWordsHash, allocations.back().dataWords);
    return allocations.size() - 1;
}

void ShaderResources::rehashDataWords() {
    dataWordsHash = FnvOffset;
    // Data buffers are the non-guest allocations with a buffer (an address-role allocation has
    // none), appended in binding order by buildPrepare: the order DataWordsHash(shader) hashes.
    for (const auto& allocation : allocations) {
        if (!allocation.guest && allocation.buffer != nullptr) mixDataWords(dataWordsHash, allocation.dataWords);
    }
}

std::uint64_t ShaderResources::DataWordsHash(const CompiledShader& shader) {
    Require(shader.program != nullptr, "missing compiled shader");
    std::uint64_t hash = FnvOffset;
    for (const auto& binding : shader.program->bindings) {
        if (DataRole(binding.role)) mixDataWords(hash, binding.guestDescriptor);
    }
    return hash;
}

bool ShaderResources::DataWordsDiffer(const CompiledShader& shader) const {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    if (program.bindings.size() != bindings.size()) return true;
    for (std::size_t index = 0; index < program.bindings.size(); ++index) {
        const auto& binding = program.bindings[index];
        if (!DataRole(binding.role)) continue;
        if (bindings[index].allocations.size() != 1) return true;
        const auto& allocation = allocations[bindings[index].allocations.front()];
        if (allocation.dataWords.size() != binding.guestDescriptor.size() || !std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) return true;
    }
    return false;
}

bool ShaderResources::RefreshData(VkCommandBuffer commands, const CompiledShader& shader, Recorder* recorder) {
    Require(shader.program != nullptr, "missing compiled shader");
    const auto& program = *shader.program;
    Require(program.bindings.size() == bindings.size(), "template bindings disagree with the shader");
    bool recorded = false;
    // One [gputime] range of class TemplateDataRefresh around the updates, begun at the first one
    // (a refresh that finds every word equal records nothing and times nothing).
    auto timing = Recorder::NoTiming;
    std::uint64_t refreshedBytes = 0;
    for (std::size_t index = 0; index < program.bindings.size(); ++index) {
        const auto& binding = program.bindings[index];
        if (!DataRole(binding.role)) continue;
        Require(bindings[index].allocations.size() == 1, "data binding without its buffer");
        auto& allocation = allocations[bindings[index].allocations.front()];
        const auto size = binding.guestDescriptor.size() * sizeof(std::uint32_t);
        Require(allocation.buffer != nullptr && !allocation.guest && allocation.size == size && size <= MaxRefreshBytes, "template data buffer cannot take the dispatch's words");
        if (allocation.dataWords.size() == binding.guestDescriptor.size() && std::equal(allocation.dataWords.begin(), allocation.dataWords.end(), binding.guestDescriptor.begin())) continue;
        if (recorder != nullptr && timing == Recorder::NoTiming) timing = recorder->BeginGpuTiming(Recorder::CommandClass::TemplateDataRefresh);
        writeDataWords(commands, bindings[index].allocations.front(), binding.guestDescriptor);
        allocation.dataWords.assign(binding.guestDescriptor.begin(), binding.guestDescriptor.end());
        refreshedBytes += size;
        recorded = true;
    }
    if (timing != Recorder::NoTiming) recorder->EndGpuTiming(timing, refreshedBytes);
    if (recorded) rehashDataWords();
    return recorded;
}

void ShaderResources::writeDataWords(VkCommandBuffer commands, std::size_t allocation, std::span<const std::uint32_t> words) const {
    const auto& buffer = *allocations[allocation].buffer;
    const auto size = words.size() * sizeof(std::uint32_t);
    if (std::none_of(dataPatches.begin(), dataPatches.end(), [&](const DataPatch& patch) { return patch.allocation == allocation; })) {
        context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, words.data());
        return;
    }
    std::vector<std::uint32_t> patched(words.begin(), words.end());
    auto* bytes = reinterpret_cast<std::byte*>(patched.data());
    for (const auto& patch : dataPatches) {
        if (patch.allocation == allocation && patch.byte < size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
    }
    context.Resolved(&DeviceFunctions::cmdUpdateBuffer, "vkCmdUpdateBuffer")(commands, buffer.Handle(), 0, size, patched.data());
}

void ShaderResources::PrecollectSurfaces() const {
    for (const auto& range : describedRanges) GuestMemory::CollectWrites(range.address, static_cast<std::size_t>(range.bytes));
}

void ShaderResources::addImageBinding(const ShaderRecompiler::DescriptorBinding& binding, VkShaderStageFlags flags) {
    Require(binding.count != 0, "empty descriptor binding");
    if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
        // Storage images are guest textures the shader writes (looked up in stage B).
        Require(binding.role == ShaderRecompiler::DescriptorRole::GuestImages, "storage image binding has a non-image role");
        Require(binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 8u, "guest storage image descriptors must contain 8 dwords each");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorStorageImages, "shader storage-image descriptors exceed per-stage limits");
        bindings.push_back({{binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, binding.count, flags, nullptr}, {}, {}});
        plannedStorageImages += binding.count;
        deferredImages.push_back({&binding, bindings.size() - 1});
        return;
    }
    const bool sampledImage = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
    const bool samplerKind = binding.kind == ShaderRecompiler::DescriptorKind::Sampler;
    if (!(sampledImage || samplerKind)) Require(false, std::string("unsupported descriptor kind ") + kindName(binding.kind) + " for role " + roleName(binding.role));
    Require((sampledImage && binding.role == ShaderRecompiler::DescriptorRole::GuestImages) || (samplerKind && binding.role == ShaderRecompiler::DescriptorRole::GuestSamplers), "guest image descriptor role disagrees with its kind");
    Require(binding.guestDescriptor.size() % binding.count == 0, "guest image descriptor size is not a multiple of the binding count");
    const auto elementWords = binding.guestDescriptor.size() / binding.count;

    Binding item{{binding.binding, sampledImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, binding.count, flags, nullptr}, {}, {}};

    if (sampledImage) {
        Require(elementWords == 8, "guest texture descriptor must contain 8 dwords");
        Require(binding.imageShape.has_value(), "guest image binding is missing an image shape");
        Require(context.detiler != nullptr, "device texture detiler is unavailable");
        Require(context.textureCache != nullptr, "device texture cache is unavailable");
        Require(binding.count <= context.limits.maxPerStageDescriptorSampledImages, "shader sampled-image descriptors exceed per-stage limits");
        plannedSampledImages += binding.count;
        Require(plannedSampledImages <= context.limits.maxDescriptorSetSampledImages, "pipeline sampled-image descriptors exceed device limits");
    } else {
        Require(elementWords == 4, "guest sampler descriptor must contain 4 dwords");
        Require(binding.count <= context.limits.maxPerStageDescriptorSamplers, "shader sampler descriptors exceed per-stage limits");
        Require(binding.samplerDepthCompare.size() == binding.count, "guest sampler binding is missing depth comparison metadata");
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            const bool compareEnable = binding.samplerDepthCompare.at(element);
            static const bool noSamplerCache = std::getenv("APS5_NO_SAMPLER_CACHE") != nullptr;
            if (context.samplerCache != nullptr && !noSamplerCache) {
                samplers.push_back(context.samplerCache->Get(context, words, compareEnable));
            } else {
                auto resource = DecodeSamplerResource(words);
                resource.compareEnable = compareEnable;
                samplers.push_back(std::make_shared<Sampler>(context, resource));
            }
            item.imageAllocations.push_back(samplers.size() - 1);
        }
        Require(samplers.size() <= context.limits.maxDescriptorSetSamplers, "pipeline sampler descriptors exceed device limits");
    }

    bindings.push_back(std::move(item));
    if (sampledImage) deferredImages.push_back({&binding, bindings.size() - 1});
}

namespace {

// Calls `visit(binding, element, words)` for every sampled and storage image element of a compiled
// shader's bindings, in plan order (the order addImageBinding and resolveImageBinding walk).
template <typename Visit>
void forEachImageElement(const ShaderRecompiler::RecompileResult& program, Visit&& visit) {
    for (const auto& binding : program.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.count == 0) continue;
        if (binding.kind != ShaderRecompiler::DescriptorKind::SampledImage && binding.kind != ShaderRecompiler::DescriptorKind::StorageImage) continue;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) visit(binding, element, std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords));
    }
}

}

bool ShaderResources::precollectImages() {
    // The image lookups of stage B (cachedTexture, StorageTexture::Refresh) each start with a
    // GuestMemory::CollectWrites of their surface, the GetWriteWatch walk that is most of the
    // lookups' time under the lock. The walk is memoized per thread epoch, and the worker thread
    // passes no ordering point between the stages (see GuestMemory::BumpCollectEpoch), so walking
    // every surface here makes stage B's collects memo hits: the walk leaves the lock, the lookups'
    // checks stay where they were. A memo miss (the ring overflowed, an uncommitted page) just walks
    // under the lock as before; a descriptor stage B rejects is left to it. APS5_NO_PRECOLLECT=1
    // disables the pass.
    // The pass also leaves one ImageRecord per element: the decode and the surface description are
    // made once for the build, and a sampled element's cache entry is taken here (a hash lookup
    // under the cache's own mutex) so that stage B, under the device lock, runs only the
    // fastRevalidate predicate on it (see fastTexture). The collect comes before the entry is read,
    // as the predicate's rule demands (the generation the entry moves to must predate the checks).
    static const bool disabled = std::getenv("APS5_NO_PRECOLLECT") != nullptr;
    static const bool noRecords = std::getenv("APS5_NO_STAGE_A_IMAGES") != nullptr;
    if (disabled || deferredImages.empty()) return false;
    imageRecords.clear();
    nextImageRecord = 0;
    auto& counters = TextureCounts();
    for (const auto& deferred : deferredImages) {
        const auto& binding = *deferred.binding;
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            ImageRecord record;
            record.sampled = binding.kind == ShaderRecompiler::DescriptorKind::SampledImage;
            try {
                record.resource = DecodeTextureResource(words);
                record.guestBytes = DescribeSurface(record.resource).guestBytes;
                record.generation = GuestMemory::CollectWrites(record.resource.baseAddress, static_cast<std::size_t>(record.guestBytes));
                record.decoded = true;
                if (record.sampled && !noRecords && words.size() == 8 && (binding.imageDepthCompare.empty() || !binding.imageDepthCompare.at(element))) {
                    std::copy(words.begin(), words.end(), record.words.begin());
                    record.components = {ComponentSwizzleFor(record.resource.dstSelX), ComponentSwizzleFor(record.resource.dstSelY), ComponentSwizzleFor(record.resource.dstSelZ), ComponentSwizzleFor(record.resource.dstSelW)};
                    record.keys = TextureClearKeys(record.resource, record.guestBytes);
                    auto& cache = Textures();
                    std::lock_guard lock(cache.mutex);
                    if (const auto it = findTexture(cache, MakeTextureKey(context.device, words, record.components)); it != cache.entries.end()) {
                        record.texture = it->texture;
                        record.source = it->source;
                        record.entryKeys = it->keys;
                        record.entryGeneration = it->generation;
                        counters.records.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::exception&) {
                record.decoded = false;
            }
            imageRecords.push_back(std::move(record));
        }
    }
    return true;
}

std::shared_ptr<Texture> ShaderResources::fastTexture(const ImageRecord& record) {
    if (record.texture == nullptr) return nullptr;
    struct Outcome {
        bool profile;
        std::chrono::steady_clock::time_point start;
        bool hit = false;
        ~Outcome() {
            if (profile) LookupOutcomes::Add(hit ? LookupOutcomes::SampledFast : LookupOutcomes::SampledFastMiss, start);
        }
    } outcome{LookupOutcomes::Profiled(), LookupOutcomes::Profiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    // Exactly the "unchanged" branches of cachedTexture, from write stamps, the pending-results
    // registry and the DCC keys, the way fastRevalidate proves a built object current: the collect
    // first (a memo hit: stage A walked the range), then no other image may have results pending
    // over the memory (a lookup would flush them into it, or view them instead), a storage-sourced
    // view needs its image to be the cache's image of the surface and current with guest memory,
    // a snapshot needs the memory unchanged since its content matched, and the keys must be what
    // the content was made under whenever the surface has any (re-read under the lock: a flush
    // between the stages marks them uncompressed).
    const auto address = record.resource.baseAddress;
    const auto bytes = static_cast<std::size_t>(record.guestBytes);
    // A depth surface drawn over the memory serves the lookup ahead of the cache (cachedTexture's
    // DepthSurfaceTexture): its results stay in the depth image, so guest memory never changes and
    // an entry made before the surface existed (the 80-slice shadow atlas snapshotted while the area
    // loaded) would pass every check below. Served from it, the scene sampled stale shadows - dark
    // walls and foliage once the texture budget was large enough to keep that entry (6 GiB).
    // APS5_FAST_TEXTURE_IGNORES_DEPTH=1 skips the check as before.
    static const bool ignoreDepth = std::getenv("APS5_FAST_TEXTURE_IGNORES_DEPTH") != nullptr;
    if (!ignoreDepth && DepthSurfaceAt(address, record.resource.width, record.resource.height)) return nullptr;
    if (GuestMemory::CollectWrites(address, bytes) == 0) return nullptr;
    if (PendingStorageOverlaps(address, bytes, record.source.get())) return nullptr;
    auto keys = record.keys;
    if (record.resource.dccAddress != 0) {
        const auto scanStart = outcome.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Through the proof of the image whose keys they are (the view's source when the surface
        // is the image's, else the texture's): a scan only when the key range was stamped.
        keys = ProvedClearKeys(record.resource, record.guestBytes, SameKeySurface(record.source.get(), record.resource, record.guestBytes) ? record.source->KeyProof() : record.texture->KeyProof());
        if (outcome.profile) LookupOutcomes::Add(LookupOutcomes::DccScan, scanStart);
    }
    // The keys the entry was made under (a view of a pending render target was made under its clear
    // keys and stays valid while they are unchanged); a change is the full lookup's to judge.
    if (keys != record.entryKeys) return nullptr;
    if (record.source != nullptr) {
        if (!StorageImageCached(context, record.source.get()) || !GuestMemory::UnchangedSince(address, bytes, record.source->Generation())) return nullptr;
        if (record.resource.dccAddress != 0 && IsDccClear(record.source->FilledKeys())) return nullptr;
        // Under fast-clear keys the view holds only while its image's results are still pending over
        // the surface; flushed, the clear the image cannot see makes the lookup take a snapshot.
        if (keys != DccKeys::Uncompressed && StorageTexture::FindPending(address, record.guestBytes) != record.source) return nullptr;
    } else if (keys == DccKeys::Uncompressed && !GuestMemory::UnchangedSince(address, bytes, record.entryGeneration)) {
        return nullptr;
    }
    // The entry must still be the cache's, holding this object (an evicted object is not reused: the
    // next lookup would make another), and it takes the stage-A generation like a hit would.
    auto& cache = Textures();
    std::lock_guard lock(cache.mutex);
    const auto it = findTexture(cache, MakeTextureKey(context.device, record.words, record.components));
    if (it == cache.entries.end() || it->texture != record.texture) return nullptr;
    if (it->source == nullptr) it->generation = record.generation;
    touchTexture(cache, it);
    logLookup({record.texture.get(), record.resource, record.guestBytes, keys, record.source != nullptr ? 0 : record.generation, record.source.get()});
    outcome.hit = true;
    return record.texture;
}

void ShaderResources::resolveImageBinding(const ShaderRecompiler::DescriptorBinding& binding, Binding& item) {
    auto& counters = TextureCounts();
    // The element's stage-A record, when the pass ran (records follow the plan order exactly).
    const auto nextRecord = [&]() -> const ImageRecord* { return nextImageRecord < imageRecords.size() ? &imageRecords[nextImageRecord++] : nullptr; };
    if (binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) {
        const auto elementWords = binding.guestDescriptor.size() / binding.count;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * elementWords, elementWords);
            std::array<std::uint32_t, 8> nullWords{};
            if (NullTextureWords(words)) {
                nullWords = NullTextureDescriptor(binding.imageShape, false);
                words = nullWords;
            }
            const auto* record = nextRecord();
            // A descriptor the driver cannot decode is reported once per word set with where the
            // capture read it (the walk can read data that is no T#, see validImageDescriptor),
            // and binds the null texture instead of failing the draw (decodeBoundSampled; the
            // capture nulls the known cases already). APS5_NO_NULL_UNDECODABLE=1 rethrows.
            const auto decode = [&] {
                try {
                    return DecodeTextureResource(words);
                } catch (const std::exception& error) {
                    ReportUndecodedTexture(words, error.what());
                    if (!ShaderRecompiler::ResourceMaterializer::NullUndecodable()) throw;
                    sampledNullBound.fetch_add(1, std::memory_order_relaxed);
                    nullWords = NullTextureDescriptor(binding.imageShape, false);
                    words = nullWords;
                    return DecodeTextureResource(words);
                }
            };
            const auto resource = record != nullptr && record->decoded ? record->resource : decode();
            const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
            if (!firstLayer && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
            const VkComponentMapping components{ComponentSwizzleFor(resource.dstSelX), ComponentSwizzleFor(resource.dstSelY), ComponentSwizzleFor(resource.dstSelZ), ComponentSwizzleFor(resource.dstSelW)};
            const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : DescribeSurface(resource).guestBytes;
            std::shared_ptr<Texture> texture;
            if (record != nullptr && record->texture != nullptr) {
                texture = fastTexture(*record);
                (texture != nullptr ? counters.fastHits : counters.fastMisses).fetch_add(1, std::memory_order_relaxed);
            }
            if (texture == nullptr) texture = cachedTexture(context, words, resource, components, guestBytes, !binding.imageDepthCompare.empty() && binding.imageDepthCompare.at(element));
            textures.push_back(std::move(texture));
            textureFirstLayer.push_back(firstLayer);
            describedRanges.push_back({"texture", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
            item.imageAllocations.push_back(textures.size() - 1);
        }
        // The line is due even when every element took the fast path (cachedTexture reports too).
        reportTextureCounters();
        return;
    }
    // Consecutive identical storage descriptors address successive mips of one texture (dynamic-mip
    // storage writes).
    std::uint32_t mipOffset = 0;
    for (std::uint32_t element = 0; element < binding.count; ++element) {
        auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8u, 8u);
        std::array<std::uint32_t, 8> nullWords{};
        if (NullTextureWords(words)) {
            nullWords = NullTextureDescriptor(binding.imageShape, true);
            words = nullWords;
        }
        const bool sameAsPrevious = SameAsPreviousStorageElement(binding, element);
        if (sameAsPrevious) ++mipOffset;
        else mipOffset = 0;
        const auto* record = nextRecord();
        const auto resource = record != nullptr && record->decoded ? record->resource : DecodeTextureResource(words);
        const bool firstLayer = binding.imageShape == ShaderRecompiler::DescriptorImageShape::Image2D && resource.dimension == TextureDimension::k2DArray;
        if (binding.imageShape.has_value() && !firstLayer && !MatchesGuestDimension(*binding.imageShape, resource.dimension)) throw std::runtime_error("AGC graphics: guest storage texture dimension disagrees with the shader's declared image shape (shape " + std::to_string(static_cast<int>(*binding.imageShape)) + ", dimension " + std::to_string(static_cast<int>(resource.dimension)) + ")");
        const auto mip = std::min(resource.baseLevel + mipOffset, resource.mipCount - 1u);
        Require(resource.minLod <= mip * 256u, "guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
        const auto guestBytes = record != nullptr && record->decoded ? record->guestBytes : DescribeSurface(resource).guestBytes;
        const bool written = element >= binding.imageWritten.size() || binding.imageWritten[element];
        // A depth surface's memory written through a storage image (the game downsamples its depth
        // with compute) is the newest depth from now on (see NoteDepthSurfaceWrite).
        if (written) NoteDepthSurfaceWrite(resource.baseAddress, resource.width, resource.height);
        // The same surface as the previous element: its image was just looked up and refreshed.
        if (sameAsPrevious && StorageDedupeEnabled()) storageTextures.push_back(storageTextures.back());
        else storageTextures.push_back(cachedStorageTexture(context, words, resource, mip, guestBytes));
        storageMips.push_back(mip);
        storageKeys.push_back(resource.dccAddress);
        storageFirstLayer.push_back(firstLayer);
        // Images the shader only reads have nothing to store back.
        storageWritten.push_back(written);
        describedRanges.push_back({"storage", resource.baseAddress, guestBytes, resource.width, resource.height, resource.format, static_cast<int>(resource.tileMode), resource.dccAddress});
        item.imageAllocations.push_back(storageTextures.size() - 1);
    }
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> ShaderResources::PresyncSurfaces() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> surfaces;
    // A surface's lookup reads guest memory on the CPU unless it is served GPU-direct from a host
    // import: a storage image of imported memory uploads and refreshes from the import, and a
    // sampled texture of one views that image (SampledFromStorageEligible), so neither waits for
    // the producer.
    const auto consider = [&](std::uint32_t format, std::uint64_t address, std::uint64_t guestBytes, bool sampled) {
        const bool gpuDirect = sampled ? SampledFromStorageEligible(context, format, address, guestBytes) : HostImportCovers(context, address, static_cast<std::size_t>(guestBytes));
        if (!gpuDirect) surfaces.emplace_back(address, guestBytes);
    };
    if (completed) {
        // A cached object (the resource cache): Revalidate repeats the lookups of the objects the
        // build made, which never change afterwards (a lookup returning another object fails the
        // revalidation instead), so the surfaces come from the build's own record of them. A
        // sampled texture viewing a storage image refreshes like that image; a snapshot texture
        // over an eligible surface is replaced by a view without reading. Storage images are
        // checked on their own surface (a mip chain can exceed the element's descriptor). Read
        // without the device lock: these members are fixed once the build completed.
        std::size_t textureIndex = 0;
        for (const auto& range : describedRanges) {
            if (std::strcmp(range.kind, "texture") == 0 && textureIndex < textures.size()) {
                const auto& texture = textures[textureIndex++];
                if (texture->ViewsStorageImage()) consider(range.format, range.address, range.bytes, false);
                else consider(range.format, range.address, range.bytes, true);
            }
        }
        for (std::size_t index = 0; index < storageTextures.size(); ++index) {
            if (index != 0 && storageTextures[index] == storageTextures[index - 1]) continue;
            const auto& image = *storageTextures[index];
            consider(image.Descriptor().format, image.Descriptor().baseAddress, image.GuestBytes(), false);
        }
        return surfaces;
    }
    if (!imageRecords.empty()) {
        for (const auto& record : imageRecords) {
            if (record.decoded) consider(record.resource.format, record.resource.baseAddress, record.guestBytes, record.sampled);
        }
        return surfaces;
    }
    // Stage A has not run (an address-based build, or the pass is off): decoded from the shader.
    if (deferredCompute.program == nullptr) return surfaces;
    forEachImageElement(*deferredCompute.program, [&](const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t, std::span<const std::uint32_t> words) {
        try {
            const auto resource = DecodeTextureResource(words);
            consider(resource.format, resource.baseAddress, DescribeSurface(resource).guestBytes, binding.kind == ShaderRecompiler::DescriptorKind::SampledImage);
        } catch (const std::exception&) {
            // Stage B reports the bad descriptor.
        }
    });
    return surfaces;
}

ShaderResources::~ShaderResources() {
    release();
}

void ShaderResources::release() noexcept {
    // A set from the cache's pool chain goes back to it; a dedicated pool dies with its set.
    if (cachePool && context.descriptorCache != nullptr) context.descriptorCache->Free({_set, cachePool});
    if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
    if (_layout && ownsLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, _layout, nullptr);
    cachePool = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    _set = VK_NULL_HANDLE;
    _layout = VK_NULL_HANDLE;
}

VkDescriptorSetLayout ShaderResources::Layout() const {
    return _layout;
}

ShaderResources::DrawBindings::~DrawBindings() {
    if (cache != nullptr && allocation.set != VK_NULL_HANDLE) cache->Free(allocation);
}

namespace {

// The descriptor words of a built object's binding as the draw's own stages give them (its source,
// see ShaderResources::Allocation); empty when the stages do not name it.
std::span<const std::uint32_t> SourceWords(std::span<const CompiledShader> shaders, std::int32_t shader, std::uint32_t binding) {
    if (shader < 0 || static_cast<std::size_t>(shader) >= shaders.size() || shaders[static_cast<std::size_t>(shader)].program == nullptr) return {};
    const auto& bindings = shaders[static_cast<std::size_t>(shader)].program->bindings;
    if (binding >= bindings.size()) return {};
    return bindings[binding].guestDescriptor;
}

// The range a draw's own descriptor gives a guest buffer element, or `fallback` (the built one).
std::pair<std::uint64_t, std::uint64_t> SourceRange(std::span<const CompiledShader> shaders, std::int32_t shader, std::uint32_t binding, std::uint32_t element, std::pair<std::uint64_t, std::uint64_t> fallback) {
    const auto words = SourceWords(shaders, shader, binding);
    const auto offset = static_cast<std::size_t>(element) * 4;
    if (offset + 4 > words.size()) return fallback;
    const ShaderRecompiler::ShaderBufferResource descriptor{{words[offset], words[offset + 1], words[offset + 2], words[offset + 3]}};
    if (descriptor.GetSize() == 0 || descriptor.Base48() == 0) return {0, 0};
    return {descriptor.Base48(), descriptor.GetSize()};
}

// Whether a data allocation's words differ from the draw's own (rebased hits). An allocation the
// stages do not name compares equal; one without recorded words differs as `unknown`.
bool DataMoved(std::span<const CompiledShader> shaders, std::int32_t shader, std::uint32_t binding, const std::vector<std::uint32_t>& built, bool& unknown) {
    unknown = false;
    const auto words = SourceWords(shaders, shader, binding);
    if (words.empty()) return false;
    if (built.empty()) {
        unknown = true;
        return true;
    }
    return words.size() != built.size() || !std::equal(words.begin(), words.end(), built.begin());
}

}

namespace {

// The in-place read ranges one query of a ShaderResources lists (GuestBufferMemory::InPlaceReads),
// kept with its capacity on the calling thread (ScratchLease): the per-draw queries below listed
// them into a fresh vector each, three to four times per draw.
struct InPlaceReadsScratch {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    unsigned depth = 0;
};

}

bool ShaderResources::RebaseEligible(std::span<const CompiledShader> shaders, const Recorder& recorder, bool& rebased) const {
    rebased = false;
    if (_set == VK_NULL_HANDLE || usesBda) return true;
    ScratchLease<InPlaceReadsScratch> scratch;
    const auto reads = guestMemory.InPlaceReads(scratch->reads);
    for (const auto& item : allocations) {
        if (!item.guest) {
            if (item.buffer == nullptr || !DataRole(item.role)) continue;
            bool unknown = false;
            if (!DataMoved(shaders, item.sourceShader, item.sourceBinding, item.dataWords, unknown)) continue;
            rebased = true;
            if (unknown || SourceWords(shaders, item.sourceShader, item.sourceBinding).size() * sizeof(std::uint32_t) != item.size) return false;
            continue;
        }
        const auto [address, size] = SourceRange(shaders, item.sourceShader, item.sourceBinding, item.sourceElement, {item.address, item.size});
        if (address == item.address && size == item.size) continue;
        rebased = true;
        // Only an element PrepareDrawBindings would snapshot at its built address can move.
        if (item.written || address == 0 || item.address == 0 || address < item.adjustment) return false;
        if (size + item.adjustment > context.limits.maxStorageBufferRange) return false;
        if (guestMemory.WritesOverlap(item.address, item.size)) return false;
        const bool direct = std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return item.address >= range.first && item.address < range.second && item.size <= range.second - item.address; });
        if (!direct) return false;
        const auto begin = address - item.adjustment;
        const auto bytes = static_cast<std::size_t>(size) + item.adjustment;
        if (guestMemory.WritesOverlap(begin, bytes)) return false;
        if (Recorder::SnapshotWriteOverlaps(begin, bytes) || recorder.PendingWriteOverlaps(begin, bytes)) return false;
        if (recorder.QueuedStoreOverlaps(begin, bytes) || recorder.QueuedKeyStoreOverlaps(begin, bytes)) return false;
        if (PendingStorageOverlaps(begin, bytes, nullptr)) return false;
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), bytes)) return false;
    }
    return true;
}

bool ShaderResources::InPlaceBindings() {
    static const bool inPlace = std::getenv("APS5_NO_INPLACE_BINDINGS") == nullptr && std::getenv("APS5_CAPTURE_INPUTS") == nullptr;
    return inPlace;
}

std::shared_ptr<ShaderResources::DrawBindings> ShaderResources::PrepareDrawBindings(Recorder& recorder, std::span<const CompiledShader> shaders) const {
    if (_set == VK_NULL_HANDLE || usesBda) return {};
    ScratchLease<InPlaceReadsScratch> scratch;
    const auto reads = guestMemory.InPlaceReads(scratch->reads);
    const bool inPlace = InPlaceBindings();
    // Made once the first element is selected: most draws select none and return nothing.
    std::shared_ptr<DrawBindings> result;
    std::vector<std::size_t> selected;
    // The descriptor each selected element gets in the draw's set copy, in `selected` order.
    std::vector<VkDescriptorBufferInfo> infos;
    const auto select = [&](std::size_t index, const VkDescriptorBufferInfo& info) {
        if (result == nullptr) result = std::make_shared<DrawBindings>();
        selected.push_back(index);
        infos.push_back(info);
    };
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        const auto& item = allocations[index];
        if (!item.guest && item.buffer != nullptr && DataRole(item.role)) {
            // A rebased hit's data buffer: a copy holding the draw's own words, patched like the
            // built one (the guest buffer adjustments are the same: they are part of the key).
            bool unknown = false;
            if (!DataMoved(shaders, item.sourceShader, item.sourceBinding, item.dataWords, unknown)) continue;
            Require(!unknown, "a rebased data buffer without recorded words");
            const auto words = SourceWords(shaders, item.sourceShader, item.sourceBinding);
            Require(words.size() * sizeof(std::uint32_t) == item.size, "a rebased data buffer changed size");
            auto buffer = std::make_shared<Buffer>(context, item.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            auto* bytes = buffer->Bytes().data();
            std::memcpy(bytes, words.data(), item.size);
            for (const auto& patch : dataPatches) {
                if (patch.allocation == index && patch.byte < item.size) bytes[patch.byte] = static_cast<std::byte>(patch.adjustment);
            }
            select(index, {buffer->Handle(), 0, buffer->Bytes().size()});
            ++result->dataCopies;
            result->snapshots.push_back({0, std::move(buffer), index});
            continue;
        }
        if (!item.guest || item.written || guestMemory.WritesOverlap(item.address, item.size)) continue;
        // A rebased hit (RebaseEligible proved the moved range) binds the draw's own range.
        const auto [address, size] = SourceRange(shaders, item.sourceShader, item.sourceBinding, item.sourceElement, {item.address, item.size});
        const bool moved = address != item.address || size != item.size;
        const bool direct = std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return item.address >= range.first && item.address < range.second && item.size <= range.second - item.address; });
        if (!moved && (!direct || recorder.PendingWriteOverlaps(item.address, item.size))) continue;
        Require(!moved || direct, "a rebased guest buffer is not one the template snapshots");
        // In place (InPlaceBindings): an unmoved element stays bound as built; a moved one binds its
        // import at the draw's offset, which must sit on the storage buffer offset alignment as the
        // built one does (the adjustment is the object's: part of the key). Else a snapshot.
        if (inPlace && !moved) continue;
        const auto begin = address - item.adjustment;
        const auto bytes = static_cast<std::size_t>(size) + item.adjustment;
        if (inPlace) {
            const auto* import = HostImportFor(context, begin, bytes);
            if (import != nullptr && (begin - import->base) % context.limits.minStorageBufferOffsetAlignment == 0) {
                select(index, {import->buffer, begin - import->base, bytes});
                result->inPlaceReads.emplace_back(begin, begin + bytes);
                ++result->boundInPlace;
                CaptureTrace::Log("draw-inplace batch=%llu address=%llx bytes=%zu", static_cast<unsigned long long>(recorder.Submissions() + 1), static_cast<unsigned long long>(begin), bytes);
                continue;
            }
            if (result == nullptr) result = std::make_shared<DrawBindings>();
            ++(import == nullptr ? result->refusedNoImport : result->refusedAlignment);
        }
        const auto registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto generation = GuestMemory::CollectWrites(begin, bytes);
        auto buffer = recorder.ReusableDrawSnapshot(begin, bytes);
        const bool reused = buffer != nullptr;
        if (buffer == nullptr) {
            buffer = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::memcpy(buffer->Bytes().data(), reinterpret_cast<const void*>(begin), bytes);
            recorder.KeepDrawSnapshot(begin, bytes, generation, registryGeneration, buffer);
        }
        select(index, {buffer->Handle(), 0, buffer->Bytes().size()});
        ++(reused ? result->snapshotsReused : result->snapshotsMade);
        result->snapshots.push_back({begin, std::move(buffer), index});
        CaptureTrace::Log("draw-snapshot batch=%llu address=%llx bytes=%zu", static_cast<unsigned long long>(recorder.Submissions() + 1), static_cast<unsigned long long>(begin), bytes);
    }
    if (selected.empty()) return {};
    Require(context.descriptorCache != nullptr, "draw snapshots require a descriptor cache");
    std::map<VkDescriptorType, std::uint32_t> counts;
    for (const auto& binding : bindings) counts[binding.layout.descriptorType] += binding.layout.descriptorCount;
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts) sizes.push_back({type, count});
    result->cache = context.descriptorCache;
    result->allocation = result->cache->Allocate(_layout, sizes);
    Require(result->allocation.set != VK_NULL_HANDLE, "draw snapshot descriptor allocation failed");
    std::vector<VkCopyDescriptorSet> copies;
    for (const auto& binding : bindings) {
        VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
        copy.srcSet = _set;
        copy.srcBinding = binding.layout.binding;
        copy.dstSet = result->allocation.set;
        copy.dstBinding = binding.layout.binding;
        copy.descriptorCount = binding.layout.descriptorCount;
        copies.push_back(copy);
    }
    const auto update = context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets");
    update(context.device, 0, nullptr, static_cast<std::uint32_t>(copies.size()), copies.data());
    std::vector<VkWriteDescriptorSet> writes;
    for (const auto& binding : bindings) {
        for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
            const auto found = std::find(selected.begin(), selected.end(), binding.allocations[element]);
            if (found == selected.end()) continue;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = result->allocation.set;
            write.dstBinding = binding.layout.binding;
            write.dstArrayElement = static_cast<std::uint32_t>(element);
            write.descriptorCount = 1;
            write.descriptorType = binding.layout.descriptorType;
            write.pBufferInfo = &infos[static_cast<std::size_t>(found - selected.begin())];
            writes.push_back(write);
        }
    }
    update(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    recorder.Keep(result);
    return result;
}

VkBuffer ShaderResources::DataBufferFor(std::uint32_t vulkanBinding, const DrawBindings* drawBindings, std::size_t* allocation, std::byte** hostBytes) const {
    *hostBytes = nullptr;
    for (const auto& binding : bindings) {
        if (binding.layout.binding != vulkanBinding || binding.allocations.size() != 1) continue;
        const auto index = binding.allocations.front();
        *allocation = index;
        if (drawBindings != nullptr) {
            for (const auto& snapshot : drawBindings->snapshots) {
                if (snapshot.allocation != index) continue;
                *hostBytes = snapshot.buffer->Bytes().data();
                return snapshot.buffer->Handle();
            }
        }
        const auto& item = allocations[index];
        if (item.buffer == nullptr || item.guest || !DataRole(item.role)) return VK_NULL_HANDLE;
        return item.buffer->Handle();
    }
    return VK_NULL_HANDLE;
}

ShaderResources::DeferredMemo& ShaderResources::DeferredMemoFor(std::size_t allocation) const {
    return allocations[allocation].deferred;
}

void ShaderResources::Bind(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout) const {
    if (_set == VK_NULL_HANDLE) return;
    if (CheckStaleImports()) {
        for (const auto buffer : boundBuffers) {
            std::uint64_t base = 0;
            if (!ReportDestroyedImport(buffer, "a descriptor set bound now", reinterpret_cast<std::uint64_t>(_set), &base)) continue;
            static std::atomic<int> details{0};
            if (details.fetch_add(1) < 6) {
                std::fprintf(stderr, "[stale-import]   object %p: reusable %d, %zu direct regions, %zu bound buffers, %zu allocations, lease %d%s", static_cast<const void*>(this), reusable ? 1 : 0, directRegions.size(), boundBuffers.size(), allocations.size(), HoldsLease() ? 1 : 0, "\n");
                for (const auto& region : directRegions) {
                    const auto current = HostImportSerial(context, region.begin, static_cast<std::size_t>(region.end - region.begin), false);
                    std::fprintf(stderr, "[stale-import]     direct 0x%llx+0x%llx serial %llu, now %llu%s%s", static_cast<unsigned long long>(region.begin), static_cast<unsigned long long>(region.end - region.begin), static_cast<unsigned long long>(region.serial), static_cast<unsigned long long>(current), region.begin >= base && region.begin < base + 0x40000000ull ? " (in or after the destroyed import's base)" : "", "\n");
                }
            }
            break;
        }
    }
    context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, bindPoint, layout, 0, 1, &_set, 0, nullptr);
}

namespace {
// Debug aid: APS5_GPU_NO_WRITEBACK=1 keeps GPU results out of guest memory.
bool SkipWriteBack() {
    static const bool skip = std::getenv("APS5_GPU_NO_WRITEBACK") != nullptr;
    return skip;
}
}

void ShaderResources::WriteBack() {
    WriteBackBuffers();
    if (SkipWriteBack()) return;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageWritten[index]) storageTextures[index]->MarkDirty();
    }
}

void ShaderResources::MarkGpuWrites(Recorder& recorder) {
    // APS5_PROFILE_DRAW: the parts' times accumulate in `timing` (marks...Ms), which the dispatch
    // reads before and after the call for its "record: marks: ..." rows.
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto lap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto part = [&](double& total) {
        if (!profile) return;
        const auto now = std::chrono::steady_clock::now();
        total += std::chrono::duration<double, std::milli>(now - lap).count();
        lap = now;
    };
    // The ranges this use reads in place through their host imports (read-only and written elements
    // alike, and an address-based build's whole leased heaps), before the writes: a CPU store into
    // one of them (the copy HLE) must not land before the recorded work read it.
    {
        ScratchLease<InPlaceReadsScratch> scratch;
        const auto kind = guestMemory.HoldsLease() ? Recorder::ReadKind::AddressBased : Recorder::ReadKind::DispatchElement;
        if (Recorder::ReadSets()) {
            // The build's own regions as ranges, the shared address space's list by reference.
            auto set = guestMemory.InPlaceReadSet(scratch->reads);
            recorder.NotePendingReads(scratch->reads, kind);
            recorder.NotePendingReadSet(std::move(set), kind);
        } else {
            recorder.NotePendingReads(guestMemory.InPlaceReads(scratch->reads), kind);
        }
    }
    part(timing.marksReadsMs);
    if (SkipWriteBack()) {
        recorder.ReleaseClaims();
        return;
    }
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageWritten[index]) storageTextures[index]->MarkDirty();
    }
    // Written sub-ranges of buffers the GPU copied out of a host import go back into it by the GPU,
    // recorded here after the work: those regions then need no CPU write-back (HasCopiedWrites),
    // and the note and mark below cover them like direct writes.
    guestMemory.RecordCopyBacks(recorder);
    recorder.ReleaseClaims();
    part(timing.marksCopyBacksMs);
    // Only the written elements' ranges (AddWritable): a read-only element is neither noted here
    // nor marked as a direct write, so CPU reads of its memory never wait for this work.
    recorder.NotePendingWrites(guestMemory.Writes(), Recorder::WriteKind::ShaderWrite);
    part(timing.marksWritesMs);
    guestMemory.MarkDirectWrites();
    part(timing.marksDirectMs);
    if (AgcDriver::FrameTrace::Active()) {
        std::string text;
        char item[96];
        for (std::size_t index = 0; index < storageTextures.size(); ++index) {
            if (!storageWritten[index] || storageTextures[index] == nullptr) continue;
            const auto& written = storageTextures[index]->Descriptor();
            std::snprintf(item, sizeof(item), " w=0x%llx/%ux%u/f%u", static_cast<unsigned long long>(written.baseAddress), written.width, written.height, written.format);
            text += item;
        }
        for (const auto& [begin, end] : guestMemory.Writes()) {
            std::snprintf(item, sizeof(item), " wb=0x%llx+0x%llx", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
            text += item;
        }
        // The bound resources (textures with their surfaces, storage images, buffers).
        AgcDriver::FrameTrace::NoteWrites(text + " |" + Describe());
    }
    if (!BuildProfiled()) return;
    auto& counters = BufferWrites();
    // Recorded uses only (dispatches and recorded draws): a synchronous draw writes back in
    // WriteBack() and publishes no pending writes, so it has no notes to skip.
    counters.notesSkipped.fetch_add(readOnlyBuffers, std::memory_order_relaxed);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load();
    if (nowMs - last < 10000 || !counters.lastReport.compare_exchange_strong(last, nowMs)) return;
    const auto elements = counters.elements.load();
    const auto readOnly = counters.readOnly.load();
    std::fprintf(stderr, "[buffers] descriptor elements bound: %llu total, %llu written, %llu read-only; %llu pending-write notes skipped\n", static_cast<unsigned long long>(elements), static_cast<unsigned long long>(elements - readOnly), static_cast<unsigned long long>(readOnly), static_cast<unsigned long long>(counters.notesSkipped.load()));
}

void ShaderResources::WriteBackBuffers() {
    if (bda) bda->CheckFault();
    if (SkipWriteBack()) return;
    guestMemory.WriteBack();
    // The fault buffer is clear again and the lease released: a later dispatch may rearm the object.
    if (leaseTemplate) leaseIdle.store(true, std::memory_order_release);
}

bool ShaderResources::RearmLease(std::span<const CompiledShader> shaders) {
    if (!leaseTemplate || !leaseIdle.load(std::memory_order_acquire)) {
        CountLeaseReuse(LeaseReuse::Busy);
        return false;
    }
    if (!guestMemory.RearmSpace(leaseSerial)) {
        CountLeaseReuse(LeaseReuse::Space);
        return false;
    }
    // Revalidate proves reusable objects only: this use is proved like one (its direct regions are
    // none; the space stands for them), then the object is not reusable again, so no recipe or
    // draw takes it.
    reusable = true;
    bool ok = false;
    try {
        ok = Revalidate(shaders) && guestMemory.SpaceEpochCurrent();
    } catch (...) {
        reusable = false;
        guestMemory.DropRearmed();
        throw;
    }
    reusable = false;
    if (!ok) {
        guestMemory.DropRearmed();
        CountLeaseReuse(LeaseReuse::Proof);
        return false;
    }
    bool moved = false;
    if (!rebaseLease(shaders, moved)) {
        guestMemory.DropRearmed();
        CountLeaseReuse(LeaseReuse::Rebase);
        return false;
    }
    leaseIdle.store(false, std::memory_order_relaxed);
    CountLeaseReuse(LeaseReuse::Rearmed);
    if (moved) CountLeaseReuse(LeaseReuse::Rebased);
    return true;
}

bool ShaderResources::rebaseLease(std::span<const CompiledShader> shaders, bool& moved) {
    moved = false;
    struct Move {
        std::size_t index;
        std::uint64_t address;
        std::size_t size;
        VkDescriptorBufferInfo info;
        std::uint32_t adjustment;
    };
    // Every moved element is proved before anything changes: a refusal leaves the template as built.
    // The key fixes the rest: written elements keep their words, a null element stays null.
    std::vector<Move> moves;
    for (std::size_t index = 0; index < allocations.size(); ++index) {
        const auto& item = allocations[index];
        if (!item.guest || item.sourceShader < 0) continue;
        const auto [address, size] = SourceRange(shaders, item.sourceShader, item.sourceBinding, item.sourceElement, {item.address, item.size});
        if (address == item.address && size == item.size) continue;
        if (item.written || address == 0 || size == 0 || size > context.limits.maxStorageBufferRange || CheckStaleImports()) return false;
        Move move{index, address, static_cast<std::size_t>(size), {}, 0};
        if (!guestMemory.RebasedDescriptor(address, move.size, move.info, move.adjustment)) return false;
        // As RebaseEligible: no element this dispatch writes may share the moved range.
        if (guestMemory.WritesOverlap(address, move.size)) return false;
        // A changed adjustment reaches the shader through a push byte (PatchPushConstants, every
        // use) or the data buffer's patch, which the record's RefreshData rewrites.
        if (move.adjustment != item.adjustment && item.pushByte < 0 && (item.dataAllocation < 0 || !TemplateDataRefresh())) return false;
        moves.push_back(move);
    }
    if (moves.empty()) return true;
    moved = true;
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(moves.size());
    for (const auto& binding : bindings) {
        if (binding.layout.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) continue;
        for (std::size_t element = 0; element < binding.allocations.size(); ++element) {
            const auto found = std::find_if(moves.begin(), moves.end(), [&](const Move& move) { return move.index == binding.allocations[element]; });
            if (found == moves.end()) continue;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = _set;
            write.dstBinding = binding.layout.binding;
            write.dstArrayElement = static_cast<std::uint32_t>(element);
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &found->info;
            writes.push_back(write);
        }
    }
    Require(writes.size() == moves.size(), "a rebased guest buffer is not in the template's set");
    // The template is idle (its previous use completed), so its set is not in use by the GPU.
    context.Resolved(&DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    for (const auto& move : moves) {
        auto& item = allocations[move.index];
        item.address = move.address;
        item.size = move.size;
        if (move.adjustment == item.adjustment) continue;
        item.adjustment = move.adjustment;
        // The patch stays listed at zero too: the byte then says 0 again.
        if (item.pushByte >= 0) {
            const auto position = static_cast<std::uint32_t>(item.pushByte);
            auto patch = std::find_if(pushPatches.begin(), pushPatches.end(), [&](const auto& entry) { return entry.first == position; });
            if (patch != pushPatches.end()) patch->second = move.adjustment;
            else pushPatches.emplace_back(position, move.adjustment);
            continue;
        }
        const auto data = static_cast<std::size_t>(item.dataAllocation);
        auto patch = std::find_if(dataPatches.begin(), dataPatches.end(), [&](const DataPatch& entry) { return entry.allocation == data && entry.byte == item.dataByte; });
        if (patch != dataPatches.end()) patch->adjustment = move.adjustment;
        else dataPatches.push_back({data, item.dataByte, move.adjustment});
        // Forgotten words make the record's RefreshData write the buffer with the new patch.
        allocations[data].dataWords.clear();
    }
    return true;
}

bool ShaderResources::WritesMemory() const {
    return HoldsLease() || NeedsCompletion() || !guestMemory.Writes().empty() || std::any_of(storageWritten.begin(), storageWritten.end(), [](bool written) { return written; });
}

bool ShaderResources::ReadsOverlap(std::uint64_t address, std::size_t bytes) const {
    ScratchLease<InPlaceReadsScratch> scratch;
    const auto reads = guestMemory.InPlaceReads(scratch->reads);
    return std::any_of(reads.begin(), reads.end(), [&](const auto& range) { return address < range.second && range.first < address + bytes; });
}

std::vector<std::pair<VkImage, bool>> ShaderResources::StorageImages() const {
    std::vector<std::pair<VkImage, bool>> images;
    for (std::size_t index = 0; index < storageTextures.size(); ++index) {
        if (storageTextures[index] != nullptr) images.emplace_back(storageTextures[index]->Image(), storageWritten[index]);
    }
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() != nullptr) images.emplace_back(texture->StorageSource()->Image(), false);
    }
    return images;
}

bool ShaderResources::ReadsImage(const StorageTexture* image) const {
    if (image == nullptr) return false;
    for (const auto& texture : textures) {
        if (texture != nullptr && texture->StorageSource() == image) return true;
    }
    return std::any_of(storageTextures.begin(), storageTextures.end(), [&](const auto& storage) { return storage.get() == image; });
}

}
