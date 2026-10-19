#include "ShaderKeyList.hpp"
#include "CacheKey.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;
namespace KeyList = ShaderRecompiler::ShaderKeyList;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unsetEnvironment(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct Target {
    std::array<std::uint32_t, 2> capabilities{1u, 61u};
    std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    SpirvTarget target{};

    Target() {
        target.vulkanVersion = 0x00401000u;
        target.spirvVersion = 0x00010300u;
        target.subgroupSize = 32;
        target.supportedCapabilities = capabilities;
        target.supportedExtensions = extensions;
        target.maxWorkgroupSize = {1024u, 1024u, 64u};
        target.maxWorkgroupInvocations = 1024;
        target.maxWorkgroupSharedMemoryBytes = 49152;
    }
};

// A request of each stage family with every optional part the record carries, and a snapshot.
struct Sample {
    std::vector<std::uint32_t> code;
    std::vector<std::uint32_t> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu, 7u};
    Target target;
    MeshConfiguration mesh{3u, 64u, 128u, 128u, 64u, 128u, 512u, 0u, 16u};
    GraphicsCompileContext graphics{};
    RecompileRequest request{};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;

    explicit Sample(ShaderStage stage) {
        for (std::uint32_t i = 0; i < 48; ++i) code.push_back(0x9e3779b9u * (i + 1u));
        request.shader = {stage, 0x1234500u, code, 0x1233f00u, {}};
        request.context.waveSize = stage == ShaderStage::Fragment ? 32u : 64u;
        request.context.userDataBaseRegister = 0x240u;
        request.context.userData = userData;
        request.target = target.target;
        request.layout = {1u, 4u, 16u, 112u};
        if (stage == ShaderStage::Compute) request.context.compute = ShaderComputeStageInfo{{64u, 2u, 1u}, 512u, {true, false, true}, true, 2u, {100u, 3u, 1u}};
        if (stage == ShaderStage::Fragment) {
            ShaderPixelStageInfo pixel{};
            pixel.interpolatorCount = 3;
            pixel.interpolatorSettings[0] = 0x20u;
            pixel.interpolatorSettings[2] = 0x401u;
            pixel.wave32 = true;
            pixel.inputAddr = 0x302u;
            pixel.posW = true;
            pixel.earlyZ = true;
            pixel.targetOutputMode = {4u, 0u, 9u, 0u, 0u, 0u, 0u, 1u};
            pixel.targetExportMapping = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
            request.context.pixel = pixel;
        }
        if (stage == ShaderStage::Vertex || stage == ShaderStage::Mesh) {
            ShaderVertexStageInfo vertex{};
            vertex.resourcesNum = 2;
            vertex.resources[0].fields = {0x10000u, 0x00180000u, 1000u, 0x0007f000u};
            vertex.resources[1].fields = {0x1000cu, 0x00180000u, 1000u, 0x0006a000u};
            vertex.resourcesDst[0] = {4, 3, 0, 0u};
            vertex.resourcesDst[1] = {-1, 2, 1, 1u};
            vertex.fetchAttribReg = 2;
            vertex.fetchBufferReg = 6;
            vertex.fetchEmbedded = stage == ShaderStage::Mesh;
            request.context.vertex = vertex;
        }
        if (stage == ShaderStage::Mesh) {
            graphics.mesh = mesh;
            request.graphics = graphics;
        }
        DescriptorValue buffer;
        buffer.dwordCount = 4;
        buffer.dwords = {1u, 2u, 3u, 4u};
        DescriptorValue image;
        image.dwordCount = 8;
        DescriptorValue sampler;
        sampler.dwordCount = 4;
        snapshot.buffers = {buffer, buffer};
        snapshot.images = {image};
        snapshot.samplers = {sampler, sampler, sampler};
        snapshot.flattenedSrt = {9u, 8u, 7u};
        snapshot.deferredFlat = {{1u, 0x5000u}, {2u, 0x6000u}};
        snapshot.userData = userData;
        specialization.buffers = {{16u, IrBufferFormat::Invalid, 0xfacu, false}, {0u, IrBufferFormat::Invalid, 0u, true}};
        ResourceSpecialization::Image specialized;
        specialized.mipCount = 5;
        specialized.cube = true;
        specialized.depthUnorm16 = true;
        specialization.images = {specialized};
        specialization.boundDescriptors = {0u, 3u, 0xffffffffu};
    }
};

