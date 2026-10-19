#ifndef CORE_SHADER_RECOMPILER_SHADERKEYLIST_HPP
#define CORE_SHADER_RECOMPILER_SHADERKEYLIST_HPP

#include "CompiledVariant.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The shader key list (P-PRECOMPILE): one line per recompile key a run meets, so a later launch can
// fill the shader disk cache before play. A line names the code by its size and hash and carries
// every other input of the disk cache key (stage info, binding layout, specialization) plus the
// shape of the descriptor snapshot the compile populates. It holds no code bytes: the precompile
// finds the code in the player's own game files, by an optional locator (a path below app0 and a
// byte offset) or by scanning files for the code's anchor hash.
namespace ShaderRecompiler::ShaderKeyList {

// Format 3 ("k3" lines) carries the record's inputs digest after the entry name, so the list verifies
// on other builds and devices. Format 2 ("k2") and format 3 lines end in a checksum, so a cut line
// never decodes; "k2" and "k1" lines (format 1, no checksum) still decode, and a list rewrite turns
// them into k3.
inline constexpr std::uint32_t FormatVersion = 3;
// Dwords the anchor hash covers: the first AnchorDwords of the code.
inline constexpr std::size_t AnchorDwords = 8;
// Bounds a decoded record must keep: a code's dwords, and the user data, flat SRT and other counts.
inline constexpr std::uint32_t MaxCodeDwords = 1u << 20u;
inline constexpr std::size_t MaxCount = 1u << 16u;
// A key list larger than this is not read.
inline constexpr std::uintmax_t MaxListBytes = 64ull << 20u;
// A line longer than this is unreadable; records beyond MaxRecords are left out (a hand-made list).
inline constexpr std::size_t MaxLineBytes = 64u << 10u;
inline constexpr std::size_t MaxRecords = 100000;
// The locator path of the running program's own file (a port that links the game's executable into
// its own carries that code there instead of in app0/eboot.bin).
inline constexpr std::string_view ExecutableLocator = "@exe";
// The list a release ships next to the program, precompiled by default (ChooseList).
inline constexpr std::string_view ShippedListName = "shader-keys.txt";
// The file in the shader cache's entry directory that names the lists precompiled into it (PrecompileDone).
inline constexpr std::string_view MarkerName = "precompiled.txt";

struct CodeIdentity {
    std::uint32_t dwords = 0;
    std::array<std::uint64_t, 2> hash{};
    std::uint64_t anchor = 0;

    bool operator==(const CodeIdentity& other) const = default;
};

// The descriptor snapshot's shape (counts and widths, no values): Populate checks it while the
// compile allocates bindings; the values never reach the SPIR-V.
struct SnapshotShape {
    std::vector<std::uint32_t> buffers;
    std::vector<std::uint32_t> images;
    std::vector<std::uint32_t> samplers;
    std::uint32_t flattenedSrt = 0;
    // The flat offsets of the deferred slots (their guest addresses are left out).
    std::vector<std::uint32_t> deferredFlat;
    std::uint32_t userData = 0;
};

struct Record {
    // The recording's disk cache entry name and SPIR-V hash: a precompile under the same sources,
    // device and switches rebuilds that name, and must then produce that SPIR-V.
    std::string entry;
    std::uint64_t spirvHash = 0;
    // ShaderDiskCache::InputDigest of the recording's request: a precompile under other sources,
    // device or switches rebuilds it when the rebuilt request is the recording's. 0: not recorded
    // (format 1 and 2 lines).
    std::uint64_t inputDigest = 0;
    ShaderStage stage = ShaderStage::Compute;
    // Not part of any key (it names the program in diagnostics only): Describe leaves it 0.
    std::uint64_t codeAddress = 0;
    CodeIdentity code;
    std::uint32_t waveSize = 0;
    std::uint32_t userDataBaseRegister = 0;
    std::uint32_t userDataCount = 0;
    std::optional<ShaderComputeStageInfo> compute;
    std::optional<ShaderPixelStageInfo> pixel;
    std::optional<ShaderVertexStageInfo> vertex;
    std::optional<MeshConfiguration> mesh;
    BindingLayout layout{};
    ResourceSpecialization specialization;
    SnapshotShape shape;
    // Where the code lies in the game files: a path below app0 ('/' separated, ValidLocatorPath) or
    // ExecutableLocator, and a byte offset.
    std::string locatorPath;
    std::uint64_t locatorOffset = 0;
};

[[nodiscard]] CodeIdentity IdentifyCode(std::span<const std::uint32_t> code);
[[nodiscard]] std::uint64_t AnchorHash(std::span<const std::uint32_t> words);
[[nodiscard]] bool CodeMatches(std::span<const std::byte> bytes, const CodeIdentity& identity);

[[nodiscard]] Record Describe(const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization);
[[nodiscard]] std::string Encode(const Record& record);
[[nodiscard]] bool Decode(std::string_view line, Record& record);
// A relative path below app0 without "." or ".." parts, drive or backslash, or ExecutableLocator.
[[nodiscard]] bool ValidLocatorPath(std::string_view path);
// The code of `identity` at byte `offset` of `bytes`, into `words`; false (words untouched) when the
// bytes there are not that code.
[[nodiscard]] bool TakeCode(std::span<const std::byte> bytes, std::uint64_t offset, const CodeIdentity& identity, std::vector<std::uint32_t>& words);

// A key list file: its lines in order and the records decoded from them. A line that is no record
// of this format (a comment, another format, a damaged line, a repeated entry) is kept verbatim, so
// a rewrite changes the records' locators and format and nothing else.
struct ListFile {
    struct Line {
        std::string text;
        // The record of the line, or -1 for a verbatim line.
        std::ptrdiff_t record = -1;
    };
    std::vector<Line> lines;
    std::vector<Record> records;
    std::size_t unreadable = 0;
    std::size_t duplicates = 0;
    // Records left out of the list for the MaxRecords limit.
    std::size_t overLimit = 0;
    // Records read from format 1 or 2 lines.
    std::size_t legacy = 0;
};
[[nodiscard]] bool ReadList(const std::filesystem::path& path, ListFile& list);
// Writes the list to a temporary file renamed over `path`.
[[nodiscard]] bool WriteList(const std::filesystem::path& path, const ListFile& list);

// A request (and snapshot) rebuilt from a record over the code and the device's target: its disk
// cache key is the recording's when the code, the target and the switches are the same. The
// request's spans point into the storage, which must outlive it.
struct RebuiltRequest {
    RecompileRequest request;
    ResourceSnapshot snapshot;
    std::vector<std::uint32_t> userData;
    std::vector<LinkedProgram> linked;
};
void Rebuild(const Record& record, std::span<const std::uint32_t> code, const SpirvTarget& target, RebuiltRequest& rebuilt);

// One hit of a scan: the code of identity `index` lies at byte `offset`.
struct ScanHit {
    std::size_t index;
    std::uint64_t offset;

