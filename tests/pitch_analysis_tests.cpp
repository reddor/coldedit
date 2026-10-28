#include "pitch_time.hpp"
#include "processing.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <future>

using namespace coldedit;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
template<class F> void rejects(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
std::shared_ptr<Timeline> audio(unsigned rate, unsigned channels, double seconds, const std::function<float(double, unsigned)>& sample) {
    auto file = AudioFile::render(Frame(seconds * rate), rate, channels, [&](Frame at, Frame n, float* out) {
        for (Frame i = 0; i < n; ++i) for (unsigned c = 0; c < channels; ++c) out[i * channels + c] = sample(double(at + i) / rate, c);
    }, nullptr, {});
    auto t = std::make_shared<Timeline>(); t->sampleRate = rate; t->channels = channels;
    t->pieces = {{file, 0, file->frames}}; t->rebuild(); return t;
}
double hz(double midi) { return 440 * std::pow(2., (midi - 69) / 12); }
double frequency(const Timeline& t, double first, double last) {
    int crossings = 0;
    for (Frame f = Frame(first * t.sampleRate) + 1; f < Frame(last * t.sampleRate); ++f)
        if (t.sample(f - 1, 0) <= 0 && t.sample(f, 0) > 0) ++crossings;
    return crossings / (last - first);
}
void contextualTracking() {
    PitchAnalysis a; a.sampleRate = 48000; a.hop = 480; a.sourceFrames = 144000;
    for (int i = 0; i < 300; ++i) {
        PitchFrame f{Frame(i) * a.hop, float(i < 220 ? 57 + .3 * std::sin(i * .3) : 69), .98f, .2f};
        if ((i >= 80 && i < 105) || i >= 280) f.confidence = 0;
        if (i == 0 || i == 1 || (i >= 140 && i <= 142) || i == 200) f.midi -= 18;
        if (i >= 160 && i < 170) f.rms = 0; // period window still reports pitch here
        a.contour.push_back(f);
    }
    refinePitchAnalysis(a);
    CHECK(a.notes.size() == 3);
    CHECK(std::abs(a.notes[0].midi - 57) < .1); CHECK(a.notes[0].inferredFraction > .1);
    for (int i = 0; i < 300; ++i) {
        const auto& f = a.contour[i];
        if (i >= 160 && i < 170) { CHECK(f.confidence == 0); CHECK(!f.inferred); continue; }
        CHECK(f.confidence > 0);
        CHECK(std::abs(f.midi - (i < 220 ? 57 : 69)) < .4);
        if ((i >= 80 && i < 105) || (i >= 140 && i <= 142) || i >= 280) {
            CHECK(f.inferred); CHECK(f.confidence < .5);
        }
    }
    // Real sustained octave changes and ordinary vibrato are not smoothed out.
    CHECK(!a.contour[221].inferred); CHECK(a.contour[221].midi == 69);
    CHECK(std::abs(a.contour[20].midi - float(57 + .3 * std::sin(6.))) < .0001);
    CHECK(a.notes[2].inferredFraction > 0);
    auto refinedAgain = a; refinePitchAnalysis(refinedAgain);
    CHECK(refinedAgain.notes.size() == a.notes.size());
    CHECK(refinedAgain.contour[90].inferred); CHECK(refinedAgain.contour[90].confidence == a.contour[90].confidence);
    // Even a separate audible island gets a contextual guess, without filling
    // the silence on either side. No references means no fabricated pitch.
    auto island = a;
    for (int i = 0; i < 300; ++i) {
        auto& f = island.contour[i]; f.inferred = false; f.confidence = i < 80 ? .98f : 0;
        f.midi = 57; f.rms = i < 80 || (i >= 120 && i < 160) ? .2f : 0;
    }
    refinePitchAnalysis(island); CHECK(island.notes.size() == 2);
    CHECK(island.notes[1].inferredFraction == 1); CHECK(island.notes[1].midi == 57);
    for (int i = 80; i < 120; ++i) CHECK(island.contour[i].confidence == 0);
    for (auto& f : island.contour) { f.confidence = 0; f.inferred = false; }
    refinePitchAnalysis(island); CHECK(island.notes.empty());
    // Different neighbouring notes use a discrete handover, not a fake slide.
    auto handover = a;
    for (int i = 0; i < 300; ++i) {
        auto& f = handover.contour[i]; f.rms = .2f; f.inferred = false;
        f.midi = i < 150 ? 57 : 64; f.confidence = i >= 100 && i < 200 ? 0 : .98f;
    }
    refinePitchAnalysis(handover); CHECK(handover.notes.size() == 2);
    for (int i = 100; i < 200; ++i) CHECK(handover.contour[i].midi == (i < 150 ? 57 : 64));
}
void audibleGaps() {
    std::uint32_t random = 19;
    const auto voice = audio(48000, 2, 3, [&](double t, unsigned c) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        if ((t >= 1.7 && t < 2) || (t >= .35 && t < .39)) return 0.f;
        if (t >= .8 && t < 1.2) return float(.3 * (double(random) / std::numeric_limits<std::uint32_t>::max() - .5));
        return float(.3 * std::sin(2 * std::numbers::pi * hz(t < 2 ? 57.2 : 64.1) * t)) * (c ? -1 : 1);
    });
    const auto a = analyzePitch(*voice);
    CHECK(a.notes.size() == 3);
    CHECK(a.notes[1].inferredFraction > .2);
    for (const auto& f : a.contour) {
        const double t = double(f.sourceFrame) / 48000;
        if (t > .86 && t < 1.14) { CHECK(f.inferred); CHECK(std::abs(f.midi - 57.2) < .2); }
        if ((t > 1.72 && t < 1.98) || (t > .36 && t < .38)) CHECK(f.confidence == 0);
    }
    // Corrections include inferred audible regions and still skip silence.
    auto state = PitchTimeState::identity(voice, 120); state.analysis = std::make_shared<PitchAnalysis>(a); state.notes = a.notes;
    for (auto& note : state.notes) note.shift = 2;
    CHECK(state.noteShiftAt(48000) == 2); CHECK(state.noteShiftAt(86400) == 0);
    const auto rendered = renderPitchTime(state);
    CHECK(std::abs(frequency(*rendered, 1.25, 1.65) - hz(59.2)) < 4);
}
double pitchDepth(const Timeline& audio, double first = .5, double last = 3.5) {
    std::vector<double> crossings, pitches;
    for (Frame i = Frame(first * audio.sampleRate) + 1; i < Frame(last * audio.sampleRate); ++i) {
        const double a = audio.sample(i - 1, 0), b = audio.sample(i, 0);
        if (a <= 0 && b > 0) crossings.push_back(double(i - 1) - a / (b - a));
    }
    double sum = 0;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        const double midi = 69 + 12 * std::log2(audio.sampleRate / (crossings[i] - crossings[i - 1]) / 440);
        pitches.push_back(midi); sum += midi;
    }
    CHECK(pitches.size() > 30);
    const double mean = sum / pitches.size(); double variance = 0;
    for (double pitch : pitches) variance += (pitch - mean) * (pitch - mean);
    return std::sqrt(variance / pitches.size());
}
void vibratoAndSplits() {
    const auto source = audio(48000, 2, 4, [](double t, unsigned c) {
        return float(.3 * std::sin(2 * std::numbers::pi * 220 * t + .88 * std::sin(2 * std::numbers::pi * 5 * t))) * (c ? -1 : 1);
    });
    auto state = PitchTimeState::identity(source, 120);
    state.analysis = std::make_shared<PitchAnalysis>(analyzePitch(*source)); state.notes = state.analysis->notes;
    CHECK(state.notes.size() == 1); CHECK(!state.hasNoteEdits());
    const auto& middle = state.analysis->contour[125];
    state.notes[0].vibrato = 0;
    CHECK(state.hasNoteEdits());
    CHECK(std::abs(middle.midi + state.noteShiftAt(double(middle.sourceFrame)) - state.notes[0].midi) < 1e-7);
    const auto flat = renderPitchTime(state);
    state.notes[0].vibrato = 2;
    const auto doubled = renderPitchTime(state);
    const double originalDepth = pitchDepth(*source), flatDepth = pitchDepth(*flat), doubledDepth = pitchDepth(*doubled);
    std::cout << "Vibrato depth (semitones): original=" << originalDepth << " flat=" << flatDepth << " doubled=" << doubledDepth << '\n';
    CHECK(flatDepth < originalDepth * .6); CHECK(doubledDepth > originalDepth * 1.4);
    CHECK(std::abs(frequency(*flat, .5, 3.5) - 220) < 2);
    state.notes[0].shift = 3;
    const auto before = state;
    CHECK(!state.splitNote(0, state.notes[0].first + 1));
    CHECK(state.splitNote(0, 96000)); CHECK(state.notes.size() == 2);
    CHECK(state.notes[0].last == state.notes[1].first); CHECK(state.notes[1].vibrato == 2); CHECK(state.notes[1].shift == 3);
    for (Frame f = 0; f < source->frames; f += 37) CHECK(state.noteShiftAt(double(f)) == before.noteShiftAt(double(f)));
    auto split = renderPitchTime(state); auto unsplit = renderPitchTime(before);
    for (Frame f = 0; f < source->frames; ++f) CHECK(split->sample(f, 0) == unsplit->sample(f, 0));
    // Editing one half leaves the other intact, with a smooth new internal join.
    state.notes[1].shift = -2; state.notes[1].vibrato = 0;
    CHECK(state.noteShiftAt(48000) == before.noteShiftAt(48000));
    for (Frame f = 95000; f < 97000; ++f) CHECK(std::abs(state.noteShiftAt(double(f + 1)) - state.noteShiftAt(double(f))) < .05);
    CHECK(state.splitNote(1, 144000)); CHECK(state.notes[2].vibrato == 0);
    state.time.bpm = 150;
    auto timed = renderPitchTime(state); CHECK(timed->frames == 153600);
    CHECK(pitchDepth(*timed, 1.85, 2.25) < .15);
    Document doc; doc.open(source->pieces[0].source); doc.apply(unsplit, "Vibrato"); doc.apply(split, "Split");
    CHECK(doc.undo()); CHECK(doc.timeline()->pitchTime->notes.size() == 1);
    CHECK(doc.redo()); CHECK(doc.timeline()->pitchTime->notes.size() == 2);
    auto invalid = state; invalid.notes[0].vibrato = std::numeric_limits<double>::quiet_NaN(); rejects([&] { invalid.validate(); });
    invalid = state; invalid.notes[0].fadeFirst = 1; rejects([&] { invalid.validate(); });
    auto inferred = std::make_shared<PitchAnalysis>(*state.analysis);
    for (auto& frame : inferred->contour) frame.inferred = true;
    state.analysis = inferred;
    CHECK(state.noteShiftAt(48000) == 3); // no fabricated vibrato from guesses
}
void detection() {
    for (unsigned rate : {8000u, 44100u, 48000u, 96000u}) {
        const auto source = audio(rate, 2, 2.5, [](double t, unsigned c) {
            const double pitch = t < 1 ? 57.23 : 64.18;
            if (t > 1 && t < 1.3) return 0.f;
            return float(.3 * std::sin(2 * std::numbers::pi * hz(pitch) * t)) * (c ? -1 : 1);
        });
        const auto analysis = analyzePitch(*source);
        CHECK(analysis.notes.size() == 2); CHECK(analysis.hop > 0);
        CHECK(std::abs(analysis.notes[0].midi - 57.23) < .12);
        CHECK(std::abs(analysis.notes[1].midi - 64.18) < .12);
        CHECK(analysis.notes[0].last < Frame(rate * 1.06)); CHECK(analysis.notes[1].first > Frame(rate * 1.25));
        CHECK(analysis.notes[0].confidence > .9); CHECK(analysis.notes[1].confidence > .9);
        for (const auto& f : analysis.contour) if (f.sourceFrame > rate * 1.08 && f.sourceFrame < rate * 1.22) CHECK(f.confidence == 0);
    }
    // Strong harmonics and a missing fundamental must not become octave errors.
    const auto harmonic = audio(48000, 1, 1.5, [](double t, unsigned) {
        return float(.25 * std::sin(2 * std::numbers::pi * 220 * t) + .2 * std::sin(2 * std::numbers::pi * 330 * t));
    });
    const auto harmonics = analyzePitch(*harmonic); CHECK(harmonics.notes.size() == 1);
    CHECK(std::abs(harmonics.notes[0].midi - 45) < .1);
    // Vibrato stays one note rather than becoming a row of false note changes.
    const auto vibrato = audio(48000, 1, 2, [](double t, unsigned) {
        return float(.3 * std::sin(2 * std::numbers::pi * 220 * t + .88 * std::sin(2 * std::numbers::pi * 5 * t)));
    });
    const auto vibratoAnalysis = analyzePitch(*vibrato); CHECK(vibratoAnalysis.notes.size() == 1);
    CHECK(std::abs(vibratoAnalysis.notes[0].midi - 57) < .15);
    // Do not fabricate notes from silence, DC or unpitched noise.
    auto silent = audio(48000, 1, .5, [](double, unsigned) { return 0.f; }); CHECK(analyzePitch(*silent).notes.empty());
    auto dc = audio(48000, 1, .5, [](double, unsigned) { return .2f; }); CHECK(analyzePitch(*dc).notes.empty());
    std::uint32_t random = 42;
    auto noise = audio(48000, 1, 1, [&](double, unsigned) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return float(double(random) / std::numeric_limits<std::uint32_t>::max() - .5);
    });
    CHECK(analyzePitch(*noise).notes.empty());
    // An empty/silent stereo channel must not mask the melody on the other side.
    auto right = audio(48000, 2, 1, [](double t, unsigned c) { return c ? float(.3 * std::sin(2 * std::numbers::pi * 330 * t)) : 0.f; });
    CHECK(std::abs(hz(analyzePitch(*right).notes[0].midi) - 330) < 2);
    std::stop_source cancel; cancel.request_stop(); rejects([&] { analyzePitch(*harmonic, nullptr, cancel.get_token()); });
    // Cancel after analysis has started, while its source is still retained.
    const auto longVoice = audio(48000, 1, 30, [](double t, unsigned) {
        return float(.3 * std::sin(2 * std::numbers::pi * 220 * t));
    });
    std::atomic<float> progress{0}; std::stop_source running;
    auto worker = std::async(std::launch::async, [&] { analyzePitch(*longVoice, &progress, running.get_token()); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (progress.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    running.request_stop(); CHECK(progress.load() > 0);
    CHECK(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    rejects([&] { worker.get(); });
    Timeline huge; huge.frames = 480 * Frame(PitchAnalysis::maxFrames + 1); rejects([&] { analyzePitch(huge); });
}
void correction() {
    const auto source = audio(48000, 2, 3, [](double t, unsigned c) {
        const double pitch = t < 1.5 ? 57.25 : 64.2;
        return float(.3 * std::sin(2 * std::numbers::pi * hz(pitch) * t)) * (c ? -1 : 1);
    });
    auto analysis = std::make_shared<PitchAnalysis>(analyzePitch(*source)); CHECK(analysis->notes.size() == 2);
    auto state = PitchTimeState::identity(source, 120); state.analysis = analysis; state.notes = analysis->notes;
    CHECK(state.identity()); CHECK(renderPitchTime(state)->pieces.front().source == source->pieces.front().source);
    state.notes[0].shift = 57 - state.notes[0].midi; state.notes[1].shift = 67 - state.notes[1].midi;
    const auto corrected = renderPitchTime(state);
    CHECK(corrected->frames == source->frames); CHECK(std::abs(frequency(*corrected, .3, 1.3) - 220) < 3);
    CHECK(std::abs(frequency(*corrected, 1.8, 2.8) - hz(67)) < 3);
    for (Frame f = 0; f < corrected->frames; ++f) {
        CHECK(std::isfinite(corrected->sample(f, 0)));
        CHECK(std::abs(corrected->sample(f, 0) + corrected->sample(f, 1)) < .003);
    }
    // Source-coordinate notes follow a non-uniform time map, including the
    // processor's pre-roll and latency. The second note moves to 2 seconds.
    state.time.add(0); state.time.add(72000); state.time.add(source->frames); CHECK(state.time.move(1, 4));
    auto warped = renderPitchTime(state);
    CHECK(std::abs(frequency(*warped, 1.6, 1.85) - 220) < 5);
    CHECK(std::abs(frequency(*warped, 2.25, 2.75) - hz(67)) < 4);
    CHECK(warped->pitchTime->analysis == analysis); CHECK(warped->pitchTime->notes[1].first == state.notes[1].first);
    state.pitch.semitones = -12;
    auto transposed = renderPitchTime(state);
    CHECK(std::abs(frequency(*transposed, .3, 1.3) - 110) < 3);
    CHECK(std::abs(frequency(*transposed, 2.25, 2.75) - hz(55)) < 4);
    Document doc; doc.open(source->pieces.front().source); doc.apply(corrected, "Correct notes"); doc.apply(warped, "Timing");
    CHECK(doc.undo()); CHECK(doc.timeline() == corrected); CHECK(doc.redo()); CHECK(doc.timeline() == warped);
    doc.effect(0, doc.timeline()->frames, Effect::Gain, .5f); CHECK(!doc.timeline()->pitchTime);
    CHECK(doc.undo()); CHECK(doc.timeline()->pitchTime->analysis == analysis);
    // Exports resolve deferred note edits, even when exporting only a slice.
    auto pending = previewPitchTime(state); auto slice = pending->range(12000, 66000);
    const auto path = std::filesystem::current_path() / ("coldedit-notes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    exportAudio(slice, path, ExportFormat::Float32); CHECK(!needsPitchTimeRender(*pending));
    { auto file = AudioFile::open(path); CHECK(file->frames == 54000);
      for (Frame f = 0; f < file->frames; ++f) CHECK(file->sample(f, 0) == pending->sample(f + 12000, 0)); }
    std::filesystem::remove(path);
    auto invalid = state; invalid.notes[0].shift = std::numeric_limits<double>::quiet_NaN(); rejects([&] { renderPitchTime(invalid); });
    invalid = state; invalid.notes[1].first = invalid.notes[0].first; rejects([&] { invalid.validate(); });
    invalid = state; invalid.analysis.reset(); rejects([&] { invalid.validate(); });
    state.pitch = {}; for (auto& note : state.notes) note.shift = 0;
    CHECK(!state.hasPitchChanges()); state.time = StretchMap::identity(source->frames, source->sampleRate, 120);
    CHECK(renderPitchTime(state)->pieces.front().source == source->pieces.front().source);
}
int main() try {
    contextualTracking(); audibleGaps(); detection(); correction(); vibratoAndSplits();
    std::cout << "Pitch context, audible gaps, silence, outliers, detection, stereo, vibrato, correction, timing, history and export passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
