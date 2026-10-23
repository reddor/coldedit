#pragma once
#include "document.hpp"
#include <algorithm>
#include <cmath>

namespace coldedit {
// Extrema of the linearly interpolated waveform over a pixel's sample interval.
// Keep fractional edges: rounding outwards makes whole peaks pop in and out as
// the viewport moves. Interior samples still use the source peak pyramids (and
// the timeline's conservative bounds for non-destructive fades/channel mixes).
inline Peak waveformPeak(const Timeline& timeline, double first, double last, unsigned channel) noexcept {
    if (timeline.frames == 0 || channel >= timeline.channels || !(first <= last)) return {};
    // Samples outside the document are silence, including retained selections
    // past EOF. One extra sample at either end covers the interpolated boundary.
    first = std::clamp(first, -1., double(timeline.frames));
    last = std::clamp(last, -1., double(timeline.frames));
    const auto interpolate = [&](double frame) {
        const auto i = Frame(std::floor(frame));
        return float(std::lerp(double(timeline.sample(i, channel)),
                               double(timeline.sample(i + 1, channel)), frame - double(i)));
    };
    const float a = interpolate(first), b = interpolate(last);
    Peak result{std::min(a, b), std::max(a, b)};
    const auto begin = std::max<Frame>(0, Frame(std::ceil(first)));
    const auto end = std::min(timeline.frames, Frame(std::floor(last)) + 1);
    if (begin < end) {
        const auto interior = timeline.peak(begin, end, channel);
        result.lo = std::min(result.lo, interior.lo);
        result.hi = std::max(result.hi, interior.hi);
    }
    return result;
}
}
