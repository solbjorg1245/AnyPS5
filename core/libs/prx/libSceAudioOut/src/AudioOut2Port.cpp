#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "AudioOut2Ambisonics.hpp"
#include "AudioOut2Internal.hpp"
#include "AudioOut2PadMix.hpp"

static constexpr std::uint16_t OUTPUT_MAIN = 1;
static constexpr std::int16_t VOLUME_MAX = 127;
// Attribute sets are traced in full for the first few calls of a port, then once per this many.
static constexpr std::uint64_t ATTRIBUTE_TRACE_FULL = 8;
static constexpr std::uint64_t ATTRIBUTE_TRACE_EVERY = 2000;

// Port attributes as Demon's Souls sets them every tick (APS5_TRACE_AUDIOOUT2): id 0 carries the
// grain's PCM as an 8-byte value holding the buffer pointer, id 1 one float gain per channel. Id 8 (a
// u32) is 0x40 | ACN on the ambisonic object ports and id 5 (a u32) the passthrough side; both are
// stored and used only with APS5_AUDIO2_AMBISONICS. Id 9 (a byte) is not understood and ignored.
static constexpr std::uint32_t ATTRIBUTE_DATA = 0;
static constexpr std::uint32_t ATTRIBUTE_VOLUME = 1;
static constexpr std::uint32_t ATTRIBUTE_PASSTHROUGH = 5;
static constexpr std::uint32_t ATTRIBUTE_AMBISONIC = 8;
// Object ports are the 0x01xx port types.
static constexpr std::uint16_t PORT_TYPE_OBJECT_MASK = 0xFF00;
static constexpr std::uint16_t PORT_TYPE_OBJECT = 0x100;

static constexpr std::uint32_t FORMAT_CHANNELS_SHIFT = 8;
static constexpr std::uint32_t FORMAT_CHANNELS_MASK = 0xFu;
static constexpr std::uint32_t FORMAT_TYPE_MASK = 0x7Fu;

