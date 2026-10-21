#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLESTEPWINDOW_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_TITLESTEPWINDOW_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

// APS5_FRAME_PROBE's digest window (TimestepPatch): the frame step the title derived itself, read once per
// flip (FUN_140b380e0's nominal = 1/refresh, seconds). A value that is not finite or lies outside
// [1 ms, 1 s] is no step (not stored yet, or another layout): counted, never summed. The speed is the
// steps' sum over the window's wall time, so steps that do not match the flip rate show (1/59.94 s steps
// at 120 flips per second run the game at 2x).
class TitleStepWindow {
public:
    static constexpr double ShortestStep = 0.001;
    static constexpr double LongestStep = 1.0;

    static bool Plausible(double seconds) { return std::isfinite(seconds) && seconds >= ShortestStep && seconds <= LongestStep; }

    void Add(double seconds) {
        if (!Plausible(seconds)) {
            ++unreadable;
            return;
        }
        shortest = steps == 0 ? seconds : std::min(shortest, seconds);
        longest = steps == 0 ? seconds : std::max(longest, seconds);
        total += seconds;
        ++steps;
    }

    // Game seconds per wall second, and the steps as rates (all 0 for an empty window).
    double Speed(double wallSeconds) const { return wallSeconds > 0.0 ? total / wallSeconds : 0.0; }
    double MinHz() const { return steps == 0 ? 0.0 : 1.0 / longest; }
    double MaxHz() const { return steps == 0 ? 0.0 : 1.0 / shortest; }
    std::uint64_t Steps() const { return steps; }
    std::uint64_t Unreadable() const { return unreadable; }

private:
    double total = 0.0;
    double shortest = 0.0;
    double longest = 0.0;
    std::uint64_t steps = 0;
    std::uint64_t unreadable = 0;
};

#endif
