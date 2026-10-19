#include "ShaderKeyList.hpp"
#include "ShaderCacheDirectory.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <unordered_set>

namespace ShaderRecompiler::ShaderKeyList {

// A field added to one of these must be added to visitRecord as well.
static_assert(sizeof(ShaderComputeStageInfo) == 36, "ShaderComputeStageInfo changed: update visitRecord");
static_assert(sizeof(ShaderPixelStageInfo) == 172, "ShaderPixelStageInfo changed: update visitRecord");
static_assert(sizeof(ShaderVertexStageInfo) == 1040, "ShaderVertexStageInfo changed: update visitRecord");
static_assert(sizeof(MeshConfiguration) == 36, "MeshConfiguration changed: update visitRecord");
static_assert(sizeof(BindingLayout) == 16, "BindingLayout changed: update visitRecord");
static_assert(sizeof(ResourceSpecialization::Buffer) == 16, "ResourceSpecialization::Buffer changed: update visitRecord");
static_assert(sizeof(ResourceSpecialization::Image) == 40, "ResourceSpecialization::Image changed: update visitRecord");

namespace {

constexpr std::uint64_t CodeSeeds[2] = {0x6b65796c69737401ull, 0x6b65796c69737402ull};
constexpr std::uint64_t AnchorBase = 0x100000001b3ull;
constexpr std::string_view LineTag = "k2";
constexpr std::string_view LegacyLineTag = "k1";
constexpr std::uint64_t ChecksumSeed = 0x6b65796c69737403ull;
// " ~" and 16 hex digits end a k2 line.
constexpr std::size_t ChecksumChars = 16;
constexpr std::size_t MaxListLength = MaxCount;

struct Writer {
    std::vector<std::uint64_t>& out;

    template<typename TValue>
    void operator()(TValue& value) requires (std::is_integral_v<TValue> || std::is_enum_v<TValue>) {
        out.push_back(static_cast<std::uint64_t>(value));
    }

    template<typename TValue, std::size_t TSize>
    void operator()(std::array<TValue, TSize>& values) {
        for (auto& value : values) (*this)(value);
    }

    template<typename TValue, typename TFields>
    void optional(std::optional<TValue>& value, TFields&& fields) {
        bool present = value.has_value();
        (*this)(present);
        if (present) fields(*value);
    }

    template<typename TValue, typename TFields>
    void list(std::vector<TValue>& values, TFields&& fields) {
        auto count = values.size();
        (*this)(count);
        for (auto& value : values) fields(value);
    }

    void fail() {}
};

struct Reader {
    std::span<const std::uint64_t> in;
    std::size_t at = 0;
    bool failed = false;

    std::uint64_t next() {
        if (at >= in.size()) {
            failed = true;
            return 0;
        }
        return in[at++];
    }

    template<typename TValue>
    void operator()(TValue& value) requires (std::is_integral_v<TValue> || std::is_enum_v<TValue>) {
        const auto raw = next();
        if constexpr (std::is_same_v<TValue, bool>) {
            if (raw > 1u) failed = true;
            value = raw != 0u;
        } else {
            value = static_cast<TValue>(raw);
            if (static_cast<std::uint64_t>(value) != raw) failed = true;
        }
    }

    template<typename TValue, std::size_t TSize>
    void operator()(std::array<TValue, TSize>& values) {
        for (auto& value : values) (*this)(value);
    }

    template<typename TValue, typename TFields>
    void optional(std::optional<TValue>& value, TFields&& fields) {
        bool present = false;
        (*this)(present);
        value.reset();
        if (!present || failed) return;
        fields(value.emplace());
    }

    template<typename TValue, typename TFields>
    void list(std::vector<TValue>& values, TFields&& fields) {
        std::size_t count = 0;
        (*this)(count);
        values.clear();
        if (failed || count > MaxListLength || count > in.size() - at) {
            failed = true;
            return;
        }
        values.resize(count);
        for (auto& value : values) fields(value);
    }

