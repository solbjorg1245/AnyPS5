#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool Driver::drawPrecheck() {
    static const bool precheck = std::getenv("APS5_NO_DRAW_PRECHECK") == nullptr;
    return precheck;
}

std::optional<DrawVerdict> Driver::precheckDraw(const QueueState& queue, const Submission& submission, std::span<const std::uint32_t> packet, const Pm4::DrawParameters& drawParameters, std::string& rejected, bool& traceIndirect) {
    if (!drawParameters.indirect && !drawParameters.indexed && (drawParameters.indexCount == 0 || drawParameters.instanceCount == 0)) return DrawVerdict::Nothing;
    if (const auto pass = Graphics::DecodeColorMetadataPass(queue)) {
        require(!drawParameters.indirect, "indirect CB metadata passes are unsupported");
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        const std::shared_ptr<VulkanDevice> localDevice = device;
        recordLabelsForPacket(localDevice.get(), submission.queue);
        localDevice->ColorMetadataPass(*pass);
        NoteFastCensusMetadataPass();
        return DrawVerdict::Drawn;
    }
    static const bool traceIndirectEnabled = std::getenv("APS5_TRACE_INDIRECT_DRAWS") != nullptr;
    traceIndirect = traceIndirectEnabled;
    if (traceIndirect && drawParameters.indirect) std::fprintf(stderr, "[draw] indirect packet %s args 0x%llx count %u reached\n", Pm4::Name(packet[0]).c_str(), static_cast<unsigned long long>(drawParameters.indirect->arguments), drawParameters.indirect->count);
    {
        const bool colorWrites = Graphics::WritesColor(queue.context);
        const auto word = [&](std::uint32_t offset) { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        // A RESUMMARIZE or DECOMPRESS pass (DB_RENDER_CONTROL bits 4 and 12), or an in-place
        // decompress (the compress-disable bits 5 and 6 with neither test on), without clears,
        // copies or color writes only rebuilds or expands the HTILE metadata of a depth surface.
        // Depth surfaces are uncompressed host images here, so it changes nothing.
        const auto renderControl = word(0x000);
        const bool metadataPass = (renderControl & 0x1010u) != 0 || ((renderControl & 0x60u) != 0 && (word(0x200) & 3u) == 0);
        if (metadataPass && (renderControl & 0xfu) == 0 && !colorWrites) return DrawVerdict::Nothing;
        // Without a pixel shader a draw only tests and writes depth/stencil (shadow maps, depth
        // prepasses): it runs with no fragment stage (decodeDraw), or does nothing when neither
        // test is on or no depth/stencil plane is bound.
        if (!colorWrites && !queue.shader.contains(0x8)) {
            if ((word(0x200) & 3u) == 0 || ((word(0x010) & 3u) == 0 && (word(0x011) & 1u) == 0)) return DrawVerdict::Nothing;
        }
    }
    if (drawParameters.indexed) {
        const auto restart = queue.userConfig.find(0x24b);
        if (restart != queue.userConfig.end() && restart->second != 0) {
            const auto resetIndex = queue.context.find(0x103);
            const auto primitive = queue.userConfig.find(0x242);
            const std::uint32_t allOnes = drawParameters.indexSize == 2 ? 0xffffu : 0xffffffffu;
            const auto type = primitive == queue.userConfig.end() ? 0u : primitive->second & 0x3fu;
            const bool strip = type == 3 || type == 5 || type == 6;
            const bool list = type == 1 || type == 2 || type == 4;
            const auto restartDevice = device.Load();
            if (!strip && !(list && restartDevice != nullptr && restartDevice->PrimitiveListRestart())) {
                rejected = "AGC graphics: primitive restart is only supported for strips, and for lists with VK_EXT_primitive_topology_list_restart";
                return DrawVerdict::Rejected;
            }
            if (resetIndex == queue.context.end() || (resetIndex->second & allOnes) != allOnes) {
                rejected = "AGC graphics: primitive restart index other than all ones is unsupported";
                return DrawVerdict::Rejected;
            }
        }
    }
    if (drawPrecheck()) {
        rejected = Graphics::DrawRejection(queue, drawParameters.indexed);
        if (!rejected.empty()) return DrawVerdict::Rejected;
    }
    return std::nullopt;
}

}
