#pragma once
#include "document.hpp"
#include <algorithm>
#include <cmath>

namespace coldedit {
struct WaveformBuild {
    bool ready = true, building = false;
    float progress = 1;
};
// Include inserted and processed sources, not just the originally opened file.
inline WaveformBuild waveformBuild(const Timeline* timeline) {
    WaveformBuild state;
    double done = 0, total = 0;
    if (timeline) for (const auto& piece : timeline->pieces) {
        if (!piece.source || !piece.length) continue;
        const bool ready = piece.source->peaksReady();
        const float progress = piece.source->peakProgress();
        state.ready &= ready;
        state.building |= !ready && progress >= 0;
        total += double(piece.length);
        done += double(piece.length) * (ready ? 1 : std::clamp(progress, 0.f, 1.f));
    }
    state.progress = total ? float(done / total) : 1;
    return state;
}
// Extrema of the linearly interpolated waveform over a pixel's sample interval.
// Keep fractional edges: rounding outwards makes whole peaks pop in and out as
// the viewport moves. Interior samples still use the source peak pyramids (and
// the timeline's conservative bounds for non-destructive fades/channel mixes).
inline Peak waveformPeak(const Timeline& timeline, double first, double last, unsigned channel, bool peaksReady = true) noexcept {
    if (timeline.frames == 0 || channel >= timeline.channels || !(first <= last)) return {};
    // Until the cache is published, only draw actual sample detail. Isolated
    // endpoint samples at wide zoom alias badly and flicker with every zoom step.
    // This also bounds drawing-thread work while the worker scans a large file.
    if (!peaksReady && last - first > 4) return {};
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