    void fail() { failed = true; }
};

// Every field of a record but the entry name and the locator, in one order for both directions.
template<typename TIo>
void visitRecord(TIo& io, Record& record) {
    io(record.spirvHash);
    io(record.stage);
    io(record.codeAddress);
    io(record.code.dwords);
    io(record.code.hash);
    io(record.code.anchor);
    io(record.waveSize);
    io(record.userDataBaseRegister);
    io(record.userDataCount);
    io.optional(record.compute, [&](ShaderComputeStageInfo& compute) {
        io(compute.numThreads);
        io(compute.ldsSizeDwords);
        io(compute.groupIdEnable);
        io(compute.tgSizeEnable);
        io(compute.threadIdComponentCount);
        io(compute.partialThreads);
    });
    io.optional(record.pixel, [&](ShaderPixelStageInfo& pixel) {
        io(pixel.interpolatorCount);
        io(pixel.interpolatorSettings);
        io(pixel.wave32);
        io(pixel.inputAddr);
        io(pixel.hasPerspectiveCenterVgpr);
        io(pixel.perspectiveCentroid);
        io(pixel.posX);
        io(pixel.posY);
        io(pixel.posZ);
        io(pixel.posW);
        io(pixel.frontFace);
        io(pixel.ancillary);
        io(pixel.sampleShading);
        io(pixel.noPerspective);
        io(pixel.linearCentroid);
        io(pixel.pixelKillEnable);
        io(pixel.depthExportEnable);
        io(pixel.sampleMaskExportEnable);
        io(pixel.earlyZ);
        io(pixel.executeOnNoop);
        io(pixel.targetOutputMode);
        io(pixel.targetExportMapping);
    });
    io.optional(record.vertex, [&](ShaderVertexStageInfo& vertex) {
        io(vertex.resourcesNum);
        if (vertex.resourcesNum > vertex.resources.size()) {
            io.fail();
            return;
        }
        for (std::uint32_t i = 0; i < vertex.resourcesNum; ++i) {
            io(vertex.resources[i].fields);
            io(vertex.resourcesDst[i].registerStart);
            io(vertex.resourcesDst[i].registersNum);
            io(vertex.resourcesDst[i].attrId);
            io(vertex.resourcesDst[i].fetchIndex);
        }
        io(vertex.fetchAttribReg);
        io(vertex.fetchBufferReg);
        io(vertex.fetchEmbedded);
    });
    io.optional(record.mesh, [&](MeshConfiguration& mesh) {
        io(mesh.inputPrimitive);
        io(mesh.primitivesPerGroup);
        io(mesh.verticesPerGroup);
        io(mesh.maxVertices);
        io(mesh.maxPrimitives);
        io(mesh.threadsPerGroup);
        io(mesh.ldsSizeDwords);
        io(mesh.provokingVertex);
        io(mesh.esgsItemSize);
    });
    io(record.layout.descriptorSet);
    io(record.layout.firstBinding);
    io(record.layout.pushConstantOffsetBytes);
    io(record.layout.pushConstantSizeBytes);
    io.list(record.specialization.buffers, [&](ResourceSpecialization::Buffer& buffer) {
        io(buffer.packedStride);
        io(buffer.descriptorFormat);
        io(buffer.descriptorSwizzle);
        io(buffer.empty);
    });
    io.list(record.specialization.images, [&](ResourceSpecialization::Image& image) {
        io(image.numericClass);
        io(image.dimension);
        io(image.mipCount);
        io(image.conversionFormat);
        io(image.shaderSwizzle);
        io(image.indirectRoot);
        io(image.indirectMappingOffset);
        io(image.indirectSearchIterations);
        io(image.cube);
        io(image.fmask);
        io(image.depthBits);
        io(image.depthUnorm16);
        io(image.float16Store);
    });
    io.list(record.specialization.boundDescriptors, [&](std::uint32_t& value) { io(value); });
    const auto words = [&](std::uint32_t& value) { io(value); };
    io.list(record.shape.buffers, words);
    io.list(record.shape.images, words);
    io.list(record.shape.samplers, words);
    io(record.shape.flattenedSrt);
    io.list(record.shape.deferredFlat, words);
    io(record.shape.userData);
}

// The counts a rebuild allocates by stay bounded (a damaged or hand-edited list).
bool boundedRecord(const Record& record) {
    if (record.code.dwords == 0 || record.code.dwords > MaxCodeDwords || record.userDataCount > MaxCount || record.shape.flattenedSrt > MaxCount || record.shape.userData > MaxCount) return false;
    // The wave size is a divisor and a modulus in the recompiler.
    if (record.waveSize != 32u && record.waveSize != 64u) return false;
    if (record.userDataBaseRegister > MaxCount) return false;
    // Descriptor widths index DescriptorValue::dwords.
    const auto widths = [](const std::vector<std::uint32_t>& values, std::uint32_t low, std::uint32_t high) {
        return values.size() <= MaxCount && std::all_of(values.begin(), values.end(), [&](std::uint32_t width) { return width >= low && width <= high; });
    };
    constexpr std::uint32_t Capacity = static_cast<std::uint32_t>(std::tuple_size_v<decltype(DescriptorValue::dwords)>);
    if (!widths(record.shape.buffers, 1u, Capacity) || !widths(record.shape.images, 1u, Capacity) || !widths(record.shape.samplers, 1u, Capacity)) return false;
    if (record.shape.deferredFlat.size() > MaxCount || record.specialization.buffers.size() > MaxCount || record.specialization.images.size() > MaxCount || record.specialization.boundDescriptors.size() > MaxCount) return false;
    if (record.compute) {
        const auto& compute = *record.compute;
        if (compute.ldsSizeDwords > MaxCount || compute.threadIdComponentCount > 3u) return false;
        for (const auto threads : compute.numThreads) {
            if (threads == 0 || threads > 1024u) return false;
        }
    }
    if (record.pixel && record.pixel->interpolatorCount > record.pixel->interpolatorSettings.size()) return false;
    if (record.mesh && record.mesh->ldsSizeDwords > MaxCount) return false;
    return true;
}

std::uint64_t lineChecksum(std::string_view body) {
    return HashBytes(std::as_bytes(std::span(body.data(), body.size())), ChecksumSeed);
}

bool startsWithTag(std::string_view line, std::string_view tag) {
    return line.size() > tag.size() && line.substr(0, tag.size()) == tag && line[tag.size()] == ' ';
}

bool validEntry(std::string_view entry) {
    return entry.size() == 36 && entry.substr(32) == ".bin" && entry.substr(0, 32).find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::uint64_t anchorPower() {
    std::uint64_t power = 1;
    for (std::size_t i = 1; i < AnchorDwords; ++i) power *= AnchorBase;
    return power;
}

constexpr unsigned FilterBits = 22;

std::size_t filterSlot(std::uint64_t anchor) {
    return static_cast<std::size_t>((anchor * 0x9e3779b97f4a7c15ull) >> (64u - FilterBits));
}

struct KeyFile {
    HostMutex mutex;
    bool opened = false;
    std::FILE* file = nullptr;
    std::unordered_set<std::string> entries;
    std::atomic<std::uint64_t> recorded{0};
};

KeyFile& keyFile() {
    static KeyFile* instance = new KeyFile();
    return *instance;
}

const char* keyListPath() {
    static const char* path = [] { const char* value = std::getenv("APS5_SHADER_KEYS"); return value != nullptr && *value != '\0' ? value : nullptr; }();
    return path;
}

}

std::uint64_t AnchorHash(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < words.size() && i < AnchorDwords; ++i) hash = hash * AnchorBase + words[i];
    return hash;
}

CodeIdentity IdentifyCode(std::span<const std::uint32_t> code) {
    const auto bytes = std::as_bytes(code);
    return {static_cast<std::uint32_t>(code.size()), {HashBytes(bytes, CodeSeeds[0]), HashBytes(bytes, CodeSeeds[1])}, AnchorHash(code)};
}

bool CodeMatches(std::span<const std::byte> bytes, const CodeIdentity& identity) {
    return bytes.size() == static_cast<std::size_t>(identity.dwords) * sizeof(std::uint32_t) && HashBytes(bytes, CodeSeeds[0]) == identity.hash[0] && HashBytes(bytes, CodeSeeds[1]) == identity.hash[1];
}

Record Describe(const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization) {
    Record record;
    record.stage = request.shader.stage;
    record.code = IdentifyCode(request.shader.code);
    record.waveSize = request.context.waveSize;
    record.codeAddress = request.shader.codeAddress;
    record.userDataBaseRegister = request.context.userDataBaseRegister;
    record.userDataCount = static_cast<std::uint32_t>(request.context.userData.size());
    record.compute = request.context.compute;
    record.pixel = request.context.pixel;
    record.vertex = request.context.vertex;
    if (request.graphics && request.graphics->mesh) record.mesh = request.graphics->mesh;
    record.layout = request.layout;
    record.specialization = specialization;
    for (const auto& value : snapshot.buffers) record.shape.buffers.push_back(value.dwordCount);
    for (const auto& value : snapshot.images) record.shape.images.push_back(value.dwordCount);
    for (const auto& value : snapshot.samplers) record.shape.samplers.push_back(value.dwordCount);
    record.shape.flattenedSrt = static_cast<std::uint32_t>(snapshot.flattenedSrt.size());
    for (const auto& slot : snapshot.deferredFlat) record.shape.deferredFlat.push_back(slot.first);
    record.shape.userData = static_cast<std::uint32_t>(snapshot.userData.size());
    return record;
}

std::string Encode(const Record& record) {
    std::vector<std::uint64_t> values;
    Writer writer{values};
    auto copy = record;
    visitRecord(writer, copy);
    std::string line;
    line.reserve(64 + values.size() * 4);
    line += LineTag;
    line += ' ';
    line += record.entry;
    char digits[24];
    for (const auto value : values) {
        const auto end = std::to_chars(digits, digits + sizeof(digits), value, 16).ptr;
        line += ' ';
        line.append(digits, end);
    }
    if (!record.locatorPath.empty()) {
        const auto end = std::to_chars(digits, digits + sizeof(digits), record.locatorOffset, 16).ptr;
        line += " @ ";
        line.append(digits, end);
        line += ' ';
        line += record.locatorPath;
    }
    char checksum[ChecksumChars + 1];
    std::snprintf(checksum, sizeof(checksum), "%016llx", static_cast<unsigned long long>(lineChecksum(line)));
    line += " ~";
    line += checksum;
    return line;
}

bool Decode(std::string_view line, Record& record) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (startsWithTag(line, LineTag)) {
        // The checksum: exactly 16 hex digits after the last " ~", over everything before it.
        const auto mark = line.rfind(" ~");
        if (mark == std::string_view::npos || line.size() - mark - 2 != ChecksumChars) return false;
        const auto digits = line.substr(mark + 2);
        std::uint64_t checksum = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), checksum, 16);
        if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size()) return false;
        line = line.substr(0, mark);
        if (checksum != lineChecksum(line)) return false;
    } else if (!startsWithTag(line, LegacyLineTag)) {
        return false;
    }
    std::size_t at = 0;
    const auto token = [&]() -> std::string_view {
        while (at < line.size() && line[at] == ' ') ++at;
        const auto start = at;
        while (at < line.size() && line[at] != ' ') ++at;
        return line.substr(start, at - start);
    };
    const auto tag = token();
    if (tag != LineTag && tag != LegacyLineTag) return false;
    record = Record{};
    const auto entry = token();
    if (!validEntry(entry)) return false;
    record.entry = std::string(entry);
    std::vector<std::uint64_t> values;
    bool locator = false;
    while (true) {
        const auto text = token();
        if (text.empty()) break;
        if (text == "@") {
            locator = true;
            break;
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return false;
        values.push_back(value);
    }
    Reader reader{values};
    visitRecord(reader, record);
    if (reader.failed || reader.at != values.size() || !boundedRecord(record)) return false;
    if (locator) {
        const auto offset = token();
        const auto parsed = std::from_chars(offset.data(), offset.data() + offset.size(), record.locatorOffset, 16);
        if (offset.empty() || parsed.ec != std::errc() || parsed.ptr != offset.data() + offset.size()) return false;
        if (at < line.size()) ++at;
        record.locatorPath = std::string(line.substr(at));
        if (!ValidLocatorPath(record.locatorPath)) return false;
    }
    return true;
}

