#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_KEYEDMEMO_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_KEYEDMEMO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

// A 64-bit hash of a byte key, eight bytes per step (the memo's quick compare; the store keeps its
// own hash).
inline std::uint64_t KeyHash(std::span<const std::byte> key) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ (static_cast<std::uint64_t>(key.size()) * 0x100000001b3ull);
    const auto mix = [&](std::uint64_t chunk) {
        hash = (hash ^ chunk) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29u;
    };
    std::size_t at = 0;
    for (; at + sizeof(std::uint64_t) <= key.size(); at += sizeof(std::uint64_t)) {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, key.data() + at, sizeof(chunk));
        mix(chunk);
    }
    if (at < key.size()) {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, key.data() + at, key.size() - at);
        mix(chunk);
    }
    return hash;
}

// A thread's memo in front of a keyed store that hands out shared objects (the pipeline store,
// CachedFastPipeline; docs/design/draw-fastpath.md 2.7). An entry answers a key equal to its own
// byte for byte while
// - the store's removal count (bumped by every entry the store drops) is the one read before the
//   store answered the entry: the store still holds the object under that key;
// - the owner (the device instance the store checks entries against) is the same live object;
// - the object still lives (the entry holds it weakly: a memo must not keep a device's objects
//   past the device).
// The store would then answer the same object, so a hit skips its lock and its lookup. Misses are
// the caller's to answer through the store and Note.
template<typename TValue, std::size_t TEntries>
class KeyedMemo {
public:
    std::shared_ptr<TValue> Find(std::span<const std::byte> key, std::uint64_t hash, std::uint64_t removals, const void* owner) const {
        for (const auto& entry : entries) {
            if (entry.hash != hash || entry.removals != removals || entry.owner != owner || entry.key.size() != key.size()) continue;
            // A noted owner that died may have left its address to a new one.
            if (entry.owner != nullptr && entry.ownerLife.expired()) continue;
            if (!key.empty() && std::memcmp(entry.key.data(), key.data(), key.size()) != 0) continue;
            if (auto value = entry.value.lock()) return value;
        }
        return nullptr;
    }

    void Note(std::span<const std::byte> key, std::uint64_t hash, std::uint64_t removals, const void* owner, std::weak_ptr<const void> ownerLife, const std::shared_ptr<TValue>& value) {
        auto& entry = entries[next];
        next = (next + 1) % TEntries;
        entry.key.assign(key.begin(), key.end());
        entry.hash = hash;
        entry.removals = removals;
        entry.owner = owner;
        entry.ownerLife = std::move(ownerLife);
        entry.value = value;
    }

    std::uint64_t hits = 0;
    std::uint64_t misses = 0;

private:
    struct Entry {
        std::vector<std::byte> key;
        std::uint64_t hash = 0;
        std::uint64_t removals = 0;
        const void* owner = nullptr;
        std::weak_ptr<const void> ownerLife;
        std::weak_ptr<TValue> value;
    };
    std::array<Entry, TEntries> entries{};
    std::size_t next = 0;
};

}

#endif
