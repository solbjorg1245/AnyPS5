#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/FrameTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <shared_mutex>

namespace AgcDriver::DriverDetail {

namespace {

// The FrameTrace line of a draw: packet, verdict, programs, the written color targets and the depth
// target (`targets` receives their addresses for a dump).
std::string describeDraw(const QueueState& queue, std::uint32_t header, std::uint32_t queueId, const char* verdict, const std::string& reason, std::vector<std::uint64_t>& targets) {
    const auto reg = [](const auto& bank, std::uint32_t offset) {
        const auto it = bank.find(offset);
        return it == bank.end() ? 0u : it->second;
    };
    const auto program = [&](std::uint32_t low) { return (static_cast<std::uint64_t>(reg(queue.shader, low + 1) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.shader, low)) << 8u); };
    char text[1024];
    int length = std::snprintf(text, sizeof(text), "draw q%x %s %s vs=0x%llx ps=0x%llx", queueId, Pm4::Name(header).c_str(), verdict, static_cast<unsigned long long>(program(0xc8)), static_cast<unsigned long long>(program(0x008)));
    const auto mask = reg(queue.context, 0x8e) & reg(queue.context, 0x8f);
    for (std::uint32_t slot = 0; slot < 8 && length < static_cast<int>(sizeof(text)) - 96; ++slot) {
        const auto info = reg(queue.context, 0x31c + slot * 0xfu);
        if (((mask >> (4u * slot)) & 0xfu) == 0 || ((info >> 2u) & 0x1fu) == 0) continue;
        const auto address = (static_cast<std::uint64_t>(reg(queue.context, 0x390 + slot) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x318 + slot * 0xfu)) << 8u);
        targets.push_back(address);
        length += std::snprintf(text + length, sizeof(text) - length, " rt%u=0x%llx/%08x/v%x/%x", slot, static_cast<unsigned long long>(address), info, reg(queue.context, 0x31b + slot * 0xfu), (mask >> (4u * slot)) & 0xfu);
    }
    const auto zInfo = reg(queue.context, 0x010);
    const auto stencilInfo = reg(queue.context, 0x011);
    if ((zInfo & 3u) != 0 || (stencilInfo & 1u) != 0) {
        const auto zAddress = (static_cast<std::uint64_t>(reg(queue.context, 0x01a) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x012)) << 8u);
        const auto stencilAddress = (static_cast<std::uint64_t>(reg(queue.context, 0x01b) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x013)) << 8u);
        if ((zInfo & 3u) != 0) targets.push_back(zAddress);
        if ((stencilInfo & 1u) != 0) targets.push_back(stencilAddress);
        const auto htile = (static_cast<std::uint64_t>(reg(queue.context, 0x01e) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(queue.context, 0x005)) << 8u);
        float clearDepth = 0;
        const auto clearWord = reg(queue.context, 0x00b);
        std::memcpy(&clearDepth, &clearWord, sizeof(clearDepth));
        length += std::snprintf(text + length, sizeof(text) - length, " z=0x%llx/%08x s=0x%llx/%08x view=%x htile=0x%llx clear=%g/%u", static_cast<unsigned long long>(zAddress), zInfo, static_cast<unsigned long long>(stencilAddress), stencilInfo, reg(queue.context, 0x002), static_cast<unsigned long long>(htile), clearDepth, reg(queue.context, 0x00a) & 0xffu);
    }
    if (length < static_cast<int>(sizeof(text)) - 64) length += std::snprintf(text + length, sizeof(text) - length, " dc=%08x rc=%x sc=%x", reg(queue.context, 0x200), reg(queue.context, 0x000), reg(queue.context, 0x203));
    std::string line(text);
    if (!reason.empty()) line += " | " + reason.substr(0, 160);
    return line;
}