void verifyRoundTrip() {
    for (const auto stage : {ShaderStage::Compute, ShaderStage::Fragment, ShaderStage::Vertex, ShaderStage::Mesh}) {
        Sample sample(stage);
        auto record = KeyList::Describe(sample.request, sample.snapshot, sample.specialization);
        record.entry = "0123456789abcdef0123456789abcdef.bin";
        record.spirvHash = 0xfedcba9876543210ull;
        if (stage == ShaderStage::Vertex) {
            record.locatorPath = "some dir/shaders file.bin";
            record.locatorOffset = 0x1235u;
        }
        const auto line = KeyList::Encode(record);
        require(line.find('\n') == std::string::npos, "an encoded record spans lines");
        KeyList::Record decoded;
        require(KeyList::Decode(line, decoded), "an encoded record does not decode: " + line);
        require(KeyList::Encode(decoded) == line, "a decoded record encodes differently");
        require(KeyList::Decode(line + "\r", decoded) && KeyList::Encode(decoded) == line, "a record with a CR does not decode");
        require(decoded.code == KeyList::IdentifyCode(sample.code), "the code identity changed");
        require(decoded.locatorPath == record.locatorPath && decoded.locatorOffset == record.locatorOffset, "the locator changed");
        require(decoded.specialization == sample.specialization, "the specialization changed");

        // The rebuilt request has the recording's keys: the memory key and the disk cache key.
        KeyList::RebuiltRequest rebuilt;
        KeyList::Rebuild(decoded, sample.code, sample.target.target, rebuilt);
        std::vector<std::uint64_t> original, again;
        RecompileCacheKey::Build(sample.request, original);
        RecompileCacheKey::Build(rebuilt.request, again);
        require(original == again, "the rebuilt request has another memory key");
        std::vector<std::byte> diskOriginal, diskAgain;
        ShaderDiskCache::BuildKey(sample.request, 32, sample.specialization, diskOriginal);
        ShaderDiskCache::BuildKey(rebuilt.request, 32, decoded.specialization, diskAgain);
        require(diskOriginal == diskAgain, "the rebuilt request has another disk cache key");
        const auto& snapshot = rebuilt.snapshot;
        require(snapshot.buffers.size() == 2 && snapshot.buffers[1].dwordCount == 4 && snapshot.buffers[1].dwords[0] == 0u, "the buffer shape changed");
        require(snapshot.images.size() == 1 && snapshot.images[0].dwordCount == 8 && snapshot.samplers.size() == 3, "the image or sampler shape changed");
        require(snapshot.flattenedSrt.size() == 3 && snapshot.userData.size() == sample.userData.size(), "the flat or user data shape changed");
        require(snapshot.deferredFlat.size() == 2 && snapshot.deferredFlat[1].first == 2u && snapshot.deferredFlat[1].second == 0u, "the deferred slots changed");
        require(rebuilt.request.context.userData.size() == sample.userData.size(), "the user data count changed");
    }
}

// The format 1 form of a line: the k1 tag, no inputs digest and no checksum.
std::string legacyLine(const std::string& line) {
    const auto entryEnd = line.find(' ', 3);
    const auto digestEnd = line.find(' ', entryEnd + 1);
    return "k1" + line.substr(2, entryEnd - 2) + line.substr(digestEnd, line.rfind(" ~") - digestEnd);
}

// A line body with its checksum (ShaderKeyList.cpp's ChecksumSeed).
std::string seal(const std::string& body) {
    char checksum[24];
    std::snprintf(checksum, sizeof(checksum), " ~%016llx", static_cast<unsigned long long>(HashBytes(std::as_bytes(std::span(body.data(), body.size())), 0x6b65796c69737403ull)));
    return body + checksum;
}

// The format 2 form of a line: the k2 tag, a checksum and no inputs digest.
std::string sealedLine(const std::string& line) {
    return seal("k2" + legacyLine(line).substr(2));
}

