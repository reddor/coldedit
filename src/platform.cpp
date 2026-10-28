#include "platform.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <stdexcept>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <unistd.h>
#endif
namespace coldedit {
std::string utf8(const std::filesystem::path& p) { const auto u = p.u8string(); return {u.begin(), u.end()}; }
std::filesystem::path fileDialog(GLFWwindow* window, bool save, const std::filesystem::path& suggested) {
#ifdef _WIN32
    wchar_t name[32768]{};
    if (!suggested.empty()) wcsncpy_s(name, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = glfwGetWin32Window(window);
    const bool flac = save && suggested.extension() == ".flac";
    const bool mp3 = save && suggested.extension() == ".mp3";
    ofn.lpstrFilter = save ? (mp3 ? L"MP3 audio (*.mp3)\0*.mp3\0" : flac ? L"FLAC audio (*.flac)\0*.flac\0" : L"Wave audio (*.wav)\0*.wav\0") : L"Audio (*.wav;*.flac;*.mp3)\0*.wav;*.flac;*.mp3\0All files\0*.*\0";
    ofn.lpstrFile = name; ofn.nMaxFile = 32768;
    ofn.lpstrDefExt = mp3 ? L"mp3" : flac ? L"flac" : L"wav"; ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? 0 : OFN_FILEMUSTEXIST);
    ofn.lpstrTitle = save ? L"Export audio to a new file" : L"Open WAV, FLAC or MP3 audio";
    if (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) return name;
#else
    (void)window; (void)save; (void)suggested;
#endif
    return {};
}
std::uint64_t residentBytes() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_heap_size();
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
#elif defined(__linux__)
    std::ifstream f("/proc/self/statm"); std::uint64_t total = 0, resident = 0;
    if (f >> total >> resident) return resident * std::uint64_t(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}
#ifdef __EMSCRIPTEN__
EM_JS(void, browserPicker, (bool pasteFromFile), { window.coldeditWeb.pickFile(!!pasteFromFile); });
EM_JS(void, browserClearImport, (), { window.coldeditWeb.clearImport(); });
EM_JS(void, browserSetDirty, (bool dirty), { window.coldeditWeb.dirty = !!dirty; });
EM_JS(void, browserShowRecorder, (), { window.coldeditRecorder.show(); });
EM_JS(int, downloadBrowserFile, (const char* path), { return window.coldeditWeb.download(UTF8ToString(path)) ? 1 : 0; });
EM_JS(int, loadBrowserWheelZoom, (), {
    try { return localStorage.getItem('coldedit.wheelZooms') === 'false' ? 0 : 1; }
    catch (_) { return 1; }
});
EM_JS(int, saveBrowserWheelZoom, (bool zoom), {
    try { localStorage.setItem('coldedit.wheelZooms', zoom ? 'true' : 'false'); return 1; }
    catch (_) { return 0; }
});
EM_JS(unsigned, loadBrowserAudioBudget, (), {
    try {
        const saved = localStorage.getItem('coldedit.audioBudgetMiB');
        return saved === '512' ? 512 : saved === '256' ? 256 : 128;
    }
    catch (_) { return 128; }
});
EM_JS(int, saveBrowserAudioBudget, (unsigned mib), {
    document.getElementById('audio-budget').textContent = String(mib);
    try { localStorage.setItem('coldedit.audioBudgetMiB', String(mib)); return 1; }
    catch (_) { return 0; }
});
unsigned loadWebAudioBudget() { return loadBrowserAudioBudget(); }
bool saveWebAudioBudget(unsigned mib) { return saveBrowserAudioBudget(mib) != 0; }
void webOpenPicker(bool pasteFromFile) { browserPicker(pasteFromFile); }
void webClearImport() { browserClearImport(); }
void webSetDirty(bool dirty) { browserSetDirty(dirty); }
void webShowRecorder() { browserShowRecorder(); }
bool webDownload(const std::filesystem::path& path) { return downloadBrowserFile(utf8(path).c_str()) != 0; }
bool loadWheelZoomPreference() { return loadBrowserWheelZoom() != 0; }
bool saveWheelZoomPreference(bool zoom) { return saveBrowserWheelZoom(zoom) != 0; }
#else
namespace {
std::filesystem::path preferencesPath() {
#ifdef _WIN32
    wchar_t directory[32768]{};
    const auto length = GetEnvironmentVariableW(L"APPDATA", directory, 32768);
    if (length > 0 && length < 32768) return std::filesystem::path(directory) / "ColdEdit" / "preferences.ini";
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config) return fromUtf8(config) / "coldedit" / "preferences.ini";
    if (const char* home = std::getenv("HOME"); home && *home) return fromUtf8(home) / ".config" / "coldedit" / "preferences.ini";
#endif
    return {};
}
}
bool loadWheelZoomPreference() {
    try {
        std::ifstream input(preferencesPath());
        std::string setting;
        while (std::getline(input, setting)) {
            if (setting == "wheel_zooms=0") return false;
            if (setting == "wheel_zooms=1") return true;
        }
    } catch (const std::exception&) {}
    return true;
}
bool saveWheelZoomPreference(bool zoom) {
    try {
        const auto path = preferencesPath();
        if (path.empty()) return false;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        output << "wheel_zooms=" << (zoom ? 1 : 0) << '\n';
        output.close();
        return bool(output);
    } catch (const std::exception&) { return false; }
}
#endif
static void write16(std::ostream& f, unsigned v) { f.put(char(v)); f.put(char(v >> 8)); }
static void write32(std::ostream& f, unsigned v) { write16(f, v); write16(f, v >> 16); }
void saveFramebuffer(const std::filesystem::path& path, int w, int h) {
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(std::size_t(stride) * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) std::swap(pixels[std::size_t(y) * stride + x * 3], pixels[std::size_t(y) * stride + x * 3 + 2]);
    std::ofstream f(path, std::ios::binary);
    f.write("BM", 2); write32(f, 54 + stride * h); write32(f, 0); write32(f, 54);
    write32(f, 40); write32(f, w); write32(f, h); write16(f, 1); write16(f, 24);
    write32(f, 0); write32(f, stride * h); write32(f, 2835); write32(f, 2835); write32(f, 0); write32(f, 0);
    f.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    if (!f) throw std::runtime_error("Could not save framebuffer capture.");
}
}
