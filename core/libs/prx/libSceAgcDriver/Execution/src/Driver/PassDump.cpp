#include "prx/libSceAgcDriver/Execution/include/Driver/PassDump.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TexelStats.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::PassDump {

namespace {

using Clock = std::chrono::steady_clock;

enum class Phase { Idle, Armed, Active, Ending };

// One image to read back: a color target's mip, a depth target's slice, a written storage image.
struct Target {
    std::uint64_t address = 0;
    std::uint32_t level = 0;
    std::uint32_t layer = 0;
    int slot = -1;
};

// The open draw pass: the draws between two changes of the targets.
struct Pass {
    bool open = false;
    std::uint32_t queue = 0;
    std::string key;
    std::vector<Target> colors;
    std::optional<Target> depth;
    std::vector<Target> storage;
    std::uint64_t drawn = 0;
    std::uint64_t skipped = 0;
    std::set<std::string> vs;
    std::set<std::string> ps;
    std::set<std::uint64_t> inputs;
};

// What the rows of one event (a pass, a dispatch, a flip, the present) share.
struct Event {
    std::uint64_t index = 0;
    const char* kind = "";
    std::uint32_t queue = 0;
    std::uint64_t drawn = 0;
    std::uint64_t skipped = 0;
    std::string vs;
    std::string ps;
    std::string cs;
    std::string inputs;
    std::string storage;
};

struct Dump {
    HostMutex mutex;
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<std::uint64_t> presents{0};
    std::filesystem::path folder;
    std::FILE* index = nullptr;
    std::uint64_t rows = 0;
    std::uint64_t events = 0;
    std::uint64_t rawBytes = 0;
    double readMs = 0;
    Clock::time_point started;
    Pass pass;
};

Dump& dump() {
    static Dump instance;
    return instance;
}

HostRecursiveMutex& serialMutex() {
    static HostRecursiveMutex mutex;
    return mutex;
}

bool rawEnabled() {
    static const bool raw = [] {
        const char* value = std::getenv("APS5_PASS_DUMP_RAW");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return raw;
}

std::uint64_t atPresent() {
    static const std::uint64_t present = [] {
        const char* value = std::getenv("APS5_PASS_DUMP_AT");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    return present;
}

const std::filesystem::path& baseDirectory() {
    static const std::filesystem::path base = []() -> std::filesystem::path {
        if (const char* directory = std::getenv("APS5_PASS_DUMP_DIR")) return directory;
        if (const char* capture = std::getenv("APS5_CAPTURE_DIR")) return std::filesystem::path(capture).parent_path();
        return ".";
    }();
    return base;
}

double since(Clock::time_point began) {
    return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

template <typename TBank>
std::uint32_t reg(const TBank& bank, std::uint32_t offset) {
    const auto it = bank.find(offset);
    return it == bank.end() ? 0u : it->second;
}

// A program address (as the FrameTrace lines give it) with an FNV-1a hash of its first 256 code
// bytes, which tells two programs loaded at one address apart.
std::string programId(std::uint64_t address) {
    if (address == 0) return {};
    char text[48];
    const auto* code = reinterpret_cast<const unsigned char*>(address);
    if (GuestMemory::Accessible(code, 256)) {
        std::uint32_t hash = 2166136261u;
        for (std::size_t at = 0; at < 256; ++at) hash = (hash ^ code[at]) * 16777619u;
        std::snprintf(text, sizeof(text), "%llx:%08x", static_cast<unsigned long long>(address), hash);
    } else {
        std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(address));
    }
    return text;
}

std::uint64_t graphicsProgram(const QueueState& queue, std::uint32_t low) {
    return (static_cast<std::uint64_t>(reg(queue.shader, low + 1) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.shader, low)) << 8u);
}

std::string joined(const std::set<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) text += ';';
        text += item;
    }
    return text;
}

std::string joinedAddresses(const std::set<std::uint64_t>& addresses) {
    std::string text;
    char item[24];
    for (const auto address : addresses) {
        std::snprintf(item, sizeof(item), "%s%llx", text.empty() ? "" : ";", static_cast<unsigned long long>(address));
        text += item;
    }
    return text;
}

