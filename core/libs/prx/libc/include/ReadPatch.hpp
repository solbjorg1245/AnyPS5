#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_READPATCH_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_READPATCH_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Read patches: byte ranges of a host file that every guest read of it sees replaced, with no copy of the
// file (UiOverlay redraws two sprites of the 335 MB UI atlas this way). They are registered before the title
// starts (the first guest path resolve); with none registered a read pays one atomic load. Readers: APR
// ReadFile, sceKernelRead/Pread (read/pread) and AIO on descriptors from sceKernelOpen.
namespace ReadPatch {

struct Range {
    std::uint64_t offset = 0;
    std::string bytes;
};
using Ranges = std::vector<Range>;

// Copies the parts of `ranges` that overlap the file bytes [offset, offset + size) over `destination`, which
// holds those bytes as read. Returns how many bytes were replaced.
inline std::uint64_t Apply(const Ranges& ranges, std::uint64_t offset, void* destination, std::uint64_t size) {
    std::uint64_t replaced = 0;
    for (const auto& range : ranges) {
        const auto begin = std::max(offset, range.offset);
        const auto end = std::min(offset + size, range.offset + range.bytes.size());
        if (begin >= end) continue;
        std::memcpy(static_cast<char*>(destination) + (begin - offset), range.bytes.data() + (begin - range.offset), end - begin);
        replaced += end - begin;
    }
    return replaced;
}

// The registry key of a host path: normalised, native separators, ASCII case folded on Windows (the host
// file system ignores case, and guest paths may spell a name either way).
inline std::filesystem::path::string_type Key(const std::filesystem::path& path) {
    auto key = path.lexically_normal().make_preferred().native();
#ifdef _WIN32
    for (auto& character : key) {
        if (character >= 'A' && character <= 'Z') character = static_cast<std::filesystem::path::value_type>(character - 'A' + 'a');
    }
#endif
    return key;
}

}

// Registers `bytes` to replace the bytes at `offset` of hostPath in every later read of it.
void AddReadPatch_nid_no_patch(const std::filesystem::path& hostPath, std::uint64_t offset, std::string bytes);
// The ranges registered for hostPath, or null; readers keep the pointer per open file.
std::shared_ptr<const ReadPatch::Ranges> ReadPatchesFor_nid_no_patch(const std::filesystem::path& hostPath);
// Descriptors: sceKernelOpen records every file it opens (a reused number starts clean), close forgets it,
// and the descriptor readers ask for the file's ranges.
void TrackReadPatchDescriptor_nid_no_patch(int fd, const std::filesystem::path& hostPath);
void ForgetReadPatchDescriptor_nid_no_patch(int fd);
std::shared_ptr<const ReadPatch::Ranges> ReadPatchesForDescriptor_nid_no_patch(int fd);
// ReadPatch::Apply on a finished read; the first read it changes is named on stderr.
void ApplyReadPatches_nid_no_patch(const ReadPatch::Ranges& ranges, const char* api, std::uint64_t offset, void* destination, std::uint64_t size);

#endif