// Values that index, size an allocation or divide later must not get past Decode.
void verifyHostile() {
    Sample sample(ShaderStage::Compute);
    const auto base = KeyList::Describe(sample.request, sample.snapshot, sample.specialization);
    const auto accepts = [&](const std::function<void(KeyList::Record&)>& change) {
        auto record = base;
        record.entry = "0123456789abcdef0123456789abcdef.bin";
        change(record);
        KeyList::Record decoded;
        return KeyList::Decode(KeyList::Encode(record), decoded);
    };
    require(accepts([](KeyList::Record&) {}), "the sample record does not decode");
    require(accepts([](KeyList::Record& r) { r.waveSize = 32; }) && accepts([](KeyList::Record& r) { r.waveSize = 64; }), "a valid wave size is refused");
    require(!accepts([](KeyList::Record& r) { r.waveSize = 0; }), "wave size 0 decodes");
    require(!accepts([](KeyList::Record& r) { r.waveSize = 16; }), "wave size 16 decodes");
    require(accepts([](KeyList::Record& r) { r.shape.images = {8u, 1u}; r.shape.samplers = {4u}; }), "valid descriptor widths are refused");
    require(!accepts([](KeyList::Record& r) { r.shape.images = {9u}; }), "an image width of 9 decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.images = {0xffffffffu}; }), "an image width of 2^32-1 decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.samplers = {8u, 9u}; }), "a sampler width of 9 decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.images = {0u}; }), "an image width of 0 decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.buffers = {0x10000000u}; }), "a buffer width of 2^28 decodes");
    require(!accepts([](KeyList::Record& r) { r.userDataCount = 0xffffffffu; }), "a huge user data count decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.flattenedSrt = 0xffffffffu; }), "a huge flat SRT count decodes");
    require(!accepts([](KeyList::Record& r) { r.shape.userData = 0x7fffffffu; }), "a huge snapshot user data count decodes");
    require(!accepts([](KeyList::Record& r) { r.code.dwords = 0xffffffffu; }), "a huge code size decodes");
    require(!accepts([](KeyList::Record& r) { r.userDataBaseRegister = 0xffffffffu; }), "a huge user data base register decodes");
    require(!accepts([](KeyList::Record& r) { r.compute->ldsSizeDwords = 0xffffffffu; }), "a huge LDS size decodes");
    require(!accepts([](KeyList::Record& r) { r.compute->numThreads[1] = 0u; }), "a zero thread count decodes");
    require(!accepts([](KeyList::Record& r) { r.specialization.images[0].mipCount = 0u; }), "a mip count of 0 decodes");
    require(!accepts([](KeyList::Record& r) { r.specialization.images[0].mipCount = 0xffffffffu; }), "a mip count of 2^32-1 decodes");
    require(!accepts([](KeyList::Record& r) { r.specialization.images[0].mipCount = 17u; }), "a mip count of 17 decodes");
    require(accepts([](KeyList::Record& r) { r.specialization.images[0].mipCount = 16u; }), "a mip count of 16 is refused");
    require(!accepts([](KeyList::Record& r) { r.compute->numThreads[0] = 1025u; }), "1025 threads decode");
    require(!accepts([](KeyList::Record& r) { r.compute->threadIdComponentCount = 4u; }), "4 thread id components decode");
    require(!accepts([](KeyList::Record& r) { r.stage = static_cast<ShaderStage>(8); }), "a stage beyond the enum decodes");
    require(!accepts([](KeyList::Record& r) { r.specialization.images[0].dimension = static_cast<RdnaImageDimension>(99); }), "an image dimension beyond the enum decodes");
    {
        Sample fragment(ShaderStage::Fragment);
        auto pixelBase = KeyList::Describe(fragment.request, fragment.snapshot, fragment.specialization);
        pixelBase.entry = "0123456789abcdef0123456789abcdef.bin";
        KeyList::Record again;
        require(KeyList::Decode(KeyList::Encode(pixelBase), again), "the fragment sample does not decode");
        pixelBase.pixel->interpolatorCount = 33u;
        require(!KeyList::Decode(KeyList::Encode(pixelBase), again), "33 interpolators decode");
    }
    {
        Sample mesh(ShaderStage::Mesh);
        auto meshBase = KeyList::Describe(mesh.request, mesh.snapshot, mesh.specialization);
        meshBase.entry = "0123456789abcdef0123456789abcdef.bin";
        KeyList::Record again;
        require(KeyList::Decode(KeyList::Encode(meshBase), again), "the mesh sample does not decode");
        auto big = meshBase;
        big.mesh->ldsSizeDwords = 0xffffffffu;
        require(!KeyList::Decode(KeyList::Encode(big), again), "a huge mesh LDS decodes");
        big = meshBase;
        big.mesh->maxVertices = 100000u;
        require(!KeyList::Decode(KeyList::Encode(big), again), "100000 mesh vertices decode");
        big = meshBase;
        big.mesh->threadsPerGroup = 4096u;
        require(!KeyList::Decode(KeyList::Encode(big), again), "4096 mesh threads decode");
    }
    // A count of list entries larger than the tokens that follow.
    KeyList::Record decoded;
    auto line = KeyList::Encode([&] { auto r = base; r.entry = "0123456789abcdef0123456789abcdef.bin"; return r; }());
    line = legacyLine(line) + " ffffffffffffffff";
    require(!KeyList::Decode(line, decoded), "a record with a trailing huge value decodes");
    // The list reader leaves out oversized lines and records beyond the limit.
    const auto path = std::filesystem::temp_directory_path() / "aps5-shader-key-list-hostile.txt";
    {
        auto record = base;
        record.entry = "0123456789abcdef0123456789abcdef.bin";
        const auto good = KeyList::Encode(record);
        std::ofstream out(path, std::ios::binary);
        out << good << '\n' << std::string(KeyList::MaxLineBytes + 1, 'x') << '\n';
        record.entry = "1123456789abcdef0123456789abcdef.bin";
        out << KeyList::Encode(record) << '\n';
    }
    KeyList::ListFile list;
    require(KeyList::ReadList(path, list) && list.records.size() == 2 && list.unreadable == 1 && list.lines.size() == 3 && list.lines[1].record < 0 && list.lines[1].text.size() == KeyList::MaxLineBytes + 1, "an oversized line was dropped or the good ones lost");
    // The records beyond MaxRecords are counted, and their lines stay.
    {
        auto record = base;
        std::ofstream out(path, std::ios::binary);
        for (std::size_t i = 0; i < KeyList::MaxRecords + 3; ++i) {
            char name[40];
            std::snprintf(name, sizeof(name), "%032zx.bin", i);
            record.entry = name;
            out << KeyList::Encode(record) << '\n';
        }
    }
    KeyList::ListFile many;
    require(KeyList::ReadList(path, many) && many.records.size() == KeyList::MaxRecords && many.overLimit == 3 && many.lines.size() == KeyList::MaxRecords + 3, "the record limit did not cut");
    std::error_code error;
    std::filesystem::remove(path, error);
}

