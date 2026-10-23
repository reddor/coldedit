#pragma once
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace coldedit::web {
inline constexpr std::uint64_t inputLimit = 64ull * 1024 * 1024;
inline constexpr std::uint64_t exportLimit = 128ull * 1024 * 1024;
inline constexpr std::uint64_t defaultAudioLimit = 128ull * 1024 * 1024;
inline constexpr std::uint64_t extendedAudioLimit = 256ull * 1024 * 1024;
inline std::atomic<std::uint64_t> audioLimit{defaultAudioLimit};
inline std::atomic<std::uint64_t> audioBytes{0};
inline bool setAudioLimit(std::uint64_t bytes) {
    if ((bytes != defaultAudioLimit && bytes != extendedAudioLimit) || bytes < audioBytes.load()) return false;
    audioLimit.store(bytes); return true;
}
struct AudioBudgetExceeded : std::runtime_error {
    std::uint64_t additionalBytes;
    explicit AudioBudgetExceeded(std::uint64_t additional)
        : std::runtime_error("Audio would exceed the web editor's shared " + std::to_string(audioLimit.load() / (1024 * 1024)) +
            " MiB budget (including effects, clipboard and undo history). Choose a larger audio budget in the ColdEdit menu, process a shorter selection, or use the native app."), additionalBytes(additional) {}
};
inline void checkAudio(std::uint64_t bytes, std::uint64_t owned = 0) {
    const auto others = audioBytes.load() - owned;
    const auto limit = audioLimit.load();
    if (bytes > limit - std::min(others, limit))
        throw AudioBudgetExceeded(bytes > owned ? bytes - owned : 0);
}
// One reservation per storage allocation, shared by every timeline referencing it.
struct AudioReservation {
    std::uint64_t bytes = 0;
    AudioReservation() = default;
    AudioReservation(const AudioReservation&) = delete;
    AudioReservation& operator=(const AudioReservation&) = delete;
    ~AudioReservation() { audioBytes.fetch_sub(bytes); }
    void resize(std::uint64_t next) {
        auto used = audioBytes.load();
        do {
            const auto limit = audioLimit.load();
            if (next > limit - std::min(used - bytes, limit))
                throw AudioBudgetExceeded(next > bytes ? next - bytes : 0);
        } while (!audioBytes.compare_exchange_weak(used, used - bytes + next));
        bytes = next;
    }
};
inline void checkInput(std::uint64_t bytes) {
    if (bytes > inputLimit) throw std::runtime_error("The web editor opens WAV, FLAC and MP3 files up to 64 MiB. Use ColdEdit for Windows for larger recordings (download above the editor).");
}
inline void checkExport(std::uint64_t frames, unsigned channels, unsigned bits) {
    // Divide first: even an extreme pasted timeline cannot overflow this check.
    const auto bytesPerFrame = channels * (bits / 8);
    if (!bytesPerFrame || frames > (exportLimit - 56) / bytesPerFrame)
        throw std::runtime_error("This export exceeds the web editor's 128 MiB limit. Export a selection, choose a lower bit depth, or use ColdEdit for Windows (download above the editor).");
}
}