bool ValidLocatorPath(std::string_view path) {
    if (path == ExecutableLocator) return true;
    if (path.empty() || path.front() == '/') return false;
    for (const char c : path) {
        if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20u) return false;
    }
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

bool TakeCode(std::span<const std::byte> bytes, std::uint64_t offset, const CodeIdentity& identity, std::vector<std::uint32_t>& words) {
    const auto size = static_cast<std::size_t>(identity.dwords) * sizeof(std::uint32_t);
    if (offset > bytes.size() || size > bytes.size() - offset || !CodeMatches(bytes.subspan(static_cast<std::size_t>(offset), size), identity)) return false;
    words.resize(identity.dwords);
    std::memcpy(words.data(), bytes.data() + offset, size);
    return true;
}

bool ReadList(const std::filesystem::path& path, ListFile& list) {
    list = ListFile{};
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > MaxListBytes) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::unordered_set<std::string> entries;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > MaxLineBytes) {
            ++list.unreadable;
            continue;
        }
        ListFile::Line kept{line};
        if (!line.empty() && line[0] != '#') {
            Record record;
            if (list.records.size() >= MaxRecords) ++list.overLimit;
            else if (!Decode(line, record)) ++list.unreadable;
            else if (!entries.insert(record.entry).second) ++list.duplicates;
            else {
                if (startsWithTag(line, LegacyLineTag)) ++list.legacy;
                kept.record = static_cast<std::ptrdiff_t>(list.records.size());
                list.records.push_back(std::move(record));
            }
        }
        list.lines.push_back(std::move(kept));
    }
    return true;
}

