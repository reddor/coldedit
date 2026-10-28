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
std::shared_ptr<Timeline> signal(unsigned rate, unsigned channels, double seconds, const std::function<float(Frame, unsigned)>& sample) {
    const Frame frames = Frame(std::llround(seconds * rate));
    auto audio = AudioFile::render(frames, rate, channels, [&](Frame at, Frame n, float* data) {
        for (Frame i = 0; i < n; ++i) for (unsigned c = 0; c < channels; ++c) data[i * channels + c] = sample(at + i, c);
    }, nullptr, {});
    auto t = std::make_shared<Timeline>(); t->sampleRate = rate; t->channels = channels;
    t->pieces = {{audio, 0, frames}}; t->rebuild(); return t;
}
double frequency(const Timeline& t, double start, double end) {
    int crossings = 0; const Frame a = Frame(start * t.sampleRate), b = Frame(end * t.sampleRate);
    for (Frame f = a + 1; f < b; ++f) if (t.sample(f - 1, 0) <= 0 && t.sample(f, 0) > 0) ++crossings;
    return crossings / (end - start);
}
double energy(const Timeline& t, Frame a, Frame b) {
    double sum = 0; for (Frame f = a; f < b; ++f) { const double s = t.sample(f, 0); sum += s * s; }
    return sum;
}
void renderTests() {
    for (const auto rate : {8000u, 44100u, 48000u}) for (const auto channels : {1u, 2u}) {
        auto source = signal(rate, channels, 3, [rate](Frame f, unsigned c) {
            return float(.4 * std::sin(2 * std::numbers::pi * 440 * double(f) / rate)) * (c ? -1.f : 1.f);
        });
        auto map = PitchTimeState::identity(source, 120);
        CHECK(map.time.add(0)); CHECK(map.time.add(rate)); CHECK(map.time.add(rate * 2)); CHECK(map.time.add(source->frames)); CHECK(!map.time.add(rate)); CHECK(!map.time.add(0));
        auto unchanged = renderPitchTime(map);
        CHECK(unchanged->pieces.front().source == source->pieces.front().source);
        CHECK(map.time.move(1, 2.5));
        auto warped = renderPitchTime(map);
        CHECK(warped->frames == source->frames); CHECK(warped->channels == channels);
        CHECK(std::abs(frequency(*warped, .25, 1) - 440) < 5);
        CHECK(std::abs(frequency(*warped, 1.4, 1.9) - 440) < 5);
        CHECK(energy(*warped, rate / 4, rate / 2) > rate * .005);
        CHECK(energy(*warped, warped->frames - rate / 10, warped->frames) > rate * .002);
        if (channels == 2) {
            double difference = 0;
            for (Frame f = rate / 4; f < warped->frames; ++f) difference += std::abs(warped->sample(f, 0) + warped->sample(f, 1));
            CHECK(difference / warped->frames < .003);
        }
        const auto fixed = map.time.anchors; map.time.bpm = 150;
        auto faster = renderPitchTime(map);
        CHECK(faster->frames == Frame(std::llround(source->frames * .8)));
        for (std::size_t i = 0; i < fixed.size(); ++i) {
            CHECK(faster->pitchTime->time.anchors[i].beat == fixed[i].beat);
            CHECK(faster->pitchTime->time.anchors[i].sourceFrame == fixed[i].sourceFrame);
        }
        CHECK(faster->pitchTime->source == warped->pitchTime->source);
        CHECK(faster->pitchTime->time.sourceBpm == 120);
        CHECK(std::abs(frequency(*faster, .25, .9) - 440) < 5);
        map.time.bpm = 80; auto slower = renderPitchTime(map); CHECK(slower->frames == source->frames * 3 / 2);
        CHECK(std::abs(frequency(*slower, .25, 1.5) - 440) < 5);
    }
}
void mapAndHistoryTests() {
    auto source = signal(48000, 1, 2, [](Frame, unsigned) { return .2f; });
    auto map = PitchTimeState::identity(source, 120); CHECK(map.time.add(0)); CHECK(map.time.add(48000)); CHECK(map.time.add(96000));
    CHECK(map.time.move(1, 2.5)); CHECK(std::abs(map.time.sourceAt(60000) - 48000) < 1e-6);
    CHECK(std::abs(map.time.outputAt(48000) - 60000) < 1e-6);
    CHECK(map.time.move(1, 1000)); map.validate(); CHECK(map.time.anchors[1].beat < map.time.anchors.back().beat);
    CHECK(map.time.move(1, -1000)); map.validate(); CHECK(map.time.anchors[1].beat > 0);
    CHECK(!map.time.move(1, std::numeric_limits<double>::quiet_NaN()));
    auto invalid = map; invalid.time.bpm = 0; rejects([&] { renderPitchTime(invalid); });
    invalid = map; invalid.time.anchors[1].beat = invalid.time.anchors.back().beat; rejects([&] { renderPitchTime(invalid); });
    invalid = map; invalid.time.anchors[1].sourceFrame = std::numeric_limits<double>::quiet_NaN(); rejects([&] { renderPitchTime(invalid); });
    map.time.move(1, 2.5); auto warped = renderPitchTime(map);
    Document doc; doc.open(source->pieces[0].source); const auto original = doc.timeline();
    doc.apply(warped, "Stretch"); CHECK(doc.dirty());
    CHECK(doc.undo()); CHECK(doc.timeline() == original); CHECK(!doc.dirty());
    CHECK(doc.redo()); CHECK(doc.timeline()->pitchTime == warped->pitchTime);
    CHECK(doc.reclaimableAudioBytes() == 0); // original PCM is still the stretch source
    doc.clearHistory(); CHECK(doc.timeline()->pitchTime->source->frames == 96000);
    auto changed = *doc.timeline()->pitchTime; changed.time.bpm = 100;
    auto slower = renderPitchTime(changed); doc.apply(slower, "Tempo");
    CHECK(doc.reclaimableAudioBytes() == warped->pieces[0].source->storageBytes());
    doc.fade(0, 100, false); CHECK(!doc.timeline()->pitchTime); // further effects bake the audible warp
    CHECK(doc.undo()); CHECK(doc.timeline()->pitchTime == slower->pitchTime);
    auto clean = processAudio(*doc.timeline(), 0, 100, {Cleanup::RemoveDC, 0}); CHECK(!clean->pitchTime);
    std::stop_source cancel; cancel.request_stop();
    rejects([&] { renderPitchTime(map, nullptr, cancel.get_token()); });
    rejects([&] { analyzeRhythm(*source, nullptr, cancel.get_token()); });
    auto tiny = signal(48000, 1, .0001, [](Frame, unsigned) { return .2f; });
    auto tinyMap = PitchTimeState::identity(tiny, 120); tinyMap.time.bpm = 60;
    CHECK(renderPitchTime(tinyMap)->frames == tiny->frames * 2);
    tinyMap = PitchTimeState::identity(tiny, 120); tinyMap.time.add(0); tinyMap.time.add(double(tiny->frames) - .25); tinyMap.time.bpm = 280;
    auto subframeTail = renderPitchTime(tinyMap);
    CHECK(subframeTail->frames == Frame(std::llround(tiny->frames * 120. / 280)));
    for (Frame f = 0; f < subframeTail->frames; ++f) CHECK(std::isfinite(subframeTail->sample(f, 0)));
}
void rhythmTests() {
    auto silent = signal(48000, 2, 3, [](Frame, unsigned) { return 0.f; });
    auto analysis = analyzeRhythm(*silent); CHECK(analysis.bpm == 0); CHECK(analysis.transients.empty());
    auto clicks = signal(48000, 2, 10, [](Frame f, unsigned c) {
        const auto phase = f % 24000;
        return phase < 480 ? float(.8 * std::exp(-double(phase) / 100) * (c ? -1 : 1)) : 0.f;
    });
    analysis = analyzeRhythm(*clicks);
    CHECK(std::abs(analysis.bpm - 120) < 1); CHECK(analysis.confidence > .7);
    CHECK(analysis.transients.size() >= 19);
    for (const auto frame : analysis.transients) CHECK(frame % 24000 < 240);
    auto tone = signal(48000, 1, 3, [](Frame f, unsigned) { return float(.4 * std::sin(2 * std::numbers::pi * 440 * double(f) / 48000)); });
    CHECK(analyzeRhythm(*tone).bpm == 0);
    // Anchor alignment: move an isolated attack from 1 s to 1.25 s. Latency
    // compensation must not leave a silent lead-in or the attack at its old time.
    auto attack = signal(48000, 1, 3, [](Frame f, unsigned) {
        return f >= 48000 && f < 48480 ? float(.8 * std::exp(-double(f - 48000) / 150)) : 0.f;
    });
    auto map = PitchTimeState::identity(attack, 120); CHECK(map.time.add(0)); CHECK(map.time.add(48000)); CHECK(map.time.add(attack->frames)); CHECK(map.time.move(1, 2.5));
    auto warped = renderPitchTime(map);
    CHECK(energy(*warped, 57600, 62400) > 1);
    CHECK(energy(*warped, 45600, 50400) < .01);
}
void outerAnchorTests() {
    auto source = signal(48000, 1, 8, [](Frame f, unsigned) { return float(f % 1000) / 1000; });
    auto map = PitchTimeState::identity(source, 120);
    CHECK(map.time.anchors.empty()); CHECK(!map.time.move(0, 6));
    CHECK(map.time.add(96000)); CHECK(!map.time.move(0, 6)); // a single anchor cannot stretch
    CHECK(map.time.add(288000)); CHECK(map.time.move(0, 6)); // first anchor: 2 s -> beat at 3 s
    CHECK(map.time.startBeat() == 2); CHECK(map.time.frames() == 336000); // file is now 7 seconds
    CHECK(map.time.outputAt(96000) == 96000); // still exactly 2 seconds before the first anchor
    CHECK(map.time.outputAt(288000) == 240000);
    for (Frame f : {Frame(0), Frame(10000), Frame(95999)}) CHECK(std::abs(map.time.sourceAt(double(f)) - double(f)) < 1e-8);
    CHECK(map.time.frames() - map.time.outputAt(288000) == 96000); // trailing two seconds are intact too
    CHECK(map.time.move(1, 14)); CHECK(map.time.frames() == 384000);
    CHECK(map.time.frames() - map.time.outputAt(288000) == 96000);
    const auto anchors = map.time.anchors; map.time.bpm = 150;
    CHECK(map.time.frames() == 307200); CHECK(map.time.outputAt(96000) == 76800);
    CHECK(map.time.frames() - map.time.outputAt(288000) == 76800); // BPM scales the whole file, including tails
    for (std::size_t i = 0; i < anchors.size(); ++i) CHECK(map.time.anchors[i].beat == anchors[i].beat);
    map.time.bpm = 120; CHECK(map.time.move(0, -2)); map.validate(); // a leading edge may precede beat one
    CHECK(map.time.outputAt(96000) == 96000); CHECK(std::abs(map.time.sourceAt(40000) - 40000) < 1e-8);
}
void backgroundTests() {
    auto source = signal(48000, 2, 3, [](Frame f, unsigned c) {
        return float(.4 * std::sin(2 * std::numbers::pi * 440 * double(f) / 48000)) * (c ? -1.f : 1.f);
    });
    auto map = PitchTimeState::identity(source, 120); map.time.add(24000); map.time.add(96000); map.time.move(1, 5);
    const auto bytes = AudioFile::audioBytes();
    auto preview = previewPitchTime(map); CHECK(AudioFile::audioBytes() == bytes);
    CHECK(needsPitchTimeRender(*preview)); const auto wrapper = preview->pieces[0].source;
    for (Frame f = 0; f < preview->frames; f += 1777) {
        const auto at = map.time.sourceAt(double(f)); const auto a = Frame(std::floor(at));
        CHECK(std::abs(preview->sample(f, 0) - std::lerp(source->sample(a, 0), source->sample(a + 1, 0), float(at - a))) < 1e-6);
    }
    CHECK(preview->mixPeak(0, preview->frames, .5, .5).hi == 0);
    auto slice = preview->range(1000, 90000); // edits and clipboard retain the upgradeable audio source
    std::stop_source cancel; cancel.request_stop(); rejects([&] { resolvePitchTime(*preview, nullptr, cancel.get_token()); });
    CHECK(needsPitchTimeRender(*preview));
    Document doc; doc.open(source->pieces[0].source); doc.apply(preview, "Stretch");
    const auto revision = doc.timeline();
    PitchTimeRenderQueue queue; queue.update(preview);
    auto newerMap = map; newerMap.time.move(1, 6); auto newer = previewPitchTime(newerMap);
    doc.apply(newer, "Move again"); queue.update(newer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (needsPitchTimeRender(*newer) || queue.active()) {
        queue.update(newer); CHECK(queue.error().empty()); CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    queue.update(newer); CHECK(doc.timeline() == newer); CHECK(!needsPitchTimeRender(*newer));
    CHECK(doc.undo()); CHECK(doc.timeline() == revision); // completion did not add history or replace the current edit
    resolvePitchTime(slice); CHECK(!needsPitchTimeRender(*preview)); CHECK(wrapper->pitchTimeResult());
    CHECK(slice.sample(5000, 0) == wrapper->pitchTimeResult()->sample(6000, 0));
    CHECK(std::abs(frequency(*preview, .8, 1.8) - 440) < 5);
    // Foreground DSP resolves pending previews before measuring or baking them.
    auto pending = previewPitchTime(map); auto cleaned = processAudio(*pending, 0, pending->frames, {Cleanup::RemoveDC, 0});
    CHECK(!needsPitchTimeRender(*pending)); CHECK(std::abs(frequency(*cleaned, .8, 1.8) - 440) < 5);
    auto exporting = previewPitchTime(map);
    const auto path = std::filesystem::current_path() / ("coldedit-stretch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    exportAudio(*exporting, path, ExportFormat::Float32);
    CHECK(!needsPitchTimeRender(*exporting));
    { auto audio = AudioFile::open(path); Timeline exported; exported.sampleRate = audio->sampleRate; exported.channels = audio->channels;
      exported.pieces = {{audio, 0, audio->frames}}; exported.rebuild(); CHECK(std::abs(frequency(exported, .8, 1.8) - 440) < 5); }
    std::filesystem::remove(path);
}
int main() try {
    renderTests(); mapAndHistoryTests(); rhythmTests(); outerAnchorTests(); backgroundTests();
    std::cout << "Stretch, pitch, timing, stereo, history, cancellation and rhythm checks passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
