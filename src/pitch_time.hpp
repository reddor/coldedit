#pragma once
#include "stretch.hpp"
#include "pitch_analysis.hpp"
#include <utility>

namespace coldedit {
struct PitchSettings {
    double semitones = 0;
    double formantSemitones = 0;
    bool preserveFormants = true;
    double formantBaseHz = 0; // zero lets the processor estimate the fundamental
    bool identity() const { return semitones == 0 && formantSemitones == 0; }
    void validate(unsigned sampleRate) const;
    bool operator==(const PitchSettings&) const = default;
};

// Both controls always render from this retained source, never from a previous
// processed result. A document/history revision owns one immutable state.
struct PitchTimeState {
    using NoteKey = std::pair<unsigned, Frame>; // voice and source start survive timing edits
    struct HarmonyVoice { std::vector<PitchNote> notes; double gain = .35; };
    std::shared_ptr<const Timeline> source;
    StretchMap time;
    PitchSettings pitch;
    std::shared_ptr<const PitchAnalysis> analysis;
    std::vector<PitchNote> notes;
    // Empty slots are reused so removing a voice never renumbers other voices.
    std::vector<HarmonyVoice> harmonies;
    std::size_t noteCount() const;
    std::pair<unsigned, std::size_t> locateNote(std::size_t index) const;
    const std::vector<PitchNote>& voiceNotes(unsigned voice) const;
    std::vector<PitchNote>& voiceNotes(unsigned voice);
    const PitchNote& noteAt(std::size_t index) const;
    PitchNote& noteAt(std::size_t index);
    NoteKey noteKey(std::size_t index) const;
    unsigned addHarmony(const std::vector<std::size_t>& indices, double semitones);
    static PitchTimeState identity(std::shared_ptr<const Timeline> source, double bpm);
    bool hasNoteEdits() const;
    bool hasPitchChanges() const { return !pitch.identity() || hasNoteEdits(); }
    bool identity() const { return time.identity() && !hasPitchChanges(); }
    double noteShiftAt(double sourceFrame, unsigned voice = 0) const;
    bool splitNote(std::size_t index, Frame sourceFrame);
    void validate() const;
};

std::shared_ptr<Timeline> renderPitchTime(const PitchTimeState& state,
    std::atomic<float>* progress = nullptr, std::stop_token stop = {});
// Cheap waveform/varispeed preview, not an audible pitch preview. Clients with
// active pitch/formant changes must render before adopting this for playback.
std::shared_ptr<Timeline> previewPitchTime(const PitchTimeState& state);
bool needsPitchTimeRender(const Timeline& timeline);
void resolvePitchTime(const Timeline& timeline, std::atomic<float>* progress = nullptr,
    std::stop_token stop = {});

// Latest document wins. Completion upgrades only its own audio source, never
// replacing a document/history revision. DSP runs outside the audio callback.
class PitchTimeRenderQueue {
public:
    ~PitchTimeRenderQueue();
    void update(std::shared_ptr<const Timeline> timeline, bool suspended = false);
    void suspend();
    void wait(std::stop_token stop) const; // foreground worker only
    bool active() const { return running_.load(); }
    float progress() const { return progress_.load(); }
    const std::string& error() const { return error_; }
    void retry() { failed_.reset(); error_.clear(); }
private:
    std::jthread worker_;
    std::atomic<bool> running_{false}, done_{false};
    std::atomic<float> progress_{0};
    std::shared_ptr<const Timeline> target_, failed_;
    std::string workerError_, error_;
};
}