bool WriteList(const std::filesystem::path& path, const ListFile& list) {
    std::string text;
    for (const auto& line : list.lines) {
        text += line.record >= 0 ? Encode(list.records[static_cast<std::size_t>(line.record)]) : line.text;
        text += '\n';
    }
    return WriteFileAtomically(path, std::as_bytes(std::span(text)));
}

void Rebuild(const Record& record, std::span<const std::uint32_t> code, const SpirvTarget& target, RebuiltRequest& rebuilt) {
    rebuilt.userData.assign(record.userDataCount, 0u);
    rebuilt.linked.clear();
    auto& request = rebuilt.request;
    request = RecompileRequest{};
    request.shader = {record.stage, record.codeAddress, code, 0, {}};
    request.context.waveSize = record.waveSize;
    request.context.userDataBaseRegister = record.userDataBaseRegister;
    request.context.userData = rebuilt.userData;
    request.context.compute = record.compute;
    request.context.pixel = record.pixel;
    request.context.vertex = record.vertex;
    request.context.memory = {};
    request.target = target;
    request.layout = record.layout;
    if (record.mesh) {
        GraphicsCompileContext graphics{};
        graphics.linkedPrograms = rebuilt.linked;
        graphics.mesh = record.mesh;
        request.graphics = graphics;
    }
    request.useCache = true;
    auto& snapshot = rebuilt.snapshot;
    snapshot = ResourceSnapshot{};
    const auto values = [](const std::vector<std::uint32_t>& widths, std::vector<DescriptorValue>& out) {
        for (const auto width : widths) {
            DescriptorValue value;
            value.dwordCount = width;
            out.push_back(value);
        }
    };
    values(record.shape.buffers, snapshot.buffers);
    values(record.shape.images, snapshot.images);
    values(record.shape.samplers, snapshot.samplers);
    snapshot.flattenedSrt.assign(record.shape.flattenedSrt, 0u);
    for (const auto offset : record.shape.deferredFlat) snapshot.deferredFlat.emplace_back(offset, 0u);
    snapshot.userData.assign(record.shape.userData, 0u);
}

