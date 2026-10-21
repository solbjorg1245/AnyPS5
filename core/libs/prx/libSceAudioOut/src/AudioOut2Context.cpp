#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "AudioOut2Ambisonics.hpp"
#include "AudioOut2Internal.hpp"

// An AudioOut2 context is the hardware output queue: every push mixes the ports' current grain
// (num_grains samples) and appends it to a queue of queue_depth grains that plays in real time. The
// title paces its mixer on that queue (Demon's Souls polls GetQueueLevel and pushes non-blocking
// whenever a slot is free), so the level must follow the output clock: with an SDL device open the
// device's own queue is the hardware queue, otherwise a wall-clock model of it stands in.

using Clock = std::chrono::steady_clock;

static constexpr std::size_t CONTEXT_MEMORY = 0x10000;
static constexpr std::uint32_t DEFAULT_MAX_PORTS = 16;
// Pushes, polls and advances traced individually before the trace falls back to the per-second summary.
static constexpr std::uint64_t CALL_TRACE_FULL = 16;
// Silence queued ahead of the first grain (and again whenever the SDL queue ran dry) so scheduling
// jitter of the pushing thread does not starve the device; the queue level reported to the title
// counts only what lies beyond it.
static constexpr std::uint32_t CUSHION_MS = 40;
// The SDL queue is not allowed to run further ahead than this; grains beyond it are dropped.
static constexpr std::uint32_t MAX_QUEUED_MS = 250;
// A blocking push on a full queue gives up after this long.
static constexpr std::chrono::milliseconds FULL_WAIT_TIMEOUT{200};
static constexpr std::chrono::milliseconds FULL_WAIT_STEP{1};
static constexpr std::uint16_t DEVICE_SAMPLES = 512;
// The summed ports (a 7.1 bed folded to stereo plus the object ports) peak above full scale in the
// intro cutscene (2.1 measured); the device takes float and clips hard, so the mix is attenuated
// and clamped (AUDIO_OUT2_SUM_GAIN). The decoded mix (APS5_AUDIO2_AMBISONICS) plays a source ahead at its
// own level instead of summing 36 harmonics, so it needs no attenuation (AUDIO_OUT2_DECODED_GAIN); the
// gain follows the decoder's actual mode, and APS5_AUDIO2_GAIN overrides it on either path.
static constexpr std::chrono::seconds DIGEST_INTERVAL{10};
// APS5_AUDIO_DUMP=<file.wav> records the mix of the first context that pushes as 16-bit stereo PCM WAV:
// what is queued, after the master gain and the clamp, APS5_AUDIO_DUMP_SECONDS long at most (default
// 300 s, 55 MB). The header's sizes are patched every 10 s of audio and when the context goes, so a
// killed game still leaves a valid file. It records the content, not the delivery (see primes).
static constexpr std::uint32_t DUMP_DEFAULT_SECONDS = 300;
static constexpr std::uint64_t DUMP_PATCH_FRAMES = AUDIO_OUT2_SAMPLE_RATE * 10ull;
static constexpr std::size_t DUMP_FILE_BUFFER = 1 << 20;
static constexpr std::size_t WAV_HEADER_BYTES = 44;
static constexpr std::size_t OUTPUT_FRAME_BYTES = AUDIO_OUT2_OUTPUT_CHANNELS * sizeof(float);
static constexpr std::uint32_t OUTPUT_BYTES_PER_MS = AUDIO_OUT2_SAMPLE_RATE * OUTPUT_FRAME_BYTES / 1000;
static constexpr std::chrono::seconds PAD_PROBE_INTERVAL{2};
static constexpr std::uint32_t PAD_SLACK_GRAINS = 2;

bool AudioOut2TraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_AUDIOOUT2") != nullptr;
    return enabled;
}

bool AudioOut2DigestEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

static float MasterGain() {
    static const float gain = [] {
        const char* value = std::getenv("APS5_AUDIO2_GAIN");
        bool valid = true;
        const float chosen = AudioOut2MasterGain(AudioOut2AmbisonicMode(), value, valid);
        if (!valid) std::fprintf(stderr, "[audio] APS5_AUDIO2_GAIN=%s refused (a number above 0, at most %g): gain %.2f\n", value, static_cast<double>(AUDIO_OUT2_GAIN_MAX), static_cast<double>(chosen));
        return chosen;
    }();
    return gain;
}

