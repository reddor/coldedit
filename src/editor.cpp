#include "editor.hpp"
#include "platform.hpp"
#include "waveform.hpp"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <limits>
#include <utility>
#include <unordered_set>
#ifdef __EMSCRIPTEN__
#include "web_limits.hpp"
#include "imgui_internal.h"
#endif

namespace coldedit {
namespace {
constexpr ImU32 bg = IM_COL32(16, 20, 25, 255), panel = IM_COL32(22, 27, 33, 255), line = IM_COL32(43, 50, 59, 255);
constexpr ImU32 text = IM_COL32(225, 231, 236, 255), muted = IM_COL32(135, 148, 161, 255);
constexpr ImU32 teal = IM_COL32(98, 222, 193, 255), blue = IM_COL32(115, 174, 230, 255), amber = IM_COL32(241, 189, 115, 255);
constexpr const char* exportFormats[] = {"WAV / 16-bit PCM", "WAV / 24-bit PCM", "WAV / 32-bit float", "FLAC / 16-bit", "FLAC / 24-bit", "MP3 / VBR high", "MP3 / 192 kbps", "MP3 / 320 kbps"};
std::string timecode(double frames, unsigned rate, bool precise = false) {
    const double seconds = frames / std::max(1u, rate);
    const auto ms = std::int64_t(std::llround(seconds * 1000));
    char out[80];
    if (precise) std::snprintf(out, sizeof(out), "%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    else if (seconds >= 60) std::snprintf(out, sizeof(out), "%lld:%05.2f", ms / 60000, std::fmod(seconds, 60.));
    else std::snprintf(out, sizeof(out), "%.2f s", seconds);
    return out;
}
std::string decimal(double value, int digits = 1) { std::ostringstream o; o << std::fixed << std::setprecision(digits) << value; return o.str(); }
double tickStep(double seconds) {
    const double magnitude = std::pow(10., std::floor(std::log10(std::max(.000001, seconds))));
    for (double factor : {1., 2., 5., 10.}) if (factor * magnitude >= seconds) return factor * magnitude;
    return magnitude * 10;
}
}

Editor::Editor(GLFWwindow* window) : window_(window) {
    wheelZooms_ = loadWheelZoomPreference();
#ifdef __EMSCRIPTEN__
    web::setAudioLimit(std::uint64_t(loadWebAudioBudget()) * 1024 * 1024);
#endif
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto& s = ImGui::GetStyle(); ImGui::StyleColorsDark();
    s.WindowPadding = {0, 0}; s.FramePadding = {12, 8}; s.ItemSpacing = {8, 8};
    s.WindowRounding = 0; s.ChildRounding = 8; s.FrameRounding = 5; s.PopupRounding = 8;
    s.FrameBorderSize = 0; s.PopupBorderSize = 1; s.ScrollbarSize = 10; s.GrabRounding = 4;
    s.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(bg);
    s.Colors[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(panel);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.10f, .13f, .16f, 1);
    s.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(text);
    s.Colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(muted);
    s.Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(line);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.12f, .15f, .18f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.17f, .22f, .26f, 1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.18f, .29f, .29f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(.15f, .19f, .22f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.21f, .29f, .31f, 1);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(.24f, .37f, .35f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(.17f, .3f, .29f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.2f, .36f, .32f, 1);
    s.Colors[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(teal);
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(.6f, 1, .85f, 1);
    s.Colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(teal);
    s.Colors[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(line);
    baseStyle_ = s;
    resident_ = residentBytes();
    if (!playback_.available()) status_ = playback_.error();
}
void Editor::updateDpi(float contentScale, float framebufferScale) {
    const auto next = DpiMetrics::from(contentScale, framebufferScale);
    if (fontsLoaded_ && std::abs(next.layout - dpi_.layout) < .001f && std::abs(next.raster - dpi_.raster) < .001f) return;
    dpi_ = next; ++dpiChanges_;
    auto& io = ImGui::GetIO();
    // Reset from the original style; repeated monitor changes must not compound.
    ImGui::GetStyle() = baseStyle_;
    ImGui::GetStyle().ScaleAllSizes(dpi_.layout);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    bodyFont_ = titleFont_ = monoFont_ = nullptr;
    const auto fontPixels = [&](float size) { return std::floor(size * dpi_.raster); };
#ifdef _WIN32
    if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) bodyFont_ = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", fontPixels(17));
    if (std::filesystem::exists("C:/Windows/Fonts/seguisb.ttf")) titleFont_ = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", fontPixels(22));
    if (std::filesystem::exists("C:/Windows/Fonts/consola.ttf")) monoFont_ = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", fontPixels(19));
#elif defined(__EMSCRIPTEN__)
    ImFontConfig config;
    config.PixelSnapH = true; config.OversampleH = config.OversampleV = 1;
    bodyFont_ = io.Fonts->AddFontFromFileTTF("/fonts/Roboto-Medium.ttf", fontPixels(17), &config);
    titleFont_ = io.Fonts->AddFontFromFileTTF("/fonts/Roboto-Medium.ttf", fontPixels(22), &config);
    monoFont_ = io.Fonts->AddFontFromFileTTF("/fonts/Cousine-Regular.ttf", fontPixels(19), &config);
#endif
    if (!bodyFont_) { ImFontConfig config; config.SizePixels = fontPixels(17); bodyFont_ = io.Fonts->AddFontDefault(&config); }
    if (!titleFont_) titleFont_ = bodyFont_;
    if (!monoFont_) monoFont_ = bodyFont_;
    io.FontDefault = bodyFont_;
    io.FontGlobalScale = 1.f / dpi_.framebuffer;
    io.MouseDragThreshold = px(6); io.MouseDoubleClickMaxDist = px(6);
    fontsLoaded_ = true;
    overviewTimeline_ = nullptr;
#ifndef __EMSCRIPTEN__
    glfwSetWindowSizeLimits(window_, int(px(960)), int(px(660)), GLFW_DONT_CARE, GLFW_DONT_CARE);
#endif
}
Editor::~Editor() {
    job_.request_stop(); if (job_.joinable()) job_.join();
    playback_.setTimeline(nullptr);
    clipboard_.clear(); document_ = Document{}; source_.reset();
}
void Editor::label(ImVec2 p, const std::string& value, ImU32 color, ImFont* font) {
    font = font ? font : bodyFont_; ImGui::GetWindowDrawList()->AddText(font, font->FontSize * ImGui::GetIO().FontGlobalScale, px(p), color, value.c_str());
}
bool Editor::button(const char* name, ImVec2 size, bool accent) {
    if (accent) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.32f, .78f, .66f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.04f, .12f, .11f, 1)); }
    const bool result = ImGui::Button(name, px(size));
    if (accent) ImGui::PopStyleColor(2);
    return result;
}
bool Editor::iconButton(const char* id, int icon, bool active, const char* tooltip) {
    const auto p = logical(ImGui::GetCursorScreenPos());
    const bool pressed = ImGui::InvisibleButton(id, px({42, 40}));
    auto d = drawList();
    d.AddRectFilled(p, {p.x + 42, p.y + 40}, active ? teal : (ImGui::IsItemHovered() ? IM_COL32(48, 61, 69, 255) : IM_COL32(34, 42, 50, 255)), 6);
    const ImU32 col = active ? bg : text; const float x = p.x + 21, y = p.y + 20;
    if (icon == 0) d.AddTriangleFilled({x - 5, y - 8}, {x - 5, y + 8}, {x + 8, y}, col);
    if (icon == 1) { d.AddRectFilled({x - 6, y - 7}, {x - 2, y + 7}, col, 1); d.AddRectFilled({x + 2, y - 7}, {x + 6, y + 7}, col, 1); }
    if (icon == 2) d.AddRectFilled({x - 6, y - 6}, {x + 6, y + 6}, col, 1);
    if (icon == 3) { d.AddLine({x - 7, y - 7}, {x - 7, y + 7}, col, 2); d.AddTriangleFilled({x + 6, y - 7}, {x + 6, y + 7}, {x - 4, y}, col); }
    if (icon == 4) { d.AddRect({x - 9, y - 6}, {x + 9, y + 6}, col, 4, 0, 1.7f); d.AddTriangleFilled({x + 5, y - 10}, {x + 11, y - 6}, {x + 5, y - 2}, col); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}
bool Editor::toolButton(const char* id, ToolIcon icon, const char* caption, const std::string& tooltip, bool enabled, bool active) {
    const auto p = logical(ImGui::GetCursorScreenPos());
    const float height = compact_ ? 44.f : 34.f;
    const float width = *caption ? 39 + ImGui::CalcTextSize(caption).x / dpi_.layout : height;
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(.17f, .30f, .29f, 1) : ImVec4(0, 0, 0, 0));
    const bool pressed = button(id, {width, height});
    ImGui::PopStyleColor();
    const ImU32 color = ImGui::GetColorU32(active ? ImGuiCol_CheckMark : ImGuiCol_Text);
    auto d = drawList();
    const float x = p.x + height * .5f, y = p.y + height * .5f;
    auto stroke = [&](float ax, float ay, float bx, float by) { d.AddLine({x + ax, y + ay}, {x + bx, y + by}, color, 1.5f); };
    auto box = [&](float ax, float ay, float bx, float by) { d.AddRect({x + ax, y + ay}, {x + bx, y + by}, color, 1, 0, 1.5f); };
    switch (icon) {
    case ToolIcon::Undo: case ToolIcon::Redo: {
        const float flip = icon == ToolIcon::Undo ? 1.f : -1.f;
        stroke(-7 * flip, -3, 2 * flip, -3); stroke(2 * flip, -3, 6 * flip, 1);
        stroke(6 * flip, 1, 6 * flip, 5); stroke(-7 * flip, -3, -3 * flip, -7); stroke(-7 * flip, -3, -3 * flip, 1);
        break;
    }
    case ToolIcon::Cut:
        d.list->AddCircle(px({x - 5, y + 5}), px(3), color, 12, px(1.5f));
        d.list->AddCircle(px({x + 5, y + 5}), px(3), color, 12, px(1.5f));
        stroke(-3, 3, 7, -7); stroke(3, 3, -7, -7);
        break;
    case ToolIcon::Copy: box(-3, -3, 7, 8); stroke(-7, 3, -7, -7); stroke(-7, -7, 3, -7); break;
    case ToolIcon::Paste: box(-7, -5, 7, 8); d.AddRectFilled({x - 4, y - 7}, {x + 4, y - 3}, color, 1); break;
    case ToolIcon::Delete:
        stroke(-8, -5, 8, -5); stroke(-3, -8, 3, -8); stroke(-6, -3, -5, 8); stroke(-5, 8, 5, 8); stroke(5, 8, 6, -3);
        stroke(-2, -2, -2, 5); stroke(2, -2, 2, 5); break;
    case ToolIcon::Trim:
        stroke(-5, -8, -5, 5); stroke(-8, -5, 8, -5); stroke(-5, 5, 8, 5); stroke(5, -5, 5, 8); break;
    case ToolIcon::Fit:
        stroke(-8, -7, -8, 7); stroke(8, -7, 8, 7); stroke(-5, 0, 5, 0);
        stroke(-5, 0, -2, -3); stroke(-5, 0, -2, 3); stroke(5, 0, 2, -3); stroke(5, 0, 2, 3); break;
    case ToolIcon::Selection:
        box(-8, -7, 3, 4); stroke(-5, -1, -2, -1); stroke(6, 6, 9, 9);
        d.list->AddCircle(px({x + 3, y + 3}), px(4), color, 16, px(1.5f)); break;
    }
    if (*caption) label({p.x + 32, p.y + (34 - ImGui::GetFontSize() / dpi_.layout) * .5f}, caption, color);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip.c_str());
    return pressed;
}
void Editor::requestOpen(const std::filesystem::path& p) {
    if (busy_) { status_ = "Finish or cancel the current operation before opening another file."; return; }
    if (document_.dirty()) { pendingPath_ = p; pendingClose_ = false; confirmDiscard_ = true; }
    else openNow(p);
}
void Editor::openNow(const std::filesystem::path& p) {
    error_.clear();
    playback_.pause(); opening_ = true;
    auto result = std::make_shared<std::shared_ptr<AudioFile>>();
    startJob("Opening audio", [this, p, result](std::stop_token stop) {
        *result = AudioFile::open(p, &jobProgress_, stop);
        if (stop.stop_requested()) { result->reset(); throw std::runtime_error("Import cancelled."); }
    }, [this, p, result] { acceptFile(std::move(*result), p); });
}
void Editor::acceptFile(std::shared_ptr<AudioFile> file, const std::filesystem::path& p) {
#ifdef __EMSCRIPTEN__
        // Do not retain another document's audio through the browser clipboard.
        clipboard_.clear();
#endif
        playback_.pause(); document_.open(file); source_ = std::make_unique<SourceInfo>(file); sourcePath_ = p;
        title_ = utf8(p.filename());
        cursor_ = anchor_ = head_ = 0;
        selecting_ = boundaryDrag_.active = rangeDrag_.active = false; view_.fit(double(source_->frames));
        playback_.setTimeline(document_.timeline());
        status_ = "Opened " + title_; overviewTimeline_ = waveformTimeline_ = nullptr;
}
void Editor::loadSmokeAudio() {
    // Only the native --smoke-test harness creates this disposable test signal.
    auto audio = AudioFile::render(48000 * 24, 48000, 2, [](Frame at, Frame count, float* samples) {
        for (Frame i = 0; i < count; ++i) for (unsigned c = 0; c < 2; ++c)
            samples[i * 2 + c] = float(.4 * std::sin((at + i) * .03 + c));
    }, nullptr, {});
    acceptFile(std::move(audio), "smoke-test.wav");
    wheelZooms_ = true;
}
void Editor::requestClose() {
    if (busy_) { error_ = "An operation is running. Cancel it or wait for it to finish before closing."; return; }
    if (document_.dirty()) { pendingClose_ = true; pendingPath_.clear(); confirmDiscard_ = true; }
    else close_ = true;
}
void Editor::openDialog() {
#ifdef __EMSCRIPTEN__
    webOpenPicker();
#elif defined(_WIN32)
    auto path = fileDialog(window_, false); if (!path.empty()) requestOpen(path);
#else
    openPathPopup_ = true;
#endif
}
void Editor::exportDialog(bool selectionOnly) {
    if (!document_.timeline() || busy_ || (selectionOnly && !selected())) return;
    selectionExport_ = selectionOnly;
    const std::string suffix = std::string(selectionOnly ? "-selection" : "-edited") + exportExtension(static_cast<ExportFormat>(exportFormat_));
#ifdef __EMSCRIPTEN__
    auto name = fromUtf8(utf8(sourcePath_.stem()) + suffix);
    exportTo(std::filesystem::path("/exports") / name.filename(), selectionOnly);
#elif defined(_WIN32)
    auto suggested = sourcePath_.parent_path() / fromUtf8(utf8(sourcePath_.stem()) + suffix);
    auto path = fileDialog(window_, true, suggested); if (!path.empty()) exportTo(path, selectionOnly); else continueAfterExport_ = false;
#else
    exportPathPopup_ = true;
#endif
}
void Editor::startJob(const std::string& name, std::function<void(std::stop_token)> task, std::function<void()> done, std::function<void()> retry) {
    if (busy_) return;
    if (job_.joinable()) job_.join();
    busy_.store(true); jobDone_.store(false); jobProgress_.store(0); jobLabel_ = name; jobError_.clear(); jobSuccess_.clear(); onJobDone_ = std::move(done);
    onJobRetry_ = std::move(retry); jobMemoryNeeded_ = 0;
    const bool sync = syncPlayback_; syncPlayback_ = false;
    job_ = std::jthread([this, task = std::move(task), sync](std::stop_token stop) {
        try {
            if (sync && !playback_.waitForTimeline(stop)) throw std::runtime_error("Processing cancelled.");
            task(stop);
        }
#ifdef __EMSCRIPTEN__
        catch (const web::AudioBudgetExceeded& e) { jobError_ = e.what(); jobMemoryNeeded_ = e.additionalBytes; }
#endif
        catch (const std::exception& e) { std::lock_guard lock(jobMutex_); jobError_ = e.what(); }
        jobDone_.store(true, std::memory_order_release);
    });
}
std::uint64_t Editor::reclaimableAudioBytes() const {
    auto retained = clipboard_;
    for (const auto& t : {cleanupBase_, std::shared_ptr<const Timeline>(cleanupPreview_)})
        if (t) retained.insert(retained.end(), t->pieces.begin(), t->pieces.end());
    return document_.reclaimableAudioBytes(retained);
}
std::size_t Editor::currentPeakBytes() const {
    const auto t = document_.timeline(); if (!t) return 0;
    std::unordered_set<const AudioFile*> counted;
    std::size_t bytes = 0;
    for (const auto& p : t->pieces) if (p.source && counted.insert(p.source.get()).second) bytes += p.source->peakBytes();
    return bytes;
}
#ifdef __EMSCRIPTEN__
void Editor::audioBudgetSetting() {
    const auto used = AudioFile::audioBytes();
    for (const unsigned mib : {128u, 256u, 512u}) {
        const auto bytes = std::uint64_t(mib) * 1024 * 1024;
        const auto label = std::to_string(mib) + (mib == 128 ? " MiB (default)" : " MiB");
        ImGui::BeginDisabled(busy_ || used > bytes);
        if (ImGui::RadioButton(label.c_str(), web::audioLimit.load() == bytes) && web::setAudioLimit(bytes)) {
            status_ = saveWebAudioBudget(mib) ? "Web audio budget: " + std::to_string(mib) + " MiB" : "Audio budget changed for this session; could not save it.";
        }
        ImGui::EndDisabled();
        if (used > bytes && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Retained audio must fit before lowering the budget.");
    }
    ImGui::TextDisabled("Audio in use: %.1f MiB", used / 1048576.);
}
void Editor::clearAudioBuffers() {
    const auto current = document_.timeline();
    if (busy_ || !current) return;
    const bool playing = playback_.playing();
    cursor_ = playback_.position();
    // Retire any older playback revision before releasing its history sources.
    playback_.setTimeline(current, cursor_);
    clipboard_.clear(); clipboardRate_ = clipboardChannels_ = 0;
    document_.clearHistory(); syncPlayback_ = true;
    if (playing) playback_.play(cursor_, selected() ? first() : 0, selected() ? last() : current->frames, loop_);
    status_ = "Undo/redo and clipboard cleared; current audio kept.";
}
#endif
void Editor::exportTo(const std::filesystem::path& requestedPath, bool selectionOnly) {
    auto t = document_.timeline();
    if (!t || (selectionOnly && !selected())) return;
    if (selectionOnly) t = std::make_shared<Timeline>(t->range(first(), last()));
    const auto format = static_cast<ExportFormat>(exportFormat_);
    auto path = requestedPath; path.replace_extension(exportExtension(format));
#ifdef __EMSCRIPTEN__
    try { if (isWav(format)) web::checkExport(std::uint64_t(t->frames), t->channels, exportBits(format)); }
    catch (const std::exception& e) { error_ = e.what(); continueAfterExport_ = false; return; }
    std::filesystem::remove(path); // Only a private temporary export, never a user's disk file.
#endif
    const bool closeAfter = !selectionOnly && continueAfterExport_ && pendingClose_;
    const auto openAfter = !selectionOnly && continueAfterExport_ && !pendingClose_ ? pendingPath_ : std::filesystem::path{};
    continueAfterExport_ = false;
    startJob(selectionOnly ? "Exporting selection" : isMp3(format) ? "Exporting MP3" : isFlac(format) ? "Exporting FLAC" : "Exporting WAV", [this, t, path, format](std::stop_token stop) { exportAudio(*t, path, format, &jobProgress_, stop); }, [this, path, t, selectionOnly, closeAfter, openAfter] {
#ifdef __EMSCRIPTEN__
        if (!webDownload(path)) { status_ = "Could not prepare the download. Your edits are still here."; return; }
#endif
        if (!selectionOnly && document_.timeline() == t) document_.markSaved(); status_ = "Exported " + utf8(path.filename());
        if (closeAfter) close_ = true;
        else if (!openAfter.empty()) openNow(openAfter);
    });
}
void Editor::afterEdit(const std::string& labelText) {
    auto t = document_.timeline(); if (!t) return;
    cursor_ = std::clamp(cursor_, Frame(0), t->frames);
    playback_.setTimeline(t, cursor_); view_.bounds(double(selectionExtent())); status_ = labelText; overviewTimeline_ = waveformTimeline_ = nullptr;
}
void Editor::normalize() {
    openCleanup(Cleanup::Normalize);
}
Frame Editor::edgeFrames() const {
    const auto t = document_.timeline(); return t ? std::max<Frame>(1, Frame(std::llround(t->sampleRate * edgeFadeMs_ / 1000))) : 0;
}
void Editor::snapSelection(bool movingEdgeOnly) {
    const auto t = document_.timeline(); if (!t || busy_) return;
    const Frame radius = std::max<Frame>(1, t->sampleRate / 200);
    Frame a = movingEdgeOnly || anchor_ > t->frames ? anchor_ : zeroCrossing(*t, anchor_, radius);
    Frame b = head_ > t->frames ? head_ : zeroCrossing(*t, head_, radius);
    if (selected() && (a == b || (anchor_ < head_) != (a < b))) return;
    anchor_ = a; head_ = b;
    if (!selected()) { cursor_ = head_; playback_.seek(cursor_); }
    status_ = "Snapped to nearby zero crossings";
}
void Editor::openCleanup(Cleanup kind) {
    const auto t = document_.timeline(); if (!t || !t->frames || busy_) return;
    playback_.pause(); cleanupCursor_ = playback_.position(); cleanupBase_ = t;
    cleanupFirst_ = selected() ? first() : 0; cleanupLast_ = selected() ? last() : t->frames;
    cleanupKind_ = int(kind); cleanupBypass_ = false; cleanupPreview_.reset(); cleanupPopup_ = true;
    highPassHz_ = std::min(highPassHz_, float(t->sampleRate * .45)); lowPassHz_ = std::min(lowPassHz_, float(t->sampleRate * .45));
}
void Editor::resetCleanup() {
    playback_.setTimeline(document_.timeline(), cleanupCursor_); cleanupPreview_.reset(); cleanupBypass_ = false;
}
void Editor::previewCleanup() {
    if (!cleanupPreview_) return;
    const bool wasPlaying = playback_.playing();
    const Frame position = wasPlaying ? playback_.position() : cleanupFirst_;
    playback_.setTimeline(cleanupBypass_ ? cleanupBase_ : cleanupPreview_, position);
    playback_.play(position, cleanupFirst_, cleanupLast_, true);
}
void Editor::prepareCleanup(bool apply) {
    if (!cleanupBase_ || busy_) return;
    if (cleanupPreview_) {
        if (apply) { document_.apply(cleanupPreview_, cleanupName(Cleanup(cleanupKind_))); afterEdit(cleanupName(Cleanup(cleanupKind_))); cleanupClose_ = true; }
        else previewCleanup();
        return;
    }
    const auto base = cleanupBase_; const auto a = cleanupFirst_, b = cleanupLast_;
    const CleanupSettings settings{Cleanup(cleanupKind_), cleanupKind_ == 0 ? normalizeDb_ : cleanupKind_ == 2 ? highPassHz_ : lowPassHz_};
    auto result = std::make_shared<std::shared_ptr<Timeline>>();
    startJob(cleanupName(settings.kind), [this, base, a, b, settings, result](std::stop_token stop) {
        *result = processAudio(*base, a, b, settings, &jobProgress_, stop);
    }, [this, result, apply] { cleanupPreview_ = std::move(*result); prepareCleanup(apply); },
       [this, apply] { cleanupPopup_ = true; prepareCleanup(apply); });
}
void Editor::action(const std::string& name) try {
    auto t = document_.timeline(); if (!t || busy_) return;
    if (name == "select") { anchor_ = 0; head_ = t->frames; return; }
    if (name == "clear") { anchor_ = head_ = cursor_; return; }
    if (name == "snap") { snapSelection(); return; }
    if (name == "edge-fades") { if (selected()) { document_.fadeEdges(first(), last(), edgeFrames()); afterEdit("Faded selection edges"); } return; }
    if (name == "dc") { openCleanup(Cleanup::RemoveDC); return; }
    if (name == "high-pass") { openCleanup(Cleanup::HighPass); return; }
    if (name == "low-pass") { openCleanup(Cleanup::LowPass); return; }
    const char* channelCommands[] = {"extract-left", "extract-right", "mono", "swap", "invert", "invert-left", "invert-right"};
    for (int i = 0; i < 7; ++i) if (name == channelCommands[i]) {
        document_.channelTool(ChannelTool(i), selected() ? first() : 0, selected() ? last() : t->frames); afterEdit(document_.undoLabel()); return;
    }
    if (name == "fit") { view_.show(0, double(t->frames)); return; }
    if (name == "zoom-selection") { if (selected()) view_.show(double(first()), double(last() - first())); return; }
    if (name == "export-selection") { exportDialog(true); return; }
    if (name == "insert-silence") {
        playback_.pause(); cursor_ = playback_.position(); silenceAt_ = cursor_;
        const long double frames = std::round(static_cast<long double>(silenceSeconds_) * t->sampleRate);
        silenceFrames_ = std::isfinite(silenceSeconds_) && frames >= 1 && frames < static_cast<long double>(std::numeric_limits<Frame>::max())
            ? Frame(frames) : t->sampleRate;
        silenceSeconds_ = double(silenceFrames_) / t->sampleRate;
        insertSilencePopup_ = true; return;
    }
    if (name == "fade-in" || name == "fade-out") {
        if (!selected()) return;
        document_.fade(first(), last(), name == "fade-out", static_cast<FadeCurve>(fadeCurve_));
        afterEdit(name == "fade-out" ? "Fade out" : "Fade in"); return;
    }
    if (name == "copy" || name == "cut") {
        if (!selected() || first() >= t->frames) return;
        clipboard_ = t->slice(first(), last()); clipboardRate_ = t->sampleRate; clipboardChannels_ = t->channels;
        status_ = "Copied " + timecode(double(last() - first()), t->sampleRate);
        if (name == "copy") return;
    }
    if (name == "undo") { if (document_.undo()) afterEdit("Undo"); return; }
    if (name == "redo") { if (document_.redo()) afterEdit("Redo"); return; }
    if (name == "normalize") { normalize(); return; }
    if (name == "paste" || name == "paste-replace") {
        if (clipboard_.empty()) return;
        if (clipboardRate_ != t->sampleRate || clipboardChannels_ != t->channels) { error_ = "Clipboard audio must have the same sample rate and channel count."; return; }
        if (name == "paste" && selected()) {
            playback_.pause(); cursor_ = playback_.position();
            auto clip = std::make_shared<Timeline>(); clip->sampleRate = clipboardRate_; clip->channels = clipboardChannels_; clip->pieces = clipboard_; clip->rebuild();
            const auto a = first(), b = last(); auto result = std::make_shared<std::shared_ptr<Timeline>>();
            startJob("Pasting into selection", [this, t, clip, a, b, result](std::stop_token stop) {
                *result = mixAudio(*t, *clip, a, b, &jobProgress_, stop);
            }, [this, result] { document_.apply(std::move(*result), "Paste into selection"); afterEdit("Mixed clipboard audio into selection"); },
               [this] { action("paste"); });
            return;
        }
        const Frame pos = std::min(selected() ? first() : cursor_, t->frames), end = selected() ? last() : cursor_;
        Frame length = 0; for (const auto& p : clipboard_) length += p.length;
        document_.replace(pos, end, clipboard_, "Paste", autoEdgeFades_ ? edgeFrames() : 0); anchor_ = pos; head_ = pos + length; cursor_ = pos; afterEdit("Pasted audio"); return;
    }
    if (name == "delete" || name == "cut") {
        if (!selected()) return;
        const Frame pos = first(); document_.replace(first(), last(), {}, name == "cut" ? "Cut" : "Delete", autoEdgeFades_ ? edgeFrames() : 0);
        cursor_ = std::min(pos, document_.timeline()->frames); afterEdit(name == "cut" ? "Cut audio; selection range retained" : "Deleted audio; selection range retained"); return;
    }
    if (name == "trim") {
        if (!selected() || first() >= t->frames) return; const Frame length = std::min(last(), t->frames) - first(); document_.trim(first(), last(), autoEdgeFades_ ? edgeFrames() : 0);
        anchor_ = cursor_ = 0; head_ = length; afterEdit("Trimmed to selection"); view_.show(0, double(length)); return;
    }
    const auto a = selected() ? first() : 0, b = selected() ? last() : t->frames;
    if (name == "reverse") document_.effect(a, b, Effect::Reverse);
    if (name == "silence") document_.effect(a, b, Effect::Silence);
    if (name == "gain") document_.effect(a, b, Effect::Gain, std::pow(10.f, gainDb_ / 20.f));
    afterEdit(name == "gain" ? "Applied " + decimal(gainDb_) + " dB gain" : name);
} catch (const std::exception& e) {
    error_ = e.what();
}
void Editor::togglePlay() {
    auto t = document_.timeline(); if (!t || !t->frames) return;
    if (playback_.playing()) { playback_.pause(); cursor_ = playback_.position(); }
    else {
        Frame p = cursor_; if (p >= t->frames) p = 0;
        playback_.play(p, selected() ? first() : 0, selected() ? last() : t->frames, loop_);
        if (!playback_.available()) error_ = playback_.error();
    }
}
void Editor::shortcuts() {
    auto& io = ImGui::GetIO(); if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
    const bool ctrl = io.KeyCtrl || io.KeySuper;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (ctrl && pressed(ImGuiKey_O)) openDialog();
    if (ctrl && pressed(ImGuiKey_S)) exportDialog();
    if (ctrl && pressed(ImGuiKey_E)) exportDialog(io.KeyShift);
    if (pressed(ImGuiKey_F1)) help_ = true;
    if (pressed(ImGuiKey_Space)) togglePlay();
    if (ctrl && pressed(ImGuiKey_Z)) action(io.KeyShift ? "redo" : "undo");
    if (ctrl && pressed(ImGuiKey_Y)) action("redo");
    if (ctrl && pressed(ImGuiKey_A)) action("select");
    if (ctrl && pressed(ImGuiKey_C)) action("copy");
    if (ctrl && pressed(ImGuiKey_X)) action("cut");
    if (ctrl && pressed(ImGuiKey_V)) action(io.KeyShift ? "paste-replace" : "paste");
    if (ctrl && pressed(ImGuiKey_T)) action("trim");
    if (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace)) action("delete");
    if (pressed(ImGuiKey_Escape)) action("clear");
    if (!ctrl && pressed(ImGuiKey_F)) action("fit");
    if (!ctrl && pressed(ImGuiKey_Z)) action("zoom-selection");
    if (!ctrl && pressed(ImGuiKey_R)) action("reverse");
    if (!ctrl && pressed(ImGuiKey_J)) action("snap");
    if (!ctrl && pressed(ImGuiKey_I)) action("fade-in");
    if (!ctrl && pressed(ImGuiKey_O)) action("fade-out");
    if (!ctrl && pressed(ImGuiKey_Insert)) action("insert-silence");
    if (!ctrl && pressed(ImGuiKey_L)) { loop_ = !loop_; if (playback_.playing()) { playback_.pause(); togglePlay(); } }
    if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) view_.zoom(.65, .5);
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) view_.zoom(1.5, .5);
    if (pressed(ImGuiKey_Home)) { cursor_ = 0; playback_.seek(0); view_.show(0, view_.targetSpan); }
    if (pressed(ImGuiKey_End) && document_.timeline()) { cursor_ = document_.timeline()->frames; playback_.seek(cursor_); view_.show(double(cursor_) - view_.targetSpan, view_.targetSpan); }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) view_.pan(-view_.targetSpan * .08);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) view_.pan(view_.targetSpan * .08);
}
void Editor::header(float width) {
    auto d = drawList();
    const float headerHeight = compact_ && width < 640 ? 118.f : 64.f;
    d.AddRectFilled({0, 0}, {width, headerHeight}, panel); d.AddLine({0, headerHeight}, {width, headerHeight}, line);
    ImGui::SetCursorPos(px({16, 12}));
    const bool menuOpen = ImGui::IsPopupOpen("Main menu");
    ImGui::PushStyleColor(ImGuiCol_Button, menuOpen ? ImVec4(.17f, .30f, .29f, 1) : ImVec4(0, 0, 0, 0));
    const bool menuPressed = button("##main-menu", {144, 40});
    ImGui::PopStyleColor();
    const float heights[] = {7, 15, 23, 12, 18};
    for (int i = 0; i < 5; ++i) d.AddRectFilled({24.f + i * 4, 32 - heights[i] / 2}, {26.f + i * 4, 32 + heights[i] / 2}, teal, 1);
    label({57, 18}, "coldedit", text, titleFont_);
    d.AddTriangleFilled({142, 30}, {150, 30}, {146, 34}, muted);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Main menu  /  F10");
    if (menuPressed ||
        (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_F10, false)))
        ImGui::OpenPopup("Main menu");
    mainMenu();
    if (compact_) {
        ImGui::SetCursorPos(px({168, 12}));
        if (iconButton("play", playback_.playing() ? 1 : 0, true, "Play / pause")) togglePlay();
        ImGui::SetCursorPos(px({width - 84, 12}));
        if (button(compactTools_ ? "Back" : "Tools", {72, 40})) compactTools_ = !compactTools_;
        const auto t = document_.timeline();
        const bool ready = t && !busy_;
        const bool paste = ready && !clipboard_.empty() && clipboardRate_ == t->sampleRate && clipboardChannels_ == t->channels;
        ImGui::SetCursorPos(px(width >= 640 ? ImVec2{228, 12} : ImVec2{12, 68}));
        if (toolButton("##undo", ToolIcon::Undo, "", "Undo", ready && document_.canUndo())) action("undo");
        ImGui::SameLine(0, px(4));
        if (toolButton("##redo", ToolIcon::Redo, "", "Redo", ready && document_.canRedo())) action("redo");
        ImGui::SameLine(0, px(4));
        if (toolButton("##cut", ToolIcon::Cut, "", "Cut selection", ready && selected())) action("cut");
        ImGui::SameLine(0, px(4));
        if (toolButton("##paste", ToolIcon::Paste, "", "Paste", paste)) action("paste");
        ImGui::BeginDisabled(!ready);
        ImGui::SameLine(0, px(4)); if (button("-", {44, 44})) view_.zoom(1.5, .5);
        ImGui::SameLine(0, px(4)); if (button("+", {44, 44})) view_.zoom(.65, .5);
        ImGui::EndDisabled();
        return;
    }
    d.AddLine({176, 22}, {176, 42}, line);
    transport();
    d.AddLine({396, 22}, {396, 42}, line);
    ImGui::SetCursorPos(px({408, 15}));
    const auto t = document_.timeline();
    const bool ready = t && !busy_, selection = ready && selected();
    const bool paste = ready && !clipboard_.empty() && clipboardRate_ == t->sampleRate && clipboardChannels_ == t->channels;
    const bool roomy = width >= 1120;
    auto next = [&] { ImGui::SameLine(0, px(4)); };
    auto separator = [&] {
        ImGui::SameLine(0, px(9));
        const float x = logical(ImGui::GetCursorScreenPos()).x;
        d.AddLine({x, 22}, {x, 42}, line);
        ImGui::Dummy(px({1, 34})); ImGui::SameLine(0, px(9));
    };
    if (toolButton("##undo", ToolIcon::Undo, "", "Undo " + document_.undoLabel() + "  /  Ctrl+Z", ready && document_.canUndo())) action("undo");
    next(); if (toolButton("##redo", ToolIcon::Redo, "", "Redo " + document_.redoLabel() + "  /  Ctrl+Shift+Z", ready && document_.canRedo())) action("redo");
    separator();
    if (toolButton("##cut", ToolIcon::Cut, "Cut", "Cut selection  /  Ctrl+X", selection)) action("cut");
    next(); if (toolButton("##copy", ToolIcon::Copy, "Copy", "Copy selection  /  Ctrl+C", ready && selected())) action("copy");
    next(); if (toolButton("##paste", ToolIcon::Paste, "Paste", selected() ? "Paste into selection (add signals)  /  Ctrl+V\nStops at the selection end; no stretching or repetition.\nPaste replace  /  Ctrl+Shift+V" : "Paste at cursor  /  Ctrl+V\nClipboard audio must match the sample rate and channel count.", paste)) action("paste");
    next(); if (toolButton("##delete", ToolIcon::Delete, "", "Delete selection  /  Del", ready && selected())) action("delete");
    next(); if (toolButton("##trim", ToolIcon::Trim, roomy ? "Trim" : "", "Keep only the selection  /  Ctrl+T", ready && selected())) action("trim");
    separator();
    if (toolButton("##fit", ToolIcon::Fit, "", "Fit entire file  /  F", ready && t->frames > 0)) action("fit");
    next(); if (toolButton("##zoom-selection", ToolIcon::Selection, "", "Zoom to selection  /  Z", ready && selected())) action("zoom-selection");
}
void Editor::mainMenu() {
    std::function<void()> run;
    const auto t = document_.timeline();
    const bool ready = t && !busy_, hasAudio = ready && t->frames > 0;
    const bool selection = ready && selected();
    const bool paste = ready && !clipboard_.empty() && clipboardRate_ == t->sampleRate && clipboardChannels_ == t->channels;
    ImGui::SetNextWindowPos(px({16, 58}));
    ImGui::SetNextWindowSizeConstraints(px({280, 0}), compact_ ? ImVec2{ImGui::GetIO().DisplaySize.x - px(24), ImGui::GetIO().DisplaySize.y - px(70)} : ImVec2{FLT_MAX, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, px({10, 10}));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, px({10, 8}));
    if (ImGui::BeginPopup("Main menu")) {
        auto item = [&](const char* title, const char* shortcut, const char* command, bool enabled) {
            if (ImGui::MenuItem(title, compact_ ? nullptr : shortcut, false, enabled)) run = [this, command] { action(command); };
        };
        ImGui::TextDisabled("COLDEDIT"); ImGui::Separator();
        if (ImGui::MenuItem("Open audio...", "Ctrl+O", false, !busy_)) run = [this] { openDialog(); };
        if (ImGui::MenuItem("Export audio...", "Ctrl+E", false, ready)) run = [this] { exportDialog(); };
        item("Save selection...", "Ctrl+Shift+E", "export-selection", selection);
        ImGui::Separator();
        if (ImGui::BeginMenu("Edit")) {
            item(("Undo " + document_.undoLabel()).c_str(), "Ctrl+Z", "undo", ready && document_.canUndo());
            item(("Redo " + document_.redoLabel()).c_str(), "Ctrl+Shift+Z", "redo", ready && document_.canRedo());
            ImGui::Separator();
            item("Cut", "Ctrl+X", "cut", selection); item("Copy", "Ctrl+C", "copy", selection);
            item(selection ? "Paste into selection" : "Paste", "Ctrl+V", "paste", paste);
            item("Paste replace", "Ctrl+Shift+V", "paste-replace", paste && selection); item("Delete", "Del", "delete", selection);
            item("Trim to selection", "Ctrl+T", "trim", selection);
            ImGui::Separator(); item("Insert silence...", "Insert", "insert-silence", ready); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Selection")) {
            item("Select all", "Ctrl+A", "select", hasAudio);
            item("Clear selection", "Esc", "clear", selection);
            item("Snap to zero crossings", "J", "snap", hasAudio);
            ImGui::MenuItem("Snap on selection release", nullptr, &snapZero_);
            item("Fade selection edges", nullptr, "edge-fades", selection);
            ImGui::MenuItem("Automatic edit edge fades", nullptr, &autoEdgeFades_);
            item("Zoom to selection", "Z", "zoom-selection", selection); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Process")) {
            ImGui::TextDisabled("Selected range");
            item("Fade in", "I", "fade-in", selection); item("Fade out", "O", "fade-out", selection);
            if (ImGui::BeginMenu("Fade curve")) {
                ImGui::RadioButton("Linear", &fadeCurve_, 0); ImGui::RadioButton("Smooth", &fadeCurve_, 1); ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled(selected() ? "Applies to selection" : "Applies to entire file"); ImGui::Separator();
            item("Apply inspector gain", nullptr, "gain", hasAudio);
            item("Normalize...", nullptr, "normalize", hasAudio);
            item("Remove DC offset...", nullptr, "dc", hasAudio);
            item("High-pass filter...", nullptr, "high-pass", hasAudio);
            item("Low-pass filter...", nullptr, "low-pass", hasAudio);
            ImGui::Separator();
            if (ImGui::BeginMenu("Channels")) {
                ImGui::TextDisabled("Entire document");
                item("Extract left as mono", nullptr, "extract-left", hasAudio && t->channels == 2);
                item("Extract right as mono", nullptr, "extract-right", hasAudio && t->channels == 2);
                item("Mix stereo to mono", nullptr, "mono", hasAudio && t->channels == 2);
                item("Swap left / right", nullptr, "swap", hasAudio && t->channels == 2);
                ImGui::Separator(); ImGui::TextDisabled(selected() ? "Selected range" : "Entire document");
                item("Invert polarity", nullptr, "invert", hasAudio);
                item("Invert left polarity", nullptr, "invert-left", hasAudio && t->channels == 2);
                item("Invert right polarity", nullptr, "invert-right", hasAudio && t->channels == 2); ImGui::EndMenu();
            }
            item("Reverse", "R", "reverse", hasAudio); item("Silence", nullptr, "silence", hasAudio); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Playback")) {
            if (ImGui::MenuItem(playback_.playing() ? "Pause" : "Play", "Space", false, hasAudio)) run = [this] { togglePlay(); };
            if (ImGui::MenuItem("Stop", nullptr, false, hasAudio)) run = [this] { playback_.pause(); cursor_ = selected() ? first() : 0; playback_.seek(cursor_); };
            if (ImGui::MenuItem("Loop", "L", loop_, hasAudio)) run = [this] { loop_ = !loop_; if (playback_.playing()) { playback_.pause(); togglePlay(); } };
            ImGui::MenuItem("Follow playback", nullptr, &follow_); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            item("Fit entire file", "F", "fit", hasAudio); item("Zoom to selection", "Z", "zoom-selection", selection);
            if (ImGui::MenuItem("Zoom in", "+", false, hasAudio)) run = [this] { view_.zoom(.65, .5); };
            if (ImGui::MenuItem("Zoom out", "-", false, hasAudio)) run = [this] { view_.zoom(1.5, .5); };
            if (ImGui::MenuItem("Reset view", nullptr, false, hasAudio)) run = [this] { verticalZoom_ = 1; action("fit"); };
            ImGui::Separator();
            if (ImGui::MenuItem("Wheel zooms by default", nullptr, &wheelZooms_)) {
                if (!saveWheelZoomPreference(wheelZooms_)) status_ = "Wheel preference changed for this session; could not save it.";
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Main waveform: Ctrl swaps zoom and pan.\nNavigator: wheel always zooms around the current view.");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Export format", !busy_)) {
            for (int i = 0; i < IM_ARRAYSIZE(exportFormats); ++i) if (ImGui::MenuItem(exportFormats[i], nullptr, exportFormat_ == i)) exportFormat_ = i;
            const auto format = static_cast<ExportFormat>(exportFormat_);
            if (isMp3(format) && t && mp3SampleRate(t->sampleRate, mp3Bitrate(format)) != t->sampleRate)
                ImGui::TextDisabled("MP3 output: %.2f kHz", mp3SampleRate(t->sampleRate, mp3Bitrate(format)) / 1000.);
            ImGui::EndMenu();
        }
#ifdef __EMSCRIPTEN__
        if (ImGui::BeginMenu("Web audio budget", !busy_)) {
            audioBudgetSetting();
            ImGui::TextDisabled("More edits use more memory.");
            ImGui::Separator();
            if (ImGui::MenuItem("Clear undo/redo and clipboard", nullptr, false,
                ready && (document_.canUndo() || document_.canRedo() || !clipboard_.empty())))
                run = [this] { clearAudioBuffers(); };
            ImGui::TextDisabled("Keeps current audio and edits.");
            ImGui::TextDisabled("Freed space is reused.");
            ImGui::EndMenu();
        }
#endif
        if (ImGui::MenuItem("Keyboard shortcuts", "F1")) run = [this] { help_ = true; };
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem("Quit", nullptr, false, !busy_)) run = [this] { requestClose(); };
#endif
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    // Run after EndPopup so file dialogs/modals belong to the editor window.
    if (run) run();
}
void Editor::transport() {
    ImGui::SetCursorPos(px({192, 12}));
    if (iconButton("start", 3, false, "Return to start  /  Home")) { cursor_ = 0; playback_.seek(0); }
    ImGui::SameLine(0, px(8)); if (iconButton("play", playback_.playing() ? 1 : 0, true, "Play / pause  /  Space")) togglePlay();
    ImGui::SameLine(0, px(8)); if (iconButton("stop", 2, false, "Stop and return to selection start")) { playback_.pause(); cursor_ = selected() ? first() : 0; playback_.seek(cursor_); }
    ImGui::SameLine(0, px(8)); if (iconButton("loop", 4, loop_, "Loop selection or whole file  /  L")) { loop_ = !loop_; if (playback_.playing()) { playback_.pause(); togglePlay(); } }
}
void Editor::playbackPanel(ImVec2 p, ImVec2 size) {
    // Keep monitoring visible while the editing inspector scrolls independently.
    ImGui::SetCursorScreenPos(px(p));
    ImGui::BeginChild("Playback", px(size), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto d = drawList();
    const unsigned rate = source_ ? source_->sampleRate : 48000;
    label({p.x + 16, p.y + 16}, "PLAYHEAD", muted);
    label({p.x + 16, p.y + 40}, timecode(double(cursor_), rate, true), text, monoFont_);
    label({p.x + 16, p.y + 70}, "Duration", muted);
    label({p.x + 82, p.y + 70}, timecode(document_.timeline() ? double(document_.timeline()->frames) : 0, rate, true), muted, monoFont_);
    label({p.x + 16, p.y + 106}, "Monitor", muted);
    ImGui::SetCursorPos(px({88, 98})); ImGui::SetNextItemWidth(px(size.x - 104));
    if (ImGui::SliderFloat("##volume", &volumeDb_, -60, 6, "%.1f dB")) playback_.setVolume(std::pow(10.f, volumeDb_ / 20));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback volume only; exported audio is unchanged.");
    const float x = p.x + 16, right = p.x + size.x - 16;
    label({x, p.y + 143}, "OUTPUT", muted);
    for (unsigned c = 0; c < 2; ++c) {
        const float y = p.y + 167 + c * 9;
        d.AddRectFilled({x, y}, {right, y + 4}, IM_COL32(39, 49, 56, 255), 2);
        const float level = std::clamp((20 * std::log10(std::max(.001f, playback_.meter(c))) + 60) / 60, 0.f, 1.f);
        if (level > 0) d.AddRectFilled({x, y}, {x + (right - x) * level, y + 4}, level > .96f ? amber : teal, 2);
    }
    ImGui::EndChild();
}
void Editor::waveform(ImVec2 p, ImVec2 size) {
    wavePos_ = p; waveSize_ = size;
    auto t = document_.timeline(); if (!t) { emptyState(p, size); return; }
    auto d = drawList(); auto& io = ImGui::GetIO();
    const auto mouse = logical(io.MousePos);
    constexpr float ruler = 34, gutter = 46;
    const float left = p.x + gutter, right = p.x + size.x, top = p.y + ruler, bottom = p.y + size.y;
    const float width = std::max(1.f, right - left), laneH = (size.y - ruler) / t->channels;
    d.AddRectFilled(p, {right, bottom}, panel, 8);
    d.PushClipRect({left, p.y}, {right, bottom}, true);
    auto xAt = [&](double f) { return left + float((f - view_.start) / view_.span) * width; };
    const double step = tickStep(view_.span / t->sampleRate / std::max(2.f, width / 105.f));
    for (double sec = std::ceil(view_.start / t->sampleRate / step) * step; sec * t->sampleRate <= view_.start + view_.span; sec += step) {
        const float x = xAt(sec * t->sampleRate);
        d.AddLine({x, p.y + 28}, {x, bottom}, IM_COL32(40, 49, 57, 255));
        std::string tick;
        if (step < .01) tick = decimal(sec, 4) + " s"; else if (step < .1) tick = decimal(sec, 3) + " s"; else tick = timecode(sec * t->sampleRate, t->sampleRate);
        label({x + 6, p.y + 7}, tick, muted);
    }
    if (selected()) {
        const float a = xAt(double(first())), b = xAt(double(last()));
        d.AddRectFilled({a, top}, {b, bottom}, IM_COL32(71, 177, 153, 28));
        d.AddRectFilled({a, p.y}, {b, top}, IM_COL32(80, 190, 162, 40));
    }
    if (last() > t->frames) {
        const float end = xAt(double(t->frames));
        d.AddRectFilled({end, top}, {right, bottom}, IM_COL32(10, 15, 19, 95));
        d.AddLine({end, p.y}, {end, bottom}, muted);
        label({end + 8, top + 8}, "END OF AUDIO", muted);
    }
    // Anchor columns to the framebuffer grid, including partial pixels at the
    // plot edges on fractional DPI scales. Never round the sample coordinates.
    const double rasterLeft = double(left) * dpi_.raster, rasterRight = double(right) * dpi_.raster;
    const int firstPixel = int(std::floor(rasterLeft));
    const int columns = std::max(1, int(std::ceil(rasterRight)) - firstPixel);
    const double samplesPerPixel = view_.span / columns;
    const bool peaksReady = std::all_of(t->pieces.begin(), t->pieces.end(), [](const Piece& piece) { return !piece.source || piece.source->peaksReady(); });
    if (waveformTimeline_ != t.get() || waveformStart_ != view_.start || waveformSpan_ != view_.span || waveformColumns_ != columns ||
        waveformRasterLeft_ != rasterLeft || waveformRasterRight_ != rasterRight || !waveformReady_) {
        waveformCache_.resize(std::size_t(columns) * t->channels);
        const auto frameAtEdge = [&](int x) {
            return view_.start + std::clamp((firstPixel + x - rasterLeft) / (rasterRight - rasterLeft), 0., 1.) * view_.span;
        };
        for (unsigned c = 0; c < t->channels; ++c) for (int x = 0; x < columns; ++x) {
            waveformCache_[std::size_t(c) * columns + x] = waveformPeak(*t, frameAtEdge(x), frameAtEdge(x + 1), c);
        }
        waveformTimeline_ = t.get(); waveformStart_ = view_.start; waveformSpan_ = view_.span;
        waveformColumns_ = columns; waveformReady_ = peaksReady;
        waveformRasterLeft_ = rasterLeft; waveformRasterRight_ = rasterRight;
    }
    for (unsigned c = 0; c < t->channels; ++c) {
        const float center = top + laneH * (c + .5f), amplitude = std::max(1.f, laneH * .38f);
        d.PushClipRect({left, top + laneH * c}, {right, top + laneH * (c + 1)}, true);
        for (float v : {-1.f, -.5f, 0.f, .5f, 1.f}) {
            const float y = center + v * amplitude;
            d.AddLine({left, y}, {right, y}, v == 0 ? IM_COL32(57, 72, 80, 255) : IM_COL32(31, 39, 46, 255));
        }
        const ImU32 color = c ? blue : teal;
        for (int x = 0; x < columns; ++x) {
            const auto peak = waveformCache_[std::size_t(c) * columns + x];
            const float lo = center - std::clamp(peak.hi * verticalZoom_, -1.2f, 1.2f) * amplitude;
            const float hi = center - std::clamp(peak.lo * verticalZoom_, -1.2f, 1.2f) * amplitude;
            const int pixel = firstPixel + x;
            const float coverage = float(std::min(double(pixel + 1), rasterRight) - std::max(double(pixel), rasterLeft));
            d.AddWaveformColumn(pixel, lo, hi, color, dpi_.raster, coverage);
        }
        if (samplesPerPixel < .16) {
            const Frame firstFrame = std::max<Frame>(0, Frame(std::floor(view_.start)) - 1);
            const Frame endFrame = std::min(t->frames, Frame(std::ceil(view_.start + view_.span)) + 1);
            for (Frame f = firstFrame; f < endFrame; ++f) {
                const ImVec2 point{xAt(double(f)), center - std::clamp(t->sample(f, c) * verticalZoom_, -1.2f, 1.2f) * amplitude};
                d.AddCircleFilled(point, 2.2f, color);
            }
        }
        d.PopClipRect();
        if (c) d.AddLine({left, top + laneH * c}, {right, top + laneH * c}, line);
    }
    const bool inside = mouse.x >= left && mouse.x <= right && mouse.y >= p.y && mouse.y <= bottom;
    const SelectionEdge edge = selected() && inside ? hitSelectionEdge(mouse.x, xAt(double(first())), xAt(double(last()))) : SelectionEdge::None;
    if (selected()) for (Frame f : {first(), last()}) {
        const float x = xAt(double(f)); d.AddLine({x, p.y}, {x, bottom}, IM_COL32(98, 222, 193, 170));
        const bool active = boundaryDrag_.active && f == head_;
        const bool hot = edge == (f == first() ? SelectionEdge::Start : SelectionEdge::End);
        const float grip = hot || active ? 5.f : 3.f;
        d.AddRectFilled({x - grip, top - 8}, {x + grip, top + 8}, hot || active ? text : teal, 2);
    }
    const float playX = xAt(double(cursor_));
    d.AddLine({playX, p.y + 25}, {playX, bottom}, amber, 1.5f);
    d.AddTriangleFilled({playX - 5, p.y + 24}, {playX + 5, p.y + 24}, {playX, p.y + 31}, amber);
    d.PopClipRect();
    d.AddLine({p.x, top}, {right, top}, line);
    for (unsigned c = 0; c < t->channels; ++c) {
        const float y = top + laneH * c;
        label({p.x + 15, y + 14}, t->channels == 1 ? "M" : c ? "R" : "L", c ? blue : teal);
        label({p.x + 9, y + laneH * .5f - 9}, "0", muted);
        if (laneH > 150) { label({p.x + 8, y + laneH * .13f}, "+1", muted); label({p.x + 8, y + laneH * .83f}, "-1", muted); }
    }
    if (source_ && !source_->peaksReady()) {
        std::string message = source_->peakProgress() < 0 ? "Peak cache unavailable. Zoom in for sample detail." : "Building waveform  " + decimal(source_->peakProgress() * 100, 0) + "%";
        label({left + 20, top + 16}, message, muted);
    }
    ImGui::SetCursorScreenPos(px({left, p.y}));
    ImGui::InvisibleButton("waveform", px({width, size.y}), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const auto frameAtMouse = [&] { return Frame(std::llround(view_.start + (mouse.x - left) / width * view_.span)); };
    if (hovered && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
        if (io.KeyAlt) verticalZoom_ = std::clamp(verticalZoom_ * std::exp(io.MouseWheel * .12f), .25f, 8.f);
        else {
            const bool zoom = wheelZooms_ != (io.KeyCtrl || io.KeySuper);
            if (zoom && io.MouseWheel != 0) view_.zoom(std::exp(-io.MouseWheel * .22), (mouse.x - left) / width);
            const float pan = io.MouseWheelH + (zoom ? 0.f : io.MouseWheel);
            if (pan != 0) view_.pan(-pan * view_.targetSpan * .075);
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Frame frame = frameAtMouse();
        const bool doubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        // The first click can move a selection edge or clear the range on the ruler.
        // Toggle using the selection that existed before that first click.
        if (!doubleClick) fullSelectionAtClick_ = selected() && first() == 0 && last() == t->frames;
        if (doubleClick) {
            if (fullSelectionAtClick_) {
                anchor_ = head_ = cursor_ = std::clamp(frame, Frame(0), t->frames);
                playback_.seek(cursor_);
            } else { anchor_ = 0; head_ = t->frames; }
            selecting_ = boundaryDrag_.active = rangeDrag_.active = false;
        } else if (edge != SelectionEdge::None && !io.KeyShift && !io.KeyAlt) {
            rangeDrag_.active = false;
            boundaryDragLimit_ = selectionExtent();
            boundaryDrag_.begin(edge, anchor_, head_, frame);
            boundaryDrag_.update(frame, boundaryDragLimit_, anchor_, head_);
            selecting_ = true;
        } else if (selected() && frame >= first() && frame <= last() && !io.KeyShift && !io.KeyAlt && mouse.y >= top) {
            boundaryDrag_.active = false; rangeDrag_.begin(anchor_, head_, frame, t->frames);
            selecting_ = true;
        } else {
            boundaryDrag_.active = rangeDrag_.active = false;
            head_ = std::clamp(frame, Frame(0), t->frames);
            if (!io.KeyShift) anchor_ = head_;
            cursor_ = head_; playback_.seek(cursor_); selecting_ = mouse.y >= top;
        }
    }
    if ((hovered && edge != SelectionEdge::None) || boundaryDrag_.active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hovered && edge != SelectionEdge::None && !selecting_) ImGui::SetTooltip("Drag to adjust selection %s", edge == SelectionEdge::Start ? "start" : "end");
    if (rangeDrag_.active || (hovered && edge == SelectionEdge::None && selected() && frameAtMouse() > first() && frameAtMouse() < last() && mouse.y >= top && !io.KeyAlt && !io.KeyShift)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (!selecting_) ImGui::SetTooltip("Drag to move the selection range; audio stays in place.\nAlt-drag to draw a new range. Shift-click to extend.");
    }
    if (selecting_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Moving a range must retain its exact length, even with snapping on.
            if (snapZero_ && !rangeDrag_.active) snapSelection(boundaryDrag_.active);
            selecting_ = false; boundaryDrag_.active = rangeDrag_.active = false;
            // A playing loop adopts the finished range without restarting on every pixel.
            if (loop_ && playback_.playing()) playback_.play(playback_.position(), selected() ? first() : 0, selected() ? last() : t->frames, true);
        }
        else if (ImGui::IsMousePosValid()) {
            if (mouse.x < left || mouse.x > right) {
                const float distance = mouse.x < left ? mouse.x - left : mouse.x - right;
                view_.pan(distance / width * view_.span * io.DeltaTime * 8, true);
            }
            if (rangeDrag_.active) rangeDrag_.update(frameAtMouse(), anchor_, head_);
            else if (boundaryDrag_.active) boundaryDrag_.update(frameAtMouse(), boundaryDragLimit_, anchor_, head_);
            else head_ = std::clamp(frameAtMouse(), Frame(0), t->frames);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) { view_.pan(-io.MouseDelta.x / dpi_.layout / width * view_.span, true); ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
    if (ImGui::BeginPopupContextItem("Wave actions")) {
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, selected())) action("copy");
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, selected())) action("cut");
        if (ImGui::MenuItem(selected() ? "Paste into selection" : "Paste", "Ctrl+V", false, !clipboard_.empty())) action("paste");
        if (ImGui::MenuItem("Paste replace", "Ctrl+Shift+V", false, selected() && !clipboard_.empty())) action("paste-replace");
        if (ImGui::MenuItem("Save selection...", "Ctrl+Shift+E", false, selected())) action("export-selection");
        if (ImGui::MenuItem("Trim", "Ctrl+T", false, selected())) action("trim");
        if (ImGui::MenuItem("Reverse", "R")) action("reverse");
        if (ImGui::MenuItem("Silence")) action("silence");
        ImGui::EndPopup();
    }
}
void Editor::overview(ImVec2 p, ImVec2 size) {
    auto t = document_.timeline();
    auto d = drawList();
    label(p, "NAVIGATOR", muted);
    const char* hint = t ? "Drag to explore the whole file" : "No audio loaded";
    const float hintWidth = ImGui::CalcTextSize(hint).x / dpi_.layout;
    if (size.x > hintWidth + 125) label({p.x + size.x - hintWidth, p.y}, hint, muted);
    p.y += 26; size.y -= 26;
    d.AddRectFilled(p, {p.x + size.x, p.y + size.y}, panel, 5);
    if (!t) {
        d.AddLine({p.x + 8, p.y + size.y * .5f}, {p.x + size.x - 8, p.y + size.y * .5f}, line);
        return;
    }
    const int columns = std::max(1, int(std::ceil(size.x * dpi_.raster)));
    const float columnWidth = size.x / columns;
    const bool ready = source_ && source_->peaksReady();
    const double total = view_.total;
    if (overviewTimeline_ != t.get() || overviewCache_.size() != std::size_t(columns) || overviewReady_ != ready || overviewExtent_ != total) {
        overviewExtent_ = total;
        overviewTimeline_ = t.get(); overviewReady_ = ready; overviewCache_.resize(columns);
        for (int x = 0; x < columns; ++x) overviewCache_[x] = t->peak(Frame(double(x) / columns * total), Frame(double(x + 1) / columns * total), 0);
    }
    envelopeUpper_.resize(columns); envelopeLower_.resize(columns);
    for (int x = 0; x < columns; ++x) {
        const auto peak = overviewCache_[x]; const float center = p.y + size.y * .5f;
        envelopeUpper_[x] = {p.x + (x + .5f) * columnWidth, center - peak.hi * size.y * .43f};
        envelopeLower_[x] = {p.x + (x + .5f) * columnWidth, center - peak.lo * size.y * .43f};
    }
    d.AddEnvelope(envelopeUpper_.data(), envelopeLower_.data(), columns, IM_COL32(74, 113, 113, 255), 1.f / dpi_.raster);
    const float a = p.x + float(view_.start / total) * size.x, b = p.x + float((view_.start + view_.span) / total) * size.x;
    d.AddRectFilled({a, p.y}, {b, p.y + size.y}, IM_COL32(98, 222, 193, 20), 4);
    d.AddRect({a, p.y}, {b, p.y + size.y}, IM_COL32(98, 222, 193, 170), 4, 0, 1);
    const float cx = p.x + float(double(cursor_) / total) * size.x; d.AddLine({cx, p.y}, {cx, p.y + size.y}, amber);
    ImGui::SetCursorScreenPos(px(p)); ImGui::InvisibleButton("navigator", px(size));
    const auto& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        view_.zoom(std::exp(-io.MouseWheel * .22), .5);
    } else if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.MouseDelta.x != 0)) {
        const double frame = (logical(io.MousePos).x - p.x) / size.x * total;
        view_.show(frame - view_.targetSpan * .5, view_.targetSpan, true);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to pan. Wheel to zoom around the current view.");
}
void Editor::inspector(ImVec2 p, ImVec2 size) {
    ImGui::SetCursorScreenPos(px(p)); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, px({16, 16}));
    ImGui::BeginChild("Inspector", px(size), ImGuiChildFlags_AlwaysUseWindowPadding);
    const auto buttonPair = [&](const char* left, const char* leftAction, const char* right, const char* rightAction) {
        const auto& style = ImGui::GetStyle();
        const float halfWidth = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * .5f);
        const float labelWidth = std::max(ImGui::CalcTextSize(left).x, ImGui::CalcTextSize(right).x);
        const bool sameRow = halfWidth >= labelWidth + 2 * style.FramePadding.x;
        if (button(left, {sameRow ? halfWidth / dpi_.layout : -1.f, 34})) action(leftAction);
        if (sameRow) ImGui::SameLine();
        if (button(right, {-1, 34})) action(rightAction);
    };
    if (compact_) {
        ImGui::Text("Playhead  %s", timecode(double(cursor_), source_ ? source_->sampleRate : 48000, true).c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##compact-volume", &volumeDb_, -60, 6, "Monitor %.1f dB")) playback_.setVolume(std::pow(10.f, volumeDb_ / 20));
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    }
    auto t = document_.timeline(); ImGui::TextColored(ImVec4(.38f, .87f, .76f, 1), "SELECTION");
    ImGui::SameLine(); ImGui::TextDisabled(selected() ? "  ACTIVE" : "  NONE");
    ImGui::Spacing();
    const unsigned rate = t ? t->sampleRate : 48000;
    const auto readout = [&](const char* name, Frame value) { ImGui::TextDisabled("%s", name); ImGui::SameLine(px(82)); ImGui::PushFont(monoFont_); ImGui::TextUnformatted(timecode(double(value), rate, true).c_str()); ImGui::PopFont(); };
    readout("Start", first()); readout("End", last()); readout("Length", last() - first());
    ImGui::TextDisabled("%lld samples", static_cast<long long>(last() - first()));
    ImGui::Spacing();
    ImGui::BeginDisabled(!t || busy_);
    if (ImGui::TreeNode("Exact sample range")) {
        Frame a = first(), b = last(); ImGui::SetNextItemWidth(px(155));
        if (ImGui::InputScalar("In", ImGuiDataType_S64, &a)) { anchor_ = std::clamp(a, Frame(0), selectionExtent()); head_ = std::max(anchor_, b); }
        ImGui::SetNextItemWidth(px(155));
        if (ImGui::InputScalar("Out", ImGuiDataType_S64, &b)) { anchor_ = a; head_ = std::clamp(b, a, selectionExtent()); }
        ImGui::TreePop();
    }
    ImGui::BeginDisabled(!selected());
    ImGui::Spacing(); ImGui::TextDisabled("FADE SELECTION");
    const char* curves[] = {"Linear", "Smooth"};
    ImGui::SetNextItemWidth(-1); ImGui::Combo("##fade-curve", &fadeCurve_, curves, 2);
    buttonPair("Fade in", "fade-in", "Fade out", "fade-out");
    ImGui::EndDisabled();
    if (!selected()) ImGui::TextDisabled("Select a range to fade.");
    ImGui::Spacing(); ImGui::BeginDisabled(!selected());
    if (button("Zoom to selection", {-1, 34})) action("zoom-selection");
    buttonPair("Trim", "trim", "Delete", "delete");
    if (button("Save selection...", {-1, 34})) exportDialog(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save selected audio using the export format below.\nCtrl+Shift+E");
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (button("Insert silence...", {-1, 34})) action("insert-silence");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::TextDisabled("PROCESS");
    ImGui::TextWrapped(selected() ? "Applies to the selected range." : "Applies to the entire file.");
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##gain", &gainDb_, -24, 12, "Gain  %+.1f dB");
    if (button("Apply gain", {-1, 34})) action("gain");
    if (button("Normalize...", {-1, 34})) normalize();
    if (button("Remove DC offset...", {-1, 34})) action("dc");
    buttonPair("High-pass...", "high-pass", "Low-pass...", "low-pass");
    buttonPair("Reverse", "reverse", "Silence", "silence");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    // These sections already sit inside a padded panel; keep the full content
    // width for controls instead of spending another indent on their labels.
    if (ImGui::TreeNodeEx("Clean edit boundaries", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        ImGui::Checkbox("Snap to zero crossings", &snapZero_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On selection release: search within 5 ms.\nStereo chooses the quietest crossing across both channels.\nExact sample entry stays exact. Press J to snap explicitly.");
        ImGui::Checkbox("Automatic edge fades", &autoEdgeFades_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Short Smooth fades around cut, delete, paste, trim and inserted silence.\nIncluded in the edit's single undo step.");
        ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##edge-ms", &edgeFadeMs_, 1, 10, "Edges  %.1f ms");
        ImGui::BeginDisabled(!selected()); if (button("Fade selection edges", {-1, 34})) action("edge-fades"); ImGui::EndDisabled();
    }
    if (ImGui::TreeNodeEx("Channel tools", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        ImGui::TextWrapped("Conversion applies to the entire document.");
        ImGui::BeginDisabled(!t || t->channels != 2);
        buttonPair("Extract left", "extract-left", "Extract right", "extract-right");
        if (button("Mix to mono", {-1, 34})) action("mono"); if (button("Swap left / right", {-1, 34})) action("swap"); ImGui::EndDisabled();
        ImGui::TextWrapped(selected() ? "Polarity: selected range." : "Polarity: entire document.");
        if (button("Invert polarity", {-1, 34})) action("invert");
    }
    ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::TextDisabled("EXPORT FORMAT");
    ImGui::SetNextItemWidth(-1); ImGui::Combo("##format", &exportFormat_, exportFormats, IM_ARRAYSIZE(exportFormats));
    const auto format = static_cast<ExportFormat>(exportFormat_);
    if (isMp3(format)) {
        ImGui::TextDisabled("Lossy audio / LAME 3.100");
        if (t && mp3SampleRate(t->sampleRate, mp3Bitrate(format)) != t->sampleRate)
            ImGui::TextWrapped("MP3 export resamples to %.2f kHz.", mp3SampleRate(t->sampleRate, mp3Bitrate(format)) / 1000.);
        else ImGui::TextDisabled("Original sample rate preserved");
    } else ImGui::TextDisabled("Original sample rate preserved");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::Checkbox("Follow playback", &follow_);
    ImGui::TextDisabled("Vertical zoom"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##vertical", &verticalZoom_, .25f, 8, "%.1fx", ImGuiSliderFlags_Logarithmic);
    if (button("Reset view", {-1, 30})) { action("fit"); verticalZoom_ = 1; }
    ImGui::EndChild(); ImGui::PopStyleVar();
}
void Editor::emptyState(ImVec2 p, ImVec2 size) {
    auto d = drawList(); d.AddRectFilled(p, {p.x + size.x, p.y + size.y}, panel, 8);
    const float cx = p.x + size.x * .5f, cy = p.y + size.y * (compact_ ? .3f : .45f);
    if (compact_ && size.y < 190) {
        ImGui::SetCursorScreenPos(px({cx - 72.5f, p.y + (size.y - 44) * .5f}));
        if (button("Open audio", {145, 44}, true)) openDialog();
        return;
    }
    const auto centeredLabel = [&](float y, const char* value, ImU32 color, ImFont* font) {
        const float fontSize = font->FontSize * ImGui::GetIO().FontGlobalScale;
        const float width = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0, value).x / dpi_.layout;
        label({cx - width * .5f, y}, value, color, font);
    };
    centeredLabel(cy - 5, "Make room for your sound.", text, titleFont_);
    centeredLabel(cy + 33, compact_ ? "Open a WAV, FLAC or MP3 file." : "Drop a WAV, FLAC or MP3 file here to get started.", muted, bodyFont_);
    ImGui::SetCursorScreenPos(px({cx - 72.5f, cy + 78})); if (button("Open audio", {145, 40}, true)) openDialog();
    if (!compact_ || size.y > 330) centeredLabel(cy + 145, compact_ ? "Audio stays on your device." : "WAV / FLAC / MP3    /    MONO & STEREO", muted, bodyFont_);
}
void Editor::modals() {
    std::function<void()> retryAfterPopup;
    if (confirmDiscard_) { ImGui::OpenPopup("Keep your changes?"); confirmDiscard_ = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, px(compact_ ? ImVec2{16, 16} : ImVec2{24, 22}));
    const float modalWidth = std::min(520.f, logical(ImGui::GetIO().DisplaySize).x - 24);
    const float contentWidth = modalWidth - (compact_ ? 32 : 48);
    const auto modalText = [&](const char* value) {
        if (compact_) ImGui::TextWrapped("%s", value); else ImGui::TextUnformatted(value);
    };
    const auto fitModal = [&] {
        if (!compact_) return;
        const auto display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({display.x * .5f, display.y * .5f}, ImGuiCond_Always, {.5f, .5f});
        ImGui::SetNextWindowSize({px(modalWidth), 0}, ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints({px(modalWidth), 0}, {px(modalWidth), display.y - px(24)});
    };
    if (cleanupPopup_) { ImGui::OpenPopup("Audio cleanup"); cleanupPopup_ = false; }
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * .5f, ImGui::GetIO().DisplaySize.y * .5f}, ImGuiCond_Always, {.5f, .5f});
    ImGui::SetNextWindowSize({px(520), 0}, ImGuiCond_Always);
    fitModal();
    if (ImGui::BeginPopupModal("Audio cleanup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (compact_) ImGui::TextWrapped("Range: %s - %s", timecode(double(cleanupFirst_), cleanupBase_->sampleRate, true).c_str(), timecode(double(cleanupLast_), cleanupBase_->sampleRate, true).c_str());
        else ImGui::Text("Range: %s - %s", timecode(double(cleanupFirst_), cleanupBase_->sampleRate, true).c_str(), timecode(double(cleanupLast_), cleanupBase_->sampleRate, true).c_str());
        ImGui::BeginDisabled(busy_);
        const char* kinds[] = {"Normalize", "Remove DC offset", "High-pass filter", "Low-pass filter"};
        ImGui::SetNextItemWidth(px(std::min(360.f, contentWidth)));
        bool changed = ImGui::Combo("##cleanup-kind", &cleanupKind_, kinds, 4);
        ImGui::SetNextItemWidth(px(std::min(360.f, contentWidth)));
        if (cleanupKind_ == 0) {
            changed |= ImGui::SliderFloat("##normalize-target", &normalizeDb_, -60, 0, "Target  %.1f dBFS");
            modalText("Linked channels preserve stereo balance. Sample peaks, not LUFS.");
        } else if (cleanupKind_ == 1) modalText("Subtract the measured average of each channel in this range.");
        else {
            float& cutoff = cleanupKind_ == 2 ? highPassHz_ : lowPassHz_;
            changed |= ImGui::SliderFloat("##filter-cutoff", &cutoff, 10, float(cleanupBase_->sampleRate * .49), "Cutoff  %.0f Hz", ImGuiSliderFlags_Logarithmic);
            modalText("12 dB/octave Butterworth filter. Original sample rate preserved.");
        }
        if (changed) resetCleanup();
        ImGui::Spacing();
        if (button(playback_.playing() ? "Stop preview" : "Preview", {120, 36})) {
            if (playback_.playing()) playback_.pause(); else prepareCleanup(false);
        }
        ImGui::SameLine(); ImGui::BeginDisabled(!cleanupPreview_);
        if (ImGui::Checkbox("Bypass", &cleanupBypass_)) { if (playback_.playing()) previewCleanup(); }
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (button("Apply", {120, 36}, true)) prepareCleanup(true);
        ImGui::SameLine();
        if (button("Cancel", {120, 36}) || (!busy_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            resetCleanup(); cleanupBase_.reset(); ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (busy_) {
            ImGui::ProgressBar(jobProgress_.load(), px({std::min(360.f, contentWidth), 0}));
            if (button("Cancel processing", {160, 32})) job_.request_stop();
        }
        if (cleanupClose_) {
            cleanupClose_ = false; cleanupPreview_.reset(); cleanupBase_.reset(); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (insertSilencePopup_) { ImGui::OpenPopup("Insert silence"); insertSilencePopup_ = false; }
    fitModal();
    if (ImGui::BeginPopupModal("Insert silence", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto t = document_.timeline();
        ImGui::Text("At cursor: %s", timecode(double(silenceAt_), t->sampleRate, true).c_str());
        modalText("Existing audio moves right; the inserted silence becomes selected.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(px(std::min(220.f, contentWidth - 80)));
        if (ImGui::InputDouble("Seconds", &silenceSeconds_, 0, 0, "%.6f")) {
            const long double frames = std::round(static_cast<long double>(silenceSeconds_) * t->sampleRate);
            silenceFrames_ = std::isfinite(silenceSeconds_) && frames >= 1 && frames < static_cast<long double>(std::numeric_limits<Frame>::max())
                ? Frame(frames) : 0;
        }
        ImGui::SetNextItemWidth(px(std::min(220.f, contentWidth - 80)));
        if (ImGui::InputScalar("Samples", ImGuiDataType_S64, &silenceFrames_)) silenceSeconds_ = double(silenceFrames_) / t->sampleRate;
        const bool valid = silenceFrames_ > 0 && silenceFrames_ <= std::numeric_limits<Frame>::max() - t->frames;
        if (valid) {
            if (compact_) ImGui::TextWrapped("%lld samples / %.6f seconds at %u Hz", static_cast<long long>(silenceFrames_), double(silenceFrames_) / t->sampleRate, t->sampleRate);
            else ImGui::TextDisabled("%lld samples / %.6f seconds at %u Hz", static_cast<long long>(silenceFrames_), double(silenceFrames_) / t->sampleRate, t->sampleRate);
        }
        else ImGui::TextColored(ImVec4(.95f, .74f, .45f, 1), "Enter a positive duration that fits in the timeline.");
        ImGui::Spacing(); ImGui::BeginDisabled(!valid);
        if (button("Insert", {110, 36}, true) || (valid && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
            document_.insertSilence(silenceAt_, silenceFrames_, autoEdgeFades_ ? edgeFrames() : 0);
            anchor_ = cursor_ = silenceAt_; head_ = silenceAt_ + silenceFrames_;
            silenceSeconds_ = double(silenceFrames_) / t->sampleRate;
            afterEdit("Inserted silence"); ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (button("Cancel", {100, 36}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { silenceSeconds_ = 1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    fitModal();
    if (ImGui::BeginPopupModal("Keep your changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        modalText("This document has edits that have not been exported.");
        modalText("Export audio to keep your edits before opening another file or closing.");
        ImGui::Spacing();
        if (button("Export audio...", {150, 36}, true)) { continueAfterExport_ = true; ImGui::CloseCurrentPopup(); exportDialog(); }
        if (!compact_) ImGui::SameLine(); if (button("Discard edits", {130, 36})) { ImGui::CloseCurrentPopup(); if (pendingClose_) close_ = true; else openNow(pendingPath_); }
        if (!compact_) ImGui::SameLine(); if (button("Cancel", {90, 36})) {
            ImGui::CloseCurrentPopup();
#ifdef __EMSCRIPTEN__
            webClearImport();
#endif
        }
        ImGui::EndPopup();
    }
#ifdef __EMSCRIPTEN__
    if (memoryRetry_) ImGui::OpenPopup("Audio memory budget");
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * .5f, ImGui::GetIO().DisplaySize.y * .5f}, ImGuiCond_Always, {.5f, .5f});
    ImGui::SetNextWindowSize({px(modalWidth), 0}, ImGuiCond_Always);
    fitModal();
    if (ImGui::BeginPopupModal("Audio memory budget", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto used = AudioFile::audioBytes(), reclaimed = reclaimableAudioBytes(), limit = web::audioLimit.load();
        const auto after = used - std::min(used, reclaimed);
        const bool fits = used <= limit && memoryNeeded_ <= limit - used;
        const bool fitsCleared = reclaimed && after <= limit && memoryNeeded_ <= limit - after;
        ImGui::TextWrapped("%s needs another %.1f MiB of audio storage.", jobLabel_.c_str(), memoryNeeded_ / 1048576.);
        ImGui::Spacing(); audioBudgetSetting(); ImGui::Spacing();
        ImGui::TextWrapped("Clearing undo and redo can free %.1f MiB.", reclaimed / 1048576.);
        ImGui::TextWrapped("Your current audio, selection, clipboard and unsaved changes will be kept. Cleared edits cannot be undone.");
        if (!fits && !fitsCleared) {
            ImGui::Spacing();
            ImGui::TextWrapped("Clearing undo is not enough at this budget. Increase the budget, process a shorter selection, or use the native app.");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!fits);
        if (button("Retry", {-1, 36}, fits)) {
            retryAfterPopup = std::exchange(memoryRetry_, {}); error_.clear(); memoryNeeded_ = 0; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!fitsCleared);
        if (button("Clear undo and retry", {-1, 36}, !fits && fitsCleared)) {
            // Release any retired playback revision before the retry allocates.
            playback_.setTimeline(document_.timeline(), cursor_);
            document_.clearHistory(); syncPlayback_ = true;
            retryAfterPopup = std::exchange(memoryRetry_, {}); error_.clear(); memoryNeeded_ = 0; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (button("Cancel", {-1, 36}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            memoryRetry_ = {}; memoryNeeded_ = 0; error_.clear(); ImGui::CloseCurrentPopup();
            if (cleanupBase_) cleanupPopup_ = true;
        }
        ImGui::EndPopup();
    }
#endif
    if (!error_.empty() && !memoryRetry_) ImGui::OpenPopup("A quick note");
    fitModal();
    if (ImGui::BeginPopupModal("A quick note", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(px(std::min(460.f, contentWidth))); ImGui::TextUnformatted(error_.c_str()); ImGui::PopTextWrapPos();
        if (button("Got it", {110, 36}, true)) { error_.clear(); ImGui::CloseCurrentPopup(); } ImGui::EndPopup();
    }
    if (help_) { ImGui::OpenPopup("Find your way around"); help_ = false; }
    fitModal();
    if (ImGui::BeginPopupModal("Find your way around", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TIMELINE"); ImGui::Spacing();
        const char* rows[][2] = {{wheelZooms_ ? "Wheel" : "Ctrl + wheel", "Zoom around the pointer"}, {wheelZooms_ ? "Ctrl + wheel" : "Wheel", "Scroll through audio"}, {"Horizontal wheel", "Scroll through audio"}, {"Navigator wheel", "Zoom around the current view"}, {"Middle-button drag", "Pan directly"}, {"Alt + wheel", "Adjust vertical scale"}, {"Drag / Shift-click", "Select / extend range"}, {"Drag inside / Alt-drag", "Move range / draw new range"}, {"Double-click", "Select all / clear full selection"}, {"Space / L", "Play-pause / toggle loop"}, {"F / Z", "Fit file / zoom to selection"}, {"Ctrl + X / C / V", "Cut / copy / paste (mix into selection)"}, {"Ctrl + Shift + V", "Replace selection with clipboard"}, {"Ctrl + T / Delete", "Trim / delete selection"}, {"Ctrl + Z / Ctrl + Shift + Z", "Undo / redo"}, {"R / Home / End", "Reverse / jump to start / end"}, {"Ctrl + O / Ctrl + E", "Open audio / export audio"}, {"I / O", "Fade in / fade out selection"}, {"Insert", "Insert silence at cursor"}, {"J", "Snap selection to zero crossings"}, {"Ctrl + Shift + E", "Save selected audio (WAV/FLAC/MP3)"}};
        if (ImGui::BeginTable("keys", 2, 0, px({compact_ ? contentWidth : 510, 0}))) { for (auto& row : rows) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", row[0]); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", row[1]); } ImGui::EndTable(); }
        ImGui::Spacing(); if (button("Back to the sound", {-1, 36}, true)) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
    }
    if (openPathPopup_) { ImGui::OpenPopup("Open path"); openPathPopup_ = false; }
    fitModal();
    if (ImGui::BeginPopupModal("Open path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(px(480)); ImGui::InputText("WAV / FLAC / MP3 path", pathInput_, sizeof(pathInput_));
        if (button("Open")) { requestOpen(fromUtf8(pathInput_)); ImGui::CloseCurrentPopup(); } ImGui::SameLine(); if (button("Cancel")) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
    }
    if (exportPathPopup_) { ImGui::OpenPopup("Export path"); exportPathPopup_ = false; }
    fitModal();
    if (ImGui::BeginPopupModal("Export path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(px(480)); ImGui::InputText("New audio path", pathInput_, sizeof(pathInput_));
        if (button("Export")) { exportTo(fromUtf8(pathInput_), selectionExport_); ImGui::CloseCurrentPopup(); } ImGui::SameLine(); if (button("Cancel")) { continueAfterExport_ = false; ImGui::CloseCurrentPopup(); } ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (retryAfterPopup) retryAfterPopup();
}
bool Editor::animating() const { return ImGui::GetTime() - lastInteraction_ < .3 || ImGui::IsAnyItemActive() || playback_.playing() || busy_ || selecting_ || (source_ && !source_->peaksReady() && source_->peakProgress() >= 0) || view_.start != view_.targetStart || view_.span != view_.targetSpan; }
void Editor::render() {
    if (jobDone_.exchange(false, std::memory_order_acquire)) {
        if (job_.joinable()) job_.join(); busy_.store(false);
        // Move the callback before invoking it: exporting from the discard prompt
        // may start an import with its own completion callback.
        auto done = std::exchange(onJobDone_, {});
        auto retry = std::exchange(onJobRetry_, {});
        const auto failure = jobError_;
        if (opening_) {
            opening_ = false;
#ifdef __EMSCRIPTEN__
            webClearImport();
#endif
        }
        if (!failure.empty()) {
            error_ = failure;
            if (jobMemoryNeeded_ && retry) { memoryNeeded_ = jobMemoryNeeded_; memoryRetry_ = std::move(retry); }
        } else if (done) done();
    }
    auto& io = ImGui::GetIO();
    if (document_.timeline()) view_.bounds(double(std::max(selectionExtent(), rangeDrag_.active ? rangeDrag_.limit : boundaryDrag_.active ? boundaryDragLimit_ : Frame(0))));
    view_.update(io.DeltaTime);
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 || io.MouseWheelH != 0) lastInteraction_ = ImGui::GetTime();
    if (source_) cursor_ = playback_.position();
    if (playback_.playing() && follow_) view_.follow(double(cursor_));
    frameMs_ = std::lerp(frameMs_, io.DeltaTime * 1000.f, .05f);
    if (ImGui::GetTime() - lastMemoryPoll_ > 1) { resident_ = residentBytes(); lastMemoryPoll_ = ImGui::GetTime(); }
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("ColdEdit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    shortcuts(); const float width = io.DisplaySize.x / dpi_.layout, height = io.DisplaySize.y / dpi_.layout;
    compact_ = width < 960 || height < 500;
    header(width);
    if (compact_) {
        const float top = width >= 640 ? 81.f : 130.f;
        if (compactTools_) {
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, px(22));
            inspector({12, top}, {width - 24, height - top - 46});
            ImGui::PopStyleVar();
        } else {
            const float canvasHeight = std::max(64.f, height - top - 108);
            waveform({12, top}, {width - 24, canvasHeight});
            overview({12, top + 10 + canvasHeight}, {width - 24, 54});
        }
    } else {
        const float mainWidth = width - 294, canvasHeight = std::max(180.f, height - 220);
        waveform({24, 81}, {mainWidth, canvasHeight});
        overview({24, 81 + canvasHeight + 18}, {mainWidth, 73});
        playbackPanel({width - 250, 81}, {226, 198});
        inspector({width - 250, 291}, {226, height - 337});
    }
    auto d = drawList(); d.AddLine({0, height - 37}, {width, height - 37}, line);
    const auto performance = decimal(currentPeakBytes() / 1048576., 1) + " MB peaks  /  " + decimal(resident_ / 1048576., 0) +
#ifdef __EMSCRIPTEN__
        " MB WASM heap";
    webSetDirty(document_.dirty());
#else
        " MB resident";
#endif
    float statusRight = width - 24;
    if (width >= 1200) {
        const float performanceWidth = ImGui::CalcTextSize(performance.c_str()).x / dpi_.layout;
        statusRight -= performanceWidth;
        label({statusRight, height - 27}, performance, muted);
        statusRight -= 24;
    }
    if (source_ && !compact_) {
        const auto detail = decimal(source_->sampleRate / 1000., 1) + " kHz   /   " + (document_.timeline()->channels == 2 ? "Stereo" : "Mono") + "   /   " + (source_->format == "WAV" ? std::to_string(source_->bits) + (source_->floating ? "-bit float" : "-bit PCM") : source_->format);
        statusRight -= ImGui::CalcTextSize(detail.c_str()).x / dpi_.layout;
        label({statusRight, height - 27}, detail, muted);
        statusRight -= 24;
    }
    if (busy_) {
        const float cancelX = std::min(310.f, statusRight - 140);
        ImGui::SetCursorPos(px({cancelX, height - 34}));
        if (button("Cancel operation", {140, 28})) job_.request_stop();
        statusRight = cancelX - 12;
    }
    // Long filenames and operation messages cannot cover the audio properties.
    ImGui::PushClipRect(px({24, height - 36}), px({statusRight, height}), true);
    label({24, height - 27}, busy_ ? jobLabel_ + "  " + decimal(jobProgress_.load() * 100, 0) + "%" : status_, busy_ ? teal : muted);
    ImGui::PopClipRect();
    modals(); ImGui::End();
    const std::string windowTitle = "ColdEdit - " + title_ + (document_.dirty() ? " *" : ""); glfwSetWindowTitle(window_, windowTitle.c_str());
}
void Editor::smokeStep(int frame) {
    if (!source_) return;
    const auto frames = document_.timeline()->frames;
    if (frame == 0) playback_.setVolume(0); // Exercise real output without making noise.
    if (frame == 8) { anchor_ = frames / 4; head_ = frames * 2 / 5; action("copy"); }
    if (frame == 12) action("reverse");
    if (frame == 16) action("undo");
    if (frame == 20) action("redo");
    if (frame == 24) { action("paste-replace"); action("undo"); }
    if (frame == 28) view_.zoom(.55, .4);
    if (frame == 34) togglePlay();
    if (frame == 64) { playback_.pause(); cursor_ = playback_.position(); }
    if (frame == 55) view_.pan(view_.span * .12);
    if (frame == 75) { cursor_ = frames / 3; playback_.seek(cursor_); }
    if (frame == 88) action("clear"); // Start a fresh selection before testing edge edits.
    if (frame == 98) { smokePassed_ = smokePassed_ && selected() && last() - first() < frames / 4; smokeSpan_ = view_.targetSpan; }
    if (frame == 105) smokePassed_ = smokePassed_ && view_.targetSpan < smokeSpan_;
    if (frame == 110) { view_.show(view_.start, view_.span, true); smokeFirst_ = first(); smokeLast_ = last(); }
    if (frame == 118) {
        smokeEdgesPassed_ &= first() > smokeFirst_ && last() == smokeLast_;
        ++smokeBoundaryChecks_; smokeFirst_ = first(); smokeLast_ = last();
    }
    if (frame == 128) {
        smokeEdgesPassed_ &= first() == smokeFirst_ && last() > smokeLast_;
        ++smokeBoundaryChecks_; smokeFirst_ = first(); smokeLast_ = last();
    }
    if (frame == 138) {
        smokeEdgesPassed_ &= first() == smokeLast_ && last() > smokeLast_;
        ++smokeBoundaryChecks_; smokeFirst_ = first(); smokeLast_ = last();
    }
    if (frame == 152) {
        smokeEdgesPassed_ &= first() == smokeFirst_ && last() > smokeLast_ && !boundaryDrag_.active;
        ++smokeBoundaryChecks_;
    }
    if (frame == 156) {
        const auto before = document_.timeline();
        action("fade-in"); smokePassed_ &= document_.timeline()->sample(first(), 0) == 0;
        action("undo"); smokePassed_ &= document_.timeline() == before;
        action("fade-out"); smokePassed_ &= document_.timeline()->sample(last() - 1, 0) == 0;
        action("undo"); smokePassed_ &= document_.timeline() == before;
        const auto range = before->range(first(), last());
        smokePassed_ &= range.frames == last() - first();
        document_.insertSilence(cursor_, before->sampleRate); afterEdit("Inserted silence");
        smokePassed_ &= document_.timeline()->frames == before->frames + before->sampleRate;
        action("undo"); smokePassed_ &= document_.timeline() == before;
    }
    if (frame == 160) {
        anchor_ = frames / 4; head_ = frames / 2; view_.show(0, double(frames), true);
        smokeFirst_ = first(); smokeLast_ = last();
    }
    if (frame == 168) {
        smokePassed_ &= first() > smokeFirst_ && last() - first() == smokeLast_ - smokeFirst_ && !rangeDrag_.active;
        anchor_ = smokeFirst_; head_ = smokeLast_;
        action("cut"); smokePassed_ &= first() == smokeFirst_ && last() == smokeLast_;
        action("undo"); smokePassed_ &= document_.timeline()->frames == frames;
    }
}
void Editor::smokeInput(int frame) {
    auto& io = ImGui::GetIO();
    auto mouse = [&](float x, float y) { const auto p = px({x, y}); io.AddMousePosEvent(p.x, p.y); };
    const auto edgeX = [&](Frame f) { return wavePos_.x + 46 + float((double(f) - view_.start) / view_.span) * (waveSize_.x - 46); };
    if (frame == 91) { io.AddFocusEvent(true); mouse(wavePos_.x + 360, wavePos_.y + 130); }
    if (frame == 92) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 94) mouse(wavePos_.x + 520, wavePos_.y + 130);
    if (frame == 96) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (frame == 100) io.AddMouseWheelEvent(0, 1);
    if (frame == 111) mouse(edgeX(smokeFirst_) + 3, wavePos_.y + 34);
    if (frame == 112) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 114) mouse(edgeX(smokeFirst_) + 63, wavePos_.y + 34);
    if (frame == 116) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (frame == 121) mouse(edgeX(smokeLast_) - 3, wavePos_.y + 140);
    if (frame == 122) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 124) mouse(edgeX(smokeLast_) + 57, wavePos_.y + 140);
    if (frame == 126) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (frame == 131) mouse(edgeX(smokeFirst_), wavePos_.y + 34);
    if (frame == 132) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 134) mouse(edgeX(smokeLast_) + 40, wavePos_.y + 34);
    if (frame == 136) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (frame == 141) mouse(edgeX(smokeLast_), wavePos_.y + 90);
    if (frame == 142) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 144) mouse(wavePos_.x + waveSize_.x + 80, wavePos_.y + 90);
    if (frame == 150) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    if (frame == 161) mouse(edgeX((smokeFirst_ + smokeLast_) / 2), wavePos_.y + 90);
    if (frame == 162) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (frame == 164) mouse(edgeX((smokeFirst_ + smokeLast_) / 2) + 80, wavePos_.y + 90);
    if (frame == 166) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
}
std::string Editor::diagnostics() const {
    std::ostringstream out;
    out << "{\n  \"ui_input_checks_passed\": " << (smokeBoundaryChecks_ ? (smokePassed() ? "true" : "false") : "null")
        << ",\n  \"selection_boundary_checks\": " << smokeBoundaryChecks_
        << ",\n  \"selection_boundaries_passed\": " << (smokeBoundaryChecks_ ? (smokeEdgesPassed_ ? "true" : "false") : "null")
        << ",\n  \"ui_scale\": " << dpi_.layout
        << ",\n  \"font_raster_scale\": " << dpi_.raster
        << ",\n  \"font_pixel_size\": " << bodyFont_->FontSize
        << ",\n  \"dpi_rebuilds\": " << dpiChanges_
        << ",\n  \"display_width\": " << ImGui::GetIO().DisplaySize.x
        << ",\n  \"display_height\": " << ImGui::GetIO().DisplaySize.y
        << ",\n  \"compact_layout\": " << (compact_ ? "true" : "false")
        << ",\n  \"tools_open\": " << (compact_ && compactTools_ ? "true" : "false")
        << ",\n  \"framebuffer_scale\": " << ImGui::GetIO().DisplayFramebufferScale.x
        << ",\n  \"font_global_scale\": " << ImGui::GetIO().FontGlobalScale
        << ",\n  \"can_undo\": " << (document_.canUndo() ? "true" : "false")
        << ",\n  \"can_redo\": " << (document_.canRedo() ? "true" : "false")
        << ",\n  \"popup_open\": " << (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ? "true" : "false")
        << ",\n  \"audio_available\": " << (playback_.available() ? "true" : "false")
        << ",\n  \"playing\": " << (playback_.playing() ? "true" : "false")
        << ",\n  \"position\": " << playback_.position()
        << ",\n  \"busy\": " << (busy_ ? "true" : "false")
        << ",\n  \"dirty\": " << (document_.dirty() ? "true" : "false")
        << ",\n  \"selection_start\": " << first()
        << ",\n  \"selection_end\": " << last()
        << ",\n  \"view_start\": " << view_.start
        << ",\n  \"view_cursor\": " << cursor_
        << ",\n  \"view_span\": " << view_.span
        << ",\n  \"view_target_start\": " << view_.targetStart
        << ",\n  \"view_target_span\": " << view_.targetSpan
        << ",\n  \"wheel_zooms\": " << (wheelZooms_ ? "true" : "false")
        << ",\n  \"vertical_zoom\": " << verticalZoom_
        << ",\n  \"follow_playback\": " << (follow_ ? "true" : "false")
        << ",\n  \"title\": " << std::quoted(title_)
        << ",\n  \"error\": " << std::quoted(error_)
        << ",\n  \"peak_bytes\": " << currentPeakBytes()
        << ",\n  \"resident_bytes\": " << residentBytes()
        << ",\n  \"history_states\": " << document_.historySize()
        << ",\n  \"frames\": " << (document_.timeline() ? document_.timeline()->frames : 0)
        << ",\n  \"channels\": " << (document_.timeline() ? document_.timeline()->channels : 0)
        << ",\n  \"processed_bytes\": " << AudioFile::processedBytes()
        << ",\n  \"audio_bytes\": " << AudioFile::audioBytes()
#ifdef __EMSCRIPTEN__
        << ",\n  \"frame_count\": " << ImGui::GetFrameCount()
        << ",\n  \"pending_input_events\": " << ImGui::GetCurrentContext()->InputEventsQueue.Size
        << ",\n  \"audio_limit\": " << web::audioLimit.load()
        << ",\n  \"memory_recovery\": " << (memoryRetry_ ? "true" : "false")
        << ",\n  \"memory_needed\": " << memoryNeeded_
        << ",\n  \"memory_reclaimable\": " << (memoryRetry_ ? reclaimableAudioBytes() : 0)
#endif
        << ",\n  \"export_format\": " << exportFormat_
        << ",\n  \"sample_rate\": " << (source_ ? source_->sampleRate : 0)
        << ",\n  \"cleanup_preview\": " << (cleanupPreview_ ? "true" : "false")
        << ",\n  \"cleanup_bypass\": " << (cleanupBypass_ ? "true" : "false")
        << ",\n  \"snap_zero\": " << (snapZero_ ? "true" : "false")
        << ",\n  \"auto_edge_fades\": " << (autoEdgeFades_ ? "true" : "false")
        << ",\n  \"undo_label\": " << std::quoted(document_.undoLabel())
        << ",\n  \"underruns\": " << playback_.underruns() << "\n}\n";
    return out.str();
}
}
