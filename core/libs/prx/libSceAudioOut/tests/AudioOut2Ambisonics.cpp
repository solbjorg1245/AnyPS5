#include "prx/libSceAudioOut/src/AudioOut2Ambisonics.hpp"
#include "prx/libSceAudioOut/src/AudioOut2PadMix.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "failed: %s\n", what);
        std::abort();
    }
}

static constexpr double PI = 3.14159265358979323846;

struct Stereo {
    double left;
    double right;
};

// Encodes a unit plane wave from an azimuth (degrees, positive = left) and elevation with SN3D and
// decodes it with the decoder's gains.
static Stereo Decode(const AudioOut2AmbisonicDecoder& decoder, double azimuth, double elevation = 0) {
    const double a = azimuth * PI / 180;
    const double e = elevation * PI / 180;
    const double x = std::cos(e) * std::cos(a);
    const double y = std::cos(e) * std::sin(a);
    const double z = std::sin(e);
    Stereo out{0, 0};
    for (std::uint32_t acn = 0; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) {
        const double harmonic = AudioOut2Sn3d(acn, x, y, z);
        out.left += decoder.left[acn] * harmonic;
        out.right += decoder.right[acn] * harmonic;
    }
    return out;
}

static double Db(double value) {
    return 20 * std::log10(std::abs(value) + 1e-12);
}

static double PowerDb(const Stereo& s) {
    return 10 * std::log10(s.left * s.left + s.right * s.right + 1e-12);
}

static bool Near(double a, double b, double tolerance = 1e-4) {
    return std::abs(a - b) <= tolerance;
}