void verifyRejects() {
    Sample sample(ShaderStage::Compute);
    auto record = KeyList::Describe(sample.request, sample.snapshot, sample.specialization);
    record.entry = "0123456789abcdef0123456789abcdef.bin";
    const auto sealed = KeyList::Encode(record);
    KeyList::Record decoded;
    require(sealed.compare(0, 3, "k3 ") == 0 && KeyList::Decode(sealed, decoded), "a format 3 line does not decode");
    // Format 3 carries the inputs digest after the entry name; format 2 lines (no digest) still decode.
    auto digested = record;
    digested.inputDigest = 0x0123456789abcdefull;
    const auto withDigest = KeyList::Encode(digested);
    require(KeyList::Decode(withDigest, decoded) && decoded.inputDigest == digested.inputDigest && KeyList::Encode(decoded) == withDigest, "the inputs digest does not round-trip");
    require(KeyList::Decode(sealedLine(withDigest), decoded) && decoded.inputDigest == 0 && KeyList::Encode(decoded) == sealed, "a format 2 line does not decode to the record without a digest");
    const auto body = withDigest.substr(0, withDigest.rfind(" ~"));
    const auto digestStart = body.find(' ', 3) + 1;
    require(!KeyList::Decode(seal(body.substr(0, digestStart) + "zz" + body.substr(body.find(' ', digestStart))), decoded), "a non-hex inputs digest decodes");
    require(!KeyList::Decode(seal("k3" + legacyLine(withDigest).substr(2)), decoded), "a format 3 line without its digest decodes");
    require(KeyList::Decode(sealed, decoded), "the format 3 line does not decode again");
    require(record.codeAddress == 0x1234500u && decoded.codeAddress == record.codeAddress, "the record carries the code address");
    // A cut line never decodes: the checksum, or a value before it.
    require(!KeyList::Decode(sealed.substr(0, sealed.size() - 1), decoded), "a line missing its checksum's last digit decodes");
    require(!KeyList::Decode(sealed.substr(0, sealed.rfind(" ~")), decoded), "a format 3 line without its checksum decodes");
    auto cut = sealed;
    cut.erase(cut.rfind(" ~") - 1, 1);
    require(!KeyList::Decode(cut, decoded), "a line whose last value lost a digit decodes");
    // The structure checks, over format 1 lines (no checksum to fail first).
    const auto line = legacyLine(sealed);
    require(KeyList::Decode(line, decoded) && KeyList::Encode(decoded) == sealed, "a format 1 line does not decode to the same record");
    require(!KeyList::Decode("", decoded), "an empty line decodes");
    require(!KeyList::Decode("k4" + line.substr(2), decoded), "another format tag decodes");
    require(!KeyList::Decode("k1 0123.bin" + line.substr(line.find(".bin") + 4), decoded), "a short entry name decodes");
    require(!KeyList::Decode(line.substr(0, line.rfind(' ')), decoded), "a record missing its last value decodes");
    require(!KeyList::Decode(line + " 0", decoded), "a record with an extra value decodes");
    require(!KeyList::Decode(line + " zz", decoded), "a record with a non-hex value decodes");
    require(!KeyList::Decode(line + " @", decoded), "a locator without offset decodes");
    require(!KeyList::Decode(line + " @ 10", decoded), "a locator without path decodes");
    // The spirv hash is first; the stage second: a value beyond the enum's range must not decode.
    auto tokens = line;
    const auto stageStart = tokens.find(' ', tokens.find(' ', 3) + 1) + 1;
    const auto stageEnd = tokens.find(' ', stageStart);
    require(KeyList::Decode(tokens, decoded), "the record itself does not decode");
    tokens.replace(stageStart, stageEnd - stageStart, "100000000");
    require(!KeyList::Decode(tokens, decoded), "a stage beyond 32 bits decodes");

    // Counts a rebuild allocates by stay bounded.
    const auto rejects = [&](auto change, const char* what) {
        auto changed = record;
        change(changed);
        KeyList::Record out;
        require(!KeyList::Decode(KeyList::Encode(changed), out), std::string("a record with ") + what + " decodes");
    };
    rejects([](KeyList::Record& r) { r.userDataCount = static_cast<std::uint32_t>(KeyList::MaxCount + 1); }, "a huge user data count");
    rejects([](KeyList::Record& r) { r.shape.flattenedSrt = 0xffffffffu; }, "a huge flat SRT");
    rejects([](KeyList::Record& r) { r.shape.userData = static_cast<std::uint32_t>(KeyList::MaxCount + 1); }, "a huge snapshot user data count");
    rejects([](KeyList::Record& r) { r.code.dwords = 0; }, "an empty code");
    rejects([](KeyList::Record& r) { r.code.dwords = KeyList::MaxCodeDwords + 1; }, "a huge code");
    // Locators stay below app0 (or name the program).
    for (const char* path : {"/etc/passwd", "C:/Windows/win.ini", "a/../../b", "..", "./a", "a//b", "a/", "a\\b", "x\ty"}) {
        rejects([&](KeyList::Record& r) { r.locatorPath = path; r.locatorOffset = 4; }, (std::string("the locator ") + path).c_str());
        require(!KeyList::ValidLocatorPath(path), std::string("the locator path is valid: ") + path);
    }
    for (const char* path : {"eboot.bin", "materials/!cookedshaders/a b.csdr", "@exe"}) {
        auto located = record;
        located.locatorPath = path;
        located.locatorOffset = 0x40;
        require(KeyList::Decode(KeyList::Encode(located), decoded) && decoded.locatorPath == path, std::string("the locator does not decode: ") + path);
    }
}

