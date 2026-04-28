/**
 * xr_timing.h - Shared OpenXR timing signals
 *
 * Published by the render thread after each xrWaitFrame(), read by the
 * render thread's presentation-latency measurement to extend the
 * "presentation" stage to cover all the way to predicted photon emission.
 *
 * Contribution 2 of the IEEE Telepresence 2026 paper: extend the
 * GStreamer-embedded pipeline probes with the runtime's predicted display
 * time so the instrumentation chain covers the display-side latency.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace XrTiming {
    // Raw OpenXR predictedDisplayTime for the current frame. On Android/Quest
    // this is CLOCK_MONOTONIC nanoseconds, so CLOCK_MONOTONIC can be queried
    // at any later instant and subtracted to get the remaining time to photons.
    inline std::atomic<int64_t> predictedDisplayTimeXr{0};
}