// One line at the first context: the audio switches in effect.
static void LogSwitches() {
    static const bool logged = [] {
        const char* dump = std::getenv("APS5_AUDIO_DUMP");
        std::fprintf(stderr, "[audio] switches: ambisonics %u (APS5_AUDIO2_AMBISONICS), haptics %s (APS5_AUDIO2_HAPTICS_MAIN), gain %.2f (APS5_AUDIO2_GAIN), dump %s (APS5_AUDIO_DUMP), digest %s\n",
            AudioOut2AmbisonicMode(), AudioOut2HapticsMain() ? "main mix without a pad card" : "off the speakers without a pad card", static_cast<double>(MasterGain()),
            dump != nullptr && *dump != '\0' ? dump : "off", AudioOut2DigestEnabled() ? "on" : "off");
        return true;
    }();
    (void)logged;
}

struct AudioDump {
    std::FILE* file = nullptr;
    std::uint64_t frames = 0;
    std::uint64_t maxFrames = 0;
    std::uint64_t patchedFrames = 0;
    std::vector<std::int16_t> samples;
};
// The first context to push owns the dump; only it touches g_dump, under its context lock.
static std::atomic<AudioOut2Context*> g_dumpOwner{nullptr};
static AudioDump g_dump;

static void PutLe(std::uint8_t* out, std::uint32_t value, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; index++) out[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

static void WriteDumpHeader() {
    constexpr std::uint32_t frameBytes = AUDIO_OUT2_OUTPUT_CHANNELS * sizeof(std::int16_t);
    const auto dataBytes = static_cast<std::uint32_t>(g_dump.frames * frameBytes);
    std::uint8_t header[WAV_HEADER_BYTES] = {};
    std::memcpy(header, "RIFF", 4);
    PutLe(header + 4, static_cast<std::uint32_t>(WAV_HEADER_BYTES - 8) + dataBytes, 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    PutLe(header + 16, 16, 4);
    PutLe(header + 20, 1, 2);
    PutLe(header + 22, AUDIO_OUT2_OUTPUT_CHANNELS, 2);
    PutLe(header + 24, AUDIO_OUT2_SAMPLE_RATE, 4);
    PutLe(header + 28, AUDIO_OUT2_SAMPLE_RATE * frameBytes, 4);
    PutLe(header + 32, frameBytes, 2);
    PutLe(header + 34, 16, 2);
    std::memcpy(header + 36, "data", 4);
    PutLe(header + 40, dataBytes, 4);
    std::fflush(g_dump.file);
    std::fseek(g_dump.file, 0, SEEK_SET);
    std::fwrite(header, 1, sizeof(header), g_dump.file);
    std::fseek(g_dump.file, 0, SEEK_END);
    std::fflush(g_dump.file);
    g_dump.patchedFrames = g_dump.frames;
}

static void CloseDump() {
    if (g_dump.file == nullptr) return;
    WriteDumpHeader();
    std::fclose(g_dump.file);
    g_dump.file = nullptr;
    std::fprintf(stderr, "[audio] dump closed after %.1f s\n", static_cast<double>(g_dump.frames) / AUDIO_OUT2_SAMPLE_RATE);
}

// Appends the context's mixed grain to the dump. The caller holds the context lock.
static void DumpGrain(AudioOut2Context& context) {
    static const char* path = std::getenv("APS5_AUDIO_DUMP");
    if (path == nullptr || *path == '\0') return;
    if (g_dumpOwner.load(std::memory_order_acquire) != &context) {
        AudioOut2Context* expected = nullptr;
        if (!g_dumpOwner.compare_exchange_strong(expected, &context)) return;
        const char* seconds = std::getenv("APS5_AUDIO_DUMP_SECONDS");
        const auto limit = seconds != nullptr ? std::strtoul(seconds, nullptr, 10) : 0ul;
        g_dump.maxFrames = static_cast<std::uint64_t>(limit != 0 ? limit : DUMP_DEFAULT_SECONDS) * AUDIO_OUT2_SAMPLE_RATE;
        g_dump.file = std::fopen(path, "wb");
        if (g_dump.file == nullptr) {
            std::fprintf(stderr, "[audio] dump: cannot open %s\n", path);
            return;
        }
        std::setvbuf(g_dump.file, nullptr, _IOFBF, DUMP_FILE_BUFFER);
        WriteDumpHeader();
        std::fprintf(stderr, "[audio] dump: ctx %u to %s (%llu s at most)\n", context.id, path, static_cast<unsigned long long>(g_dump.maxFrames / AUDIO_OUT2_SAMPLE_RATE));
    }
    if (g_dump.file == nullptr) return;
    const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(context.grain, g_dump.maxFrames - g_dump.frames));
    g_dump.samples.resize(frames * AUDIO_OUT2_OUTPUT_CHANNELS);
    for (std::size_t index = 0; index < g_dump.samples.size(); index++) g_dump.samples[index] = static_cast<std::int16_t>(std::lrint(context.mix[index] * 32767.0f));
    std::fwrite(g_dump.samples.data(), sizeof(std::int16_t), g_dump.samples.size(), g_dump.file);
    g_dump.frames += frames;
    if (g_dump.frames >= g_dump.maxFrames) CloseDump();
    else if (g_dump.frames - g_dump.patchedFrames >= DUMP_PATCH_FRAMES) WriteDumpHeader();
}

double AudioOut2TraceSeconds() {
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

static AudioOut2Context* FromHandle(AudioOut2ContextHandle ctx) {
    return reinterpret_cast<AudioOut2Context*>(ctx);
}

static Clock::duration GrainDuration(const AudioOut2Context& context) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(static_cast<double>(context.grain) / AUDIO_OUT2_SAMPLE_RATE));
}