void verifyTakeCode() {
    std::vector<std::uint32_t> words(64);
    for (std::uint32_t i = 0; i < words.size(); ++i) words[i] = 0x01000193u * (i + 7u);
    const std::vector<std::uint32_t> code(words.begin() + 10, words.begin() + 30);
    const auto identity = KeyList::IdentifyCode(code);
    const auto bytes = std::as_bytes(std::span(words));
    std::vector<std::uint32_t> out{1u};
    require(KeyList::TakeCode(bytes, 40, identity, out) && out == code, "the code at its offset is not taken");
    out = {1u};
    require(!KeyList::TakeCode(bytes, 44, identity, out) && out == std::vector<std::uint32_t>{1u}, "the code at a wrong offset is taken");
    require(!KeyList::TakeCode(bytes, bytes.size() - 8, identity, out), "a code running past the end is taken");
    require(!KeyList::TakeCode(bytes, bytes.size() + 4, identity, out), "an offset beyond the file is taken");
    require(!KeyList::TakeCode(bytes, ~std::uint64_t{0}, identity, out), "the largest offset is taken");
    auto stale = words;
    stale[20] ^= 0x100u;
    require(!KeyList::TakeCode(std::as_bytes(std::span(stale)), 40, identity, out), "a changed code (a stale locator) is taken");
}

// A list rewrite keeps every line that is no record verbatim and upgrades format 1 records.
void verifyListFile(const std::filesystem::path& directory) {
    Sample sample(ShaderStage::Compute);
    auto first = KeyList::Describe(sample.request, sample.snapshot, sample.specialization);
    first.entry = "0123456789abcdef0123456789abcdef.bin";
    auto second = first;
    second.entry = "00000000000000000000000000000002.bin";
    const auto foreign = "k9 a line of a later format";
    const auto damaged = KeyList::Encode(second).substr(0, 60);
    const auto path = directory / "list.txt";
    {
        std::ofstream file(path, std::ios::binary);
        file << "# a comment\r\n" << legacyLine(KeyList::Encode(first)) << "\r\n" << foreign << "\n" << KeyList::Encode(second) << "\n" << KeyList::Encode(first) << "\n" << damaged;
    }
    KeyList::ListFile list;
    require(KeyList::ReadList(path, list), "the list does not read");
    require(list.records.size() == 2 && list.lines.size() == 6, "the list read " + std::to_string(list.records.size()) + " records in " + std::to_string(list.lines.size()) + " lines, not 2 in 6");
    require(list.unreadable == 2 && list.duplicates == 1 && list.legacy == 1, "the list counts are wrong");
    require(list.lines[1].record == 0 && list.lines[3].record == 1 && list.lines[4].record < 0, "the lines do not map to their records");
    list.records[1].locatorPath = "materials/x.csdr";
    list.records[1].locatorOffset = 0x100;
    require(KeyList::WriteList(path, list), "the list does not write");
    KeyList::ListFile again;
    require(KeyList::ReadList(path, again), "the rewritten list does not read");
    require(again.lines.size() == 6 && again.lines[0].text == "# a comment" && again.lines[2].text == foreign && again.lines[5].text == damaged, "the rewrite changed a line that is no record");
    require(again.lines[4].text == KeyList::Encode(first), "the rewrite changed a repeated record's line");
    require(again.records.size() == 2 && again.legacy == 0 && again.unreadable == 2 && again.duplicates == 1, "the rewritten list counts are wrong");
    require(again.records[1].locatorPath == "materials/x.csdr" && again.records[1].locatorOffset == 0x100, "the rewrite lost the locator");
    require(!KeyList::ReadList(directory / "no such list.txt", again), "a missing list reads");
}