// The FrameTrace line of a dispatch: packet, compute program and grid (an indirect grid is read by the
// GPU, the packet words are its arguments' address).
std::string describeDispatch(const QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t queueId) {
    const auto reg = [&](std::uint32_t offset) {
        const auto it = queue.shader.find(offset);
        return it == queue.shader.end() ? 0u : it->second;
    };
    const auto program = (static_cast<std::uint64_t>(reg(0x20d) & 0xffu) << 40u) | (static_cast<std::uint64_t>(reg(0x20c)) << 8u);
    char text[384];
    int length = std::snprintf(text, sizeof(text), "dispatch q%x %s cs=0x%llx args=%x,%x,%x ud=", queueId, Pm4::Name(packet[0]).c_str(), static_cast<unsigned long long>(program), packet.size() > 1 ? packet[1] : 0u, packet.size() > 2 ? packet[2] : 0u, packet.size() > 3 ? packet[3] : 0u);
    // COMPUTE_USER_DATA_0..15
    for (std::uint32_t index = 0; index < 16 && length < static_cast<int>(sizeof(text)) - 10; ++index) length += std::snprintf(text + length, sizeof(text) - length, "%s%x", index == 0 ? "" : ",", reg(0x240 + index));
    return text;
}

}

template <typename TWork>
void Driver::timed(double WorkerProfile::*bucket, TWork&& work) {
    static thread_local WorkerProfile profile;
    const auto begin = std::chrono::steady_clock::now();
    work();
    const auto end = std::chrono::steady_clock::now();
    profile.*bucket += std::chrono::duration<double, std::milli>(end - begin).count();
    static const bool report = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (report && end - profile.reported > std::chrono::seconds(10)) {
        profile.reported = end;
        std::fprintf(stderr, "[gpu] worker at %.0f s: dispatch %.1f s, draw %.1f s, wait %.1f s\n", std::chrono::duration<double>(end - profile.start).count(), profile.dispatchMs / 1000, profile.drawMs / 1000, profile.waitMs / 1000);
    }
}

// Logs a dispatch of a traced frame (FrameTrace); a dump after it saves every live image.
void Driver::traceDispatch(const QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t queueId) {
    if (!FrameTrace::Active()) return;
    const auto line = describeDispatch(queue, packet, queueId) + FrameTrace::TakeWrites();
    const auto entry = FrameTrace::Record(line);
    if (!entry.dump) return;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (const auto localDevice = device.Load()) localDevice->CaptureImages(entry.prefix, {});
    // The buffer ranges the dispatch writes (its " wb=<address>+<bytes>" entries), as stored in guest
    // memory once the device is idle.
    for (auto at = line.find(" wb=0x"); at != std::string::npos; at = line.find(" wb=0x", at + 1)) {
        char* end = nullptr;
        const auto address = std::strtoull(line.c_str() + at + 6, &end, 16);
        const auto bytes = end != nullptr && end[0] == '+' ? std::strtoull(end + 1, nullptr, 16) : 0ull;
        if (address == 0 || bytes == 0 || bytes > (64ull << 20u) || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "wb_%llx.bin", static_cast<unsigned long long>(address));
        if (std::FILE* file = std::fopen((entry.prefix + name).c_str(), "wb")) {
            std::fwrite(reinterpret_cast<const void*>(address), 1, static_cast<std::size_t>(bytes), file);
            std::fclose(file);
        }
    }
}

template <typename TWork>
void Driver::tolerate(const char* kind, TWork&& work) {
    try {
        work();
    } catch (const std::exception& error) {
        reportSkip(kind, error.what());
    }
}

