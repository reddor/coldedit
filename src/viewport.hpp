#pragma once
#include <algorithm>
#include <cmath>

namespace coldedit {
// All positions are double precision sample frames, even for multi-hour files.
struct Viewport {
    double start = 0, span = 1, targetStart = 0, targetSpan = 1, total = 1;
    void bounds(double frames) { total = std::max(1.0, frames); clamp(targetStart, targetSpan); clamp(start, span); }
    void clamp(double& left, double& width) const { width = std::clamp(width, std::min(16.0, total), total); left = std::clamp(left, 0.0, std::max(0.0, total - width)); }
    void fit(double frames) { total = std::max(1.0, frames); start = targetStart = 0; span = targetSpan = total; }
    void show(double left, double width, bool instant = false) { targetStart = left; targetSpan = width; clamp(targetStart, targetSpan); if (instant) { start = targetStart; span = targetSpan; } }
    void zoom(double factor, double anchor) {
        anchor = std::clamp(anchor, 0.0, 1.0);
        const double frame = start + anchor * span;
        targetSpan = std::clamp(targetSpan * factor, std::min(16.0, total), total);
        targetStart = frame - anchor * targetSpan; clamp(targetStart, targetSpan);
    }
    void pan(double frames, bool instant = false) { show(targetStart + frames, targetSpan, instant); }
    void follow(double frame) {
        // Let playback advance through the view before scrolling forward.
        if (frame < start + span * (2.0 / 3.0)) return;
        // Track directly so even a view shorter than the animation's settling
        // time keeps the playhead visible. Zoom still eases through update().
        start = frame - span * (2.0 / 3.0);
        targetStart = frame - targetSpan * (2.0 / 3.0);
        clamp(start, span); clamp(targetStart, targetSpan);
    }
    bool update(double dt) {
        const double k = 1.0 - std::exp(-std::clamp(dt, 0.0, 0.1) * 24.0);
        start += (targetStart - start) * k; span += (targetSpan - span) * k;
        if (std::abs(targetStart - start) < .005) start = targetStart;
        if (std::abs(targetSpan - span) < .005) span = targetSpan;
        return start != targetStart || span != targetSpan;
    }
};
}
