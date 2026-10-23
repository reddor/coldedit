#include "document.hpp"
#include "processing.hpp"
#include "viewport.hpp"
#include "waveform.hpp"
#include "interaction.hpp"
#include "web_limits.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace coldedit;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string("Check failed: ") + #x + " at line " + std::to_string(__LINE__)); } while (0)
static void near(float a, float b, float tolerance = 0.000001f) { CHECK(std::abs(a - b) <= tolerance); }
template <class F> static void throws(F&& f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
static void u16(std::ostream& f, unsigned x) { f.put(char(x)); f.put(char(x >> 8)); }
static void u32(std::ostream& f, std::uint32_t x) { u16(f, x); u16(f, x >> 16); }
static std::filesystem::path root;
static std::filesystem::path fixture(const char* name, unsigned bits, unsigned channels, bool floating, const std::vector<float>& data, bool extensible = false, bool junk = false) {
    auto p = root / name;
    const unsigned bytes = unsigned(data.size()) * bits / 8, fmt = extensible ? 40 : 16;
    std::ofstream f(p, std::ios::binary);
    f.write("RIFF", 4); u32(f, 20 + fmt + bytes + (bytes & 1) + (junk ? 12 : 0)); f.write("WAVE", 4);
    if (junk) { f.write("JUNK", 4); u32(f, 3); f.write("abc\0", 4); }
    f.write("fmt ", 4); u32(f, fmt); u16(f, extensible ? 0xfffe : floating ? 3 : 1); u16(f, channels);
    u32(f, 48000); u32(f, 48000 * channels * bits / 8); u16(f, channels * bits / 8); u16(f, bits);
    if (extensible) { u16(f, 22); u16(f, bits); u32(f, channels == 2 ? 3 : 4); u32(f, floating ? 3 : 1); u16(f, 0); u16(f, 0x10); const unsigned char tail[] = {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}; f.write(reinterpret_cast<const char*>(tail), 8); }
    f.write("data", 4); u32(f, bytes);
    for (float v : data) {
        std::uint32_t q = 0;
        if (floating) q = std::bit_cast<std::uint32_t>(v);
        else if (bits == 8) q = std::uint32_t(std::clamp(std::llround(v * 128 + 128), 0LL, 255LL));
        else { const double scale = std::pow(2., bits - 1); q = std::uint32_t(std::int32_t(std::llround(std::clamp(v * scale, -scale, scale - 1)))); }
        for (unsigned b = 0; b < bits / 8; ++b) f.put(char(q >> (b * 8)));
    }
    if (bytes & 1) f.put(0);
    return p;
}
static void parserTests() {
    for (unsigned bits : {8u, 16u, 24u, 32u}) for (unsigned channels : {1u, 2u}) {
        auto p = fixture("pcm.wav", bits, channels, false, {-.75f, -.5f, 0, .5f, .25f, -.25f}, bits == 24, true);
        auto s = AudioFile::open(p); CHECK(s->frames == Frame(6 / channels)); CHECK(s->bits == bits); CHECK(s->channels == channels);
        near(s->sample(0, 0), -.75f); near(s->sample(Frame(3 / channels), 3 % channels), .5f);
        near(s->sample(-1, 0), 0); near(s->sample(900, 0), 0); near(s->sample(0, 4), 0);
        s->waitForPeaks(); CHECK(s->peaksReady());
    }
    auto p = fixture("float.wav", 32, 1, true, {.12345f, -1.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}, true);
    auto s = AudioFile::open(p); near(s->sample(0, 0), .12345f); near(s->sample(1, 0), -1.5f); near(s->sample(2, 0), 0); near(s->sample(3, 0), 0); s.reset();
    auto hugeFloat = AudioFile::open(fixture("headroom.wav", 32, 1, true, {std::numeric_limits<float>::max()}));
    Document headroom; headroom.open(hugeFloat); headroom.effect(0, 1, Effect::Gain, 1000); CHECK(std::isfinite(headroom.timeline()->sample(0, 0)));
    auto empty = AudioFile::open(fixture("empty.wav", 16, 2, false, {})); CHECK(empty->frames == 0); empty->waitForPeaks();
    throws([&] { AudioFile::open(root / "missing.wav"); });
    auto corrupt = fixture("bad.wav", 16, 1, false, {0, .5});
    { std::fstream f(corrupt, std::ios::binary | std::ios::in | std::ios::out); f.seekp(4); u32(f, 0xffffffff); }
    throws([&] { AudioFile::open(corrupt); });
    corrupt = fixture("bad.wav", 16, 1, false, {0, .5});
    { std::fstream f(corrupt, std::ios::binary | std::ios::in | std::ios::out); f.seekp(40); u32(f, 1000000); }
    throws([&] { AudioFile::open(corrupt); });
    corrupt = fixture("bad.wav", 16, 1, false, {0, .5});
    { std::fstream f(corrupt, std::ios::binary | std::ios::in | std::ios::out); f.seekp(32); u16(f, 0); }
    throws([&] { AudioFile::open(corrupt); });
    corrupt = fixture("bad.wav", 24, 2, false, {0, .5}, true);
    { std::fstream f(corrupt, std::ios::binary | std::ios::in | std::ios::out); f.seekp(52); f.put(0); }
    throws([&] { AudioFile::open(corrupt); });
    throws([&] { AudioFile::open(fixture("surround.wav", 16, 3, false, {0, 0, 0})); });
    // Exercise every truncation boundary without trusting the RIFF header.
    auto good = fixture("original.wav", 16, 2, false, {.1f, .2f, .3f, .4f}, true, true);
    std::ifstream input(good, std::ios::binary); std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        auto target = root / "truncated.wav"; { std::ofstream f(target, std::ios::binary); f.write(bytes.data(), std::streamsize(n)); }
        throws([&] { AudioFile::open(target); });
    }
}
static void peakTests() {
    std::vector<float> samples(65536, 0); samples[255] = -.9f; samples[256] = .8f; samples[32768] = .99f;
    auto s = AudioFile::open(fixture("peaks.wav", 32, 1, true, samples)); s->waitForPeaks();
    auto p = s->peak(0, 257, 0); CHECK(p.lo <= -.9f && p.hi >= .8f);
    p = s->peak(32760, 33020, 0); CHECK(p.hi >= .99f); CHECK(s->peakBytes() < samples.size() / 20);
    p = s->peak(255, 256, 0); near(p.lo, -.9f); near(p.hi, -.9f);
    // Cached intervals must exclude impulses immediately outside either edge,
    // including at the 256 -> 4096 -> 65536 cache-level boundaries.
    std::mt19937 random(73);
    for (int i = 0; i < 500; ++i) {
        const Frame a = random() % 65536, b = std::min<Frame>(65536, a + 1 + random() % 17000);
        const auto exact = std::minmax_element(samples.begin() + a, samples.begin() + b);
        p = s->peak(a, b, 0); near(p.lo, *exact.first); near(p.hi, *exact.second);
    }
    for (Frame end : {255, 256, 4095, 4096, 32768, 65535}) {
        p = s->peak(257, std::max<Frame>(258, end), 0);
        near(p.lo, 0); near(p.hi, end > 32768 ? .99f : 0);
    }
    Document d; d.open(s); d.effect(0, s->frames, Effect::Reverse);
    p = d.timeline()->peak(s->frames - 257, s->frames, 0); CHECK(p.lo <= -.9f && p.hi >= .8f);
    d.effect(0, s->frames, Effect::Gain, -.5f); p = d.timeline()->peak(0, s->frames, 0); CHECK(p.lo < -.49f && p.hi > .44f);
}
static void waveformTests() {
    auto impulse = AudioFile::open(fixture("waveform-impulse.wav", 32, 1, true, {0, 0, 1, 0, 0}));
    impulse->waitForPeaks();
    Document doc; doc.open(impulse);
    auto peak = [&](double a, double b) { return waveformPeak(*doc.timeline(), a, b, 0); };
    // Moving a pixel edge past an impulse must approach it continuously, not
    // jump from zero to the full peak as integer floor/ceil queries would.
    near(peak(.4999, 1.9999).hi, .9999f);
    near(peak(.5001, 2.0001).hi, 1.f);
    near(peak(1.9999, 3.4999).hi, 1.f);
    near(peak(2.0001, 3.5001).hi, .9999f);
    near(peak(1.25, 1.75).lo, .25f); near(peak(1.25, 1.75).hi, .75f);
    near(peak(1.5, 1.5).lo, .5f); near(peak(1.5, 1.5).hi, .5f);
    near(peak(-8, -2).hi, 0); near(peak(6, 8).lo, 0);

    auto dc = AudioFile::open(fixture("waveform-dc.wav", 32, 1, true, {.4f, .4f, .4f}));
    dc->waitForPeaks(); doc.open(dc);
    near(peak(.2, .3).lo, .4f); near(peak(.2, .3).hi, .4f); // No integer sample in this pixel.
    near(peak(0, 2).lo, .4f); near(peak(0, 2).hi, .4f); // Do not add a zero baseline.
    near(peak(2.25, 2.75).lo, .1f); near(peak(2.25, 2.75).hi, .3f);
    near(peak(2.75, 5).lo, 0); near(peak(2.75, 5).hi, .1f);

    // Compare against an independent flat-PCM oracle, including cache-block
    // boundaries, stereo, edits, empty pixels, and samples outside the file.
    std::vector<float> samples(10000 * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = float(std::sin(i * .17) * .7 + std::cos(i * .031) * .2);
    auto source = AudioFile::open(fixture("waveform-stereo.wav", 32, 2, true, samples));
    source->waitForPeaks(); doc.open(source);
    doc.effect(253, 4101, Effect::Reverse); doc.effect(251, 4099, Effect::Gain, -.7f);
    doc.insertSilence(4090, 17); doc.channelTool(ChannelTool::Swap, 2000, 4500);
    const auto timeline = doc.timeline();
    std::vector<float> flat(std::size_t(timeline->frames) * 2);
    for (Frame i = 0; i < timeline->frames; ++i) for (unsigned c = 0; c < 2; ++c) flat[std::size_t(i) * 2 + c] = timeline->sample(i, c);
    const auto value = [&](Frame i, unsigned c) { return i < 0 || i >= timeline->frames ? 0.f : flat[std::size_t(i) * 2 + c]; };
    const auto reference = [&](double a, double b, unsigned c) {
        Peak out{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        // Intersect every line segment with the pixel and inspect its ends.
        for (Frame i = Frame(std::floor(a)); i <= Frame(std::floor(b)); ++i) {
            const double left = std::max(a, double(i)), right = std::min(b, double(i + 1));
            for (double f : {left, right}) {
                const float v = float((1 - (f - i)) * value(i, c) + (f - i) * value(i + 1, c));
                out.lo = std::min(out.lo, v); out.hi = std::max(out.hi, v);
            }
        }
        return out;
    };
    std::mt19937 random(821);
    for (int i = 0; i < 500; ++i) {
        const double a = double(random() % 1003000) / 100 - 10;
        const double b = a + (i % 2 ? double(random() % 100) / 100 : random() % 5000);
        for (unsigned c = 0; c < 2; ++c) {
            const auto actual = waveformPeak(*timeline, a, b, c), expected = reference(a, b, c);
            near(actual.lo, expected.lo); near(actual.hi, expected.hi);
        }
    }
    // Small viewport movements around sample boundaries, across the old 1/4
    // samples-per-pixel transitions and several physical-pixel densities.
    for (double dpi : {1., 1.25, 2.}) for (double span : {700., 1399., 1400., 1401., 5599., 5600., 5601.}) {
        const double step = span / (1400 * dpi);
        for (double a : {254., 255., 256., 4095., 4096.}) {
            const auto before = waveformPeak(*timeline, a - .0001, a + step - .0001, 0);
            const auto after = waveformPeak(*timeline, a + .0001, a + step + .0001, 0);
            // All PCM values lie in [-1,1], so linear segments have slope <=2.
            CHECK(std::abs(before.lo - after.lo) < .00041f);
            CHECK(std::abs(before.hi - after.hi) < .00041f);
        }
    }
    Timeline empty; near(waveformPeak(empty, 0, 1, 0).hi, 0);
}
static void documentTests() {
    std::vector<float> original; for (int i = 0; i < 64; ++i) original.push_back(float(i - 32) / 64);
    auto source = AudioFile::open(fixture("edits.wav", 32, 1, true, original));
    Document doc; doc.open(source); CHECK(!doc.dirty());
    doc.effect(10, 30, Effect::Reverse); CHECK(doc.dirty());
    near(doc.timeline()->sample(10, 0), original[29]); near(doc.timeline()->sample(29, 0), original[10]);
    doc.replace(15, 25, {}, "Delete"); CHECK(doc.timeline()->frames == 54);
    CHECK(doc.undo()); CHECK(doc.undo()); CHECK(!doc.dirty()); CHECK(doc.redo()); doc.markSaved(); CHECK(!doc.dirty());
    doc.effect(10, 30, Effect::Silence); CHECK(doc.dirty()); doc.undo(); CHECK(!doc.dirty());
    doc.open(source);
    std::vector<std::vector<float>> states{original}; std::size_t current = 0;
    std::mt19937 random(42);
    for (int iteration = 0; iteration < 2400; ++iteration) {
        auto expected = states[current]; const int operation = int(random() % 11);
        if (operation == 0 && current) { CHECK(doc.undo()); --current; }
        else if (operation == 1 && current + 1 < states.size()) { CHECK(doc.redo()); ++current; }
        else if (!expected.empty()) {
            auto a = std::size_t(random() % expected.size()), b = std::size_t(random() % expected.size()); if (a > b) std::swap(a, b); ++b;
            bool changed = true;
            if (operation == 2) { doc.effect(Frame(a), Frame(b), Effect::Reverse); std::reverse(expected.begin() + a, expected.begin() + b); }
            else if (operation == 3) { doc.effect(Frame(a), Frame(b), Effect::Gain, .5f); for (auto j = a; j < b; ++j) expected[j] *= .5f; }
            else if (operation == 4) { doc.effect(Frame(a), Frame(b), Effect::Silence); std::fill(expected.begin() + a, expected.begin() + b, 0.f); }
            else if (operation == 5 && expected.size() > 8) { doc.replace(Frame(a), Frame(b), {}, "Delete"); expected.erase(expected.begin() + a, expected.begin() + b); }
            else if (operation == 8 || operation == 9) {
                const bool out = operation == 9, smooth = random() % 2;
                doc.fade(Frame(a), Frame(b), out, smooth ? FadeCurve::Smooth : FadeCurve::Linear);
                for (auto j = a; j < b; ++j) {
                    double gain = b - a <= 1 ? 0 : double(j - a) / double(b - a - 1);
                    if (out && b - a > 1) gain = 1 - gain;
                    if (smooth) gain = gain * gain * (3 - 2 * gain);
                    expected[j] *= float(gain);
                }
            }
            else if (operation == 10 && expected.size() < 500) {
                const auto count = 1 + random() % 17;
                doc.insertSilence(Frame(a), count); expected.insert(expected.begin() + a, count, 0);
            }
            else if (expected.size() < 500) {
                auto clipboard = doc.timeline()->slice(Frame(a), Frame(b)); std::vector<float> copy(expected.begin() + a, expected.begin() + b);
                const auto where = std::size_t(random() % (expected.size() + 1)); doc.replace(Frame(where), Frame(where), clipboard, "Paste"); expected.insert(expected.begin() + where, copy.begin(), copy.end());
            } else changed = false;
            if (changed) { states.resize(current + 1); states.push_back(expected); if (states.size() > 129) states.erase(states.begin()); current = states.size() - 1; }
        }
        CHECK(doc.timeline()->frames == Frame(states[current].size()));
        for (std::size_t f = 0; f < states[current].size(); ++f) near(doc.timeline()->sample(Frame(f), 0), states[current][f]);
        CHECK(doc.historySize() <= 129);
    }
    doc.open(source); doc.trim(3, 19); CHECK(doc.timeline()->frames == 16); near(doc.timeline()->sample(0, 0), original[3]);
    auto pieces = doc.timeline()->slice(0, 16); CHECK(pieces.size() == 1 && pieces[0].source == source);
    doc.replace(0, 16, {}, "Delete all"); CHECK(doc.timeline()->frames == 0); doc.undo(); CHECK(doc.timeline()->frames == 16);
}
static void fadeAndSilenceTests() {
    std::vector<float> samples;
    for (int i = 0; i < 513; ++i) { samples.push_back(.75f); samples.push_back(-.25f); }
    auto source = AudioFile::open(fixture("fades.wav", 32, 2, true, samples)); source->waitForPeaks();
    Document d;
    for (auto curve : {FadeCurve::Linear, FadeCurve::Smooth}) for (bool out : {false, true}) {
        d.open(source);
        d.effect(40, 70, Effect::Gain, .5f); // Selection spans pieces with different gains.
        auto before = d.timeline(); d.fade(10, 90, out, curve); auto faded = d.timeline();
        CHECK(d.undoLabel() == (out ? "Fade out" : "Fade in"));
        for (Frame i = 0; i < 513; ++i) for (unsigned c = 0; c < 2; ++c) {
            double gain = 1;
            if (i >= 10 && i < 90) {
                gain = double(i - 10) / 79; if (out) gain = 1 - gain;
                if (curve == FadeCurve::Smooth) gain = gain * gain * (3 - 2 * gain);
            }
            near(faded->sample(i, c), float(before->sample(i, c) * gain));
        }
        for (const auto& p : faded->pieces) CHECK(p.source == source); // No rendered PCM copies.
        auto range = faded->range(20, 80);
        CHECK(range.frames == 60 && range.channels == 2 && range.sampleRate == 48000);
        for (Frame i = 0; i < 60; ++i) for (unsigned c = 0; c < 2; ++c) near(range.sample(i, c), faded->sample(i + 20, c));
        d.effect(15, 95, Effect::Reverse);
        for (Frame i = 15; i < 95; ++i) near(d.timeline()->sample(i, 0), faded->sample(109 - i, 0));
        CHECK(d.undo()); CHECK(d.timeline() == faded); CHECK(d.undo()); CHECK(d.timeline() == before); CHECK(d.redo());
        d.fade(10, 90, !out, curve); // Opposing envelopes overlap; peaks must enclose all samples.
        d.effect(0, 513, Effect::Gain, -2);
        for (Frame a = 0; a < 110; ++a) for (unsigned c = 0; c < 2; ++c) {
            const auto p = d.timeline()->peak(a, a + 29, c);
            for (Frame i = a; i < a + 29; ++i) { const float v = d.timeline()->sample(i, c); CHECK(v >= p.lo - 1e-6f && v <= p.hi + 1e-6f); }
        }
    }
    d.open(source); d.fade(12, 13, false); near(d.timeline()->sample(12, 0), 0); d.undo();
    d.fade(12, 13, true); near(d.timeline()->sample(12, 1), 0);
    auto unchanged = d.timeline(); d.fade(30, 30, false); CHECK(d.timeline() == unchanged);
    for (Frame at : {Frame(0), Frame(100), Frame(513)}) {
        d.open(source); auto before = d.timeline(); d.insertSilence(at, 48000);
        CHECK(d.timeline()->frames == 48513); CHECK(d.timeline()->pieces.size() <= 3);
        for (Frame f = 0; f < 48513; ++f) for (unsigned c = 0; c < 2; ++c)
            near(d.timeline()->sample(f, c), f < at ? before->sample(f, c) : f < at + 48000 ? 0 : before->sample(f - 48000, c));
        CHECK(d.undo()); CHECK(d.timeline() == before); CHECK(d.redo());
    }
    auto empty = AudioFile::open(fixture("empty-silence.wav", 16, 1, false, {})); d.open(empty);
    d.insertSilence(0, 1); CHECK(d.timeline()->frames == 1); near(d.timeline()->sample(0, 0), 0);
    d.insertSilence(0, 0); CHECK(d.timeline()->frames == 1);
    throws([&] { d.insertSilence(0, std::numeric_limits<Frame>::max()); }); CHECK(d.timeline()->frames == 1);
    d.insertSilence(1, Frame(48000) * 60 * 60 * 24 * 365);
    CHECK(d.timeline()->pieces.size() == 1); // A year of silence still occupies one tiny piece.
}
static void cleanupTests() {
    std::vector<float> data;
    for (int i = 0; i < 48000; ++i) { const auto v = float(std::sin(i * 2 * 3.141592653589793 / 48) * .3); data.push_back(v + .2f); data.push_back(-v - .1f); }
    auto source = AudioFile::open(fixture("cleanup.wav", 32, 2, true, data)); source->waitForPeaks();
    Document d; d.open(source); const auto original = d.timeline();
    for (auto tool : {ChannelTool::ExtractLeft, ChannelTool::ExtractRight, ChannelTool::Mono, ChannelTool::Swap, ChannelTool::Invert, ChannelTool::InvertLeft, ChannelTool::InvertRight}) {
        d.open(source); d.fade(100, 500, false); auto before = d.timeline();
        d.channelTool(tool, 200, 700); auto after = d.timeline();
        const bool mono = int(tool) < 3;
        CHECK(after->channels == (mono ? 1u : 2u)); CHECK(after->frames == before->frames);
        for (Frame f = 0; f < 1000; ++f) for (unsigned c = 0; c < after->channels; ++c) {
            float expected = before->sample(f, c);
            if (tool == ChannelTool::ExtractRight) expected = before->sample(f, 1);
            if (tool == ChannelTool::Mono) expected = (before->sample(f, 0) + before->sample(f, 1)) * .5f;
            if (tool == ChannelTool::Swap) expected = before->sample(f, 1 - c);
            if (f >= 200 && f < 700 && (tool == ChannelTool::Invert || (tool == ChannelTool::InvertLeft && c == 0) || (tool == ChannelTool::InvertRight && c == 1))) expected = -expected;
            near(after->sample(f, c), expected);
        }
        for (const auto& p : after->pieces) CHECK(p.source == source);
        CHECK(d.undo()); CHECK(d.timeline() == before); CHECK(d.redo()); CHECK(d.timeline() == after);
    }
    d.open(source); d.channelTool(ChannelTool::Mono, 0, 0);
    auto monoPeak = d.timeline()->peak(0, 48000, 0); near(monoPeak.hi, .05f); near(monoPeak.lo, .05f);
    auto monoPath = root / "mixed-mono.wav"; exportWav(*d.timeline(), monoPath, ExportFormat::Float32);
    auto mono = AudioFile::open(monoPath); CHECK(mono->channels == 1); near(mono->sample(100, 0), .05f);
    auto dc = processAudio(*original, 1000, 47000, {Cleanup::RemoveDC, 0});
    std::array<double, 2> sums{};
    for (Frame f = 1000; f < 47000; ++f) for (unsigned c = 0; c < 2; ++c) sums[c] += dc->sample(f, c);
    for (auto sum : sums) CHECK(std::abs(sum / 46000) < 1e-7);
    near(dc->sample(999, 0), original->sample(999, 0)); near(dc->sample(47000, 1), original->sample(47000, 1));
    const auto tempPath = dc->pieces[1].source->path;
    d.open(source); d.apply(dc, "Remove DC offset"); dc.reset(); CHECK(d.undo()); CHECK(d.redo());
    CHECK(std::filesystem::exists(tempPath)); d.open(source); CHECK(!std::filesystem::exists(tempPath));
    auto normalized = processAudio(*original, 0, 48000, {Cleanup::Normalize, -6});
    near(exactPeak(*normalized, 0, 48000), float(std::pow(10., -.3)), 1e-6f);
    CHECK(normalized->pieces.front().source == source);
    auto quiet = AudioFile::open(fixture("quiet.wav", 32, 1, true, {1e-20f, -1e-20f})); d.open(quiet);
    normalized = processAudio(*d.timeline(), 0, 2, {Cleanup::Normalize, 0}); near(normalized->sample(0, 0), 1);
    throws([&] { processAudio(*original, 0, 48000, {Cleanup::Normalize, 1}); });
    throws([&] { processAudio(*original, 0, 48000, {Cleanup::LowPass, 24000}); });
    throws([&] { processAudio(*original, 0, 48000, {Cleanup::HighPass, std::numeric_limits<double>::quiet_NaN()}); });
    std::stop_source stop; stop.request_stop();
    throws([&] { processAudio(*original, 0, 48000, {Cleanup::RemoveDC, 0}, nullptr, stop.get_token()); });
    std::stop_source midway;
    throws([&] { AudioFile::render(10000, 48000, 2, [&](Frame, Frame, float*) { midway.request_stop(); }, nullptr, midway.get_token()); });
    // Measure the steady-state response at low, cutoff, and high frequencies.
    for (unsigned rate : {8000u, 44100u, 48000u, 96000u}) for (auto kind : {Cleanup::HighPass, Cleanup::LowPass}) {
        for (double frequency : {100., 1000., 3000.}) {
            auto tone = AudioFile::render(rate, rate, 1, [&](Frame at, Frame n, float* block) {
                for (Frame i = 0; i < n; ++i) block[i] = float(std::sin((at + i) * 2 * 3.141592653589793 * frequency / rate) * .5);
            }, nullptr, {});
            d.open(tone); auto filtered = processAudio(*d.timeline(), 0, rate, {kind, 1000});
            double inputPower = 0, outputPower = 0;
            for (Frame f = rate / 2; f < rate; ++f) { inputPower += std::pow(tone->sample(f, 0), 2); outputPower += std::pow(filtered->sample(f, 0), 2); }
            const double warped = std::tan(3.141592653589793 * frequency / rate) / std::tan(3.141592653589793 * 1000 / rate);
            const double expected = kind == Cleanup::LowPass ? 1 / std::sqrt(1 + std::pow(warped, 4)) : 1 / std::sqrt(1 + std::pow(1 / warped, 4));
            CHECK(std::abs(std::sqrt(outputPower / inputPower) - expected) < .002);
        }
    }
    d.open(source); auto before = d.timeline(); d.fadeEdges(100, 1000, 144);
    near(d.timeline()->sample(100, 0), 0); near(d.timeline()->sample(999, 1), 0);
    near(d.timeline()->sample(500, 0), before->sample(500, 0)); CHECK(d.undo()); CHECK(d.timeline() == before);
    d.replace(300, 600, {}, "Delete", 144);
    near(d.timeline()->sample(299, 0), 0); near(d.timeline()->sample(300, 1), 0);
    CHECK(d.historySize() == 2); CHECK(d.undo()); CHECK(d.timeline() == before);
    d.trim(100, 900, 144); near(d.timeline()->sample(0, 0), 0); near(d.timeline()->sample(799, 1), 0);
    auto crossing = AudioFile::open(fixture("crossing.wav", 32, 1, true, {1, 1, 1, -.1f, -.2f, -.3f, .1f, .2f})); d.open(crossing);
    CHECK(zeroCrossing(*d.timeline(), 4, 2) == 3); CHECK(zeroCrossing(*d.timeline(), 5, 2) == 6);
    CHECK(zeroCrossing(*d.timeline(), 1, 1) == 1); CHECK(zeroCrossing(*d.timeline(), 0, 20) == 0);
    CHECK(zeroCrossing(*d.timeline(), 8, 20) == 8);
}
static void mixTests() {
    auto file = AudioFile::open(fixture("mix-source.wav", 32, 2, true, {.8f, -.2f, .5f, .3f, -.1f, .7f, .4f, -.5f, .2f, .1f}));
    Document d; d.open(file); d.fade(0, 5, false); d.channelTool(ChannelTool::Swap, 0, 5);
    const auto original = d.timeline(); const auto clip = original->range(1, 4);
    for (const auto range : {std::pair<Frame, Frame>{1, 2}, {1, 4}, {1, 5}, {4, 8}, {7, 10}}) {
        const auto [a, b] = range; const Frame end = a + std::min(b - a, clip.frames);
        auto mixed = mixAudio(*original, clip, a, b);
        CHECK(mixed->frames == std::max(original->frames, end));
        for (Frame f = 0; f < mixed->frames; ++f) for (unsigned c = 0; c < 2; ++c)
            near(mixed->sample(f, c), original->sample(f, c) + (f >= a && f < end ? clip.sample(f - a, c) : 0));
        d.apply(mixed, "Paste into selection"); CHECK(d.undo()); CHECK(d.timeline() == original); CHECK(d.redo()); CHECK(d.timeline() == mixed); CHECK(d.undo());
    }
    Timeline empty; empty.channels = 2; empty.sampleRate = file->sampleRate;
    auto recovered = mixAudio(empty, *original, 0, original->frames);
    CHECK(recovered->frames == original->frames);
    for (Frame f = 0; f < recovered->frames; ++f) for (unsigned c = 0; c < 2; ++c) near(recovered->sample(f, c), original->sample(f, c));
    d.open(file); const auto loud = mixAudio(*d.timeline(), *d.timeline(), 0, 5);
    near(loud->sample(0, 0), 1.6f); // Float headroom is retained until playback/export.
    auto mismatch = clip; mismatch.sampleRate = 44100;
    throws([&] { mixAudio(*original, mismatch, 0, 3); }); mismatch = clip; mismatch.channels = 1;
    throws([&] { mixAudio(*original, mismatch, 0, 3); });
    throws([&] { mixAudio(*original, clip, 3, 3); });
    std::stop_source stop; stop.request_stop();
    throws([&] { mixAudio(*original, clip, 0, 3, nullptr, stop.get_token()); });
    CHECK(d.timeline()->pieces.front().source == file);
}
static void exportTests() {
    auto s = AudioFile::open(fixture("export-source.wav", 32, 2, true, {-.5f, .5f, -.25f, .25f, 0, .125f})); Document d; d.open(s);
    d.effect(1, 3, Effect::Reverse);
    d.fade(0, 3, false, FadeCurve::Smooth);
    d.insertSilence(1, 2);
    for (auto format : {ExportFormat::PCM16, ExportFormat::PCM24, ExportFormat::Float32}) {
        auto snapshot = d.timeline(); const auto history = d.historySize();
        auto selection = snapshot->range(1, 4);
        auto p = root / ("export-" + std::to_string(int(format)) + ".wav"); exportWav(selection, p, format);
        auto out = AudioFile::open(p); CHECK(out->frames == 3); CHECK(out->sampleRate == 48000); CHECK(out->channels == 2);
        for (Frame f = 0; f < 3; ++f) for (unsigned c = 0; c < 2; ++c) near(out->sample(f, c), snapshot->sample(f + 1, c));
        CHECK(d.timeline() == snapshot && d.historySize() == history && d.dirty());
        throws([&] { exportWav(*d.timeline(), p, format); });
    }
    auto mono = AudioFile::open(fixture("mono.wav", 16, 1, false, {.25f})); d.open(mono);
    auto odd = root / "odd.wav"; exportWav(*d.timeline(), odd, ExportFormat::PCM24); CHECK(std::filesystem::file_size(odd) == 48);
    auto reread = AudioFile::open(odd); CHECK(reread->frames == 1); near(reread->sample(0, 0), .25f);
    std::stop_source stop; stop.request_stop(); auto cancelled = root / "cancelled.wav";
    throws([&] { exportWav(*d.timeline(), cancelled, ExportFormat::PCM16, nullptr, stop.get_token()); }); CHECK(!std::filesystem::exists(cancelled));
    auto unicode = root / std::filesystem::path(u8"音声-ä.wav"); exportWav(*d.timeline(), unicode, ExportFormat::PCM16); CHECK(AudioFile::open(unicode)->frames == 1);
    // Duplicating audio beyond RIFF capacity must fail before creating a temporary file.
    Timeline huge; huge.channels = 2; huge.frames = Frame(UINT32_MAX); throws([&] { exportWav(huge, root / "huge.wav", ExportFormat::Float32); });
    huge.frames = std::numeric_limits<Frame>::max(); throws([&] { exportWav(huge, root / "huge.wav", ExportFormat::Float32); });
}
static void memoryRecoveryTests() {
    auto source = AudioFile::open(fixture("recovery.wav", 16, 2, false, std::vector<float>(9600, .2f)));
    Document d; d.open(source); d.clearHistory(); CHECK(!d.dirty());
    d.effect(0, source->frames, Effect::Gain, .5f);
    CHECK(d.reclaimableAudioBytes() == 0); // Metadata edits share the original allocation.
    const auto bytes = source->storageBytes();
    std::weak_ptr<AudioFile> retired = source;
    auto clipboard = d.timeline()->pieces;
    auto filtered = processAudio(*d.timeline(), 0, source->frames, {Cleanup::LowPass, 1000});
    d.apply(filtered, "Filter"); filtered.reset(); source.reset();
    CHECK(d.reclaimableAudioBytes() == bytes); CHECK(d.reclaimableAudioBytes(clipboard) == 0);
    const auto before = d.timeline(); d.clearHistory();
    CHECK(d.timeline() == before); CHECK(d.dirty()); CHECK(!d.canUndo()); CHECK(!d.canRedo());
    CHECK(!retired.expired()); clipboard.clear(); CHECK(retired.expired()); // Saved state must not pin PCM.
    d.markSaved(); d.effect(0, 4800, Effect::Gain, .5f); CHECK(d.undo()); CHECK(!d.dirty());
    d.clearHistory(); CHECK(!d.dirty()); CHECK(!d.canRedo());
    d.effect(0, 4800, Effect::Reverse); CHECK(d.dirty()); CHECK(d.undo()); CHECK(!d.dirty());
    // Budget boundaries and lowering guard, without allocating hundreds of MiB.
    CHECK(web::audioLimit.load() == web::defaultAudioLimit);
    {
        web::AudioReservation held; held.resize(web::defaultAudioLimit);
        throws([] { web::checkAudio(1); });
        CHECK(web::setAudioLimit(web::extendedAudioLimit));
        web::checkAudio(web::defaultAudioLimit);
        held.resize(web::defaultAudioLimit + 1);
        CHECK(!web::setAudioLimit(web::defaultAudioLimit));
        CHECK(!web::setAudioLimit(512ull * 1024 * 1024));
        try { web::checkAudio(web::defaultAudioLimit); CHECK(false); }
        catch (const web::AudioBudgetExceeded& e) { CHECK(e.additionalBytes == web::defaultAudioLimit); CHECK(std::string(e.what()).find("256 MiB") != std::string::npos); }
    }
    CHECK(web::audioBytes.load() == 0); CHECK(web::setAudioLimit(web::defaultAudioLimit));
}
static void viewportTests() {
    Viewport v; v.fit(1e10); v.show(1e8, 1e6, true); double anchor = v.start + v.span * .37;
    v.zoom(.5, .37);
    for (int i = 0; i < 60; ++i) { v.update(1. / 144); CHECK(std::abs(v.start + v.span * .37 - anchor) < .01); }
    v.pan(-1e20); for (int i = 0; i < 100; ++i) v.update(.1); CHECK(v.start >= 0);
    v.zoom(1e-20, .5); for (int i = 0; i < 100; ++i) v.update(.1); CHECK(v.span >= 16);
    v.fit(0); CHECK(v.span == 1); v.zoom(.1, .5); v.update(1); CHECK(std::isfinite(v.start) && std::isfinite(v.span));
}
static void interactionTests() {
    for (float scale : {1.f, 1.25f, 1.5f, 2.f, 3.f}) {
        auto windows = DpiMetrics::from(scale, 1);
        near(windows.layout, scale); near(windows.raster, scale);
        auto retina = DpiMetrics::from(scale, scale);
        near(retina.layout, 1); near(retina.raster, scale); near(retina.framebuffer, scale);
        // The same physical hit target and drag must map to the same sample range.
        CHECK(hitSelectionEdge((100 + 6) * scale / windows.layout, 100, 200) == SelectionEdge::Start);
        CHECK(hitSelectionEdge((200 - 6) * scale / windows.layout, 100, 200) == SelectionEdge::End);
    }
    near(DpiMetrics::from(std::numeric_limits<float>::quiet_NaN(), 0).layout, 1);
    CHECK(hitSelectionEdge(150, 100, 200) == SelectionEdge::None);
    CHECK(hitSelectionEdge(100.8, 100, 101) == SelectionEdge::End);
    CHECK(hitSelectionEdge(100.2, 100, 101) == SelectionEdge::Start);
    for (bool backwards : {false, true}) {
        Frame anchor = backwards ? 200 : 100, head = backwards ? 100 : 200;
        SelectionBoundaryDrag drag;
        drag.begin(SelectionEdge::Start, anchor, head, 103);
        drag.update(103, 1000, anchor, head); CHECK(anchor == 200 && head == 100); // No grab-radius jump.
        drag.update(123, 1000, anchor, head); CHECK(anchor == 200 && head == 120);
        drag.update(253, 1000, anchor, head); CHECK(anchor == 200 && head == 250); // Crossing keeps the other edge fixed.
        drag.update(-100, 1000, anchor, head); CHECK(anchor == 200 && head == 0);
        drag.begin(SelectionEdge::End, 100, 200, 198);
        drag.update(248, 1000, anchor, head); CHECK(anchor == 100 && head == 250);
        drag.update(48, 1000, anchor, head); CHECK(anchor == 100 && head == 50);
        drag.update(2000, 1000, anchor, head); CHECK(anchor == 100 && head == 1000);
        SelectionRangeDrag range;
        range.begin(backwards ? 200 : 100, backwards ? 100 : 200, 150, 1000);
        range.update(150, anchor, head); CHECK(std::min(anchor, head) == 100 && std::max(anchor, head) == 200);
        range.update(250, anchor, head); CHECK(std::min(anchor, head) == 200 && std::max(anchor, head) == 300);
        CHECK((anchor > head) == backwards);
        range.update(-100, anchor, head); CHECK(std::min(anchor, head) == 0 && std::max(anchor, head) == 100);
        range.update(1500, anchor, head); CHECK(std::min(anchor, head) == 900 && std::max(anchor, head) == 1000);
        range.begin(700, 1000, 800, 700); // Cut at EOF: preserve the retained range.
        range.update(300, anchor, head); CHECK(anchor == 200 && head == 500);
        range.begin(0, 1000, 500, 0); // Cut everything: retain the full original length.
        range.update(-500, anchor, head); CHECK(anchor == 0 && head == 1000);
    }
}
int main() {
    root = std::filesystem::current_path() / ("coldedit-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        parserTests(); peakTests(); waveformTests(); documentTests(); fadeAndSilenceTests(); cleanupTests(); mixTests(); exportTests(); memoryRecoveryTests(); viewportTests(); interactionTests();
        web::checkInput(web::inputLimit); ++checks;
        throws([] { web::checkInput(web::inputLimit + 1); });
        for (unsigned bits : {16u, 24u, 32u}) for (unsigned channels : {1u, 2u}) {
            auto frames = (web::exportLimit - 56) / (channels * bits / 8);
            web::checkExport(frames, channels, bits); ++checks;
            throws([=] { web::checkExport(frames + 1, channels, bits); });
            throws([=] { web::checkExport(UINT64_MAX, channels, bits); });
        }
        std::filesystem::remove_all(root);
        std::cout << "PASS: " << checks << " checks, including 2400 randomized edit/history operations.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << "\nFixtures: " << root << '\n'; return 1; }
}