    bool operator==(const ScanHit& other) const = default;
};

// Finds codes in file bytes by their anchor (a rolling hash over AnchorDwords dwords at every byte
// alignment), each anchor match verified by the code's full hash. Codes shorter than AnchorDwords
// are never found. Built once for the identities, then run over any number of files.
class Scanner {
public:
    explicit Scanner(std::span<const CodeIdentity> identities);

    // Every offset of every identity in `bytes`, by alignment phase then offset.
    [[nodiscard]] std::vector<ScanHit> Find(std::span<const std::byte> bytes) const;

private:
    std::vector<CodeIdentity> identities;
    std::vector<std::uint64_t> filter;
    std::vector<std::pair<std::uint64_t, std::size_t>> anchors;
};

// APS5_SHADER_KEYS=<file>: every variant the recompiler creates (from the disk cache or compiled)
// appends its record to the file, once per disk cache entry name (lines already there count).
[[nodiscard]] bool Recording();
void Note(const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization, std::string_view entry, std::uint64_t inputDigest, const CompiledVariant& variant);
[[nodiscard]] std::uint64_t Recorded();

enum class PrecompileStatus {
    // Stored under the recording's entry name, with the recording's SPIR-V.
    Stored,
    // Stored under another entry name than the recording's (another build, device or switch set),
    // with the recording's inputs digest: the rebuilt request is the recording's, so the entry is
    // the one the game asks for here, compiled as the game's own miss would compile it.
    StoredByInputs,
    // Stored with neither the recording's entry name nor its inputs digest: only with storeUnverified.
    StoredUnverified,
    // Not compiled: neither the recording's entry name nor its inputs digest, without storeUnverified.
    Unverified,
    Present,
    Diverged,
    Disabled,
};

struct PrecompileOutcome {
    PrecompileStatus status = PrecompileStatus::Disabled;
    std::string entry;
    std::uint64_t spirvHash = 0;
    // The rebuilt request's inputs digest (a list rewrite adds it to records verified by entry name).
    std::uint64_t inputDigest = 0;
};

// Compiles the variant of a rebuilt request into the disk cache unless its entry is there
// (Recompiler.cpp: the compile the recompiler itself runs on a miss). A variant whose entry
// name is the recording's but whose SPIR-V is not is reported Diverged and not stored; one whose
// entry name and inputs digest both differ from the recording's is compiled and stored only with
// storeUnverified.
[[nodiscard]] PrecompileOutcome Precompile(const Record& record, const RebuiltRequest& rebuilt, bool storeUnverified);

// The list a launch precompiles, from APS5_PRECOMPILE: a path names that list (precompiled at every
// launch); unset or empty, the shipped list (ShippedListName in the program's directory) when it
// exists; "0" turns the precompile off. An empty path: no precompile.
struct ListChoice {
    std::filesystem::path path;
    bool shipped = false;
};
[[nodiscard]] ListChoice ChooseList(const char* setting, const std::filesystem::path& programDirectory);

// The shipped list is precompiled only while the cache is cold for it: until the cache's entry
// directory holds a MarkerName line for the list (the hash of its bytes) on this cache context
// (ShaderDiskCache::ContextDigest: device target and switch set; the directory is per source version).
[[nodiscard]] bool PrecompileDone(const std::filesystem::path& entryDirectory, std::uint64_t listHash, std::uint64_t context);
// Appends the list's line (a marker over its size cap starts again); false when the file cannot be written.
[[nodiscard]] bool MarkPrecompiled(const std::filesystem::path& entryDirectory, std::uint64_t listHash, std::uint64_t context);
// Whether the marker can be written (the directory created, the marker opened for appending): an
// unwritable cache would lose every precompiled entry and run the precompile again at every launch.
[[nodiscard]] bool MarkerWritable(const std::filesystem::path& entryDirectory);

// What one precompile of the shipped list did, for the marker decision.
struct PrecompileTally {
    std::size_t records = 0;
    std::size_t codes = 0;
    std::size_t notFound = 0;
    std::size_t cut = 0;
    std::size_t failed = 0;
    std::uint64_t writeFailures = 0;
};
// A run is marked only when a later run cannot do better: it went to the end (no budget cut), every
// record compiled without an exception, every store reached the disk, and the game files held some of
// the codes (all locators stale: another exe or dump, which a later relink or patch can fix). Records
// left unverifiable or diverged are not retried: the next run would decide them the same way.
[[nodiscard]] bool MarkAllowed(const PrecompileTally& tally);

}

#endif
