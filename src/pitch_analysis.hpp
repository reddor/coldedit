#pragma once
#include "document.hpp"

namespace coldedit {
struct PitchFrame {
    Frame sourceFrame = 0;
    float midi = 0, confidence = 0; // zero means silence or no pitch context
    float rms = 0; // short-window AC level, independent of pitch confidence
    bool inferred = false;
};
struct PitchNote {
    Frame first = 0, last = 0; // half-open interval in retained source frames
    double midi = 0, shift = 0;
    float confidence = 0;
    float inferredFraction = 0;
    double vibrato = 1; // scales measured pitch variation around midi
    Frame fadeFirst = -1, fadeLast = -1; // retained outer envelope after splitting
    double envelopeAt(double sourceFrame, unsigned sampleRate) const;
    double shiftAt(double sourceFrame, unsigned sampleRate) const;
};
struct PitchAnalysis {
    Frame sourceFrames = 0, hop = 0;
    unsigned sampleRate = 0;
    std::vector<PitchFrame> contour;
    std::vector<PitchNote> notes;
    static constexpr std::size_t maxFrames = 1000000, maxNotes = 8192;
};
// Refine raw detector frames using sustained pitch context, then form notes.
// Audible gaps may be inferred; silence never acquires a pitch.
void refinePitchAnalysis(PitchAnalysis& analysis, std::stop_token stop = {});
// Monophonic, 50-1200 Hz. Streaming analysis uses bounded filter/window storage;
// only the compact contour and note suggestions grow with recording length.
PitchAnalysis analyzePitch(const Timeline& source, std::atomic<float>* progress = nullptr,
    std::stop_token stop = {});
}
