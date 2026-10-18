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
constexpr std::string_view LineTag = "k1";
constexpr std::size_t MaxListLength = 1u << 16u;

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
    record.codeAddress = request.shader.codeAddress;
    record.code = IdentifyCode(request.shader.code);
    record.waveSize = request.context.waveSize;
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
    return line;
}

bool Decode(std::string_view line, Record& record) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    std::size_t at = 0;
    const auto token = [&]() -> std::string_view {
        while (at < line.size() && line[at] == ' ') ++at;
        const auto start = at;
        while (at < line.size() && line[at] != ' ') ++at;
        return line.substr(start, at - start);
    };
    if (token() != LineTag) return false;
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
    if (reader.failed || reader.at != values.size()) return false;
    if (locator) {
        const auto offset = token();
        const auto parsed = std::from_chars(offset.data(), offset.data() + offset.size(), record.locatorOffset, 16);
        if (offset.empty() || parsed.ec != std::errc() || parsed.ptr != offset.data() + offset.size()) return false;
        if (at < line.size()) ++at;
        record.locatorPath = std::string(line.substr(at));
        if (record.locatorPath.empty()) return false;
    }
    return true;
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
                if (line.size() > LineTag.size() + 37 && line.compare(0, LineTag.size() + 1, std::string(LineTag) + " ") == 0) keys.entries.insert(line.substr(LineTag.size() + 1, 36));
            }
        }
        keys.file = std::fopen(path, "ab");
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