void verifyScanner() {
    std::mt19937 random(1234);
    std::vector<std::byte> file(1u << 20u);
    for (auto& byte : file) byte = static_cast<std::byte>(random() & 0xffu);
    const auto words = [&](std::size_t count) {
        std::vector<std::uint32_t> code(count);
        for (auto& word : code) word = static_cast<std::uint32_t>(random());
        return code;
    };
    const auto place = [&](const std::vector<std::uint32_t>& code, std::size_t offset) { std::memcpy(file.data() + offset, code.data(), code.size() * sizeof(std::uint32_t)); };
    const auto a = words(64), b = words(300), shortCode = words(5), c = words(40);
    place(a, 4097);
    place(a, 600002);
    place(b, 8192);
    place(shortCode, 20000);
    // c's anchor without the rest of c: an anchor hit the full hash refuses.
    auto decoy = c;
    decoy[30] ^= 1u;
    place(decoy, 30000);
    // b runs to the very end of the file.
    place(b, file.size() - b.size() * sizeof(std::uint32_t));
    const std::vector<KeyList::CodeIdentity> identities{KeyList::IdentifyCode(a), KeyList::IdentifyCode(b), KeyList::IdentifyCode(shortCode), KeyList::IdentifyCode(c)};
    const KeyList::Scanner scanner(identities);
    auto hits = scanner.Find(file);
    std::sort(hits.begin(), hits.end(), [](const auto& l, const auto& r) { return l.offset < r.offset; });
    const std::vector<KeyList::ScanHit> expected{{0, 4097}, {1, 8192}, {0, 600002}, {1, file.size() - b.size() * sizeof(std::uint32_t)}};
    require(hits == expected, "the scan found " + std::to_string(hits.size()) + " hits, not the 4 placed");
    require(KeyList::CodeMatches(std::span(file).subspan(4097, a.size() * 4), identities[0]), "the code at its offset does not match");
    require(!KeyList::CodeMatches(std::span(file).subspan(4097, a.size() * 4 - 4), identities[0]), "a shorter span matches");
    require(scanner.Find(std::span(file).first(16)).empty(), "a file shorter than the anchor has hits");
}

struct ComputeRequest {
    std::vector<std::uint32_t> code{0xe0700000u, 0x80000000u, 0xbf810000u};
    std::array<std::uint32_t, 4> userData{0x10000000u, 0x00000000u, 0x40u, 0x00027facu};
    std::array<std::uint32_t, 1> capabilities{1u};
    RecompileRequest request{};

    ComputeRequest() {
        request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
        request.context.waveSize = 64;
        request.context.userDataBaseRegister = 0;
        request.context.userData = userData;
        request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32;
        request.target.supportedCapabilities = capabilities;
        request.layout.pushConstantSizeBytes = 128;
    }
};

// A real recompile records its key; the record, rebuilt over the same code and target, finds the
// entry present, and once the entry is gone compiles it again to the recorded SPIR-V. A record of
// another build (another entry name) is stored by its inputs digest, and one without a digest is not.
void verifyRecordAndPrecompile(const std::filesystem::path& keys) {
    // The list ends in a cut line (a recorder killed mid-write): the first record starts a line of its own.
    {
        std::ofstream file(keys, std::ios::binary);
        file << "# seeded\nk2 0123456789abcdef";
    }
    ComputeRequest compute;
    const auto compiled = Recompile(compute.request);
    ShaderDiskCache::Flush();
    require(!compiled.spirv.empty(), "the compile produced no SPIR-V");
    require(KeyList::Recorded() == 1, "the compile recorded " + std::to_string(KeyList::Recorded()) + " keys, not 1");
    KeyList::ListFile list;
    require(KeyList::ReadList(keys, list), "the key list does not read");
    require(list.records.size() == 1 && list.unreadable == 1 && list.lines.size() == 3, "the key list holds " + std::to_string(list.records.size()) + " records and " + std::to_string(list.unreadable) + " unreadable lines, not 1 and 1");
    const auto& record = list.records[0];
    require(record.code == KeyList::IdentifyCode(compute.code) && record.locatorPath.empty(), "the recorded code identity is wrong");
    KeyList::RebuiltRequest rebuilt;
    KeyList::Rebuild(record, compute.code, compute.request.target, rebuilt);
    require(record.inputDigest != 0 && record.inputDigest == ShaderDiskCache::InputDigest(rebuilt.request, record.specialization), "the record does not carry the rebuilt request's inputs digest");
    auto outcome = KeyList::Precompile(record, rebuilt, false);
    require(outcome.status == KeyList::PrecompileStatus::Present && outcome.entry == record.entry && outcome.inputDigest == record.inputDigest, "the precompile does not find the recorded entry present");
    const auto entryFile = ShaderDiskCache::EntryDirectory() / record.entry;
    std::filesystem::remove(entryFile);
    // Other SPIR-V under the recording's entry name is never stored.
    auto changed = record;
    changed.spirvHash ^= 1u;
    outcome = KeyList::Precompile(changed, rebuilt, true);
    ShaderDiskCache::Flush();
    require(outcome.status == KeyList::PrecompileStatus::Diverged && !std::filesystem::exists(entryFile), "a diverged variant was stored");
    // Neither the recording's entry name nor its inputs digest: skipped, or stored when asked to.
    changed = record;
    changed.entry = "ffffffffffffffffffffffffffffffff.bin";
    changed.inputDigest ^= 1u;
    outcome = KeyList::Precompile(changed, rebuilt, false);
    ShaderDiskCache::Flush();
    require(outcome.status == KeyList::PrecompileStatus::Unverified && outcome.entry == record.entry && !std::filesystem::exists(entryFile), "an unverifiable variant was stored");
    outcome = KeyList::Precompile(changed, rebuilt, true);
    ShaderDiskCache::Flush();
    require(outcome.status == KeyList::PrecompileStatus::StoredUnverified && std::filesystem::is_regular_file(entryFile), "an unverifiable variant was not stored when asked to");
    std::filesystem::remove(entryFile);
    // A list recorded by another build (another source version: another entry name) with the inputs digest.
    changed = record;
    changed.entry = "ffffffffffffffffffffffffffffffff.bin";
    outcome = KeyList::Precompile(changed, rebuilt, false);
    ShaderDiskCache::Flush();
    require(outcome.status == KeyList::PrecompileStatus::StoredByInputs && outcome.entry == record.entry && outcome.spirvHash == record.spirvHash && std::filesystem::is_regular_file(entryFile), "a record of another build was not stored by its inputs digest");
    std::filesystem::remove(entryFile);
    // The same record from a format 2 list (no digest) cannot be verified.
    changed.inputDigest = 0;
    outcome = KeyList::Precompile(changed, rebuilt, false);
    ShaderDiskCache::Flush();
    require(outcome.status == KeyList::PrecompileStatus::Unverified && !std::filesystem::exists(entryFile), "a record of another build without a digest was stored");
    outcome = KeyList::Precompile(record, rebuilt, false);
    require(outcome.status == KeyList::PrecompileStatus::Stored, "the precompile did not store the missing entry");
    require(outcome.entry == record.entry && outcome.spirvHash == record.spirvHash, "the precompile compiled other SPIR-V than the recording");
    ShaderDiskCache::Flush();
    require(std::filesystem::is_regular_file(entryFile), "the precompiled entry was not written");
    // A second recompile of the same key in this process records nothing new.
    static_cast<void>(Recompile(compute.request));
    require(KeyList::Recorded() == 1, "a repeated key was recorded again");
}

}

