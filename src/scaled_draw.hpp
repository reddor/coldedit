#pragma once
#include "imgui.h"
#include <cmath>

namespace coldedit {
// Custom canvas geometry is expressed in 96-DPI logical units. ImGui widgets
// receive scaled sizes too; input remains in native GLFW/ImGui coordinates.
struct ScaledDrawList {
    ImDrawList* list;
    float scale;
    ImVec2 point(ImVec2 p) const { return {p.x * scale, p.y * scale}; }
    void AddRectFilled(ImVec2 a, ImVec2 b, ImU32 color, float rounding = 0) const {
        list->AddRectFilled(point(a), point(b), color, rounding * scale);
    }
    void AddRect(ImVec2 a, ImVec2 b, ImU32 color, float rounding = 0, ImDrawFlags flags = 0, float thickness = 1) const {
        list->AddRect(point(a), point(b), color, rounding * scale, flags, thickness * scale);
    }
    void AddLine(ImVec2 a, ImVec2 b, ImU32 color, float thickness = 1) const {
        list->AddLine(point(a), point(b), color, thickness * scale);
    }
    void AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 color) const {
        list->AddTriangleFilled(point(a), point(b), point(c), color);
    }
    void AddCircleFilled(ImVec2 center, float radius, ImU32 color) const {
        list->AddCircleFilled(point(center), radius * scale, color);
    }
    void AddWaveformColumn(int pixel, float top, float bottom, ImU32 color, float raster, float coverage = 1) const {
        // A 1.5-physical-pixel stroke on the fixed column grid. Its extra width
        // covers 1/2 of each adjacent pixel, softening steep edges without a
        // zoom-dependent stroke or rasterizer rounding of subpixel rectangles.
        constexpr float sideCoverage = .15f;
        // Analytic vertical coverage keeps silence and DC visible at all zooms.
        const float a = top * raster - .5f, b = bottom * raster + .5f;
        const float first = std::floor(a), last = std::floor(b);
        const float toDraw = scale / raster;
        const auto rect = [&](float y0, float y1, float alpha) {
            if (y1 <= y0) return;
            for (int offset = -1; offset <= 1; ++offset) {
                const float horizontal = offset == 0 ? 1.f : sideCoverage;
                const auto opacity = ImU32(std::lround(float((color & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT) * coverage * alpha * horizontal));
                if (!opacity) continue;
                list->AddRectFilled({(pixel + offset) * toDraw, y0 * toDraw}, {(pixel + offset + 1) * toDraw, y1 * toDraw},
                                    (color & ~IM_COL32_A_MASK) | (opacity << IM_COL32_A_SHIFT));
            }
        };
        if (first == last) rect(first, first + 1, b - a);
        else {
            rect(first, first + 1, first + 1 - a);
            rect(first + 1, last, 1);
            rect(last, last + 1, b - last);
        }
    }
    void AddEnvelope(const ImVec2* upper, const ImVec2* lower, int count, ImU32 color, float edgeWidth) const {
        if (count < 2) return;
        // One connected strip: no antialiased seams between individual columns.
        list->PrimReserve((count - 1) * 6, count * 2);
        const auto base = list->_VtxCurrentIdx;
        for (int i = 0; i < count - 1; ++i) {
            const auto v = base + unsigned(i * 2);
            for (auto index : {v, v + 2, v + 1, v + 1, v + 2, v + 3}) list->PrimWriteIdx(static_cast<ImDrawIdx>(index));
        }
        const auto uv = ImGui::GetFontTexUvWhitePixel();
        for (int i = 0; i < count; ++i) {
            list->PrimWriteVtx(point(upper[i]), uv, color);
            list->PrimWriteVtx(point(lower[i]), uv, color);
        }
        // Only the outside contours are antialiased, in physical pixel units.
        for (int i = 0; i < count; ++i) list->PathLineTo(point(upper[i]));
        list->PathStroke(color, 0, edgeWidth * scale);
        for (int i = 0; i < count; ++i) list->PathLineTo(point(lower[i]));
        list->PathStroke(color, 0, edgeWidth * scale);
    }
    void PushClipRect(ImVec2 a, ImVec2 b, bool intersect) const { list->PushClipRect(point(a), point(b), intersect); }
    void PopClipRect() const { list->PopClipRect(); }
};
}
