#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Upscale.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libc/include/PortSettings.hpp"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

// APS5_UPSCALER_PORT's part of a pass: tried, with the plan's reset, or why it was skipped.
struct PortNote {
    bool tried = false, reset = false;
    std::optional<Upscale::Skip> skip;
    std::string detail;
};

// The GPU ms per pass of a [gputime] program key since [gputime] last reported, "n/a" without
// APS5_PROFILE_GPU.
std::string gpuPerPass(std::uint64_t key) {
    char gpu[32] = "n/a";
    if (Graphics::Recorder::GpuTimingEnabled()) {
        const auto totals = Graphics::Recorder::GpuTimingTotals();
        const auto found = totals.Programs().find(key);
        if (found != totals.Programs().end() && found->second.count != 0) std::snprintf(gpu, sizeof(gpu), "%.3f", found->second.ms / static_cast<double>(found->second.count));
    }
    return gpu;
}

// The [upscale] rows (docs/design/dlss.md S1), all queues': "bilinear on" at the first pass, one
// "skipped" line per reason, and every 10 s the passes and skips, with the GPU time per pass under
// APS5_PROFILE_GPU (the pass's ranges since [gputime] last reported). Under APS5_UPSCALER_PORT (S2):
// "port on" at the first temporal pass, one "port skipped" line per reason, and every 10 s the
// temporal passes, resets and fallbacks first (the bilinear row then only counts the fallbacks).
void noteUpscale(const MfsrPacket::Dispatch& dispatch, std::optional<Upscale::Skip> skip, const std::string& detail, const PortNote& port) {
    static HostMutex mutex;
    static std::chrono::steady_clock::time_point start;
    static std::uint64_t passes = 0, skipped = 0, total = 0, portPasses = 0, portResets = 0, portSkipped = 0, portTotal = 0;
    static std::uint32_t reasons = 0, portReasons = 0;
    const auto output = MfsrPacket::DecodeImageSize(dispatch.images[0]);
    const auto color = MfsrPacket::DecodeImageSize(dispatch.images[1]);
    const auto render = dispatch.Field<std::array<std::uint32_t, 2>>(MfsrPacket::RenderSizeOffset);
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (passes + skipped + portPasses + portSkipped == 0) start = now;
    if (port.skip) {
        ++portSkipped;
        const auto bit = 1u << static_cast<unsigned>(*port.skip);
        if ((portReasons & bit) == 0) {
            portReasons |= bit;
            std::fprintf(stderr, "[upscale] port skipped (bilinear instead): %s (pointers 0x%x, render %ux%u)%s%s\n", Upscale::SkipName(*port.skip), dispatch.present, render[0], render[1],
                port.detail.empty() ? "" : ": ", port.detail.c_str());
        }
    }
    if (port.tried && !port.skip && !skip) {
        ++portPasses;
        if (port.reset) ++portResets;
        if (portTotal++ == 0) {
            const auto& signs = Upscale::PortSigns();
            std::fprintf(stderr, "[upscale] port on: output %ux%u, render %ux%u, signs %s\n", output.width, output.height, render[0], render[1], signs.letters[0] != '\0' ? signs.letters.data() : "default");
        }
    } else if (skip) {
        ++skipped;
        const auto bit = 1u << static_cast<unsigned>(*skip);
        if ((reasons & bit) == 0) {
            reasons |= bit;
            std::fprintf(stderr, "[upscale] skipped: %s (pointers 0x%x, output %ux%u format %u, MainColor %ux%u format %u, render %ux%u)%s%s\n", Upscale::SkipName(*skip), dispatch.present,
                output.width, output.height, output.format, color.width, color.height, color.format, render[0], render[1], detail.empty() ? "" : ": ", detail.c_str());
        }
    } else {
        ++passes;
        if (total++ == 0) std::fprintf(stderr, "[upscale] bilinear on: output %ux%u format %u, MainColor %ux%u format %u, render %ux%u\n", output.width, output.height, output.format,
            color.width, color.height, color.format, render[0], render[1]);
    }
    if (now - start < std::chrono::seconds(10)) return;
    static const bool portOn = Upscale::PortEnabled();
    if (portOn) {
        std::fprintf(stderr, "[upscale] port %llu passes, %llu resets, %llu skipped (fell back to bilinear), %s ms GPU per pass\n", static_cast<unsigned long long>(portPasses), static_cast<unsigned long long>(portResets),
            static_cast<unsigned long long>(portSkipped), gpuPerPass(Upscale::PortTimingKey).c_str());
    }
    if (!portOn || passes + skipped != 0) {
        std::fprintf(stderr, "[upscale] bilinear %llu passes, %llu skipped, %s ms GPU per pass\n", static_cast<unsigned long long>(passes), static_cast<unsigned long long>(skipped), gpuPerPass(Upscale::TimingKey).c_str());
    }
    passes = skipped = portPasses = portResets = portSkipped = 0;
}

}

void Driver::upscale(const MfsrPacket::Dispatch& dispatch, const Submission& submission) {
    static const bool port = Upscale::PortEnabled();
    // What the history images hold (PlanTemporal's previous), under the GPU mutex.
    static Upscale::History history;
    const auto planned = Upscale::PlanBilinear(dispatch);
    std::optional<Upscale::Skip> skip;
    std::string detail;
    PortNote portNote;
    if (const auto* reason = std::get_if<Upscale::Skip>(&planned)) {
        skip = *reason;
    } else {
        try {
            if (PipeCensusActive() && submission.queue == 0) NotePipeCensusLock(PipeCensusLock::Other);
            // The pass records here, under this thread's lock: the recording pipe's items first (the
            // NOP drains nothing at its boundary).
            if (recordPipeDrawing(submission.queue)) drainRecordPipe(PipeCensusDrain::Other);
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            if (device == nullptr) device = std::make_shared<VulkanDevice>();
            const std::shared_ptr<VulkanDevice> localDevice = device;
            recordLabelsForPacket(localDevice.get(), submission.queue);
            if (port) {
                // The temporal pass, or the bilinear one for this frame (and a reset at the next).
                portNote.tried = true;
                const auto temporal = Upscale::PlanTemporal(dispatch, history, Upscale::PortSigns());
                if (const auto* reason = std::get_if<Upscale::Skip>(&temporal)) {
                    portNote.skip = *reason;
                } else {
                    portNote.reset = std::get<Upscale::TemporalPlan>(temporal).reset;
                    try {
                        portNote.skip = localDevice->UpscaleTemporal(std::get<Upscale::TemporalPlan>(temporal));
                    } catch (const std::exception& error) {
                        portNote.skip = Upscale::Skip::Failed;
                        portNote.detail = error.what();
                    }
                }
                history = portNote.skip ? Upscale::History{} : std::get<Upscale::TemporalPlan>(temporal).next;
            }
            if (!port || portNote.skip) skip = localDevice->UpscaleBilinear(std::get<Upscale::Plan>(planned));
            if (!skip) Graphics::Recorder::CountRecordedWork();
        } catch (const std::exception& error) {
            skip = Upscale::Skip::Failed;
            detail = error.what();
        }
    }
    noteUpscale(dispatch, skip, detail, portNote);
}

}

namespace AgcDriver::Upscale {

bool Enabled() {
    static const bool enabled = GetUpscalerChain_nid_no_patch().exec;
    return enabled;
}

bool PortEnabled() {
    static const bool enabled = GetUpscalerChain_nid_no_patch().port;
    return enabled;
}

}
