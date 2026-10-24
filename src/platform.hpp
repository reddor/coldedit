#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
struct GLFWwindow;
namespace coldedit {
std::filesystem::path fileDialog(GLFWwindow* window, bool save, const std::filesystem::path& suggested = {});
std::string utf8(const std::filesystem::path& path);
inline std::filesystem::path fromUtf8(std::string_view value) { return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size())); }
std::uint64_t residentBytes();
bool loadWheelZoomPreference();
bool saveWheelZoomPreference(bool zoom);
void saveFramebuffer(const std::filesystem::path& path, int width, int height);
#ifdef __EMSCRIPTEN__
unsigned loadWebAudioBudget();
bool saveWebAudioBudget(unsigned mib);
void webOpenPicker(bool pasteFromFile = false);
bool webDownload(const std::filesystem::path& path);
void webClearImport();
void webSetDirty(bool dirty);
#endif
}