std::string joinedTargets(const std::vector<Target>& targets) {
    std::set<std::uint64_t> addresses;
    for (const auto& target : targets) addresses.insert(target.address);
    return joinedAddresses(addresses);
}

// A FrameTrace note (ShaderResources::noteFrameTrace): its " w=0x<address>/<w>x<h>/f<format>/m<mip>"
// written images, " wb=0x<address>+0x<bytes>" written buffer ranges, and from the bound-resource
// list the " texture 0x<address>" and " storage 0x<address>" entries it did not write (the inputs).
void parseNote(const std::string& note, std::set<std::uint64_t>& inputs, std::vector<Target>& written, std::vector<std::pair<std::uint64_t, std::uint64_t>>* buffers) {
    for (auto at = note.find(" w=0x"); at != std::string::npos; at = note.find(" w=0x", at + 1)) {
        unsigned long long address = 0;
        unsigned width = 0, height = 0, format = 0, mip = 0;
        if (std::sscanf(note.c_str() + at, " w=0x%llx/%ux%u/f%u/m%u", &address, &width, &height, &format, &mip) < 1 || address == 0) continue;
        bool known = false;
        for (const auto& target : written) known = known || (target.address == address && target.level == mip);
        if (!known && written.size() < 32) written.push_back({address, mip, 0, static_cast<int>(written.size())});
    }
    if (buffers != nullptr) {
        for (auto at = note.find(" wb=0x"); at != std::string::npos; at = note.find(" wb=0x", at + 1)) {
            unsigned long long address = 0, bytes = 0;
            if (std::sscanf(note.c_str() + at, " wb=0x%llx+0x%llx", &address, &bytes) == 2 && buffers->size() < 32) buffers->emplace_back(address, bytes);
        }
    }
    for (const char* kind : {" texture 0x", " storage 0x"}) {
        const auto length = std::strlen(kind);
        for (auto at = note.find(kind); at != std::string::npos; at = note.find(kind, at + 1)) {
            const auto address = std::strtoull(note.c_str() + at + length, nullptr, 16);
            bool own = false;
            for (const auto& target : written) own = own || target.address == address;
            if (address != 0 && !own && inputs.size() < 64) inputs.insert(address);
        }
    }
}

// <seq>.raw: "PASSDUMP1 vkformat=.. format=.. width=.. height=.. layers=.. texelbytes=..", a newline,
// then the layers' rows packed (width x texelbytes bytes each). The file name, "capped" past the
// dump's 2 GiB, or empty.
std::string writeRaw(Dump& state, std::uint64_t seq, const Graphics::ImageReadback& image) {
    constexpr std::uint64_t Cap = 2ull << 30u;
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * image.texelBytes;
    const std::size_t pitch = static_cast<std::size_t>(image.rowTexels) * image.texelBytes;
    const std::uint64_t bytes = static_cast<std::uint64_t>(rowBytes) * image.height * image.layers;
    if (bytes == 0 || pitch < rowBytes) return {};
    if (state.rawBytes + bytes > Cap) return "capped";
    const auto name = std::to_string(seq) + ".raw";
    std::FILE* file = std::fopen((state.folder / name).string().c_str(), "wb");
    if (file == nullptr) return {};
    std::fprintf(file, "PASSDUMP1 vkformat=%d format=%s width=%u height=%u layers=%u texelbytes=%u\n", static_cast<int>(image.format), Graphics::TexelFormatName(image.format), image.width, image.height, image.layers, image.texelBytes);
    for (std::size_t row = 0; row < static_cast<std::size_t>(image.height) * image.layers; ++row) {
        if (row * pitch + rowBytes > image.bytes.size()) break;
        std::fwrite(image.bytes.data() + row * pitch, 1, rowBytes, file);
    }
    std::fclose(file);
    state.rawBytes += bytes;
    return name;
}

