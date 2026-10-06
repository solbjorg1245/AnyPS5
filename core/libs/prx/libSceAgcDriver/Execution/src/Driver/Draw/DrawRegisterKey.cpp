#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace AgcDriver::DriverDetail {

namespace {

// Debug aid: APS5_TRACE_DRAW_KEY_CHURN=1 reports, every 10 s, how often the draw key repeats with
// each DrawKeyRegisters range left out (and with only that range), to find the registers that make
// keys churn ([draw-key-churn]).
void probeKeyChurn(const QueueState& queue) {
    static const bool enabled = std::getenv("APS5_TRACE_DRAW_KEY_CHURN") != nullptr;
    if (!enabled) return;
    constexpr std::size_t Ranges = Graphics::DrawKeyRegisters.size();
    static std::mutex mutex;
    static std::array<std::unordered_set<std::uint64_t>, Ranges + 1> seen;
    static std::array<std::uint64_t, Ranges + 1> repeats{};
    static std::uint64_t total = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    std::array<std::uint64_t, Ranges> rangeHash{};
    for (std::size_t r = 0; r < Ranges; ++r) {
        const auto& range = Graphics::DrawKeyRegisters[r];
        const auto& bank = range.bank == Graphics::RegisterBank::Context ? queue.context : range.bank == Graphics::RegisterBank::Shader ? queue.shader : queue.userConfig;
        std::uint64_t hash = 0xcbf29ce484222325ull;
        const auto end = range.first + range.count;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            hash = (hash ^ it->first) * 0x100000001b3ull;
            hash = (hash ^ it->second) * 0x100000001b3ull;
        }
        rangeHash[r] = hash;
    }
    std::lock_guard lock(mutex);
    ++total;
    for (std::size_t leftOut = 0; leftOut <= Ranges; ++leftOut) {
        std::uint64_t key = 0xcbf29ce484222325ull;
        for (std::size_t r = 0; r < Ranges; ++r) {
            if (r != leftOut) key = (key ^ rangeHash[r]) * 0x100000001b3ull;
        }
        if (seen[leftOut].size() > (1u << 20u)) seen[leftOut].clear();
        if (!seen[leftOut].insert(key).second) ++repeats[leftOut];
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::string text;
    char item[96];
    for (std::size_t r = 0; r < Ranges; ++r) {
        if (repeats[r] <= repeats[Ranges] + total / 50) continue;
        const auto& range = Graphics::DrawKeyRegisters[r];
        std::snprintf(item, sizeof(item), " %s 0x%x+%u: %llu", Graphics::RegisterBankName(range.bank), range.first, range.count, static_cast<unsigned long long>(repeats[r]));
        text += item;
    }
    std::fprintf(stderr, "[draw-key-churn] %llu keys (10 s), %llu repeat whole; repeats with one range left out (only ranges adding > 2%%):%s\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(repeats[Ranges]), text.c_str());
    repeats.fill(0);
    total = 0;
}

}

std::uint64_t Driver::drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial) {
    probeKeyChurn(queue);
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(deviceSerial);
    for (const auto& range : Graphics::DrawKeyRegisters) {
        const auto& bank = range.bank == Graphics::RegisterBank::Context ? queue.context : range.bank == Graphics::RegisterBank::Shader ? queue.shader : queue.userConfig;
        mix((static_cast<std::uint64_t>(range.bank) << 32u) | range.first);
        const auto end = range.first + range.count;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            mix(it->first);
            mix(it->second);
        }
    }
    for (const auto base : {0x008u, 0x088u, 0x0c8u, 0x108u, 0x148u}) {
        const auto low = queue.shader.find(base);
        const auto high = queue.shader.find(base + 1);
        if (low == queue.shader.end() || high == queue.shader.end()) {
            mix(0);
            continue;
        }
        const auto address = (static_cast<std::uint64_t>(low->second) << 8u) | (static_cast<std::uint64_t>(high->second & 0xffu) << 40u);
        auto it = registry.upper_bound(address);
        if (it == registry.begin()) {
            mix(1);
            continue;
        }
        --it;
        mix(reinterpret_cast<std::uintptr_t>(it->second.get()));
        mix(address - it->second->codeAddress);
    }
    return key;
}

bool Driver::sameVertexInfo(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b) {
    if (a.resourcesNum != b.resourcesNum || a.fetchAttribReg != b.fetchAttribReg || a.fetchBufferReg != b.fetchBufferReg || a.fetchEmbedded != b.fetchEmbedded) return false;
    for (std::uint32_t i = 0; i < a.resourcesNum && i < a.resources.size(); ++i) {
        if (a.resources[i].fields != b.resources[i].fields) return false;
        const auto& x = a.resourcesDst[i];
        const auto& y = b.resourcesDst[i];
        if (x.registerStart != y.registerStart || x.registersNum != y.registersNum || x.attrId != y.attrId || x.fetchIndex != y.fetchIndex) return false;
    }
    return true;
}