// The inputs digest leaves out what a key list record does not decide (the device target, the host
// subgroup size, the source version and the switches) and keeps the rest. Its value for a fixed
// request is pinned: a change to the request part of the disk key (CacheKey.hpp, ShaderDiskCache's
// BuildKey) changes every digest, and shipped key lists stop verifying until they are recorded again.
constexpr std::uint64_t PinnedDigest = 0x6ed03554c726762full;

void verifyDigest() {
    Sample sample(ShaderStage::Compute);
    const auto digest = ShaderDiskCache::InputDigest(sample.request, sample.specialization);
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(digest));
    require(digest == PinnedDigest, std::string("the sample's inputs digest is now ") + text + ": the disk key's request part changed; record the shipped key lists again (APS5_SHADER_KEYS) and pin the new value here");
    auto other = sample.request;
    other.target.subgroupSize = 64;
    other.target.maxWorkgroupInvocations = 512;
    require(ShaderDiskCache::InputDigest(other, sample.specialization) == digest, "the device target changes the inputs digest");
    std::vector<std::byte> key, otherKey;
    ShaderDiskCache::BuildKey(sample.request, 32, sample.specialization, key);
    ShaderDiskCache::BuildKey(other, 32, sample.specialization, otherKey);
    require(key != otherKey, "the device target left the disk key");
    other = sample.request;
    other.context.waveSize = 32;
    require(ShaderDiskCache::InputDigest(other, sample.specialization) != digest, "the wave size does not change the inputs digest");
    auto specialization = sample.specialization;
    specialization.images[0].mipCount = 4;
    require(ShaderDiskCache::InputDigest(sample.request, specialization) != digest, "the specialization does not change the inputs digest");
}