// One line of index.csv: an image read back (`image`), one that was not resident (`missing`), or an
// event without an image (a flip, a pass with nothing drawn, a dispatch writing nothing).
void writeRow(Dump& state, const Event& event, const char* plane, int slot, std::uint64_t address, const Graphics::ImageReadback* image, bool missing, Clock::time_point began) {
    if (state.index == nullptr) return;
    const auto seq = state.rows++;
    Graphics::TexelStats stats;
    std::string raw;
    const char* format = missing ? "no-image" : "";
    if (image != nullptr) {
        format = Graphics::TexelFormatName(image->format);
        const std::size_t rowBytes = static_cast<std::size_t>(image->rowTexels) * image->texelBytes;
        const std::size_t layerBytes = rowBytes * image->height;
        for (std::uint32_t layer = 0; layer < image->layers && layerBytes != 0; ++layer) {
            if ((layer + 1ull) * layerBytes > image->bytes.size()) break;
            if (!Graphics::AccumulateTexelStats(image->format, image->bytes.subspan(layer * layerBytes, layerBytes), image->width, image->height, rowBytes, stats)) break;
        }
        if (rawEnabled()) raw = writeRaw(state, seq, *image);
    }
    const auto fraction = [&](std::uint64_t count) { return stats.texels != 0 ? static_cast<double>(count) / static_cast<double>(stats.texels) : 0.0; };
    char range[64] = ",";
    if (stats.finite) std::snprintf(range, sizeof(range), "%g,%g", stats.minimum, stats.maximum);
    const double ms = since(began);
    state.readMs += ms;
    const auto u = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::fprintf(state.index, "%llu,%llu,%llu,%s,%s,%d,%u,0x%llx,%llu,%d,%s,%u,%u,%u,%u,%u,%u,%u,%u,", u(seq), u(event.index), u(state.presents.load(std::memory_order_relaxed)), event.kind, plane, slot, event.queue, u(address), u(image != nullptr ? image->guestBytes : 0), image != nullptr ? static_cast<int>(image->format) : 0, format, image != nullptr ? image->guestFormat : 0u, image != nullptr ? image->width : 0u, image != nullptr ? image->height : 0u, image != nullptr ? image->level : 0u, image != nullptr ? image->firstLayer : 0u, image != nullptr ? image->layers : 0u, image != nullptr ? image->alias : 0u, image != nullptr ? image->aliases : 0u);
    std::fprintf(state.index, "%llu,%llu,%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%s,", u(event.drawn), u(event.skipped), event.vs.c_str(), event.ps.c_str(), event.cs.c_str(), u(stats.texels), u(stats.nan), u(stats.inf), u(stats.zero), u(stats.negative), fraction(stats.nan), fraction(stats.inf), fraction(stats.zero), fraction(stats.negative), u(stats.tiles), u(stats.badTiles), u(stats.edgeTiles), range);
    std::fprintf(state.index, "%s,%s,%s,%.3f\n", event.inputs.c_str(), event.storage.c_str(), raw.c_str(), ms);
    std::fflush(state.index);
}

// Reads back the images at `target` (the GPU drained) and writes their rows; one "no-image" row when
// none is resident there.
void readTarget(Dump& state, VulkanDevice& device, const Event& event, const char* plane, const Target& target, std::uint32_t layers, bool depth) {
    auto began = Clock::now();
    const auto consume = [&](const Graphics::ImageReadback& image) {
        writeRow(state, event, plane, target.slot, target.address, &image, false, began);
        began = Clock::now();
    };
    std::size_t read = 0;
    try {
        read = depth ? device.ReadBackDepth(target.address, target.layer, consume) : device.ReadBackImage(target.address, target.level, target.layer, layers, consume);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[passdump] reading back 0x%llx failed: %s\n", static_cast<unsigned long long>(target.address), error.what());
    }
    if (read == 0) writeRow(state, event, plane, target.slot, target.address, nullptr, true, began);
}