Scanner::Scanner(std::span<const CodeIdentity> identities) : identities(identities.begin(), identities.end()), filter((std::size_t{1} << FilterBits) / 64u, 0u) {
    for (std::size_t i = 0; i < this->identities.size(); ++i) {
        if (this->identities[i].dwords < AnchorDwords) continue;
        anchors.emplace_back(this->identities[i].anchor, i);
        const auto slot = filterSlot(this->identities[i].anchor);
        filter[slot / 64u] |= std::uint64_t{1} << (slot % 64u);
    }
    std::sort(anchors.begin(), anchors.end());
}

std::vector<ScanHit> Scanner::Find(std::span<const std::byte> bytes) const {
    std::vector<ScanHit> hits;
    constexpr std::size_t window = AnchorDwords * sizeof(std::uint32_t);
    if (bytes.size() < window || anchors.empty()) return hits;
    const auto power = anchorPower();
    for (std::size_t phase = 0; phase < sizeof(std::uint32_t) && phase + window <= bytes.size(); ++phase) {
        const auto count = (bytes.size() - phase) / sizeof(std::uint32_t);
        const auto word = [&](std::size_t index) {
            std::uint32_t value;
            std::memcpy(&value, bytes.data() + phase + index * sizeof(std::uint32_t), sizeof(value));
            return static_cast<std::uint64_t>(value);
        };
        std::uint64_t hash = 0;
        for (std::size_t j = 0; j < AnchorDwords; ++j) hash = hash * AnchorBase + word(j);
        for (std::size_t index = 0;; ++index) {
            const auto slot = filterSlot(hash);
            if ((filter[slot / 64u] >> (slot % 64u)) & 1u) {
                const auto first = std::lower_bound(anchors.begin(), anchors.end(), std::pair<std::uint64_t, std::size_t>{hash, 0});
                for (auto it = first; it != anchors.end() && it->first == hash; ++it) {
                    const auto& identity = identities[it->second];
                    const auto offset = phase + index * sizeof(std::uint32_t);
                    const auto size = static_cast<std::size_t>(identity.dwords) * sizeof(std::uint32_t);
                    if (size <= bytes.size() - offset && CodeMatches(bytes.subspan(offset, size), identity)) hits.push_back({it->second, offset});
                }
            }
            if (index + AnchorDwords >= count) break;
            hash = (hash - word(index) * power) * AnchorBase + word(index + AnchorDwords);
        }
    }
    return hits;
}

