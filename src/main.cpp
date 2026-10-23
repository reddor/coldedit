#include "editor.hpp"
#include "platform.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <GLFW/glfw3.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
int main(int argc, char** argv) {
    using namespace coldedit;
    std::vector<std::string> args;
#ifdef _WIN32
    int wideCount = 0; auto wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    for (int i = 1; wide && i < wideCount; ++i) args.push_back(utf8(wide[i]));
    if (wide) LocalFree(wide);
    (void)argc; (void)argv;
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    bool smoke = false; std::filesystem::path openPath, screenshot, report;
    int maxFrames = 230;
    float smokeScale = 0;
    bool smokeDpiCycle = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--smoke-test") smoke = true;
        else if (args[i] == "--screenshot" && i + 1 < args.size()) screenshot = fromUtf8(args[++i]);
        else if (args[i] == "--report" && i + 1 < args.size()) report = fromUtf8(args[++i]);
        else if (args[i] == "--frames" && i + 1 < args.size()) maxFrames = std::max(230, std::atoi(args[++i].c_str()));
        else if (args[i] == "--smoke-scale" && i + 1 < args.size()) smokeScale = std::clamp(std::strtof(args[++i].c_str(), nullptr), 1.f, 3.f);
        else if (args[i] == "--smoke-dpi-cycle") smokeDpiCycle = true;
        else openPath = fromUtf8(args[i]);
    }
    glfwSetErrorCallback([](int code, const char* message) { std::fprintf(stderr, "GLFW %d: %s\n", code, message); });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_VISIBLE, smoke ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
    auto* window = glfwCreateWindow(1440, 900, "ColdEdit", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(window, 960, 660, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window); glfwSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 330 core");
    int result = 0;
    try {
        Editor editor(window); glfwSetWindowUserPointer(window, &editor);
        glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) { if (count) static_cast<Editor*>(glfwGetWindowUserPointer(w))->requestOpen(fromUtf8(paths[0])); });
        glfwSetWindowCloseCallback(window, [](GLFWwindow* w) { glfwSetWindowShouldClose(w, GLFW_FALSE); static_cast<Editor*>(glfwGetWindowUserPointer(w))->requestClose(); });
        if (smoke) editor.loadSmokeAudio(); if (!openPath.empty()) editor.requestOpen(openPath);
        std::vector<double> timings;
        int frame = 0;
        float previousLayout = 0;
        int previousWidth = 0, previousHeight = 0;
        while (!editor.shouldClose()) {
            if (!smoke && !editor.animating()) glfwWaitEventsTimeout(.2); else glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) { glfwWaitEventsTimeout(.05); continue; }
            float contentX = 1, contentY = 1;
            glfwGetWindowContentScale(window, &contentX, &contentY);
            int windowW, windowH, framebufferW, framebufferH;
            glfwGetWindowSize(window, &windowW, &windowH);
            glfwGetFramebufferSize(window, &framebufferW, &framebufferH);
            const float density = windowW > 0 ? float(framebufferW) / windowW : 1.f;
            float contentScale = std::max(contentX, contentY);
            const bool simulatedDpi = smoke && (smokeScale > 0 || smokeDpiCycle);
            if (simulatedDpi) {
                contentScale = smokeScale > 0 ? smokeScale : 1.f;
                if (smokeDpiCycle) contentScale *= frame < 60 ? 1.f : frame < 120 ? 1.5f : frame < 180 ? 2.f : 1.f;
            }
            const float nextLayout = DpiMetrics::from(contentScale, density).layout;
            const bool scaleChanged = previousLayout > 0 && std::abs(previousLayout - nextLayout) > .001f;
            // Update minimum sizes before a downscale, so old physical limits do
            // not prevent Windows from restoring the same logical client size.
            editor.updateDpi(contentScale, density);
            if (simulatedDpi && (scaleChanged || frame == 0))
                glfwSetWindowSize(window, int(1440 * nextLayout), int(900 * nextLayout));
            else if (scaleChanged && !glfwGetWindowAttrib(window, GLFW_MAXIMIZED))
                glfwSetWindowSize(window, int(previousWidth * nextLayout / previousLayout), int(previousHeight * nextLayout / previousLayout));
            glfwGetWindowSize(window, &previousWidth, &previousHeight);
            previousLayout = nextLayout;
            const auto start = std::chrono::steady_clock::now();
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame();
            if (smoke) editor.smokeInput(frame);
            ImGui::NewFrame();
            if (smoke) editor.smokeStep(frame);
            editor.render(); ImGui::Render();
            int w, h; glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h); glClearColor(.06f, .08f, .10f, 1); glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (frame > 5) timings.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (smoke && frame == maxFrames - 1 && !screenshot.empty()) saveFramebuffer(screenshot, w, h);
            glfwSwapBuffers(window); ++frame;
            if (smoke && frame >= maxFrames) {
                if (!editor.smokePassed()) result = 2;
                std::ofstream out(report.empty() ? "coldedit-smoke.json" : report);
                auto diagnostic = editor.diagnostics(); diagnostic.erase(diagnostic.find_last_of('}'));
                std::sort(timings.begin(), timings.end());
                out << diagnostic << ",  \"render_cpu_ms_median\": " << timings[timings.size() / 2]
                    << ",\n  \"render_cpu_ms_p95\": " << timings[std::size_t(timings.size() * .95)]
                    << ",\n  \"opengl\": \"" << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\"\n}\n";
                break;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ColdEdit: %s\n", e.what()); result = 1;
#ifdef _WIN32
        if (!smoke) MessageBoxA(nullptr, e.what(), "ColdEdit", MB_ICONERROR);
#endif
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate(); return result;
}
