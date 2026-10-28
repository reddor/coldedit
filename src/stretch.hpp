#pragma once
#include "document.hpp"

namespace coldedit {
struct StretchAnchor {
    double sourceFrame = 0, beat = 0; // beats are zero-based internally
    bool operator==(const StretchAnchor&) const = default;
};
struct StretchMap {
    Frame sourceFrames = 0;
    unsigned sampleRate = 0;
    double bpm = 120, sourceBpm = 120;
    std::vector<StretchAnchor> anchors; // user anchors only; outer audio keeps its source tempo
    static constexpr std::size_t maxAnchors = 2048;
    static StretchMap identity(Frame frames, unsigned rate, double bpm);
    bool identity() const;
    double framesPerBeat() const;
    double startBeat() const;
    Frame frames() const;
    double sourceAt(double outputFrame) const;
    double outputAt(double sourceFrame) const;
    bool add(double outputFrame);
    bool move(std::size_t index, double beat);
    void validate() const;
    bool operator==(const StretchMap&) const = default;
};

struct RhythmAnalysis {
    double bpm = 0, confidence = 0; // zero BPM means no reliable pulse found
    std::vector<Frame> transients;
    bool limited = false;
};
RhythmAnalysis analyzeRhythm(const Timeline& source, std::atomic<float>* progress = nullptr,
    std::stop_token stop = {});
}
