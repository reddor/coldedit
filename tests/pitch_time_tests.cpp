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

std::shared_ptr<Timeline> signal(unsigned rate, unsigned channels, bool vowel = false) {
    const Frame frames = rate * 2;
    auto audio = AudioFile::render(frames, rate, channels, [&](Frame at, Frame count, float* out) {
        for (Frame i = 0; i < count; ++i) {
            const double seconds = double(at + i) / rate;
            double value = .3 * std::sin(2 * std::numbers::pi * 440 * seconds);
            if (vowel) {
                value = 0;
                for (int h = 1; h <= 35; ++h) {
                    const double frequency = 100. * h;
                    const double amplitude = .003 + .12 * std::exp(-.5 * std::pow((frequency - 1000) / 150, 2));
                    value += amplitude * std::sin(2 * std::numbers::pi * frequency * seconds);
                }
            }
            for (unsigned c = 0; c < channels; ++c) out[i * channels + c] = float(value) * (c ? -1.f : 1.f);
        }
    }, nullptr, {});
    auto result = std::make_shared<Timeline>(); result->sampleRate = rate; result->channels = channels;
    result->pieces = {{audio, 0, frames}}; result->rebuild(); return result;
}
double frequency(const Timeline& t, double begin = .4, double end = 1.4) {
    int crossings = 0;
    for (Frame f = Frame(begin * t.sampleRate) + 1; f < Frame(end * t.sampleRate); ++f)
        if (t.sample(f - 1, 0) <= 0 && t.sample(f, 0) > 0) ++crossings;
    return crossings / (end - begin);
}
double spectralCentre(const Timeline& t, double fundamental) {
    double total = 0, weighted = 0;
    const Frame start = t.sampleRate / 2, count = t.sampleRate / 2;
    for (double hz = fundamental; hz <= 4000; hz += fundamental) {
        double re = 0, im = 0;
        for (Frame f = 0; f < count; ++f) {
            const double phase = 2 * std::numbers::pi * hz * double(f) / t.sampleRate;
            const double value = t.sample(start + f, 0);
            re += value * std::cos(phase); im += value * std::sin(phase);
        }
        const double power = re * re + im * im;
        total += power; weighted += power * hz;
    }
    CHECK(total > 1); return weighted / total;
}
void transposition() {
    for (const auto rate : {8000u, 44100u, 48000u}) for (const auto channels : {1u, 2u}) {
        const auto source = signal(rate, channels);
        auto state = PitchTimeState::identity(source, 120);
        CHECK(renderPitchTime(state)->pieces.front().source == source->pieces.front().source);
        for (const double semitones : {-12., 7., 12.}) {
            state.pitch.semitones = semitones;
            const auto result = renderPitchTime(state);
            CHECK(result->frames == source->frames); CHECK(result->sampleRate == rate); CHECK(result->channels == channels);
            CHECK(std::abs(frequency(*result) - 440 * std::pow(2., semitones / 12)) < 4);
            for (Frame f = 0; f < result->frames; ++f) {
                CHECK(std::isfinite(result->sample(f, 0)));
                if (channels == 2) CHECK(std::abs(result->sample(f, 0) + result->sample(f, 1)) < .003);
            }
        }
        // A tempo change and a pitch change are independent, with one source.
        state.pitch.semitones = 12; state.time.add(0); state.time.add(rate); state.time.add(source->frames);
        CHECK(state.time.move(1, 2.5)); state.time.bpm = 150;
        const auto combined = renderPitchTime(state);
        CHECK(combined->frames == Frame(std::llround(source->frames * .8)));
        CHECK(std::abs(frequency(*combined, .3, .8) - 880) < 6);
        CHECK(std::abs(frequency(*combined, 1.1, 1.5) - 880) < 6);
        auto resetPitch = *combined->pitchTime; resetPitch.pitch = {};
        const auto timingOnly = renderPitchTime(resetPitch);
        CHECK(timingOnly->frames == combined->frames); CHECK(std::abs(frequency(*timingOnly, .3, .8) - 440) < 6);
        CHECK(timingOnly->pitchTime->source == combined->pitchTime->source);
        auto resetTime = *combined->pitchTime; resetTime.time = StretchMap::identity(source->frames, rate, 120);
        const auto pitchOnly = renderPitchTime(resetTime);
        CHECK(pitchOnly->frames == source->frames); CHECK(std::abs(frequency(*pitchOnly) - 880) < 4);
        resetTime.pitch = {};
        CHECK(renderPitchTime(resetTime)->pieces.front().source == source->pieces.front().source);
    }
}
void formants() {
    const auto source = signal(48000, 1, true);
    auto state = PitchTimeState::identity(source, 120); state.pitch.formantBaseHz = 100;
    const double original = spectralCentre(*source, 100);
    state.pitch.semitones = 12; state.pitch.preserveFormants = false;
    const auto natural = renderPitchTime(state);
    state.pitch.preserveFormants = true;
    const auto preserved = renderPitchTime(state);
    const double movedCentre = spectralCentre(*natural, 200), preservedCentre = spectralCentre(*preserved, 200);
    CHECK(movedCentre > original * 1.7);
    CHECK(std::abs(preservedCentre - original) < original * .25);
    state.pitch.semitones = 0; state.pitch.formantSemitones = 7;
    const auto shifted = renderPitchTime(state);
    const double shiftedCentre = spectralCentre(*shifted, 100);
    CHECK(shiftedCentre > original * 1.25); CHECK(shiftedCentre < original * 1.8);
    CHECK(shifted->frames == source->frames);
    // Formant-only changes must not alter a simple fundamental's frequency.
    state = PitchTimeState::identity(signal(48000, 1), 120);
    state.pitch.formantSemitones = -5; state.pitch.formantBaseHz = 440;
    CHECK(std::abs(frequency(*renderPitchTime(state)) - 440) < 4);
}
void historyAndExport() {
    const auto source = signal(48000, 2);
    Document doc; doc.open(source->pieces.front().source); const auto original = doc.timeline();
    auto state = PitchTimeState::identity(original, 120);
    state.pitch.semitones = -12; state.time.bpm = 100;
    auto processed = renderPitchTime(state); doc.apply(processed, "Pitch / time");
    CHECK(doc.dirty()); CHECK(doc.historySize() == 2);
    CHECK(doc.undo()); CHECK(doc.timeline() == original); CHECK(!doc.dirty());
    CHECK(doc.redo()); CHECK(doc.timeline() == processed); CHECK(doc.timeline()->pitchTime->pitch.semitones == -12);
    doc.effect(0, processed->frames, Effect::Gain, .5f); CHECK(!doc.timeline()->pitchTime);
    CHECK(doc.undo()); CHECK(doc.timeline() == processed);
    // Change the absolute setting: +12 from the original, not another pass on -12.
    state = *processed->pitchTime; state.pitch.semitones = 12;
    auto pending = previewPitchTime(state); CHECK(needsPitchTimeRender(*pending));
    auto selection = pending->range(12000, 84000);
    const auto path = std::filesystem::current_path() / ("coldedit-pitch-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    exportAudio(selection, path, ExportFormat::Float32);
    CHECK(!needsPitchTimeRender(*pending));
    {
        auto audio = AudioFile::open(path); Timeline exported; exported.sampleRate = audio->sampleRate;
        exported.channels = audio->channels; exported.pieces = {{audio, 0, audio->frames}}; exported.rebuild();
        CHECK(exported.frames == 72000); CHECK(exported.channels == 2);
        CHECK(std::abs(frequency(exported, .2, 1.2) - 880) < 4);
        for (Frame f = 0; f < exported.frames; ++f) CHECK(exported.sample(f, 0) == pending->sample(f + 12000, 0));
    }
    std::filesystem::remove(path);
    auto cleaned = processAudio(*pending, 0, pending->frames, {Cleanup::RemoveDC, 0});
    CHECK(!cleaned->pitchTime); CHECK(std::abs(frequency(*cleaned) - 880) < 4);
    auto invalid = state; invalid.pitch.semitones = std::numeric_limits<double>::quiet_NaN(); rejects([&] { renderPitchTime(invalid); });
    invalid = state; invalid.pitch.semitones = 25; rejects([&] { previewPitchTime(invalid); });
    invalid = state; invalid.pitch.formantSemitones = -13; rejects([&] { renderPitchTime(invalid); });
    invalid = state; invalid.pitch.formantBaseHz = std::numeric_limits<double>::infinity(); rejects([&] { invalid.validate(); });
    invalid = state; invalid.time.sourceFrames++; rejects([&] { invalid.validate(); });
    std::stop_source cancel; cancel.request_stop();
    rejects([&] { renderPitchTime(state, nullptr, cancel.get_token()); });
    CHECK(doc.timeline() == processed); CHECK(doc.historySize() == 3);
}
void selections() {
    const auto source = signal(48000, 2);
    auto state = PitchTimeState::identity(source, 120);
    PitchSettings settings; settings.semitones = 12; settings.preserveFormants = false;
    state.setSelectionPitch(12000, 60000, settings);
    const auto selected = renderPitchTime(state);
    CHECK(std::abs(frequency(*selected, .4, 1.1) - 880) < 4);
    for (Frame f = 0; f < source->frames; ++f) if (f < 12000 || f >= 60000)
        for (unsigned c = 0; c < source->channels; ++c) CHECK(selected->sample(f, c) == source->sample(f, c));
    CHECK(state.selectionSettings(12000, 60000)->semitones == 12);
    CHECK(!state.selectionSettings(0, 60000));
    auto neutral = state; neutral.setSelectionPitch(12000, 60000, PitchSettings{});
    const auto neutralAudio = renderPitchTime(neutral);
    for (Frame f = 0; f < source->frames; ++f) CHECK(neutralAudio->sample(f, 0) == source->sample(f, 0));
    state.time.bpm = 150; const auto warped = renderPitchTime(state);
    auto baseline = state; baseline.rangeProcessing.clear(); const auto plain = renderPitchTime(baseline);
    CHECK(std::abs(frequency(*warped, .35, .9) - 880) < 5);
    for (Frame f = 0; f < plain->frames; ++f) if (f < 9600 || f >= 48000) CHECK(warped->sample(f, 0) == plain->sample(f, 0));
    settings.semitones = -12; state.setSelectionPitch(24000, 36000, settings);
    CHECK(state.rangeProcessing.size() == 3); state.validate();
    state.setSelectionPitch(24000, 36000, {}); CHECK(state.rangeProcessing.size() == 2); state.validate();
    state.setSelectionPitch(0, source->frames, {}); CHECK(state.rangeProcessing.empty());
    // Selected notes and harmony voices have independent settings, retained by splitting/copying.
    state = PitchTimeState::identity(source, 120);
    auto analysis = std::make_shared<PitchAnalysis>(); analysis->sourceFrames = source->frames; analysis->sampleRate = 48000; analysis->hop = 480;
    analysis->notes = {{0, 48000, 69, 0, .99f}, {48000, 96000, 69, 0, .99f}};
    state.analysis = analysis; state.notes = analysis->notes; state.notes[0].shift = 2;
    const auto before = renderPitchTime(state);
    settings.semitones = 10; settings.formantSemitones = 3; settings.formantBaseHz = 440;
    state.setSelectionPitch(0, 48000, settings, 0); CHECK(state.noteMidi(0) == 81); CHECK(state.noteMidi(1) == 69);
    const auto notes = renderPitchTime(state); CHECK(std::abs(frequency(*notes, .25, .8) - 880) < 5);
    for (Frame f = 48000; f < source->frames; ++f) CHECK(notes->sample(f, 0) == before->sample(f, 0));
    CHECK(state.splitNote(0, 24000)); const auto split = renderPitchTime(state);
    for (Frame f = 0; f < source->frames; ++f) CHECK(split->sample(f, 0) == notes->sample(f, 0));
    CHECK(state.addHarmony({0, 1}, 7) == 1); CHECK(state.noteMidi(3) == 88);
    CHECK(state.harmonies[0].processing.size() == 2); state.validate();
    settings.formantSemitones = -4; state.setSelectionPitch(0, 24000, settings, 1);
    CHECK(state.settingsAt(12000, 1).formantSemitones == -4); CHECK(state.settingsAt(12000, 0).formantSemitones == 3);
    state.setSelectionPitch(0, 48000, {}, 0); CHECK(state.noteProcessing.empty()); CHECK(!state.harmonies[0].processing.empty());
    // Formant-only selection processing changes the spectral envelope, not just metadata.
    const auto vowel = signal(48000, 1, true); auto formant = PitchTimeState::identity(vowel, 120);
    PitchSettings tone; tone.formantSemitones = 7; tone.formantBaseHz = 100;
    formant.setSelectionPitch(12000, 60000, tone); const auto changed = renderPitchTime(formant);
    CHECK(spectralCentre(*changed, 100) > spectralCentre(*vowel, 100) * 1.2);
    for (Frame f = 60000; f < vowel->frames; ++f) CHECK(changed->sample(f, 0) == vowel->sample(f, 0));
    Document doc; doc.open(source->pieces[0].source); doc.apply(selected, "Selected pitch"); doc.apply(warped, "Tempo");
    CHECK(doc.undo()); CHECK(doc.timeline() == selected); CHECK(doc.redo()); CHECK(doc.timeline() == warped);
    auto pending = previewPitchTime(*warped->pitchTime);
    const auto path = std::filesystem::current_path() / "coldedit-pitch-selection-test.wav";
    exportAudio(*pending, path, ExportFormat::Float32);
    { const auto file = AudioFile::open(path); for (Frame f = 0; f < file->frames; ++f) CHECK(file->sample(f, 0) == warped->sample(f, 0)); }
    std::filesystem::remove(path);
    auto invalid = formant; invalid.rangeProcessing[0].last = source->frames + 1; rejects([&] { invalid.validate(); });
    std::stop_source cancel; cancel.request_stop(); rejects([&] { renderPitchTime(formant, nullptr, cancel.get_token()); });
}
int main() try {
    transposition(); formants(); historyAndExport(); selections();
    std::cout << "Pitch/time: transposition, formants, stereo, resets, source retention, history, export and cancellation passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
