# Keep the upstream GLFW platform backend, with its mouse positions expressed
# in our physical-pixel ImGui coordinate space. CSS coordinates are fractional
# at browser zoom levels such as 110%; rendering ImGui in CSS space causes its
# integer text snapping to land between framebuffer pixels.
file(READ "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" web_glfw_source)
foreach(required "io.AddMousePosEvent(" "io.AddMouseButtonEvent(button, action == GLFW_PRESS);" "bd->LastValidMousePos = io.MousePos;" "glfwSetCursorPos(window, (double)io.MousePos.x, (double)io.MousePos.y);")
  string(FIND "${web_glfw_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Review the web input adapter for this ImGui version: ${required}")
  endif()
endforeach()
string(REPLACE "io.AddMousePosEvent(" "ColdEditWebMousePosition(io, " web_glfw_source "${web_glfw_source}")
string(REPLACE "io.AddMouseButtonEvent(button, action == GLFW_PRESS);"
  "ColdEditWebMouseButton(io, button, action == GLFW_PRESS);"
  web_glfw_source "${web_glfw_source}")
string(REPLACE "bd->LastValidMousePos = io.MousePos;"
  "bd->LastValidMousePos = ImVec2(io.MousePos.x / coldeditMouseScale.x, io.MousePos.y / coldeditMouseScale.y);"
  web_glfw_source "${web_glfw_source}")
string(REPLACE "glfwSetCursorPos(window, (double)io.MousePos.x, (double)io.MousePos.y);"
  "glfwSetCursorPos(window, io.MousePos.x / coldeditMouseScale.x, io.MousePos.y / coldeditMouseScale.y);"
  web_glfw_source "${web_glfw_source}")
set(web_glfw_prefix [=[
#include "imgui.h"
#include "imgui_internal.h"
#include <emscripten.h>
#include <cfloat>
#include <deque>
static ImVec2 coldeditMouseScale(1, 1);
struct ColdEditWebClick { unsigned eventId; int count; };
static std::deque<ColdEditWebClick> coldeditClicks;
void ColdEditWebInputScale(float x, float y) { coldeditMouseScale = ImVec2(x, y); }
static void ColdEditWebMouseButton(ImGuiIO& io, int button, bool down) {
    auto& events = ImGui::GetCurrentContext()->InputEventsQueue;
    const int previousSize = events.Size;
    io.AddMouseButtonEvent(button, down);
    if (!down || events.Size == previousSize) return;
    const int count = EM_ASM_INT({ return window.coldeditWeb.mouseClickCount || 0; });
    if (count > 0) coldeditClicks.push_back({events.back().EventId, count});
}
void ColdEditWebApplyClickCounts() {
    auto& context = *ImGui::GetCurrentContext();
    // Browser click counts use event timing. ImGui's frame-time detection can
    // miss a real double-click when software rendering delays queued presses.
    // Associate the count with its input event so trickling still works.
    for (const auto& event : context.InputEventsTrail) {
        while (!coldeditClicks.empty() && coldeditClicks.front().eventId <= event.EventId) {
            const auto click = coldeditClicks.front(); coldeditClicks.pop_front();
            if (click.eventId != event.EventId || event.Type != ImGuiInputEventType_MouseButton) continue;
            const int button = event.MouseButton.Button;
            if (!event.MouseButton.Down || !context.IO.MouseClicked[button]) continue;
            context.IO.MouseClickedCount[button] = context.IO.MouseClickedLastCount[button] = click.count;
            context.IO.MouseDoubleClicked[button] = click.count == 2;
        }
    }
}
static void ColdEditWebMousePosition(ImGuiIO& io, float x, float y) {
    // Preserve ImGui's unavailable-pointer sentinel without overflowing it.
    io.AddMousePosEvent(x <= -FLT_MAX ? x : x * coldeditMouseScale.x,
                       y <= -FLT_MAX ? y : y * coldeditMouseScale.y);
}
]=])
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/imgui_impl_glfw_web.cpp"
  CONTENT "${web_glfw_prefix}\n${web_glfw_source}" @ONLY)
set(coldedit_glfw_backend "${CMAKE_CURRENT_BINARY_DIR}/imgui_impl_glfw_web.cpp")