bool Recording() {
    return keyListPath() != nullptr;
}

std::uint64_t Recorded() {
    return keyFile().recorded.load(std::memory_order_relaxed);
}

void Note(const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization, std::string_view entry, const CompiledVariant& variant) {
    const char* path = keyListPath();
    if (path == nullptr) return;
    auto& keys = keyFile();
    std::lock_guard lock(keys.mutex);
    if (!keys.opened) {
        keys.opened = true;
        bool empty = true;
        {
            std::ifstream existing(path, std::ios::binary);
            std::string line;
            while (std::getline(existing, line)) {
                empty = false;
                if (line.size() > LineTag.size() + 37 && (startsWithTag(line, LineTag) || startsWithTag(line, LegacyLineTag))) keys.entries.insert(line.substr(LineTag.size() + 1, 36));
            }
        }
        // A list cut in its last line (a process killed mid-write) gets a line break first, so the
        // cut line stays one unreadable line and the next record starts its own.
        bool cut = false;
        if (!empty) {
            std::ifstream existing(path, std::ios::binary | std::ios::ate);
            if (existing && existing.tellg() > 0) {
                existing.seekg(-1, std::ios::end);
                cut = existing.get() != '\n';
            }
        }
        keys.file = std::fopen(path, "ab");
        if (keys.file != nullptr && cut) std::fputc('\n', keys.file);
        if (keys.file == nullptr) std::fprintf(stderr, "[shader-keys] cannot open %s: no keys are recorded\n", path);
        else {
            if (empty) std::fprintf(keys.file, "# AnyPS5 shader key list, format %u: recompile keys without shader code (APS5_SHADER_KEYS, APS5_PRECOMPILE)\n", FormatVersion);
            std::fprintf(stderr, "[shader-keys] recording to %s (%zu entries there)\n", path, keys.entries.size());
        }
    }
    if (keys.file == nullptr || !validEntry(entry) || !keys.entries.insert(std::string(entry)).second) return;
    auto record = Describe(request, snapshot, specialization);
    record.entry = std::string(entry);
    const auto& words = variant.result.spirv.Words();
    record.spirvHash = HashBytes(std::as_bytes(std::span(words)));
    const auto line = Encode(record);
    std::fwrite(line.data(), 1, line.size(), keys.file);
    std::fputc('\n', keys.file);
    std::fflush(keys.file);
    keys.recorded.fetch_add(1, std::memory_order_relaxed);
}

}
