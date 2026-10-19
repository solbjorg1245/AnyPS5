#ifndef CORE_LIBS_PRX_LIBSCEAUDIOOUT_SRC_AUDIOOUT2INTERNAL_HPP
#define CORE_LIBS_PRX_LIBSCEAUDIOOUT_SRC_AUDIOOUT2INTERNAL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include "SDL.h"
#include "SceTypes.hpp"
#include "AudioOut2PadMix.hpp"

// Shared by the AudioOut2 context and port files.
//
// APS5_TRACE_AUDIOOUT2=1 prints the title's use of the API to stderr: context and port parameters, every
// attribute the title sets (first few per port, then sampled), the first pushes and queue polls, and a
// once-per-second summary with the push rate and the SDL queue level.
bool AudioOut2TraceEnabled();
// Seconds since the first AudioOut2 call, for the trace.
double AudioOut2TraceSeconds();

// APS5_PROFILE_DRAW prints an `[audio] 10s:` digest line per context (AudioOut2Context.cpp).
bool AudioOut2DigestEnabled();

#define AUDIOOUT2_TRACE(...) \
    do { \
        if (AudioOut2TraceEnabled()) std::fprintf(stderr, "[audioout2] " __VA_ARGS__); \
    } while (0)

static constexpr int SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80260502);
static constexpr int SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE = static_cast<int>(0x80260503);
// Not recovered from a title or SDK header: follows the 0x802605xx pattern of the codes above. Returned
// by a non-blocking push when the modelled hardware queue is full.
static constexpr int SCE_AUDIO_OUT2_ERROR_QUEUE_FULL = static_cast<int>(0x80260507);

static constexpr std::uint32_t AUDIO_OUT2_SAMPLE_RATE = 48000;
// The grain a push carries, in samples per channel: a context's num_grains (Demon's Souls: 256, 5.33 ms).
static constexpr std::uint32_t AUDIO_OUT2_DEFAULT_GRAIN = 256;
static constexpr std::uint32_t AUDIO_OUT2_OUTPUT_CHANNELS = 2;
static constexpr std::uint32_t AUDIO_OUT2_PORT_CHANNELS_MAX = 8;

struct AudioOut2Context;

struct AudioOut2Port {
    bool used = false;
    AudioOut2Context* context = nullptr;
    std::uint16_t type = 0;
    std::uint32_t dataFormat = 0;
    std::uint32_t samplingFreq = 0;
    std::uint32_t flags = 0;
    std::uint32_t channels = 0;
    bool int16 = false;
    const void* data = nullptr;
    float volume[AUDIO_OUT2_PORT_CHANNELS_MAX] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // Attribute 8 (0x40 | ACN on ambisonic object ports) and attribute 5 (passthrough side), as set.
    std::uint32_t ambisonic = 0;
    std::uint32_t passthrough = 0;
    std::uint64_t dataSets = 0;
    std::uint64_t attributeTraces = 0;
};

// Signal meters of one digest window: sums of squares of the ports' samples after their volume, by
// group (bed, object, ambisonic order, pad), and of the output after the master gain.
struct AudioOut2Meters {
    double bed = 0.0;
    double object = 0.0;
    double order[6] = {};
    double pad = 0.0;
    double outLeft = 0.0;
    double outRight = 0.0;
    double outCross = 0.0;
    // Largest output sample before the clamp, and the samples the clamp cut.
    float peak = 0.0f;
    std::uint64_t over = 0;
    std::uint64_t frames = 0;
    // Ports of each group in the last grain; padSkipped = vibration ports left out of the main mix.
    std::uint32_t bedPorts = 0;
    std::uint32_t objectPorts = 0;
    std::uint32_t ambisonicPorts = 0;
    std::uint32_t padPorts = 0;
    std::uint32_t padSkipped = 0;
};

struct AudioOut2Context {
    std::mutex lock;
    std::uint32_t grain = AUDIO_OUT2_DEFAULT_GRAIN;
    std::uint32_t queueDepth = 1;
    // Fallback hardware queue model for a context without an SDL device: pushes that have not
    // finished playing, and the time the last of them finishes; playback is real time, so the queue
    // drains as the wall clock advances. With a device the SDL queue is the hardware queue.
    std::uint32_t queued = 0;
    std::chrono::steady_clock::time_point playHead;
    SDL_AudioDeviceID device = 0;
    // Stereo float mix of the ports for one push.
    std::vector<float> mix;
    SDL_AudioDeviceID padDevice = 0;
    std::chrono::steady_clock::time_point nextPadProbe;
    AudioOut2PadLayout padLayout;
    std::vector<float> padMix;
    std::vector<float> padFrames;
    // Trace counters.
    std::uint64_t pushes = 0;
    std::uint64_t blockingPushes = 0;
    std::uint64_t fullRejects = 0;
    std::uint64_t advances = 0;
    std::uint64_t queueLevelPolls = 0;
    std::uint64_t primes = 0;
    std::uint64_t dropped = 0;
    std::uint64_t summaryPushes = 0;
    std::uint64_t summaryAdvances = 0;
    std::uint64_t summaryPolls = 0;
    // Largest sample magnitude of the grains mixed since the last summary: the mixed signal's level.
    float summaryPeak = 0.0f;
    std::chrono::steady_clock::time_point summaryStart;
    // Digest (APS5_PROFILE_DRAW): the window's start, its meters and the totals at its start.
    std::uint32_t id = 0;
    std::chrono::steady_clock::time_point digestStart;
    AudioOut2Meters meters;
    std::uint64_t digestPushes = 0;
    std::uint64_t digestPrimes = 0;
    std::uint64_t digestDropped = 0;
};

// Mixes every port of the context that carries PCM data into out (stereo float, frames frames), summing
// onto the zeroed buffer. Returns the number of ports mixed. meters (may be null) takes the ports' levels.
std::uint32_t AudioOut2MixPorts(const AudioOut2Context& context, float* out, float* padOut, std::uint32_t frames, AudioOut2Meters* meters);
// The ambisonic decoder's mode (APS5_AUDIO2_AMBISONICS, validated: 0 = off), and whether vibration ports
// play in the main mix without a pad card (APS5_AUDIO2_HAPTICS_MAIN).
std::uint32_t AudioOut2AmbisonicMode();
bool AudioOut2HapticsMain();
bool AudioOut2HasPadPorts(const AudioOut2Context& context);
// Forgets the ports of a context being destroyed.
void AudioOut2ReleasePorts(const AudioOut2Context& context);

#endif