bool Driver::sameDecode(const DrawDecode& a, const DrawDecode& b) {
    const auto& s = a.state;
    const auto& t = b.state;
    const auto sameColor = [](const Graphics::ColorTarget& x, const Graphics::ColorTarget& y) {
        return x.address == y.address && x.extent.width == y.extent.width && x.extent.height == y.extent.height && x.format == y.format && x.bytes == y.bytes && x.componentMapping == y.componentMapping && x.tileMode == y.tileMode && x.elementBytes == y.elementBytes && x.dccAddress == y.dccAddress && x.dccAlphaOnMsb == y.dccAlphaOnMsb;
    };
    const auto sameBlend = [](const VkPipelineColorBlendAttachmentState& x, const VkPipelineColorBlendAttachmentState& y) {
        return x.blendEnable == y.blendEnable && x.srcColorBlendFactor == y.srcColorBlendFactor && x.dstColorBlendFactor == y.dstColorBlendFactor && x.colorBlendOp == y.colorBlendOp && x.srcAlphaBlendFactor == y.srcAlphaBlendFactor && x.dstAlphaBlendFactor == y.dstAlphaBlendFactor && x.alphaBlendOp == y.alphaBlendOp && x.colorWriteMask == y.colorWriteMask;
    };
    const auto sameMesh = [](const std::optional<ShaderRecompiler::MeshConfiguration>& x, const std::optional<ShaderRecompiler::MeshConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputPrimitive == y->inputPrimitive && x->primitivesPerGroup == y->primitivesPerGroup && x->verticesPerGroup == y->verticesPerGroup && x->maxVertices == y->maxVertices && x->maxPrimitives == y->maxPrimitives && x->threadsPerGroup == y->threadsPerGroup && x->ldsSizeDwords == y->ldsSizeDwords && x->provokingVertex == y->provokingVertex && x->esgsItemSize == y->esgsItemSize;
    };
    const auto sameTess = [](const std::optional<ShaderRecompiler::TessellationConfiguration>& x, const std::optional<ShaderRecompiler::TessellationConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputControlPoints == y->inputControlPoints && x->outputControlPoints == y->outputControlPoints && x->domain == y->domain && x->partitioning == y->partitioning && x->outputTopology == y->outputTopology;
    };
    if (s.stages.path != t.stages.path || s.stages.registerValue != t.stages.registerValue || s.stages.vertexWaveSize != t.stages.vertexWaveSize || s.stages.fragmentWaveSize != t.stages.fragmentWaveSize || !sameMesh(s.stages.mesh, t.stages.mesh) || !sameTess(s.stages.tessellation, t.stages.tessellation)) return false;
    if (!sameColor(s.color, t.color) || s.colors.size() != t.colors.size() || s.blends.size() != t.blends.size()) return false;
    for (std::size_t i = 0; i < s.colors.size(); ++i) {
        if (!sameColor(s.colors[i], t.colors[i])) return false;
    }
    for (std::size_t i = 0; i < s.blends.size(); ++i) {
        if (!sameBlend(s.blends[i], t.blends[i])) return false;
    }
    if (s.hasColorTarget != t.hasColorTarget || s.rectList != t.rectList || s.renderExtent.width != t.renderExtent.width || s.renderExtent.height != t.renderExtent.height || s.topology != t.topology || s.negativeOneToOne != t.negativeOneToOne || s.depthClamp != t.depthClamp || s.cullMode != t.cullMode || s.frontFace != t.frontFace || !sameBlend(s.blend, t.blend) || s.blendConstants != t.blendConstants) return false;
    if (std::memcmp(&s.viewport, &t.viewport, sizeof(VkViewport)) != 0 || std::memcmp(&s.scissor, &t.scissor, sizeof(VkRect2D)) != 0) return false;
    const auto& p = a.pixel;
    const auto& q = b.pixel;
    if (p.interpolatorCount != q.interpolatorCount || p.interpolatorSettings != q.interpolatorSettings || p.wave32 != q.wave32 || p.inputAddr != q.inputAddr || p.hasPerspectiveCenterVgpr != q.hasPerspectiveCenterVgpr || p.perspectiveCentroid != q.perspectiveCentroid || p.posX != q.posX || p.posY != q.posY || p.posZ != q.posZ || p.posW != q.posW || p.frontFace != q.frontFace || p.ancillary != q.ancillary || p.sampleShading != q.sampleShading || p.noPerspective != q.noPerspective || p.linearCentroid != q.linearCentroid || p.pixelKillEnable != q.pixelKillEnable || p.depthExportEnable != q.depthExportEnable || p.sampleMaskExportEnable != q.sampleMaskExportEnable || p.earlyZ != q.earlyZ || p.executeOnNoop != q.executeOnNoop || p.targetOutputMode != q.targetOutputMode || p.targetExportMapping != q.targetExportMapping) return false;
    if (a.roles != b.roles || a.programs.size() != b.programs.size()) return false;
    for (std::size_t i = 0; i < a.programs.size(); ++i) {
        const auto& x = a.programs[i];
        const auto& y = b.programs[i];
        if (x.binary.stage != y.binary.stage || x.binary.codeAddress != y.binary.codeAddress || x.userDataBase != y.userDataBase || x.firstUserSgpr != y.firstUserSgpr || x.userData != y.userData || x.snapshot != y.snapshot || x.codeOffset != y.codeOffset) return false;
    }
    return true;
}

}
