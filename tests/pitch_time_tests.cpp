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
int main() try {
    transposition(); formants(); historyAndExport();
    std::cout << "Pitch/time: transposition, formants, stereo, resets, source retention, history, export and cancellation passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