static std::uint32_t GrainBytes(const AudioOut2Context& context) {
    return static_cast<std::uint32_t>(context.grain * OUTPUT_FRAME_BYTES);
}

static std::uint32_t SdlQueuedMs(const AudioOut2Context& context) {
    return context.device ? SDL_GetQueuedAudioSize(context.device) / OUTPUT_BYTES_PER_MS : 0;
}

// Retires the modelled grains whose playback finished by now. The caller holds the context lock.
static void Drain(AudioOut2Context& context, Clock::time_point now) {
    const auto duration = GrainDuration(context);
    while (context.queued > 0) {
        const auto oldestEnd = context.playHead - duration * (context.queued - 1);
        if (oldestEnd > now) break;
        context.queued--;
    }
    if (context.queued == 0 && context.playHead < now) context.playHead = now;
}

// Grains queued and not yet played, as the title sees them. The caller holds the context lock.
static std::uint32_t QueueLevel(AudioOut2Context& context, Clock::time_point now) {
    Drain(context, now);
    if (context.device == 0) return context.queued;
    const auto queuedBytes = SDL_GetQueuedAudioSize(context.device);
    const auto cushionBytes = CUSHION_MS * OUTPUT_BYTES_PER_MS;
    const auto pending = queuedBytes > cushionBytes ? queuedBytes - cushionBytes : 0;
    const auto grainBytes = GrainBytes(context);
    return std::min(context.queueDepth, (pending + grainBytes - 1) / grainBytes);
}