// APS5_PRECOMPILE: a named list, the shipped list next to the program, or off; the shipped list runs
// while the cache has no marker line for it on this context.
void verifyShippedList(const std::filesystem::path& directory) {
    const auto program = directory / "program dir";
    std::filesystem::create_directories(program);
    require(KeyList::ChooseList(nullptr, program).path.empty(), "a missing shipped list is chosen");
    std::filesystem::create_directories(program / std::string(KeyList::ShippedListName));
    require(KeyList::ChooseList(nullptr, program).path.empty(), "a folder named like the shipped list is chosen");
    std::filesystem::remove(program / std::string(KeyList::ShippedListName));
    const auto shipped = program / std::string(KeyList::ShippedListName);
    {
        std::ofstream file(shipped, std::ios::binary);
        file << "# shipped\n";
    }
    auto choice = KeyList::ChooseList(nullptr, program);
    require(choice.path == shipped && choice.shipped, "the shipped list next to the program is not chosen");
    choice = KeyList::ChooseList("", program);
    require(choice.path == shipped && choice.shipped, "an empty APS5_PRECOMPILE does not choose the shipped list");
    require(KeyList::ChooseList("0", program).path.empty(), "APS5_PRECOMPILE=0 does not turn the precompile off");
    choice = KeyList::ChooseList("other keys.txt", program);
    require(choice.path == std::filesystem::path("other keys.txt") && !choice.shipped, "a named list is not chosen as before");
    require(KeyList::ChooseList(nullptr, {}).path.empty(), "a list is chosen without a program directory");

    const auto cache = directory / "marker cache" / "0123456789abcdef";
    require(!KeyList::PrecompileDone(cache, 1, 2), "a cache without a marker is warm");
    require(KeyList::MarkerWritable(cache), "a new cache directory is not writable");
    require(!KeyList::PrecompileDone(cache, 1, 2), "probing the marker marked the list");
    require(KeyList::MarkPrecompiled(cache, 1, 2), "the marker write failed");
    require(KeyList::PrecompileDone(cache, 1, 2), "the marked list is still cold");
    require(!KeyList::PrecompileDone(cache, 1, 3) && !KeyList::PrecompileDone(cache, 4, 2), "another list or context counts as precompiled");
    require(KeyList::MarkPrecompiled(cache, 4, 2), "the second marker write failed");
    require(KeyList::PrecompileDone(cache, 1, 2) && KeyList::PrecompileDone(cache, 4, 2), "a second marker line lost the first");

    // A marker over its size cap counts as cold and starts again at the next mark.
    {
        std::ofstream file(cache / std::string(KeyList::MarkerName), std::ios::binary | std::ios::app);
        const std::string filler(1023, 'x');
        for (int i = 0; i < 1100; ++i) file << filler << '\n';
    }
    require(!KeyList::PrecompileDone(cache, 1, 2), "an oversized marker is trusted");
    require(KeyList::MarkPrecompiled(cache, 5, 6), "the oversized marker is not written again");
    require(std::filesystem::file_size(cache / std::string(KeyList::MarkerName)) < 1024, "the oversized marker kept growing");
    require(KeyList::PrecompileDone(cache, 5, 6) && !KeyList::PrecompileDone(cache, 1, 2), "the rewritten marker is wrong");

    // A cache directory that cannot be created is not writable, and its marker write fails.
    const auto blocker = directory / "marker blocker";
    std::ofstream(blocker, std::ios::binary) << "file";
    require(!KeyList::MarkerWritable(blocker / "0123456789abcdef"), "a cache under a file counts as writable");
    require(!KeyList::MarkPrecompiled(blocker / "0123456789abcdef", 1, 2), "a marker under a file counts as written");

    Target first, second;
    second.target.subgroupSize = 64;
    require(ShaderDiskCache::ContextDigest(first.target) == ShaderDiskCache::ContextDigest(Target().target), "the context digest of one target changes");
    require(ShaderDiskCache::ContextDigest(first.target) != ShaderDiskCache::ContextDigest(second.target), "the context digest does not follow the device target");
}

// tools/package_release.py (hash_bytes) hashes the shipped list as the precompile does; both must agree.
void verifyListHash() {
    const std::string_view sample = "k3 shader-keys sample\r\n";
    const auto bytes = std::as_bytes(std::span(sample.data(), sample.size()));
    require(ShaderRecompiler::HashBytes(bytes) == 0xd9d5f7fe862de207ull, "HashBytes changed: change hash_bytes in tools/package_release.py with it");
    require(ShaderRecompiler::HashBytes({}) == 0, "HashBytes of nothing changed: change hash_bytes in tools/package_release.py with it");
}

// Only a run that a later launch cannot improve on is marked as done.
void verifyMarkAllowed() {
    KeyList::PrecompileTally done;
    done.records = 1315;
    done.codes = 702;
    done.notFound = 3;
    require(KeyList::MarkAllowed(done), "a finished run is not marked");
    auto tally = done;
    tally.cut = 1;
    require(!KeyList::MarkAllowed(tally), "a run cut by the time budget is marked");
    tally = done;
    tally.failed = 1;
    require(!KeyList::MarkAllowed(tally), "a run with a failed record is marked");
    tally = done;
    tally.writeFailures = 1;
    require(!KeyList::MarkAllowed(tally), "a run with a failed cache write is marked");
    tally = done;
    tally.notFound = tally.codes;
    require(!KeyList::MarkAllowed(tally), "a run that found none of the codes is marked");
    tally = done;
    tally.records = 0;
    tally.codes = 0;
    tally.notFound = 0;
    require(!KeyList::MarkAllowed(tally), "an empty list is marked");
}

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() / ("aps5-shader-key-list-test-" + std::to_string(std::random_device{}()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto keys = directory / "keys.txt";
        setEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
        setEnvironment("ANYPS5_SHADER_CACHE_DIR", (directory / "cache").string());
        setEnvironment("APS5_SHADER_KEYS", keys.string());
        // The rule switches enter the key and so the pinned digest.
        for (const char* name : {"APS5_NO_LEGACY_MAD_RULE", "APS5_NO_MULLIT_RULE", "APS5_CLAMP_NAN", "APS5_RAYTRACING", "APS5_NO_CODE_HASH_KEY"}) unsetEnvironment(name);
        verifyRoundTrip();
        verifyHostile();
        verifyRejects();
        verifyTakeCode();
        verifyListFile(directory);
        verifyScanner();
        verifyRecordAndPrecompile(keys);
        verifyDigest();
        verifyShippedList(directory);
        verifyMarkAllowed();
        verifyListHash();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::cout << "shader key list tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
