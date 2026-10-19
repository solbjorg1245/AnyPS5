#ifndef CORE_LIBS_PRX_LIBSCEAUDIOOUT_SRC_AUDIOOUT2AMBISONICS_HPP
#define CORE_LIBS_PRX_LIBSCEAUDIOOUT_SRC_AUDIOOUT2AMBISONICS_HPP

#include <cstdint>

// Demon's Souls sends its 3D mix on mono object ports as two fifth-order ambisonic scenes, one ACN
// channel per port: port attribute 8 holds 0x40 | ACN, SN3D normalised. Summed to both sides as plain
// mono ports, the harmonics cancel for most directions; the decoder below turns each channel into a
// left and a right gain for the stereo output (speakers at +-30 degrees, as GetSpeakerInfo reports).
//
// APS5_AUDIO2_AMBISONICS: unset or 0 sums the ports as before; 1 decodes the first order only (virtual
// microphones: ahead at full level on both sides, behind -3 dB, higher orders muted); 2 decodes every
// order up to the fifth, the projection of a stereo panning law onto the harmonics with max-rE weights,
// scaled to mode 1's level ahead.

static constexpr std::uint32_t AUDIO_OUT2_AMBISONIC_FLAG = 0x40;
static constexpr std::uint32_t AUDIO_OUT2_AMBISONIC_ORDER_MAX = 5;
static constexpr std::uint32_t AUDIO_OUT2_AMBISONIC_CHANNELS = (AUDIO_OUT2_AMBISONIC_ORDER_MAX + 1) * (AUDIO_OUT2_AMBISONIC_ORDER_MAX + 1);
// Port attribute 5 as SharpEmu reads it: 1 plays a mono port on the left speaker only, 2 on the right.
static constexpr std::uint32_t AUDIO_OUT2_PASSTHROUGH_LEFT = 1;
static constexpr std::uint32_t AUDIO_OUT2_PASSTHROUGH_RIGHT = 2;

struct AudioOut2AmbisonicDecoder {
    std::uint32_t mode = 0;
    float left[AUDIO_OUT2_AMBISONIC_CHANNELS] = {};
    float right[AUDIO_OUT2_AMBISONIC_CHANNELS] = {};
};

// Real SN3D spherical harmonic of an ACN channel for a direction (x ahead, y left, z up; any length).
double AudioOut2Sn3d(std::uint32_t acn, double x, double y, double z);
std::uint32_t AudioOut2AmbisonicOrder(std::uint32_t acn);
// The ACN a port's attribute 8 names, or -1 when it names none this decoder knows.
int AudioOut2AmbisonicChannel(std::uint32_t attribute);
// Mode 0 (or an unknown mode) leaves every gain at zero.
AudioOut2AmbisonicDecoder AudioOut2MakeAmbisonicDecoder(std::uint32_t mode);
// The stereo gains of a mono port: the decoder's for an ambisonic channel, one side for a passthrough
// port, 1 and 1 (the old sum) otherwise or when the decoder is off.
void AudioOut2MonoGains(const AudioOut2AmbisonicDecoder& decoder, std::uint32_t ambisonic, std::uint32_t passthrough, float& left, float& right);

#endif