// Ends the open pass: its targets read back when a draw of it drew.
void closePass(Dump& state, VulkanDevice* device) {
    auto pass = std::exchange(state.pass, Pass{});
    if (!pass.open) return;
    Event event;
    event.index = state.events++;
    event.kind = "draw-pass";
    event.queue = pass.queue;
    event.drawn = pass.drawn;
    event.skipped = pass.skipped;
    event.vs = joined(pass.vs);
    event.ps = joined(pass.ps);
    event.inputs = joinedAddresses(pass.inputs);
    event.storage = joinedTargets(pass.storage);
    if (pass.drawn == 0 || device == nullptr) {
        writeRow(state, event, "none", -1, 0, nullptr, false, Clock::now());
        return;
    }
    device->WaitIdle();
    for (const auto& color : pass.colors) readTarget(state, *device, event, "color", color, 1, false);
    if (pass.depth) readTarget(state, *device, event, "depth", *pass.depth, 1, true);
    for (const auto& image : pass.storage) readTarget(state, *device, event, "storage", image, ~0u, false);
}

// The registers that name a draw's targets (as describeDraw reads them): two draws with the same key
// render into the same pass.
std::string passKey(const QueueState& queue) {
    std::string key;
    char item[64];
    const auto mask = reg(queue.context, 0x8e) & reg(queue.context, 0x8f);
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        const auto info = reg(queue.context, 0x31c + slot * 0xfu);
        if (((mask >> (4u * slot)) & 0xfu) == 0 || ((info >> 2u) & 0x1fu) == 0) continue;
        std::snprintf(item, sizeof(item), "c%u=%x:%x/%x/%x;", slot, reg(queue.context, 0x390 + slot) & 0xffu, reg(queue.context, 0x318 + slot * 0xfu), info, reg(queue.context, 0x31b + slot * 0xfu));
        key += item;
    }
    if ((reg(queue.context, 0x010) & 3u) != 0) {
        std::snprintf(item, sizeof(item), "z=%x:%x/%x", reg(queue.context, 0x01a) & 0xffu, reg(queue.context, 0x012), reg(queue.context, 0x002));
        key += item;
    }
    return key;
}

// The pass's targets as the draw path finds them (a mip chain's surface and mip, the depth slice);
// the registers' addresses (mip 0, DB_DEPTH_VIEW's slice) when the state does not decode.
void decodeTargets(const QueueState& queue, Pass& pass) {
    try {
        const auto state = Graphics::DecodeState(queue);
        for (std::size_t slot = 0; slot < state.colors.size(); ++slot) {
            const auto& color = state.colors[slot];
            const bool chain = color.mipCount > 1;
            pass.colors.push_back({chain ? color.surfaceAddress : color.address, chain ? color.mip : 0u, 0u, static_cast<int>(slot)});
        }
        if (state.depth) pass.depth = Target{state.depth->address, 0u, state.depth->slice, 0};
        return;
    } catch (const std::exception&) {
    }
    pass.colors.clear();
    const auto mask = reg(queue.context, 0x8e) & reg(queue.context, 0x8f);
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        const auto info = reg(queue.context, 0x31c + slot * 0xfu);
        if (((mask >> (4u * slot)) & 0xfu) == 0 || ((info >> 2u) & 0x1fu) == 0) continue;
        const auto address = (static_cast<std::uint64_t>(reg(queue.context, 0x390 + slot) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x318 + slot * 0xfu)) << 8u);
        pass.colors.push_back({address, 0u, 0u, static_cast<int>(slot)});
    }
    if ((reg(queue.context, 0x010) & 3u) != 0) {
        const auto address = (static_cast<std::uint64_t>(reg(queue.context, 0x01a) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x012)) << 8u);
        pass.depth = Target{address, 0u, reg(queue.context, 0x002) & 0x7ffu, 0};
    }
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PASS_DUMP");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool FrameActive() {
    return dump().phase.load(std::memory_order_relaxed) == Phase::Active;
}

