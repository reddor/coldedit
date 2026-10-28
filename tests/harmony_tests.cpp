#include "pitch_time.hpp"
#include "processing.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace coldedit;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
template<class F> void rejects(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
double amplitude(const Timeline& t, double hz, double first, double last) {
    double re = 0, im = 0, weight = 0;
    const Frame a = Frame(first * t.sampleRate), b = Frame(last * t.sampleRate);
    for (Frame f = a; f < b; ++f) {
        const double w = .5 - .5 * std::cos(2 * std::numbers::pi * (f - a) / (b - a));
        const double phase = 2 * std::numbers::pi * hz * double(f) / t.sampleRate;
        re += t.sample(f, 0) * w * std::cos(phase); im += t.sample(f, 0) * w * std::sin(phase); weight += w;
    }
    return 2 * std::hypot(re, im) / weight;
}
PitchTimeState fixture() {
    auto file = AudioFile::render(192000, 48000, 2, [](Frame first, Frame count, float* out) {
        for (Frame i = 0; i < count; ++i) {
            const double t = double(first + i) / 48000;
            const float value = float(.3 * std::sin(2 * std::numbers::pi * (t < 2 ? 220 : 330) * t));
            out[i * 2] = value; out[i * 2 + 1] = -value;
        }
    }, nullptr, {});
    auto t = std::make_shared<Timeline>(); t->sampleRate = 48000; t->channels = 2;
    t->pieces = {{file, 0, file->frames}}; t->rebuild();
    auto state = PitchTimeState::identity(t, 120);
    auto analysis = std::make_shared<PitchAnalysis>(); analysis->sampleRate = 48000; analysis->sourceFrames = 192000; analysis->hop = 480;
    analysis->notes = {{0, 96000, 57, 0, .99f}, {96000, 192000, 69 + 12 * std::log2(330. / 440), 0, .99f}};
    for (Frame f = 0; f < t->frames; f += analysis->hop)
        analysis->contour.push_back({f, float(analysis->notes[f < 96000 ? 0 : 1].midi), .99f, .2f});
    state.analysis = analysis; state.notes = analysis->notes; return state;
}
int main() try {
    auto state = fixture(); const auto original = state;
    CHECK(state.addHarmony({0}, 4) == 1); CHECK(state.addHarmony({0}, 7) == 2);
    CHECK(state.noteCount() == 4); CHECK(state.noteKey(0) != state.noteKey(2));
    CHECK(state.hasPitchChanges()); state.validate();
    const auto triad = renderPitchTime(state);
    for (double hz : {220., 220 * std::exp2(4. / 12), 220 * std::exp2(7. / 12)}) {
        const auto value = amplitude(*triad, hz, .4, 1.6); std::cout << hz << " Hz: " << value << '\n'; CHECK(value > .065);
    }
    CHECK(amplitude(*triad, 440, .4, 1.6) < .015);
    // Only the first note was copied. The other note remains bit-exact source.
    for (Frame f = 96000; f < triad->frames; ++f) CHECK(triad->sample(f, 0) == state.source->sample(f, 0));
    for (Frame f = 0; f < triad->frames; ++f) {
        CHECK(std::isfinite(triad->sample(f, 0)));
        CHECK(std::abs(triad->sample(f, 0) + triad->sample(f, 1)) < .003);
    }
    CHECK(state.splitNote(2, 48000)); CHECK(state.noteCount() == 5);
    const auto split = renderPitchTime(state);
    for (Frame f = 0; f < triad->frames; ++f) CHECK(triad->sample(f, 0) == split->sample(f, 0));
    // Edits to one half or voice do not mutate the source or other voices.
    state.noteAt(2).shift = 12; const auto edited = renderPitchTime(state);
    std::cout << "Edited octave: " << amplitude(*edited, 440, .25, .75) << '\n';
    CHECK(amplitude(*edited, 440, .25, .75) > .025);
    CHECK(amplitude(*edited, 220 * std::exp2(4. / 12), 1.25, 1.75) > .06);
    CHECK(state.notes[0].shift == 0); CHECK(state.harmonies[1].notes[0].shift == 7);
    auto muted = state; for (auto& voice : muted.harmonies) voice.gain = 0;
    const auto silentCopies = renderPitchTime(muted);
    for (Frame f = 0; f < silentCopies->frames; ++f) CHECK(silentCopies->sample(f, 0) == state.source->sample(f, 0));
    auto doubled = original; doubled.addHarmony({0}, 12); doubled.harmonies[0].gain = .7;
    auto loud = renderPitchTime(doubled); doubled.harmonies[0].gain = .35; auto soft = renderPitchTime(doubled);
    CHECK(std::abs(amplitude(*loud, 440, .4, 1.6) / amplitude(*soft, 440, .4, 1.6) - 2) < .02);
    doubled.time.add(0); doubled.time.add(96000); doubled.time.add(192000); CHECK(doubled.time.move(1, 5));
    auto warped = renderPitchTime(doubled);
    CHECK(amplitude(*warped, 440, 1.8, 2.3) > .025);
    CHECK(amplitude(*warped, 440, 2.8, 3.5) < .015);
    auto leadOnly = doubled; leadOnly.harmonies = {}; const auto warpedLead = renderPitchTime(leadOnly);
    for (Frame f = 120000; f < warped->frames; ++f) CHECK(warped->sample(f, 0) == warpedLead->sample(f, 0));
    doubled.pitch.semitones = 12; doubled.pitch.preserveFormants = false;
    const auto transposed = renderPitchTime(doubled);
    CHECK(amplitude(*transposed, 440, .5, 1.5) > .04); CHECK(amplitude(*transposed, 880, .5, 1.5) > .02);
    // Each added voice receives the same formant treatment as an isolated edit.
    doubled.pitch.preserveFormants = true; doubled.pitch.formantSemitones = -3;
    const auto formants = renderPitchTime(doubled);
    auto onlyLead = doubled; onlyLead.harmonies = {}; const auto leadFormants = renderPitchTime(onlyLead);
    auto onlyHarmony = onlyLead; onlyHarmony.notes = doubled.harmonies[0].notes;
    const auto harmonyFormants = renderPitchTime(onlyHarmony);
    for (Frame f = 24000; f < 72000; ++f)
        CHECK(std::abs(formants->sample(f, 0) - leadFormants->sample(f, 0) - .35 * harmonyFormants->sample(f, 0)) < .000001);
    Document doc; doc.open(state.source->pieces[0].source); doc.apply(triad, "Harmony"); doc.apply(warped, "Timing");
    CHECK(doc.undo()); CHECK(doc.timeline() == triad); CHECK(doc.redo()); CHECK(doc.timeline() == warped);
    auto pending = previewPitchTime(state);
    const auto path = std::filesystem::current_path() / "coldedit-harmony-test.wav";
    exportAudio(pending->range(12000, 66000), path, ExportFormat::Float32);
    CHECK(!needsPitchTimeRender(*pending));
    { auto file = AudioFile::open(path); CHECK(file->frames == 54000);
      for (Frame f = 0; f < file->frames; ++f) CHECK(file->sample(f, 0) == edited->sample(f + 12000, 0)); }
    std::filesystem::remove(path);
    auto full = state; CHECK(full.addHarmony({0}, -12) == 3); CHECK(full.addHarmony({0}, 7) == 4);
    full.harmonies[0] = {}; CHECK(full.addHarmony({0}, -7) == 1); // freed slot can be reused
    CHECK(full.harmonies.size() == 4); CHECK(full.harmonies[3].notes[0].shift == 7);
    // More than three voices must actually render, survive splits and export.
    auto many = original; many.pitch.preserveFormants = false;
    const double intervals[] = {3, 4, 7, 10, 12, 16};
    for (unsigned i = 0; i < std::size(intervals); ++i) {
        CHECK(many.addHarmony({0}, intervals[i]) == i + 1); many.harmonies[i].gain = .15;
    }
    auto chord = renderPitchTime(many);
    for (double interval : intervals) CHECK(amplitude(*chord, 220 * std::exp2(interval / 12), .4, 1.6) > .025);
    CHECK(many.splitNote(7, 48000)); CHECK(many.harmonies.size() == 6); CHECK(many.harmonies[5].notes.size() == 2);
    const auto splitMany = renderPitchTime(many);
    for (Frame f = 0; f < chord->frames; ++f) CHECK(chord->sample(f, 0) == splitMany->sample(f, 0));
    CHECK(many.addHarmony({8}, 1) == 7); // copy the right half of a higher-numbered voice
    CHECK(many.harmonies[6].gain == .15); CHECK(many.harmonies[6].notes[0].first == 48000);
    many.harmonies[6].notes[0].shift = -12;
    auto seventh = renderPitchTime(many); CHECK(amplitude(*seventh, 110, 1.2, 1.8) > .025);
    for (Frame f = 0; f < 48000; ++f) CHECK(seventh->sample(f, 0) == chord->sample(f, 0));
    for (Frame f = 96000; f < seventh->frames; ++f) CHECK(seventh->sample(f, 0) == original.source->sample(f, 0));
    doc.apply(chord, "Six harmony voices"); doc.apply(seventh, "Split and add seventh voice");
    CHECK(doc.undo()); CHECK(doc.timeline()->pitchTime->harmonies.size() == 6);
    CHECK(doc.redo()); CHECK(doc.timeline()->pitchTime->harmonies.size() == 7);
    auto manyPending = previewPitchTime(many);
    exportAudio(*manyPending, path, ExportFormat::Float32);
    { auto file = AudioFile::open(path);
      for (Frame f = 0; f < file->frames; ++f) CHECK(file->sample(f, 0) == seventh->sample(f, 0)); }
    std::filesystem::remove(path);
    // Voice count consumes the same note budget, with no smaller hidden cap.
    auto budget = original;
    budget.harmonies.assign(PitchAnalysis::maxNotes - original.notes.size(), PitchTimeState::HarmonyVoice{{original.notes[0]}, .35});
    budget.validate(); rejects([&] { budget.addHarmony({0}, 1); });
    CHECK(!budget.splitNote(2, 48000)); budget.harmonies[0].notes.clear();
    CHECK(budget.addHarmony({0}, 1) == 1); CHECK(budget.noteCount() == PitchAnalysis::maxNotes);
    rejects([&] { state.addHarmony({0, 2}, 1); }); rejects([&] { state.addHarmony({2}, 20); });
    auto bad = state; bad.harmonies[0].gain = std::numeric_limits<double>::quiet_NaN(); rejects([&] { bad.validate(); });
    bad = state; bad.harmonies[0].notes[1].first = 0; rejects([&] { bad.validate(); });
    bad = state; bad.analysis.reset(); rejects([&] { bad.validate(); });
    auto all = original; all.addHarmony({}, -12); CHECK(all.harmonies[0].notes.size() == original.notes.size());
    std::stop_source stop; stop.request_stop(); rejects([&] { renderPitchTime(state, nullptr, stop.get_token()); });
    state.harmonies = {}; CHECK(state.identity()); CHECK(renderPitchTime(state)->pieces[0].source == state.source->pieces[0].source);
    std::cout << "Harmony mixing, voice edits, split continuity, levels, stereo, timing, formants, history and export passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