void Driver::execute(const Submission& submission) {
    if (submission.suspend) {

        static const bool suspendDrain = std::getenv("APS5_SUSPEND_DRAIN") != nullptr;
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        ++suspendPoints;
        auto& costs = submissionCosts(submission.queue);

        if (suspendDrain || !deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
            const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            if (const auto localDevice = device.Load()) {
                recordDeferredLabels(localDevice.get(), submission.queue);
                if (suspendDrain) localDevice->WaitIdle();
                else localDevice->SubmitRecorded(submission.queue == 0);
            }
            ++costs.suspends;
            if (profile) costs.suspendNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        } else {
            ++costs.suspendsSkipped;
        }
        resetGraphics = true;
        return;
    }
    QueueState* state = nullptr;
    {
        std::lock_guard lock(mutex);
        if (submission.queue == 0 && resetGraphics) {
            queues.erase(0);
            resetGraphics = false;
        }
        state = &queues[submission.queue];
    }
    auto& queue = *state;
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f execute serial=%llu queue=0x%x dwords=%zu\n", TraceMs(), static_cast<unsigned long long>(submission.serial), submission.queue, submission.commands.size());

    static const long dumpQueue = [] { const char* text = std::getenv("APS5_DUMP_QUEUE"); return text ? std::strtol(text, nullptr, 16) : -1L; }();
    if (static_cast<long>(submission.queue) == dumpQueue) {

        static int dumped = 0;
        if (dumped++ < 40) {
            std::string text = "[queue] submission " + std::to_string(submission.serial) + ":\n";
            for (std::size_t cursor = 0; cursor < submission.commands.size();) {
                const auto header = submission.commands[cursor];
                const auto count = Pm4::PacketWords(header);
                char line[200];
                int length = std::snprintf(line, sizeof(line), "[queue]   %s", Pm4::Name(header).c_str());
                for (std::size_t i = 1; i < count && i < 10 && length < 180; ++i) length += std::snprintf(line + length, sizeof(line) - length, " %08x", submission.commands[cursor + i]);
                text += line;
                text += "\n";
                cursor += count;
            }
            std::fputs(text.c_str(), stderr);
        }
    }
    PacketHistory recent{submission.commands};

    static const bool profilePackets = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    thread_local PacketProfile packetProfile;
    ++packetProfile.submissions;

    bumpEpoch(&EpochBumps::submissions);
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        if (packetEpoch()) bumpEpoch(&EpochBumps::packets);
        CheckFailure();
        const auto header = submission.commands[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        const auto count = Pm4::PacketWords(header);
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        std::shared_lock deviceUse(deviceReplacement, std::defer_lock);
        if (opcode != 0x3c && opcode != 0x93 && header != RenderingWaitPacketHeader && header != FlipPacketHeader) deviceUse.lock();

        GuestMemory::SetCurrentPacket(header == FlipPacketHeader ? 0xffffu : opcode, submission.queue);
        CaptureTrace::Log("packet submission=%llu queue=%x offset=%zu header=%08x words=%zu", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, header, packet.size());

        const auto flushStart = profilePackets ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        flushBetweenPackets(submission.queue, header, opcode == 0x49 || opcode == 0x37);
        PacketTimer packetTimer{profilePackets, header == FlipPacketHeader ? 0xffffu : opcode, submission.queue, packetProfile, std::chrono::steady_clock::now()};

        if (profilePackets) {
            packetStartedAt() = packetTimer.start;
            pendingDispatchPhases() = {};
            pendingDrawPhases() = {};
        }
        const auto finishDispatchPacket = [&](bool indirect) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDispatchPhases();
            if (pending.phases) {
                pending.ms[PhaseEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
                addDriverPhases(submission.queue != 0 ? OtherQueues : indirect ? Queue0Indirect : Queue0Direct, pending.ms, pending.hit, pending.validated);
            }
            if (indirect) return;
            auto& row = packetProfile.dispatchOutcomes[static_cast<std::size_t>(pending.outcome)];
            ++row.first;
            row.second += std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
        };
        const auto finishDrawPacket = [&](bool drawn) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDrawPhases();
            std::array<double, DrawDriverPhaseCount> ms{};
            if (drawn && pending.phases) {
                ms = pending.ms;
                ms[DrawRowEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
            } else {
                ms[DrawRowSkipped] = std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
            }
            addDrawPhases(ms, drawn && pending.phases, pending.captures);
        };
        if (profilePackets) packetProfile.flushMs += std::chrono::duration<double, std::milli>(packetTimer.start - flushStart).count();

        bool wroteOnGpu = false, endOfPipeInterrupt = false, interruptDeferred = false, drawPacket = false, sampleDump = false;
        const bool drains = preparePacketMemory(submission, queue, packet, header, opcode, wroteOnGpu, endOfPipeInterrupt, interruptDeferred, drawPacket, sampleDump);
        traceLabel(packet, submission.queue);

        const bool waitPacket = opcode == 0x3c || opcode == 0x93 || header == RenderingWaitPacketHeader;
        struct Progress {
            Driver& driver;
            bool counted;
            ~Progress() {
                if (!counted) return;
                --driver.packetsInFlight;
                ++driver.packetsDone;
            }
        } progress{*this, !waitPacket};
        if (!waitPacket) ++packetsInFlight;
        recent.Record(cursor);
        // DMA_DATA and WRITE_DATA store to memory outside draws and dispatches (depth/HTILE clears).
        if (FrameTrace::Active() && (opcode == 0x50 || opcode == 0x37)) {
            char text[160];
            int length = std::snprintf(text, sizeof(text), "packet q%x %s", submission.queue, Pm4::Name(header).c_str());
            for (std::size_t word = 1; word < packet.size() && word < 8 && length < static_cast<int>(sizeof(text)) - 10; ++word) length += std::snprintf(text + length, sizeof(text) - length, " %08x", packet[word]);
            FrameTrace::Record(text);
        }
        if (header == RenderingWaitPacketHeader) {
            timed(&WorkerProfile::waitMs, [&] { submission.renderingWaits.at(cursor)->Wait(); });
        } else if (header == FlipPacketHeader) {
            CheckFailure();
            std::uint64_t batchesAtFlip = 0, unsignaledAtFlip = 0;
            if (!drains) {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                if (!recordLabelsForPacket(localDevice.get(), submission.queue) && localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
                if (localDevice != nullptr) localDevice->FlipBatches(batchesAtFlip, unsignaledAtFlip);
            }
            ++flipsCounted;
            if (batchesAtFlip != 0) flipSerial = batchesAtFlip;
            flipBatchesUnsignaled += unsignaledAtFlip;

            auto frame = std::make_shared<FrameTiming>(++frameSerial);
            const auto now = FrameTiming::Clock::now();
            frame->IncludeSubmission(submission.serial, now, now, now, true);
            frame->SetFlip(submission.serial, cursor, now, now);
            frame->NoteFlipBatches(batchesAtFlip, unsignaledAtFlip);
            CaptureTrace::Log("flip frame=%llu submission=%llu offset=%zu batch=%llu unsignaled=%llu", static_cast<unsigned long long>(frameSerial), static_cast<unsigned long long>(submission.serial), cursor, static_cast<unsigned long long>(batchesAtFlip), static_cast<unsigned long long>(unsignaledAtFlip));
            submission.flips.at(cursor)->GpuReady(frame);
        } else if (opcode == 0x15) {
            // A dispatch the driver cannot run is skipped and reported, as a draw is, instead of
            // ending the process: Demon's Souls issues compute work whose resource tables are not
            // filled yet (a null SRT, a null texture), which the GPU tolerates.
            timed(&WorkerProfile::dispatchMs, [&] { tolerate("dispatch", [&] { dispatch(queue, packet, submission); }); });
            traceDispatch(queue, packet, submission.queue);
            Graphics::Recorder::CountRecordedWork();
            finishDispatchPacket(false);
        } else if (opcode == 0x16) {
            timed(&WorkerProfile::dispatchMs, [&] { tolerate("dispatch", [&] { dispatchIndirect(queue, packet, submission); }); });
            traceDispatch(queue, packet, submission.queue);
            Graphics::Recorder::CountRecordedWork();
            finishDispatchPacket(true);
        } else if (opcode == 0x3c || opcode == 0x93) {
            static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
            const auto waitStart = std::chrono::steady_clock::now();
            timed(&WorkerProfile::waitMs, [&] { waitMemory(packet, submission.queue, recent, submission.received, submission.heldAtSubmit.contains(cursor)); });
            if (traceGpu && std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(200)) {

                for (std::size_t next = cursor + count, shown = 0; next < submission.commands.size() && shown < 48; ++shown) {
                    const auto nextHeader = submission.commands[next];
                    const auto nextCount = Pm4::PacketWords(nextHeader);
                    const auto nextPacket = std::span(submission.commands).subspan(next, nextCount);
                    std::fprintf(stderr, "[gpu]   then %s", Pm4::Name(nextHeader).c_str());
                    for (std::size_t i = 1; i < nextPacket.size() && i < 7; ++i) std::fprintf(stderr, " %08x", nextPacket[i]);
                    std::fprintf(stderr, "\n");
                    next += nextCount;
                }
            }
        } else if (drawPacket) {
            bool drawn = false;
            timed(&WorkerProfile::drawMs, [&] { tolerate("draw", [&] {
                static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
                static const bool profileDraws = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                const auto color = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
                const auto started = profileDraws ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto countSkip = [&](Graphics::DrawSkip kind) {
                    if (profileDraws) Graphics::CountDrawSkip(kind, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
                };

                const auto skipped = [&](const std::string& what) {
                    if (traceDraws) std::fprintf(stderr, "[draw] target 0x%llx mask 0x%x failed: %.160s\n",static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e), what.c_str());

                    char suffix[80];
                    std::snprintf(suffix, sizeof(suffix), " [%s, color target 0x%llx]", Pm4::Name(header).c_str(), static_cast<unsigned long long>(color));

                    static std::set<std::pair<std::uint64_t, std::string>> dumpedTargets;
                    const std::string reason = what.substr(0, 48);
                    if (dumpedTargets.insert({color, reason}).second) {
                        char name[64];
                        std::snprintf(name, sizeof(name), "draw_%llx_%08x.regs", static_cast<unsigned long long>(color), static_cast<std::uint32_t>(std::hash<std::string>{}(reason)));
                        if (std::FILE* file = std::fopen(name, "w")) {
                            std::fprintf(file, "# %s\n", what.c_str());
                            recent.Print(file, "# packet %s\n");
                            for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                            for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                            for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                            std::fclose(file);
                        }
                    }
                    reportSkip("draw", what + suffix);
                };
                const char* traceVerdict = "drawn";
                std::string traceReason;
                try {
                    std::string rejected;
                    const auto verdict = draw(queue, packet, submission, rejected);
                    drawn = verdict == DrawVerdict::Drawn;
                    CaptureTrace::Log("draw submission=%llu queue=%x offset=%zu target=%llx mask=%x verdict=%d reason=%.256s", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e), static_cast<int>(verdict), rejected.c_str());
                    if (verdict == DrawVerdict::Rejected) {
                        traceVerdict = "rejected";
                        traceReason = rejected;
                        skipped(rejected);
                        countSkip(Graphics::DrawSkip::Prechecked);
                    } else if (verdict == DrawVerdict::Nothing) {
                        traceVerdict = "nothing";
                        countSkip(Graphics::DrawSkip::Nothing);
                    } else if (traceDraws) {
                        std::fprintf(stderr, "[draw] target 0x%llx mask 0x%x ok\n",static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e));
                    }
                } catch (const std::exception& error) {
                    CaptureTrace::Log("draw-error submission=%llu offset=%zu reason=%.256s", static_cast<unsigned long long>(submission.serial), cursor, error.what());
                    traceVerdict = "failed";
                    traceReason = error.what();
                    skipped(error.what());
                    countSkip(Graphics::DrawSkip::Thrown);
                }
                if (FrameTrace::Active()) {
                    std::vector<std::uint64_t> targets;
                    const auto entry = FrameTrace::Record(describeDraw(queue, header, submission.queue, traceVerdict, traceReason, targets) + FrameTrace::TakeWrites());
                    if (entry.dump) {
                        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                        std::lock_guard gpuLock(GuestMemory::GpuMutex());
                        if (const auto localDevice = device.Load()) localDevice->CaptureImages(entry.prefix, entry.all ? std::span<const std::uint64_t>() : std::span<const std::uint64_t>(targets));
                    }
                }
            }); });
            finishDrawPacket(drawn);
        } else if (sampleDump) {
            dumpSampleCounters(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u));
        } else if (opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
            if (!wroteOnGpu) {
                Pm4::Execute(packet, queue);
                if (opcode == 0x49 || opcode == 0x37) {
                    if (const auto label = Pm4::DecodeLabelWrite(packet)) noteLabelStore(label->address, label->Bytes(), ++eventSerial);
                }
            }
            if (endOfPipeInterrupt && !interruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
        }
        if (drawPacket) Graphics::Recorder::CountRecordedWork();
        cursor += count;
    }

    static const bool submitAtEnd = std::getenv("APS5_SUBMIT_AT_END") != nullptr;
    if (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
        auto& costs = submissionCosts(submission.queue);
        if (!submitAtEnd && submission.rewindTail == nullptr && submission.queue == 0 && workerQueued() != nullptr && workerQueued()->load(std::memory_order_acquire) != 0) {
            ++costs.endSkipped;
            return;
        }
        ++costs.endSubmits;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::End);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        const auto localDevice = device.Load();

        recordDeferredLabels(localDevice.get(), submission.queue);
        if (localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
    }
    if (submission.rewindTail != nullptr) executeRewindTail(submission);
}

}