int main() {
    Require(Near(AudioOut2Sn3d(0, 0.3, -0.2, 0.5), 1.0), "W is 1 everywhere");
    Require(Near(AudioOut2Sn3d(1, 0, 1, 0), 1.0) && Near(AudioOut2Sn3d(1, 0, -1, 0), -1.0), "ACN 1 is Y: left positive");
    Require(Near(AudioOut2Sn3d(2, 0, 0, 1), 1.0), "ACN 2 is Z");
    Require(Near(AudioOut2Sn3d(3, 1, 0, 0), 1.0) && Near(AudioOut2Sn3d(3, 0, 1, 0), 0.0), "ACN 3 is X: ahead");
    Require(Near(AudioOut2Sn3d(8, 1, 0, 0), std::sqrt(3.0) / 2), "SN3D second order, m = 2 ahead");
    Require(AudioOut2AmbisonicOrder(0) == 0 && AudioOut2AmbisonicOrder(3) == 1 && AudioOut2AmbisonicOrder(8) == 2 && AudioOut2AmbisonicOrder(35) == 5, "orders");

    Require(AudioOut2AmbisonicChannel(0x40) == 0 && AudioOut2AmbisonicChannel(0x63) == 35, "0x40 | ACN");
    Require(AudioOut2AmbisonicChannel(0) == -1 && AudioOut2AmbisonicChannel(0x3) == -1, "values below 0x40 are no ACN");
    Require(AudioOut2AmbisonicChannel(0x64) == -1 && AudioOut2AmbisonicChannel(0x140) == -1, "beyond the fifth order");

    const auto off = AudioOut2MakeAmbisonicDecoder(0);
    Require(off.mode == 0 && off.left[0] == 0.0f && off.right[0] == 0.0f, "mode 0 decodes nothing");
    Require(AudioOut2MakeAmbisonicDecoder(3).mode == 0, "unknown modes are off");

    for (std::uint32_t mode : {1u, 2u}) {
        const auto decoder = AudioOut2MakeAmbisonicDecoder(mode);
        Require(decoder.mode == mode, "mode kept");
        Require(decoder.left[0] == decoder.right[0] && decoder.left[0] > 0.5f, "W alone plays equally on both sides");
        const auto ahead = Decode(decoder, 0);
        Require(Near(ahead.left, ahead.right) && Near(ahead.left, 1.0, 1e-3), "ahead: L == R at mode 1's level");
        const auto behind = Decode(decoder, 180);
        Require(Near(behind.left, behind.right), "behind: L == R");
        Require(PowerDb(behind) < PowerDb(ahead) - 1.5, "behind is quieter than ahead");
        const auto left = Decode(decoder, 90);
        const auto right = Decode(decoder, -90);
        Require(Near(left.left, right.right) && Near(left.right, right.left), "the image is mirrored");
        Require(Db(left.left) - Db(left.right) >= (mode == 2 ? 12.0 : 6.0), "+90 degrees plays on the left");
        for (double side : {30.0, -30.0}) Require(std::abs(PowerDb(Decode(decoder, side)) - PowerDb(ahead)) <= 3.0, "the speakers' directions keep the level ahead");
        Require(Decode(decoder, 30).left > Decode(decoder, 30).right, "+30 degrees leans left");
        const auto up = Decode(decoder, 0, 90);
        Require(Near(up.left, up.right) && up.left > 0.3, "overhead plays on both sides");
        if (mode == 1) {
            for (std::uint32_t acn = 4; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) Require(decoder.left[acn] == 0.0f && decoder.right[acn] == 0.0f, "mode 1 mutes orders >= 2");
            Require(decoder.left[2] == 0.0f && Near(decoder.left[0], 0.8536) && Near(decoder.left[3], 0.1464) && Near(decoder.left[1], 0.5210) && Near(decoder.right[1], -0.5210), "first-order gains");
        } else {
            Require(decoder.left[8] != 0.0f && decoder.left[15] != 0.0f, "mode 2 keeps higher orders");
            for (std::uint32_t acn = 0; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) Require(std::isfinite(decoder.left[acn]) && std::abs(decoder.left[acn]) < 1.0f, "finite gains");
        }

        float l = 0.0f;
        float r = 0.0f;
        AudioOut2MonoGains(decoder, 0x41, 0, l, r);
        Require(l == decoder.left[1] && r == decoder.right[1], "an ambisonic port takes its channel's gains");
        AudioOut2MonoGains(decoder, 0, AUDIO_OUT2_PASSTHROUGH_LEFT, l, r);
        Require(l == 1.0f && r == 0.0f, "passthrough 1 is the left speaker");
        AudioOut2MonoGains(decoder, 0, AUDIO_OUT2_PASSTHROUGH_RIGHT, l, r);
        Require(l == 0.0f && r == 1.0f, "passthrough 2 is the right speaker");
        AudioOut2MonoGains(decoder, 0, 0, l, r);
        Require(l == 1.0f && r == 1.0f, "an unset attribute gives the old gains");
        AudioOut2MonoGains(decoder, 0x2, 0, l, r);
        Require(l == 1.0f && r == 1.0f, "a FuMa-style value is not decoded");
    }

    float l = 0.0f;
    float r = 0.0f;
    AudioOut2MonoGains(off, 0x41, AUDIO_OUT2_PASSTHROUGH_LEFT, l, r);
    Require(l == 1.0f && r == 1.0f, "decoder off: the old sum");

    Require(AudioOut2SkipInMainMix(AUDIO_OUT2_PORT_TYPE_PAD_VIBRATION, 2, false), "vibration without a card is skipped");
    Require(AudioOut2SkipInMainMix(AUDIO_OUT2_PORT_TYPE_PAD_VIBRATION, 1, false), "mono vibration without a card is skipped");
    Require(!AudioOut2SkipInMainMix(AUDIO_OUT2_PORT_TYPE_PAD_VIBRATION, 2, true), "APS5_AUDIO2_HAPTICS_MAIN=1 keeps it in the speakers");
    Require(!AudioOut2SkipInMainMix(AUDIO_OUT2_PORT_TYPE_PAD_SPEAKER, 1, false), "pad speaker sounds stay in the main mix");
    Require(!AudioOut2SkipInMainMix(0x100, 1, false) && !AudioOut2SkipInMainMix(0, 8, false), "game ports stay");
    Require(!AudioOut2SkipInMainMix(AUDIO_OUT2_PORT_TYPE_PAD_VIBRATION, 8, false), "an 8-channel type 0x6 is not a vibration port");

    // The switch path as the mixer takes it: value -> decoder mode -> master gain -> port gains.
    struct ModeCase {
        const char* value;
        std::uint32_t mode;
        bool valid;
    };
    for (const ModeCase& c : {ModeCase{nullptr, 0, true}, ModeCase{"", 0, true}, ModeCase{"0", 0, true}, ModeCase{"1", 1, true},
             ModeCase{"2", 2, true}, ModeCase{"3", 0, false}, ModeCase{"5", 0, false}, ModeCase{"-1", 0, false},
             ModeCase{"1x", 0, false}, ModeCase{"on", 0, false}, ModeCase{"4294967297", 0, false}}) {
        bool valid = false;
        const auto mode = AudioOut2ParseAmbisonicMode(c.value, valid);
        Require(mode == c.mode && valid == c.valid, "APS5_AUDIO2_AMBISONICS parse");
        const auto decoder = AudioOut2MakeAmbisonicDecoder(mode);
        Require(decoder.mode == c.mode, "the decoder takes the parsed mode");
        bool gainValid = false;
        const float gain = AudioOut2MasterGain(decoder.mode, nullptr, gainValid);
        Require(gainValid && gain == (c.mode != 0 ? AUDIO_OUT2_DECODED_GAIN : AUDIO_OUT2_SUM_GAIN), "the gain follows the decoder's mode");
        if (c.mode == 0) {
            AudioOut2MonoGains(decoder, 0x41, 0, l, r);
            Require(l == 1.0f && r == 1.0f && gain == 0.5f, "a refused mode is the old sum at the old level");
        }
    }

    struct GainCase {
        const char* value;
        float gain;
        bool valid;
    };
    for (const GainCase& c : {GainCase{"0.25", 0.25f, true}, GainCase{"2", 2.0f, true}, GainCase{"16", 16.0f, true}, GainCase{"", 0.5f, true},
             GainCase{"0", 0.5f, false}, GainCase{"-1", 0.5f, false}, GainCase{"loud", 0.5f, false}, GainCase{"0.5dB", 0.5f, false},
             GainCase{"nan", 0.5f, false}, GainCase{"inf", 0.5f, false}, GainCase{"1e30", 0.5f, false}, GainCase{"17", 0.5f, false}}) {
        bool valid = false;
        const float gain = AudioOut2MasterGain(0, c.value, valid);
        Require(gain == c.gain && valid == c.valid, "APS5_AUDIO2_GAIN on the old sum");
        Require(c.valid || AudioOut2MasterGain(2, c.value, valid) == AUDIO_OUT2_DECODED_GAIN, "a refused gain keeps the decoded gain");
    }

    std::printf("audio_out2_ambisonics_tests: ok\n");
    return 0;
}