static void OpenDevice(AudioOut2Context& context) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        AUDIOOUT2_TRACE("SDL audio init failed: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(AUDIO_OUT2_SAMPLE_RATE);
    desired.format = AUDIO_F32SYS;
    desired.channels = AUDIO_OUT2_OUTPUT_CHANNELS;
    desired.samples = DEVICE_SAMPLES;
    desired.callback = nullptr;
    SDL_AudioSpec obtained{};
    // No format change is allowed: SDL converts to the device's native format itself, so the queue
    // always takes stereo float at 48 kHz.
    context.device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (context.device == 0) {
        AUDIOOUT2_TRACE("SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(context.device, 0);
    AUDIOOUT2_TRACE("SDL device %u opened: %d Hz, format 0x%x, %u channels, %u samples\n", context.device, obtained.freq, obtained.format, obtained.channels, obtained.samples);
}

static void CloseDevice(AudioOut2Context& context) {
    if (context.device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_ClearQueuedAudio(context.device);
        SDL_CloseAudioDevice(context.device);
    }
    context.device = 0;
}

static void OpenPadDevice(AudioOut2Context& context) {
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return;
    const int count = SDL_GetNumAudioDevices(0);
    for (int index = 0; index < count; index++) {
        const char* listed = SDL_GetAudioDeviceName(index, 0);
        if (!AudioOut2IsPadAudioDevice(listed)) continue;
        const std::string name(listed);
        SDL_AudioSpec native{};
        if (SDL_GetAudioDeviceSpec(index, 0, &native) == 0 && native.channels != 0 && native.channels != AUDIO_OUT2_PAD_CHANNELS) continue;
        const auto layout = AudioOut2PadLayoutForDriver(SDL_GetCurrentAudioDriver());
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(AUDIO_OUT2_SAMPLE_RATE);
        desired.format = AUDIO_F32SYS;
        desired.channels = static_cast<Uint8>(layout.channels);
        desired.samples = DEVICE_SAMPLES;
        desired.callback = nullptr;
        SDL_AudioSpec obtained{};
        context.padDevice = SDL_OpenAudioDevice(name.c_str(), 0, &desired, &obtained, 0);
        if (context.padDevice == 0) {
            APS5_LOG_ERR("AudioOut2: could not open the controller sound card '%s': %s", name.c_str(), SDL_GetError());
            return;
        }
        context.padLayout = layout;
        context.padMix.assign(static_cast<std::size_t>(context.grain) * AUDIO_OUT2_PAD_CHANNELS, 0.0f);
        context.padFrames.assign(static_cast<std::size_t>(context.grain) * layout.channels, 0.0f);
        SDL_PauseAudioDevice(context.padDevice, 0);
        APS5_LOG_OUT("AudioOut2: controller speaker and vibration ports play on '%s'", name.c_str());
        return;
    }
}

static void ClosePadDevice(AudioOut2Context& context) {
    if (context.padDevice != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_ClearQueuedAudio(context.padDevice);
        SDL_CloseAudioDevice(context.padDevice);
    }
    context.padDevice = 0;
}

static void UpdatePadDevice(AudioOut2Context& context, Clock::time_point now) {
    if (context.padDevice != 0 && SDL_GetAudioDeviceStatus(context.padDevice) == SDL_AUDIO_STOPPED) {
        APS5_LOG_CHARS_OUT("AudioOut2: the controller sound card went away; its ports play in the main mix");
        ClosePadDevice(context);
        context.nextPadProbe = now + PAD_PROBE_INTERVAL;
    }
    if (context.padDevice != 0 || now < context.nextPadProbe) return;
    context.nextPadProbe = now + PAD_PROBE_INTERVAL;
    if (AudioOut2HasPadPorts(context)) OpenPadDevice(context);
}

static void QueuePadGrain(AudioOut2Context& context) {
    AudioOut2WritePadFrames(context.padMix.data(), context.padLayout, context.padFrames.data(), context.grain);
    const auto frameBytes = static_cast<std::uint32_t>(context.padLayout.channels * sizeof(float));
    const auto grainBytes = context.grain * frameBytes;
    const auto cushionBytes = CUSHION_MS * (AUDIO_OUT2_SAMPLE_RATE / 1000) * frameBytes;
    const auto mainQueuedBytes = context.device != 0 ? SDL_GetQueuedAudioSize(context.device) / OUTPUT_FRAME_BYTES * frameBytes : cushionBytes + context.queueDepth * grainBytes;
    const auto queuedBytes = SDL_GetQueuedAudioSize(context.padDevice);
    if (queuedBytes > mainQueuedBytes + PAD_SLACK_GRAINS * grainBytes) return;
    if (queuedBytes == 0) {
        static const std::vector<float> silence(static_cast<std::size_t>(CUSHION_MS) * AUDIO_OUT2_SAMPLE_RATE / 1000 * AUDIO_OUT2_PAD_DEVICE_CHANNELS_MAX, 0.0f);
        SDL_QueueAudio(context.padDevice, silence.data(), cushionBytes);
    }
    SDL_QueueAudio(context.padDevice, context.padFrames.data(), grainBytes);
}

static std::uint32_t Render(AudioOut2Context& context, Clock::time_point now) {
    UpdatePadDevice(context, now);
    float* pad = nullptr;
    if (context.padDevice != 0) {
        std::fill(context.padMix.begin(), context.padMix.end(), 0.0f);
        pad = context.padMix.data();
    }
    std::fill(context.mix.begin(), context.mix.end(), 0.0f);
    const bool metering = AudioOut2DigestEnabled();
    const auto mixed = AudioOut2MixPorts(context, context.mix.data(), pad, context.grain, metering ? &context.meters : nullptr);
    const float gain = MasterGain();
    for (float& sample : context.mix) {
        sample *= gain;
        if (metering) {
            const float magnitude = std::abs(sample);
            context.meters.peak = std::max(context.meters.peak, magnitude);
            if (magnitude > 1.0f) context.meters.over++;
        }
        sample = std::clamp(sample, -1.0f, 1.0f);
        if (AudioOut2TraceEnabled()) context.summaryPeak = std::max(context.summaryPeak, std::abs(sample));
    }
    if (metering) {
        for (std::uint32_t frame = 0; frame < context.grain; frame++) {
            const double left = context.mix[frame * AUDIO_OUT2_OUTPUT_CHANNELS];
            const double right = context.mix[frame * AUDIO_OUT2_OUTPUT_CHANNELS + 1];
            context.meters.outLeft += left * left;
            context.meters.outRight += right * right;
            context.meters.outCross += left * right;
        }
        context.meters.frames += context.grain;
    }
    DumpGrain(context);
    if (pad != nullptr) {
        AudioOut2FinishPadMix(pad, context.grain);
        QueuePadGrain(context);
    }
    if (context.device == 0) return mixed;
    const auto queuedBytes = SDL_GetQueuedAudioSize(context.device);
    if (queuedBytes > MAX_QUEUED_MS * OUTPUT_BYTES_PER_MS) {
        context.dropped++;
        return mixed;
    }
    if (queuedBytes == 0) {
        static const std::vector<float> silence(static_cast<std::size_t>(CUSHION_MS) * AUDIO_OUT2_SAMPLE_RATE / 1000 * AUDIO_OUT2_OUTPUT_CHANNELS, 0.0f);
        context.primes++;
        SDL_QueueAudio(context.device, silence.data(), static_cast<Uint32>(silence.size() * sizeof(float)));
    }
    SDL_QueueAudio(context.device, context.mix.data(), GrainBytes(context));
    return mixed;
}

static void TraceSummary(AudioOut2Context& context, Clock::time_point now) {
    if (!AudioOut2TraceEnabled()) return;
    if (context.summaryStart == Clock::time_point{}) {
        context.summaryStart = now;
        return;
    }
    const auto elapsed = std::chrono::duration<double>(now - context.summaryStart).count();
    if (elapsed < 1.0) return;
    std::fprintf(stderr, "[audioout2] t=%.3f ctx %p: %.1f pushes/s, %.1f advances/s, %.1f queue polls/s, hw queue %u/%u, sdl queue %u ms, mix peak %.3f; totals: pushes %llu (%llu blocking, %llu queue-full rejects), primes %llu, dropped %llu\n",
        AudioOut2TraceSeconds(), static_cast<void*>(&context), static_cast<double>(context.summaryPushes) / elapsed, static_cast<double>(context.summaryAdvances) / elapsed,
        static_cast<double>(context.summaryPolls) / elapsed, QueueLevel(context, now), context.queueDepth, SdlQueuedMs(context), static_cast<double>(context.summaryPeak),
        static_cast<unsigned long long>(context.pushes), static_cast<unsigned long long>(context.blockingPushes), static_cast<unsigned long long>(context.fullRejects),
        static_cast<unsigned long long>(context.primes), static_cast<unsigned long long>(context.dropped));
    context.summaryStart = now;
    context.summaryPushes = 0;
    context.summaryAdvances = 0;
    context.summaryPolls = 0;
    context.summaryPeak = 0.0f;
}

// `[audio] 10s:` (APS5_PROFILE_DRAW): pushes and their rate; primes = the SDL queue ran dry (an audible
// gap) and dropped = grains beyond MAX_QUEUED_MS, both in the window; the SDL queue now and the device.
// Then the RMS (summed power) of the bed, object and pad ports after their volume, the object ports by
// ambisonic order, where the vibration ports went (card, main mix, or off), and the output: RMS per
// side, the peak before the clamp, the L/R correlation (1.000 = mono) and the samples the clamp cut.
static void PrintDigest(AudioOut2Context& context, Clock::time_point now) {
    if (!AudioOut2DigestEnabled()) return;
    if (context.digestStart == Clock::time_point{}) context.digestStart = now;
    if (now - context.digestStart < DIGEST_INTERVAL) return;
    const auto elapsed = std::chrono::duration<double>(now - context.digestStart).count();
    const auto& meters = context.meters;
    const double frames = static_cast<double>(std::max<std::uint64_t>(meters.frames, 1));
    const auto rms = [frames](double sum) { return std::sqrt(sum / frames); };
    const double power = std::sqrt(meters.outLeft * meters.outRight);
    const double correlation = power > 0.0 ? meters.outCross / power : 0.0;
    const char* route = meters.padSkipped != 0 ? "off" : (context.padDevice != 0 ? "card" : "main");
    const auto pushes = context.pushes - context.digestPushes;
    std::fprintf(stderr, "[audio] 10s: ctx %u pushes %llu (%.1f/s) primes %llu dropped %llu sdl %u ms dev %u | bed %u rms %.4f | obj %u ambi %u o0..o5 rms %.4f %.4f %.4f %.4f %.4f %.4f | pad %u rms %.4f route %s | out rms %.4f %.4f peak %.3f corr %.3f over %llu | mode %u gain %.2f dump %.0f s\n",
        context.id, static_cast<unsigned long long>(pushes), static_cast<double>(pushes) / elapsed, static_cast<unsigned long long>(context.primes - context.digestPrimes),
        static_cast<unsigned long long>(context.dropped - context.digestDropped), SdlQueuedMs(context), context.device, meters.bedPorts, rms(meters.bed), meters.objectPorts,
        meters.ambisonicPorts, rms(meters.order[0]), rms(meters.order[1]), rms(meters.order[2]), rms(meters.order[3]), rms(meters.order[4]), rms(meters.order[5]), meters.padPorts,
        rms(meters.pad), route, rms(meters.outLeft), rms(meters.outRight), static_cast<double>(meters.peak), correlation, static_cast<unsigned long long>(meters.over),
        AudioOut2AmbisonicMode(), static_cast<double>(MasterGain()), g_dumpOwner.load(std::memory_order_relaxed) == &context ? static_cast<double>(g_dump.frames) / AUDIO_OUT2_SAMPLE_RATE : 0.0);
    context.meters = AudioOut2Meters{};
    context.digestStart = now;
    context.digestPushes = context.pushes;
    context.digestPrimes = context.primes;
    context.digestDropped = context.dropped;
}

extern "C" {

// The title's per-tick step between setting the ports' data and pushing. The ports are mixed at push
// time from the buffers they currently point at, and the push paces the clock, so nothing is due here.
int APS5_VABI sceAudioOut2ContextAdvance(AudioOut2ContextHandle ctx) {
    auto* context = FromHandle(ctx);
    if (!context) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    std::lock_guard lock(context->lock);
    context->advances++;
    context->summaryAdvances++;
    if (context->advances <= CALL_TRACE_FULL) AUDIOOUT2_TRACE("t=%.3f Advance ctx %p (pushes so far %llu)\n", AudioOut2TraceSeconds(), static_cast<void*>(context), static_cast<unsigned long long>(context->pushes));
    return 0;
}

int APS5_VABI sceAudioOut2ContextCreate(const AudioOut2ContextParam* params, void* buffer, size_t buffer_size, AudioOut2ContextHandle* ctx) {
    if (!params || !ctx) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    LogSwitches();
    static std::atomic<std::uint32_t> nextId{1};
    auto* context = new AudioOut2Context();
    context->id = nextId.fetch_add(1, std::memory_order_relaxed);
    context->grain = params->num_grains ? params->num_grains : AUDIO_OUT2_DEFAULT_GRAIN;
    context->queueDepth = params->queue_depth ? params->queue_depth : 1;
    context->playHead = Clock::now();
    context->mix.assign(static_cast<std::size_t>(context->grain) * AUDIO_OUT2_OUTPUT_CHANNELS, 0.0f);
    AUDIOOUT2_TRACE("t=%.3f ContextCreate: max_ports=%u max_object_ports=%u guarantee_object_ports=%u queue_depth=%u num_grains=%u flags=0x%x buffer=%p size=%zu -> ctx %p: %u-sample grains (%.2f ms), %u queued, %u Hz stereo float output\n",
        AudioOut2TraceSeconds(), params->max_ports, params->max_object_ports, params->guarantee_object_ports, params->queue_depth, params->num_grains, params->flags, buffer, buffer_size,
        static_cast<void*>(context), context->grain, 1000.0 * context->grain / AUDIO_OUT2_SAMPLE_RATE, context->queueDepth, AUDIO_OUT2_SAMPLE_RATE);
    OpenDevice(*context);
    *ctx = reinterpret_cast<AudioOut2ContextHandle>(context);
    return 0;
}

int APS5_VABI sceAudioOut2ContextDestroy(AudioOut2ContextHandle ctx) {
    auto* context = FromHandle(ctx);
    if (!context) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    AUDIOOUT2_TRACE("t=%.3f ContextDestroy ctx %p after %llu pushes\n", AudioOut2TraceSeconds(), static_cast<void*>(context), static_cast<unsigned long long>(context->pushes));
    {
        std::lock_guard lock(context->lock);
        CloseDevice(*context);
        ClosePadDevice(*context);
        if (g_dumpOwner.load(std::memory_order_acquire) == context) CloseDump();
    }
    AudioOut2ReleasePorts(*context);
    delete context;
    return 0;
}

int APS5_VABI sceAudioOut2ContextGetQueueLevel(AudioOut2ContextHandle ctx, uint32_t* queue_level, uint32_t* available_queue) {
    auto* context = FromHandle(ctx);
    if (!context) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    std::lock_guard lock(context->lock);
    const auto level = QueueLevel(*context, Clock::now());
    if (queue_level) *queue_level = level;
    if (available_queue) *available_queue = context->queueDepth - level;
    context->queueLevelPolls++;
    context->summaryPolls++;
    if (context->queueLevelPolls <= CALL_TRACE_FULL) AUDIOOUT2_TRACE("t=%.3f GetQueueLevel ctx %p -> level %u, available %u\n", AudioOut2TraceSeconds(), static_cast<void*>(context), level, context->queueDepth - level);
    return 0;
}

int APS5_VABI sceAudioOut2ContextPush(AudioOut2ContextHandle ctx, uint32_t blocking) {
    auto* context = FromHandle(ctx);
    if (!context) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    std::unique_lock lock(context->lock);
    auto now = Clock::now();
    const auto waitStart = now;
    while (QueueLevel(*context, now) >= context->queueDepth) {
        if (!blocking) {
            context->fullRejects++;
            if (context->fullRejects <= CALL_TRACE_FULL) AUDIOOUT2_TRACE("t=%.3f Push ctx %p non-blocking on a full queue (%u): rejected\n", AudioOut2TraceSeconds(), static_cast<void*>(context), context->queueDepth);
            return SCE_AUDIO_OUT2_ERROR_QUEUE_FULL;
        }
        if (now - waitStart > FULL_WAIT_TIMEOUT) break;
        lock.unlock();
        std::this_thread::sleep_for(FULL_WAIT_STEP);
        lock.lock();
        now = Clock::now();
    }
    context->queued++;
    context->playHead += GrainDuration(*context);
    const auto mixed = Render(*context, now);
    context->pushes++;
    context->summaryPushes++;
    if (blocking) context->blockingPushes++;
    if (context->pushes <= CALL_TRACE_FULL) {
        AUDIOOUT2_TRACE("t=%.3f Push ctx %p blocking=%u: grain from %u ports, hw queue %u/%u, sdl queue %u ms\n", AudioOut2TraceSeconds(), static_cast<void*>(context), blocking,
            mixed, QueueLevel(*context, now), context->queueDepth, SdlQueuedMs(*context));
    }
    TraceSummary(*context, now);
    PrintDigest(*context, now);
    return 0;
}

int APS5_VABI sceAudioOut2ContextQueryMemory(const AudioOut2ContextParam* params, size_t* memory_size) {
    if (!params || !memory_size) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    *memory_size = CONTEXT_MEMORY;
    return 0;
}

int APS5_VABI sceAudioOut2ContextResetParam(AudioOut2ContextParam* params) {
    if (!params) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    std::memset(params, 0, sizeof(*params));
    params->max_ports = DEFAULT_MAX_PORTS;
    params->queue_depth = 1;
    params->num_grains = AUDIO_OUT2_DEFAULT_GRAIN;
    return 0;
}

int APS5_VABI sceAudioOut2ContextSetAttributes(AudioOut2ContextHandle ctx, const AudioOut2Attribute* attributes, uint32_t num) {
    auto* context = FromHandle(ctx);
    if (!context) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    if (!attributes && num != 0) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < num; index++) {
        AUDIOOUT2_TRACE("t=%.3f ContextSetAttributes ctx %p: id=0x%x size=%zu value=%p (ignored)\n", AudioOut2TraceSeconds(), static_cast<void*>(context), attributes[index].attribute_id, attributes[index].value_size, attributes[index].value);
    }
    return 0;
}

}
