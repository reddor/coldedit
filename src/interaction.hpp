#pragma once
#include "audio_file.hpp"
#include <algorithm>
#include <cmath>

namespace coldedit {
// GLFW window coordinates are physical pixels on Windows, but points on Retina.
// Keep content scaling separate from framebuffer density to avoid double scaling.
struct DpiMetrics {
    float layout = 1, raster = 1, framebuffer = 1;
    static DpiMetrics from(float contentScale, float framebufferScale) {
        auto valid = [](float v) { return std::isfinite(v) && v > 0 ? std::clamp(v, .5f, 8.f) : 1.f; };
        const float content = valid(contentScale), density = valid(framebufferScale);
        return {content / density, content, density};
    }
};

enum class SelectionEdge { None, Start, End };
inline SelectionEdge hitSelectionEdge(double pointer, double start, double end, double radius = 7) {
    const double a = std::abs(pointer - start), b = std::abs(pointer - end);
    if (std::min(a, b) > radius) return SelectionEdge::None;
    return a <= b ? SelectionEdge::Start : SelectionEdge::End;
}
struct SelectionBoundaryDrag {
    bool active = false;
    Frame fixed = 0, offset = 0;
    void begin(SelectionEdge edge, Frame a, Frame b, Frame pointer) {
        const auto first = std::min(a, b), last = std::max(a, b);
        active = edge != SelectionEdge::None;
        fixed = edge == SelectionEdge::Start ? last : first;
        offset = pointer - (edge == SelectionEdge::Start ? first : last);
    }
    void update(Frame pointer, Frame total, Frame& anchor, Frame& head) const {
        anchor = std::clamp(fixed, Frame(0), total);
        head = std::clamp(pointer - offset, Frame(0), total);
    }
};
struct SelectionRangeDrag {
    bool active = false, backwards = false;
    Frame offset = 0, length = 0, limit = 0;
    void begin(Frame a, Frame b, Frame pointer, Frame total) {
        active = a != b; backwards = a > b;
        length = std::max(a, b) - std::min(a, b);
        offset = pointer - std::min(a, b);
        // A retained range may extend beyond EOF after a cut. Never shorten it.
        limit = std::max(total, std::max(a, b));
    }
    void update(Frame pointer, Frame& anchor, Frame& head) const {
        const Frame start = std::clamp(pointer - offset, Frame(0), limit - length);
        anchor = backwards ? start + length : start;
        head = backwards ? start : start + length;
    }
};
}
