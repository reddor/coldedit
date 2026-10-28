#pragma once
#include "audio_file.hpp"
#ifndef __EMSCRIPTEN__
namespace coldedit {
class MicrophoneRecording {
public:
    // Manual input exercises the capture callback without opening a device.
    explicit MicrophoneRecording(bool manualInput = false);
    ~MicrophoneRecording();
    std::shared_ptr<AudioFile> finish();
    Frame frames() const;
    float level() const;
    bool failed() const;
    std::string error() const; // Call after finish or when failed() is true.
    void capture(const float* samples, unsigned frames) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
#endif
