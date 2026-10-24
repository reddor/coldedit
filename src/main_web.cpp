#include "editor.hpp"
#include "platform.hpp"
#include "web_limits.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <memory>
#include <cstdio>

// Implemented by the narrowly adapted upstream GLFW backend (cmake/WebImGui.cmake).
void ColdEditWebInputScale(float x, float y);
void ColdEditWebApplyClickCounts();
namespace {
GLFWwindow* window = nullptr;
std::unique_ptr<coldedit::Editor> editor;
EM_JS(void, ready, (), { window.coldeditWeb.ready(); });
EM_JS(void, failed, (const char* message), { window.coldeditWeb.fail(UTF8ToString(message)); });
EM_JS(void, surfaceSize, (double* values), {
    const s = window.coldeditWeb.surface();
    HEAPF64.set([s.width, s.height, s.cssWidth, s.cssHeight, s.dpr], values / 8);
});
void frame() {
    try {
        glfwPollEvents();
        double surface[5]; surfaceSize(surface);
        const int fw = int(surface[0]), fh = int(surface[1]);
        const float density = float(surface[4]);
        int w, h; glfwGetWindowSize(window, &w, &h);
        if (w != fw || h != fh) glfwSetWindowSize(window, fw, fh);
        int actualW, actualH; emscripten_get_canvas_element_size("#canvas", &actualW, &actualH);
        if (actualW != fw || actualH != fh) emscripten_set_canvas_element_size("#canvas", fw, fh);
        // Render and snap text in physical pixels, as on Windows. Only pointer
        // coordinates cross from CSS space; wheel amounts remain logical units.
        ColdEditWebInputScale(float(fw / std::max(1., surface[2])), float(fh / std::max(1., surface[3])));
        static float previousDensity = 0;
        if (previousDensity != density && glfwGetWindowAttrib(window, GLFW_HOVERED)) {
            double x, y; glfwGetCursorPos(window, &x, &y);
            ImGui_ImplGlfw_CursorPosCallback(window, x, y);
        }
        previousDensity = density;
        editor->updateDpi(density, 1);
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
        ImGui::GetIO().DisplaySize = {float(fw), float(fh)};
        ImGui::GetIO().DisplayFramebufferScale = {1, 1};
        ImGui::NewFrame();
        ColdEditWebApplyClickCounts();
        editor->render(); ImGui::Render();
        glViewport(0, 0, fw, fh); glClearColor(.06f, .08f, .10f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // The browser presents the WebGL canvas after this animation callback.
        static bool first = true;
        if (first) { first = false; ready(); }
    } catch (const std::exception& e) { failed(e.what()); emscripten_cancel_main_loop(); }
}
}

extern "C" {
EMSCRIPTEN_KEEPALIVE int coldedit_can_import() { return editor && editor->canImport(); }
EMSCRIPTEN_KEEPALIVE void coldedit_open(const char* path) { if (editor) editor->requestOpen(coldedit::fromUtf8(path)); }
EMSCRIPTEN_KEEPALIVE void coldedit_paste_file(const char* path) { if (editor) editor->requestPasteFile(coldedit::fromUtf8(path)); }
EMSCRIPTEN_KEEPALIVE const char* coldedit_diagnostics() {
    static std::string value; value = editor ? editor->diagnostics() : "{}"; return value.c_str();
}
EMSCRIPTEN_KEEPALIVE unsigned coldedit_input_limit() { return unsigned(coldedit::web::inputLimit); }
EMSCRIPTEN_KEEPALIVE unsigned coldedit_export_limit() { return unsigned(coldedit::web::exportLimit); }
EMSCRIPTEN_KEEPALIVE unsigned coldedit_audio_limit() { return unsigned(coldedit::web::audioLimit); }
}

int main() {
    glfwSetErrorCallback([](int code, const char* message) { std::fprintf(stderr, "GLFW %d: %s\n", code, message); });
    if (!glfwInit()) { failed("Could not initialize browser graphics."); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
    window = glfwCreateWindow(1440, 900, "ColdEdit", nullptr, nullptr);
    if (!window) { failed("WebGL 2 is required. Enable hardware acceleration or use the native app."); return 1; }
    glfwMakeContextCurrent(window);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 300 es");
    try {
        std::filesystem::create_directories("/imports");
        std::filesystem::create_directories("/exports");
        editor = std::make_unique<coldedit::Editor>(window);
        emscripten_set_main_loop(frame, 0, true);
    } catch (const std::exception& e) { failed(e.what()); return 1; }
    return 0;
}
