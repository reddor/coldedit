#pragma once
#include "imgui.h"

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
    void PushClipRect(ImVec2 a, ImVec2 b, bool intersect) const { list->PushClipRect(point(a), point(b), intersect); }
    void PopClipRect() const { list->PopClipRect(); }
};
}
