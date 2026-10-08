#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

// Debug aid: APS5_TRACE_DRAW_KEY_CHURN=1 reports, every 10 s, how often the draw key repeats with
// each DrawKeyRegisters range left out (and with only that range), to find the registers that make
// keys churn ([draw-key-churn]).
void probeKeyChurn(const QueueState& queue) {
    static const bool enabled = std::getenv("APS5_TRACE_DRAW_KEY_CHURN") != nullptr;
    if (!enabled) return;
    constexpr std::size_t Ranges = Graphics::DrawKeyRegisters.size();
    static HostMutex mutex;
    // Variants: each range left out, none left out (Ranges), every shader user-word range left
    // out (Ranges + 1), those plus the CB_COLOR bases (Ranges + 2).
    static std::array<std::unordered_set<std::uint64_t>, Ranges + 3> seen;
    static std::array<std::uint64_t, Ranges + 3> repeats{};
    const auto userWords = [](const Graphics::DrawKeyRange& range) { return range.bank == Graphics::RegisterBank::Shader && range.count >= 0x21; };
    const auto colorBases = [](const Graphics::DrawKeyRange& range) { return range.bank == Graphics::RegisterBank::Context && range.first == 0x318; };
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
    bool wholeNew = false;
    for (std::size_t leftOut = 0; leftOut <= Ranges + 2; ++leftOut) {
        std::uint64_t key = 0xcbf29ce484222325ull;
        for (std::size_t r = 0; r < Ranges; ++r) {
            const auto& range = Graphics::DrawKeyRegisters[r];
            const bool out = leftOut < Ranges ? r == leftOut : leftOut == Ranges ? false : userWords(range) || (leftOut == Ranges + 2 && colorBases(range));
            if (!out) key = (key ^ rangeHash[r]) * 0x100000001b3ull;
        }
        if (seen[leftOut].size() > (1u << 20u)) seen[leftOut].clear();
        if (!seen[leftOut].insert(key).second) ++repeats[leftOut];
        else if (leftOut == Ranges) wholeNew = true;
    }
    // Which user words make a key new: for a new key whose base (every range but the user words)
    // was seen, the user words that differ from the last ones seen under that base.
    static std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> lastUserWords;
    static std::map<std::uint32_t, std::uint64_t> differingByOffset;
    static std::uint64_t newKeys = 0, newWithBaseSeen = 0;
    std::uint64_t baseKey = 0xcbf29ce484222325ull;
    for (std::size_t r = 0; r < Ranges; ++r) {
        if (!userWords(Graphics::DrawKeyRegisters[r])) baseKey = (baseKey ^ rangeHash[r]) * 0x100000001b3ull;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
    for (const auto& range : Graphics::DrawKeyRegisters) {
        if (!userWords(range)) continue;
        const auto end = range.first + range.count;
        for (auto it = queue.shader.lower_bound(range.first); it != queue.shader.end() && it->first < end; ++it) words.emplace_back(it->first, it->second);
    }
    if (wholeNew) {
        ++newKeys;
        if (const auto last = lastUserWords.find(baseKey); last != lastUserWords.end()) {
            ++newWithBaseSeen;
            const auto& old = last->second;
            std::size_t a = 0, b = 0;
            while (a < old.size() || b < words.size()) {
                if (b >= words.size() || (a < old.size() && old[a].first < words[b].first)) { ++differingByOffset[old[a].first]; ++a; }
                else if (a >= old.size() || words[b].first < old[a].first) { ++differingByOffset[words[b].first]; ++b; }
                else { if (old[a].second != words[b].second) ++differingByOffset[old[a].first]; ++a; ++b; }
            }
        }
    }
    if (lastUserWords.size() > (1u << 18u)) lastUserWords.clear();
    lastUserWords[baseKey] = std::move(words);
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
    std::vector<std::pair<std::uint64_t, std::uint32_t>> offsets;
    for (const auto& [offset, count] : differingByOffset) offsets.emplace_back(count, offset);
    std::sort(offsets.rbegin(), offsets.rend());
    std::string userText;
    for (std::size_t i = 0; i < offsets.size() && i < 16; ++i) {
        std::snprintf(item, sizeof(item), " 0x%x: %llu", offsets[i].second, static_cast<unsigned long long>(offsets[i].first));
        userText += item;
    }
    std::fprintf(stderr, "[draw-key-churn] %llu keys (10 s), %llu repeat whole; all user-word ranges left out %llu, those + CB_COLOR bases %llu; repeats with one range left out (only ranges adding > 2%%):%s; new keys %llu, of them %llu with the base (no user words) seen before: differing shader user words by offset (top 16):%s\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(repeats[Ranges]), static_cast<unsigned long long>(repeats[Ranges + 1]), static_cast<unsigned long long>(repeats[Ranges + 2]), text.c_str(), static_cast<unsigned long long>(newKeys), static_cast<unsigned long long>(newWithBaseSeen), userText.c_str());
    repeats.fill(0);
    total = 0;
    newKeys = newWithBaseSeen = 0;
    differingByOffset.clear();
}

}

DrawKey Driver::drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial) {
    probeKeyChurn(queue);
    DrawKey result;
    std::uint64_t key = 0xcbf29ce484222325ull;
    std::uint64_t base = key;
    const auto mix = [&](std::uint64_t value) {
        key = (key ^ value) * 0x100000001b3ull;
        base = (base ^ value) * 0x100000001b3ull;
    };
    // The pointer registers (DrawPointerRegisters) go into the full key alone; their words ride
    // along for the relocation of a new key.
    static_assert(DrawPointerRegisters == std::array<std::uint32_t, 8>{0x08c, 0x08d, 0x090, 0x091, 0x094, 0x095, 0x00c, 0x00d});
    const auto pointerWord = [](std::uint32_t offset) -> int {
        switch (offset) {
        case 0x08c: return 0;
        case 0x08d: return 1;
        case 0x090: return 2;
        case 0x091: return 3;
        case 0x094: return 4;
        case 0x095: return 5;
        case 0x00c: return 6;
        case 0x00d: return 7;
        default: return -1;
        }
    };
    mix(deviceSerial);
    for (const auto& range : Graphics::DrawKeyRegisters) {
        const auto& bank = range.bank == Graphics::RegisterBank::Context ? queue.context : range.bank == Graphics::RegisterBank::Shader ? queue.shader : queue.userConfig;
        mix((static_cast<std::uint64_t>(range.bank) << 32u) | range.first);
        const auto end = range.first + range.count;
        const bool shader = range.bank == Graphics::RegisterBank::Shader;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            const int pointer = shader ? pointerWord(it->first) : -1;
            if (pointer >= 0) {
                key = (key ^ it->first) * 0x100000001b3ull;
                key = (key ^ it->second) * 0x100000001b3ull;
                result.words[static_cast<std::size_t>(pointer)] = it->second;
                result.present |= 1u << static_cast<unsigned>(pointer);
                continue;
            }
            mix(it->first);
            mix(it->second);
        }
    }
    for (std::size_t program = 0; program < DrawProgramRegisters.size(); ++program) {
        const auto programBase = DrawProgramRegisters[program];
        const auto low = queue.shader.find(programBase);
        const auto high = queue.shader.find(programBase + 1);
        if (low == queue.shader.end() || high == queue.shader.end()) {
            mix(0);
            continue;
        }
        const auto address = (static_cast<std::uint64_t>(low->second) << 8u) | (static_cast<std::uint64_t>(high->second & 0xffu) << 40u);
        result.programs[program] = address;
        auto it = registry.upper_bound(address);
        if (it == registry.begin()) {
            mix(1);
            continue;
        }
        --it;
        mix(reinterpret_cast<std::uintptr_t>(it->second.get()));
        mix(address - it->second->codeAddress);
    }
    result.key = key;
    result.base = base;
    return result;
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