Serial::Serial() {
    static const bool serialize = [] {
        const char* value = std::getenv("APS5_PASS_DUMP_SERIAL");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    held = serialize && FrameActive();
    if (held) serialMutex().lock();
}

Serial::~Serial() {
    if (held) serialMutex().unlock();
}

void AtPresent(VulkanDevice& device, std::uint64_t displayAddress) {
    auto& state = dump();
    const auto present = ++state.presents;
    std::lock_guard lock(state.mutex);
    const auto phase = state.phase.load();
    if (phase == Phase::Ending) {
        closePass(state, &device);
        Event event;
        event.index = state.events++;
        event.kind = "present-blit";
        device.WaitIdle();
        readTarget(state, device, event, "display", Target{displayAddress, 0u, 0u, -1}, 1, false);
        const double seconds = std::chrono::duration<double>(Clock::now() - state.started).count();
        if (state.index != nullptr) {
            std::fprintf(state.index, "#complete,rows=%llu,events=%llu,raw_bytes=%llu,read_ms=%.0f,seconds=%.2f\n", static_cast<unsigned long long>(state.rows), static_cast<unsigned long long>(state.events), static_cast<unsigned long long>(state.rawBytes), state.readMs, seconds);
            std::fclose(state.index);
            state.index = nullptr;
        }
        FrameTrace::SetNoting(false);
        state.phase.store(Phase::Idle);
        std::fprintf(stderr, "[passdump] %s: %llu rows over %llu events, %.1f MB raw, the frame took %.1f s (%.1f s reading back)\n", state.folder.string().c_str(), static_cast<unsigned long long>(state.rows), static_cast<unsigned long long>(state.events), static_cast<double>(state.rawBytes) / (1024.0 * 1024.0), seconds, state.readMs / 1000.0);
        return;
    }
    if (phase != Phase::Idle) return;
    std::error_code error;
    const bool requested = std::filesystem::remove(baseDirectory() / "PASSDUMP_NOW", error);
    if (!requested && present != atPresent()) return;
    char stamp[32] = "dump";
    const auto now = std::time(nullptr);
    if (const auto* local = std::localtime(&now)) std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
    state.folder = baseDirectory() / "passdump" / (std::string(stamp) + "-p" + std::to_string(present));
    std::filesystem::create_directories(state.folder, error);
    state.index = std::fopen((state.folder / "index.csv").string().c_str(), "w");
    if (state.index == nullptr) {
        std::fprintf(stderr, "[passdump] cannot write %s\n", (state.folder / "index.csv").string().c_str());
        return;
    }
    std::fputs("seq,event,present,kind,plane,slot,queue,address,bytes,vkformat,format,guest_format,width,height,level,layer,layers,alias,aliases,draws,skipped,vs,ps,cs,texels,nan,inf,zero,negative,nan_frac,inf_frac,zero_frac,neg_frac,tiles,bad_tiles,edge_tiles,min,max,inputs,storage,raw,ms\n", state.index);
    std::fflush(state.index);
    state.rows = 0;
    state.events = 0;
    state.rawBytes = 0;
    state.readMs = 0;
    state.pass = Pass{};
    FrameTrace::SetNoting(true);
    state.phase.store(Phase::Armed);
    std::fprintf(stderr, "[passdump] armed at present %llu: %s (the next flip starts the frame)\n", static_cast<unsigned long long>(present), state.folder.string().c_str());
}

void AtFlip(VulkanDevice* device, std::uint32_t queueId) {
    auto& state = dump();
    const auto phase = state.phase.load(std::memory_order_relaxed);
    if (phase != Phase::Armed && phase != Phase::Active) return;
    const Serial serial;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    std::lock_guard lock(state.mutex);
    Event event;
    event.kind = "flip";
    event.queue = queueId;
    if (state.phase.load() == Phase::Armed) {
        state.started = Clock::now();
        event.index = state.events++;
        writeRow(state, event, "begin", -1, 0, nullptr, false, Clock::now());
        state.phase.store(Phase::Active);
        std::fprintf(stderr, "[passdump] dumping the frame after present %llu\n", static_cast<unsigned long long>(state.presents.load()));
    } else if (state.phase.load() == Phase::Active) {
        closePass(state, device);
        event.index = state.events++;
        writeRow(state, event, "end", -1, 0, nullptr, false, Clock::now());
        state.phase.store(Phase::Ending);
    }
}

void BeforeDraw(VulkanDevice* device, const QueueState& queue, std::uint32_t queueId) {
    if (!FrameActive()) return;
    auto key = passKey(queue);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    auto& state = dump();
    std::lock_guard lock(state.mutex);
    if (state.phase.load() != Phase::Active) return;
    if (state.pass.open && state.pass.key == key) return;
    closePass(state, device);
    state.pass.open = true;
    state.pass.queue = queueId;
    state.pass.key = std::move(key);
    decodeTargets(queue, state.pass);
}

void AfterDraw(const QueueState& queue, std::uint32_t, bool drawn, const std::string& note) {
    if (!FrameActive()) return;
    auto& state = dump();
    std::lock_guard lock(state.mutex);
    auto& pass = state.pass;
    if (state.phase.load() != Phase::Active || !pass.open) return;
    ++(drawn ? pass.drawn : pass.skipped);
    if (!drawn) return;
    if (pass.vs.size() < 32) pass.vs.insert(programId(graphicsProgram(queue, 0xc8)));
    if (pass.ps.size() < 32) pass.ps.insert(programId(graphicsProgram(queue, 0x008)));
    parseNote(note, pass.inputs, pass.storage, nullptr);
}

void BeforeWork(VulkanDevice* device, std::uint32_t queueId) {
    if (!FrameActive()) return;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    auto& state = dump();
    std::lock_guard lock(state.mutex);
    if (state.phase.load() == Phase::Active && state.pass.open && state.pass.queue == queueId) closePass(state, device);
}

void AfterDispatch(VulkanDevice* device, const QueueState& queue, std::uint32_t queueId, const std::string& note) {
    if (!FrameActive()) return;
    std::set<std::uint64_t> inputs;
    std::vector<Target> written;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> buffers;
    parseNote(note, inputs, written, &buffers);
    Event event;
    event.kind = "dispatch";
    event.queue = queueId;
    event.drawn = 1;
    event.cs = programId((static_cast<std::uint64_t>(reg(queue.shader, 0x20d) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.shader, 0x20c)) << 8u));
    event.inputs = joinedAddresses(inputs);
    event.storage = joinedTargets(written);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    auto& state = dump();
    std::lock_guard lock(state.mutex);
    if (state.phase.load() != Phase::Active) return;
    event.index = state.events++;
    if ((written.empty() && buffers.empty()) || device == nullptr) {
        writeRow(state, event, "none", -1, 0, nullptr, false, Clock::now());
        return;
    }
    device->WaitIdle();
    for (const auto& image : written) readTarget(state, *device, event, "storage", image, ~0u, false);
    // Buffer ranges as F32 (their element format is the shader's business), as stored in guest
    // memory once the device is idle.
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        const auto [address, bytes] = buffers[index];
        const auto began = Clock::now();
        const auto* data = reinterpret_cast<const std::byte*>(address);
        if (address == 0 || bytes < 4 || bytes > (64ull << 20u) || !GuestMemory::Accessible(data, static_cast<std::size_t>(bytes))) {
            writeRow(state, event, "buffer", static_cast<int>(index), address, nullptr, true, began);
            continue;
        }
        Graphics::ImageReadback image;
        image.address = address;
        image.guestBytes = bytes;
        image.format = VK_FORMAT_R32_SFLOAT;
        image.width = image.rowTexels = static_cast<std::uint32_t>(bytes / 4u);
        image.height = 1;
        image.texelBytes = 4;
        image.bytes = std::span<const std::byte>(data, static_cast<std::size_t>(bytes / 4u) * 4u);
        writeRow(state, event, "buffer", static_cast<int>(index), address, &image, false, began);
    }
}

}
