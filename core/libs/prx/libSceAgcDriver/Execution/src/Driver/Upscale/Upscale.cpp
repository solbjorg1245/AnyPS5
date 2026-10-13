#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Upscale/Upscale.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

// The [upscale] rows (docs/design/dlss.md S1), all queues': "bilinear on" at the first pass, one
// "skipped" line per reason, and every 10 s the passes and skips, with the GPU time per pass under
// APS5_PROFILE_GPU (the pass's ranges since [gputime] last reported).
void noteUpscale(const MfsrPacket::Dispatch& dispatch, std::optional<Upscale::Skip> skip, const std::string& detail) {
    static HostMutex mutex;
    static std::chrono::steady_clock::time_point start;
    static std::uint64_t passes = 0, skipped = 0, total = 0;
    static std::uint32_t reasons = 0;
    const auto output = MfsrPacket::DecodeImageSize(dispatch.images[0]);
    const auto color = MfsrPacket::DecodeImageSize(dispatch.images[1]);
    const auto render = dispatch.Field<std::array<std::uint32_t, 2>>(MfsrPacket::RenderSizeOffset);
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (passes + skipped == 0) start = now;
    if (skip) {
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
    char gpu[32] = "n/a";
    if (Graphics::Recorder::GpuTimingEnabled()) {
        const auto totals = Graphics::Recorder::GpuTimingTotals();
        const auto found = totals.Programs().find(Upscale::TimingKey);
        if (found != totals.Programs().end() && found->second.count != 0) std::snprintf(gpu, sizeof(gpu), "%.3f", found->second.ms / static_cast<double>(found->second.count));
    }
    std::fprintf(stderr, "[upscale] bilinear %llu passes, %llu skipped, %s ms GPU per pass\n", static_cast<unsigned long long>(passes), static_cast<unsigned long long>(skipped), gpu);
    passes = skipped = 0;
}

}

void Driver::upscale(const MfsrPacket::Dispatch& dispatch, const Submission& submission) {
    const auto planned = Upscale::PlanBilinear(dispatch);
    std::optional<Upscale::Skip> skip;
    std::string detail;
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
            skip = localDevice->UpscaleBilinear(std::get<Upscale::Plan>(planned));
            if (!skip) Graphics::Recorder::CountRecordedWork();
        } catch (const std::exception& error) {
            skip = Upscale::Skip::Failed;
            detail = error.what();
        }
    }
    noteUpscale(dispatch, skip, detail);
}

}
