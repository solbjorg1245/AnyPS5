#include "prx/libc/include/ReadPatch.hpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>

namespace {

struct Registry {
    std::mutex mutex;
    std::atomic<bool> any{false};
    std::map<std::filesystem::path::string_type, std::shared_ptr<const ReadPatch::Ranges>> files;
    std::unordered_map<int, std::shared_ptr<const ReadPatch::Ranges>> descriptors;
};

Registry& Patches() {
    static Registry registry;
    return registry;
}

}

void AddReadPatch_nid_no_patch(const std::filesystem::path& hostPath, std::uint64_t offset, std::string bytes) {
    auto& registry = Patches();
    std::lock_guard lock(registry.mutex);
    auto& ranges = registry.files[ReadPatch::Key(hostPath)];
    // Copied, not changed in place: a reader may hold the previous list.
    auto updated = std::make_shared<ReadPatch::Ranges>(ranges ? *ranges : ReadPatch::Ranges{});
    updated->push_back({offset, std::move(bytes)});
    ranges = std::move(updated);
    registry.any.store(true, std::memory_order_release);
}

std::shared_ptr<const ReadPatch::Ranges> ReadPatchesFor_nid_no_patch(const std::filesystem::path& hostPath) {
    auto& registry = Patches();
    if (!registry.any.load(std::memory_order_acquire)) return nullptr;
    const auto key = ReadPatch::Key(hostPath);
    std::lock_guard lock(registry.mutex);
    const auto found = registry.files.find(key);
    return found != registry.files.end() ? found->second : nullptr;
}

void TrackReadPatchDescriptor_nid_no_patch(int fd, const std::filesystem::path& hostPath) {
    auto& registry = Patches();
    if (!registry.any.load(std::memory_order_acquire)) return;
    auto ranges = ReadPatchesFor_nid_no_patch(hostPath);
    std::lock_guard lock(registry.mutex);
    if (ranges) registry.descriptors[fd] = std::move(ranges);
    else registry.descriptors.erase(fd);
}

void ForgetReadPatchDescriptor_nid_no_patch(int fd) {
    auto& registry = Patches();
    if (!registry.any.load(std::memory_order_acquire)) return;
    std::lock_guard lock(registry.mutex);
    registry.descriptors.erase(fd);
}

std::shared_ptr<const ReadPatch::Ranges> ReadPatchesForDescriptor_nid_no_patch(int fd) {
    auto& registry = Patches();
    if (!registry.any.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard lock(registry.mutex);
    const auto found = registry.descriptors.find(fd);
    return found != registry.descriptors.end() ? found->second : nullptr;
}

void ApplyReadPatches_nid_no_patch(const ReadPatch::Ranges& ranges, const char* api, std::uint64_t offset, void* destination, std::uint64_t size) {
    const auto replaced = ReadPatch::Apply(ranges, offset, destination, size);
    static std::atomic<bool> reported{false};
    if (replaced == 0 || reported.exchange(true)) return;
    std::fprintf(stderr, "[read patch] first patched read: %s at offset 0x%llx, %llu bytes, %llu replaced\n", api, static_cast<unsigned long long>(offset),
        static_cast<unsigned long long>(size), static_cast<unsigned long long>(replaced));
    std::fflush(stderr);
}