std::uint32_t AudioOut2AmbisonicMode() {
    static const std::uint32_t mode = [] {
        const char* value = std::getenv("APS5_AUDIO2_AMBISONICS");
        return value != nullptr ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return mode;
}

static const AudioOut2AmbisonicDecoder& Decoder() {
    static const AudioOut2AmbisonicDecoder decoder = AudioOut2MakeAmbisonicDecoder(AudioOut2AmbisonicMode());
    return decoder;
}

// APS5_AUDIO2_HAPTICS_MAIN=0: without a controller sound card the vibration ports stay out of the
// speakers. Unset or 1: the old route (into the main mix).
static bool HapticsMain() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_AUDIO2_HAPTICS_MAIN");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static std::mutex g_portsLock;
// Grows on demand: the title opens its bed ports plus max_object_ports object ports at once.
static std::vector<AudioOut2Port> g_ports;

static AudioOut2Port* FromHandle(AudioOut2PortHandle handle) {
    const auto index = handle - 1;
    if (handle == 0 || index >= g_ports.size() || !g_ports[index].used) return nullptr;
    return &g_ports[index];
}

// 8-channel order FL FR C LFE RL RR SL SR (a swapped rear/side pair order sums the same): the rear
// and side pairs fold into the front at -3 dB, the centre into both sides, and the LFE is dropped.
static constexpr float DOWNMIX_GAIN = 0.7071f;

static void ReadFrame(const AudioOut2Port& port, std::uint32_t frame, float* in) {
    const auto first = static_cast<std::size_t>(frame) * port.channels;
    for (std::uint32_t c = 0; c < port.channels; c++) {
        in[c] = port.int16 ? static_cast<const std::int16_t*>(port.data)[first + c] / 32768.0f : static_cast<const float*>(port.data)[first + c];
    }
}

// Sum of squares of a port's samples after its volume, over all its channels: the digest's meters.
static double PortPower(const AudioOut2Port& port, std::uint32_t frames) {
    float in[AUDIO_OUT2_PORT_CHANNELS_MAX];
    double sum = 0.0;
    for (std::uint32_t frame = 0; frame < frames; frame++) {
        ReadFrame(port, frame, in);
        for (std::uint32_t c = 0; c < port.channels; c++) sum += static_cast<double>(in[c] * port.volume[c]) * (in[c] * port.volume[c]);
    }
    return sum;
}

static void Meter(const AudioOut2Port& port, bool skipped, std::uint32_t frames, AudioOut2Meters& meters) {
    const double power = PortPower(port, frames);
    if (AudioOut2RouteForPort(port.type, port.channels) != AudioOut2Route::Main) {
        meters.pad += power;
        meters.padPorts++;
        if (skipped) meters.padSkipped++;
        return;
    }
    if ((port.type & PORT_TYPE_OBJECT_MASK) == PORT_TYPE_OBJECT) {
        meters.object += power;
        meters.objectPorts++;
    } else {
        meters.bed += power;
        meters.bedPorts++;
    }
    const int acn = AudioOut2AmbisonicChannel(port.ambisonic);
    if (acn >= 0) {
        meters.order[AudioOut2AmbisonicOrder(static_cast<std::uint32_t>(acn))] += power;
        meters.ambisonicPorts++;
    }
}

static void AccumulatePadPort(const AudioOut2Port& port, AudioOut2Route route, float* out, std::uint32_t frames) {
    float in[AUDIO_OUT2_PORT_CHANNELS_MAX];
    for (std::uint32_t frame = 0; frame < frames; frame++) {
        ReadFrame(port, frame, in);
        AudioOut2AccumulatePadFrame(route, in, port.channels, port.volume, out + static_cast<std::size_t>(frame) * AUDIO_OUT2_PAD_CHANNELS);
    }
}

static void AccumulatePort(const AudioOut2Port& port, float* out, std::uint32_t frames) {
    const auto ch = port.channels;
    const float* volume = port.volume;
    // A mono port plays on both sides unless the ambisonic decoder gives it gains of its own.
    float monoLeft = 1.0f;
    float monoRight = 1.0f;
    if (ch == 1) AudioOut2MonoGains(Decoder(), port.ambisonic, port.passthrough, monoLeft, monoRight);
    float in[AUDIO_OUT2_PORT_CHANNELS_MAX];
    for (std::uint32_t frame = 0; frame < frames; frame++) {
        ReadFrame(port, frame, in);
        float left = 0.0f;
        float right = 0.0f;
        if (ch == 1) {
            const float sample = in[0] * volume[0];
            left = sample * monoLeft;
            right = sample * monoRight;
        } else if (ch < 8) {
            left = in[0] * volume[0];
            right = in[1] * volume[1];
        } else {
            const float centre = in[2] * volume[2] * DOWNMIX_GAIN;
            left = in[0] * volume[0] + centre + (in[4] * volume[4] + in[6] * volume[6]) * DOWNMIX_GAIN;
            right = in[1] * volume[1] + centre + (in[5] * volume[5] + in[7] * volume[7]) * DOWNMIX_GAIN;
        }
        out[frame * AUDIO_OUT2_OUTPUT_CHANNELS] += left;
        out[frame * AUDIO_OUT2_OUTPUT_CHANNELS + 1] += right;
    }
}

std::uint32_t AudioOut2MixPorts(const AudioOut2Context& context, float* out, float* padOut, std::uint32_t frames, AudioOut2Meters* meters) {
    std::lock_guard lock(g_portsLock);
    if (meters != nullptr) {
        meters->bedPorts = meters->objectPorts = meters->ambisonicPorts = meters->padPorts = meters->padSkipped = 0;
    }
    std::uint32_t mixed = 0;
    for (const auto& port : g_ports) {
        if (!port.used || port.context != &context || port.data == nullptr || port.channels == 0) continue;
        const auto route = padOut != nullptr ? AudioOut2RouteForPort(port.type, port.channels) : AudioOut2Route::Main;
        const bool skipped = padOut == nullptr && AudioOut2SkipInMainMix(port.type, port.channels, HapticsMain());
        if (meters != nullptr) Meter(port, skipped, frames, *meters);
        if (skipped) continue;
        if (route == AudioOut2Route::Main) AccumulatePort(port, out, frames);
        else AccumulatePadPort(port, route, padOut, frames);
        mixed++;
    }
    return mixed;
}

bool AudioOut2HasPadPorts(const AudioOut2Context& context) {
    std::lock_guard lock(g_portsLock);
    return std::any_of(g_ports.begin(), g_ports.end(), [&context](const AudioOut2Port& port) {
        return port.used && port.context == &context && AudioOut2RouteForPort(port.type, port.channels) != AudioOut2Route::Main;
    });
}

void AudioOut2ReleasePorts(const AudioOut2Context& context) {
    std::lock_guard lock(g_portsLock);
    for (auto& port : g_ports) {
        if (port.used && port.context == &context) port = AudioOut2Port{};
    }
}

static void TraceAttribute(AudioOut2PortHandle handle, const AudioOut2Port& port, const AudioOut2Attribute& attribute, const char* verdict) {
    if (!AudioOut2TraceEnabled()) return;
    char values[128] = "";
    if (attribute.value != nullptr) {
        std::size_t used = 0;
        const auto words = std::min<std::size_t>(attribute.value_size / sizeof(std::uint32_t), 4);
        for (std::size_t index = 0; index < words && used < sizeof(values); index++) {
            std::uint32_t bits = 0;
            float value = 0.0f;
            std::memcpy(&bits, static_cast<const std::uint8_t*>(attribute.value) + index * sizeof(bits), sizeof(bits));
            std::memcpy(&value, &bits, sizeof(value));
            used += static_cast<std::size_t>(std::snprintf(values + used, sizeof(values) - used, " %08x(%g)", bits, static_cast<double>(value)));
        }
    }
    std::fprintf(stderr, "[audioout2] t=%.3f port %llu set attribute id=0x%x size=%zu value=%p%s: %s (data sets so far %llu)\n",
        AudioOut2TraceSeconds(), static_cast<unsigned long long>(handle), attribute.attribute_id, attribute.value_size, attribute.value, values, verdict,
        static_cast<unsigned long long>(port.dataSets));
}

extern "C" {

int APS5_VABI sceAudioOut2PortCreate(AudioOut2ContextHandle ctx, const AudioOut2PortParam* params, AudioOut2PortHandle* port) {
    if (!ctx) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    if (!params || !port) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(g_portsLock);
    std::size_t index = 0;
    while (index < g_ports.size() && g_ports[index].used) index++;
    if (index == g_ports.size()) g_ports.emplace_back();
    auto& entry = g_ports[index];
    entry = AudioOut2Port{};
    entry.used = true;
    entry.context = reinterpret_cast<AudioOut2Context*>(ctx);
    entry.type = params->port_type;
    entry.dataFormat = params->data_format;
    entry.samplingFreq = params->sampling_freq;
    entry.flags = params->flags;
    entry.channels = (params->data_format >> FORMAT_CHANNELS_SHIFT) & FORMAT_CHANNELS_MASK;
    const auto sampleType = params->data_format & FORMAT_TYPE_MASK;
    if (entry.channels == 0 || entry.channels > AUDIO_OUT2_PORT_CHANNELS_MAX || sampleType > 1) {
        entry = AudioOut2Port{};
        throw std::runtime_error("sceAudioOut2PortCreate: data format 0x" + [&] { char text[16]; std::snprintf(text, sizeof(text), "%x", params->data_format); return std::string(text); }() + " is not implemented");
    }
    entry.int16 = sampleType == 1;
    // Builds the decoder table (mode 2 integrates over the sphere) here rather than on the first push.
    (void)Decoder();
    *port = static_cast<AudioOut2PortHandle>(index) + 1;
    AUDIOOUT2_TRACE("t=%.3f PortCreate ctx=%llx -> port %llu: type=0x%x data_format=0x%x (%u float ch) sampling_freq=%u flags=0x%x user=%llx\n",
        AudioOut2TraceSeconds(), static_cast<unsigned long long>(ctx), static_cast<unsigned long long>(*port), params->port_type, params->data_format,
        entry.channels, params->sampling_freq, params->flags, static_cast<unsigned long long>(params->user_handle));
    return 0;
}

int APS5_VABI sceAudioOut2PortDestroy(AudioOut2PortHandle port) {
    std::lock_guard lock(g_portsLock);
    auto* entry = FromHandle(port);
    if (!entry) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    AUDIOOUT2_TRACE("t=%.3f PortDestroy port %llu (data sets %llu)\n", AudioOut2TraceSeconds(), static_cast<unsigned long long>(port), static_cast<unsigned long long>(entry->dataSets));
    *entry = AudioOut2Port{};
    return 0;
}

int APS5_VABI sceAudioOut2PortGetState(AudioOut2PortHandle port, AudioOut2PortState* state) {
    std::lock_guard lock(g_portsLock);
    if (!FromHandle(port)) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    if (!state) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    state->output = OUTPUT_MAIN;
    state->num_channels = AUDIO_OUT2_OUTPUT_CHANNELS;
    state->volume = VOLUME_MAX;
    state->reroute_counter = 0;
    state->flags = 0;
    return 0;
}

int APS5_VABI sceAudioOut2PortSetAttributes(AudioOut2PortHandle port, const AudioOut2Attribute* attributes, uint32_t num) {
    std::lock_guard lock(g_portsLock);
    auto* entry = FromHandle(port);
    if (!entry) return SCE_AUDIO_OUT2_ERROR_INVALID_HANDLE;
    if (!attributes && num != 0) return SCE_AUDIO_OUT2_ERROR_INVALID_ARGUMENT;
    for (uint32_t index = 0; index < num; index++) {
        const auto& attribute = attributes[index];
        const char* verdict = "ignored";
        if (attribute.value == nullptr) {
            verdict = "null value";
        } else if (attribute.attribute_id == ATTRIBUTE_DATA && attribute.value_size == sizeof(entry->data)) {
            std::memcpy(&entry->data, attribute.value, sizeof(entry->data));
            entry->dataSets++;
            verdict = "pcm data pointer";
        } else if (attribute.attribute_id == ATTRIBUTE_VOLUME && entry->channels != 0 && attribute.value_size == entry->channels * sizeof(float)) {
            std::memcpy(entry->volume, attribute.value, attribute.value_size);
            verdict = "volume";
        } else if (attribute.attribute_id == ATTRIBUTE_AMBISONIC && attribute.value_size >= sizeof(entry->ambisonic)) {
            std::memcpy(&entry->ambisonic, attribute.value, sizeof(entry->ambisonic));
            verdict = AudioOut2AmbisonicChannel(entry->ambisonic) >= 0 ? "ambisonic channel" : "ambisonic attribute (not an ACN)";
        } else if (attribute.attribute_id == ATTRIBUTE_PASSTHROUGH && attribute.value_size >= sizeof(entry->passthrough)) {
            std::memcpy(&entry->passthrough, attribute.value, sizeof(entry->passthrough));
            verdict = "passthrough";
        }
        const auto traces = entry->attributeTraces++;
        if (traces < ATTRIBUTE_TRACE_FULL || traces % ATTRIBUTE_TRACE_EVERY == 0) TraceAttribute(port, *entry, attribute, verdict);
    }
    return 0;
}

}
