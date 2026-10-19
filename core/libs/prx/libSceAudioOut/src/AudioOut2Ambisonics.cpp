#include "AudioOut2Ambisonics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

static constexpr double PI = 3.14159265358979323846;
// Mode 1 (first order): two virtual microphones whose sum is 1 ahead and 1/sqrt(2) behind, with the
// lateral part of a first-order pattern of strength sqrt(1 - 1/sqrt(2)) (Kyty 604bb1fc lands on the
// same W 0.854, X 0.146, Y 0.521).
static const double FIRST_ORDER_W = (2.0 + std::sqrt(2.0)) / 4.0;
static const double FIRST_ORDER_X = (2.0 - std::sqrt(2.0)) / 4.0;
static const double FIRST_ORDER_Y = std::sqrt((2.0 - std::sqrt(2.0)) / 2.0 - FIRST_ORDER_X * FIRST_ORDER_X);
// Mode 2 integrates the panning law over the sphere on this grid: midpoints in z (equal areas) times
// azimuths that include 0 and 180 degrees.
static constexpr int GRID_HEIGHTS = 64;
static constexpr int GRID_AZIMUTHS = 128;
// max-rE for a 3D decode of order N: P_n(cos(137.9 degrees / (N + 1.51))) (Zotter and Frank).
static constexpr double MAX_RE_DEGREES = 137.9;
static constexpr double MAX_RE_OFFSET = 1.51;

std::uint32_t AudioOut2AmbisonicOrder(std::uint32_t acn) {
    return static_cast<std::uint32_t>(std::sqrt(static_cast<double>(acn)));
}

static int Degree(std::uint32_t acn) {
    const int n = static_cast<int>(AudioOut2AmbisonicOrder(acn));
    return static_cast<int>(acn) - n * n - n;
}

double AudioOut2Sn3d(std::uint32_t acn, double x, double y, double z) {
    const int n = static_cast<int>(AudioOut2AmbisonicOrder(acn));
    const int m = Degree(acn);
    const int am = std::abs(m);
    const double length = std::sqrt(x * x + y * y + z * z);
    const double sinEl = length > 0 ? z / length : 0;
    const double cosEl = std::sqrt(std::max(0.0, 1 - sinEl * sinEl));
    const double azimuth = std::atan2(y, x);
    double legendre = 1;
    for (int i = 1; i <= am; ++i) legendre *= (2 * i - 1) * cosEl;
    if (n > am) {
        double previous = legendre;
        double current = sinEl * (2 * am + 1) * legendre;
        for (int l = am + 2; l <= n; ++l) {
            const double next = (sinEl * (2 * l - 1) * current - (l + am - 1) * previous) / (l - am);
            previous = current;
            current = next;
        }
        legendre = current;
    }
    double ratio = 1;
    for (int i = n - am + 1; i <= n + am; ++i) ratio /= i;
    const double norm = std::sqrt((m == 0 ? 1.0 : 2.0) * ratio);
    return norm * legendre * (m < 0 ? std::sin(am * azimuth) : std::cos(am * azimuth));
}

int AudioOut2AmbisonicChannel(std::uint32_t attribute) {
    if ((attribute & ~0xFFu) != 0 || (attribute & AUDIO_OUT2_AMBISONIC_FLAG) == 0) return -1;
    const auto acn = attribute & ~AUDIO_OUT2_AMBISONIC_FLAG;
    return acn < AUDIO_OUT2_AMBISONIC_CHANNELS ? static_cast<int>(acn) : -1;
}

// The left speaker's share of a source from a unit direction: a constant-power pan on the lateral
// component, times a level that falls from 1 ahead to 1/sqrt(2) behind (mode 1's front/back ratio).
static double PanLeft(double x, double y) {
    const double level = FIRST_ORDER_W + FIRST_ORDER_X * x;
    return level * std::cos(PI / 4 * (1 - std::clamp(y, -1.0, 1.0)));
}

static void MakeFullOrder(AudioOut2AmbisonicDecoder& decoder) {
    double weights[AUDIO_OUT2_AMBISONIC_ORDER_MAX + 1];
    const double c = std::cos(MAX_RE_DEGREES / (AUDIO_OUT2_AMBISONIC_ORDER_MAX + MAX_RE_OFFSET) * PI / 180);
    weights[0] = 1;
    weights[1] = c;
    for (std::uint32_t n = 1; n < AUDIO_OUT2_AMBISONIC_ORDER_MAX; n++) weights[n + 1] = ((2 * n + 1) * c * weights[n] - n * weights[n - 1]) / (n + 1);
    double left[AUDIO_OUT2_AMBISONIC_CHANNELS] = {};
    for (int height = 0; height < GRID_HEIGHTS; height++) {
        const double z = -1 + (height + 0.5) * 2 / GRID_HEIGHTS;
        const double radius = std::sqrt(1 - z * z);
        for (int step = 0; step < GRID_AZIMUTHS; step++) {
            const double azimuth = 2 * PI * step / GRID_AZIMUTHS;
            const double x = radius * std::cos(azimuth);
            const double y = radius * std::sin(azimuth);
            const double gain = PanLeft(x, y);
            for (std::uint32_t acn = 0; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) left[acn] += gain * AudioOut2Sn3d(acn, x, y, z);
        }
    }
    double ahead = 0;
    for (std::uint32_t acn = 0; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) {
        const auto n = AudioOut2AmbisonicOrder(acn);
        left[acn] *= (2 * n + 1) * weights[n] / (GRID_HEIGHTS * GRID_AZIMUTHS);
        ahead += left[acn] * AudioOut2Sn3d(acn, 1, 0, 0);
    }
    const double scale = (FIRST_ORDER_W + FIRST_ORDER_X) / ahead;
    for (std::uint32_t acn = 0; acn < AUDIO_OUT2_AMBISONIC_CHANNELS; acn++) {
        // Mirroring left and right flips the sine harmonics only.
        decoder.left[acn] = static_cast<float>(left[acn] * scale);
        decoder.right[acn] = Degree(acn) < 0 ? -decoder.left[acn] : decoder.left[acn];
    }
}

AudioOut2AmbisonicDecoder AudioOut2MakeAmbisonicDecoder(std::uint32_t mode) {
    AudioOut2AmbisonicDecoder decoder;
    if (mode == 1) {
        decoder.mode = mode;
        decoder.left[0] = decoder.right[0] = static_cast<float>(FIRST_ORDER_W);
        decoder.left[1] = static_cast<float>(FIRST_ORDER_Y);
        decoder.right[1] = -decoder.left[1];
        decoder.left[3] = decoder.right[3] = static_cast<float>(FIRST_ORDER_X);
    } else if (mode == 2) {
        decoder.mode = mode;
        MakeFullOrder(decoder);
    }
    return decoder;
}

void AudioOut2MonoGains(const AudioOut2AmbisonicDecoder& decoder, std::uint32_t ambisonic, std::uint32_t passthrough, float& left, float& right) {
    left = right = 1.0f;
    if (decoder.mode == 0) return;
    const int acn = AudioOut2AmbisonicChannel(ambisonic);
    if (acn >= 0) {
        left = decoder.left[acn];
        right = decoder.right[acn];
    } else if (passthrough == AUDIO_OUT2_PASSTHROUGH_LEFT) {
        right = 0.0f;
    } else if (passthrough == AUDIO_OUT2_PASSTHROUGH_RIGHT) {
        left = 0.0f;
    }
}
