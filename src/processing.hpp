#pragma once
#include "document.hpp"
namespace coldedit {
enum class Cleanup { Normalize, RemoveDC, HighPass, LowPass };
struct CleanupSettings { Cleanup kind = Cleanup::Normalize; double value = -1; };
const char* cleanupName(Cleanup kind);
std::shared_ptr<Timeline> processAudio(const Timeline& source, Frame first, Frame last,
    CleanupSettings settings, std::atomic<float>* progress = nullptr, std::stop_token stop = {});
// Sum clipboard audio into the range, trimming excess clipboard frames. The
// untouched tail stays shared; only the mixed interval needs rendered PCM.
std::shared_ptr<Timeline> mixAudio(const Timeline& source, const Timeline& clipboard,
    Frame first, Frame last, std::atomic<float>* progress = nullptr, std::stop_token stop = {});
}
