#include "audio_file.hpp"
#include "codecs.hpp"
#include "document.hpp"
#include "web_limits.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace coldedit;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("Failed: ") + #x + " at " + std::to_string(__LINE__)); } while (0)
template<class F> void throws(F&& f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
std::vector<std::uint8_t> read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary); return {(std::istreambuf_iterator<char>(f)), {}};
}
void save(const std::filesystem::path& p, const std::vector<std::uint8_t>& b) {
    std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); CHECK(bool(f));
}
void unknownLength(std::vector<std::uint8_t>& b) { b[21] &= 0xf0; std::fill(b.begin() + 22, b.begin() + 26, 0); }
void verify(const Timeline& t, const AudioFile& decoded, unsigned bits) {
    CHECK(t.frames == decoded.frames); CHECK(t.sampleRate == decoded.sampleRate); CHECK(t.channels == decoded.channels);
    const double scale = std::ldexp(1., int(bits) - 1);
    for (Frame i = 0; i < t.frames; ++i) for (unsigned c = 0; c < t.channels; ++c) {
        const auto expected = float(std::llround(std::clamp(t.sample(i, c) * scale, -scale, scale - 1)) / scale);
        CHECK(decoded.sample(i, c) == expected);
    }
}
void tests(const std::filesystem::path& root) {
    for (unsigned channels : {1u, 2u}) for (auto format : {ExportFormat::FLAC16, ExportFormat::FLAC24}) {
        auto source = AudioFile::render(12001, 44100, channels, [=](Frame at, Frame count, float* out) {
            for (Frame i = 0; i < count; ++i) for (unsigned c = 0; c < channels; ++c)
                out[i * channels + c] = float(((at + i) * (c + 1) % 4096) - 2048) / 1024; // Includes headroom/clipping.
        }, nullptr, {});
        Document doc; doc.open(source); doc.effect(102, 9111, Effect::Reverse); doc.fade(255, 8200, false);
        auto selection = doc.timeline()->range(103, 11999);
        auto file = root / (std::to_string(channels) + "-" + std::to_string(int(format)) + ".flac");
        std::atomic<float> progress{0}; exportAudio(selection, file, format, &progress);
        CHECK(progress.load() == 1); CHECK(doc.dirty());
        auto original = read(file);
        { auto decoded = AudioFile::open(file); decoded->waitForPeaks(); verify(selection, *decoded, exportBits(format)); CHECK(decoded->path == file); }
        CHECK(read(file) == original); // Releasing decoded temporary storage must never delete the input.
        throws([&] { exportAudio(selection, file, format); }); CHECK(read(file) == original);
        auto unknown = original; unknownLength(unknown); save(root / "unknown.flac", unknown);
        verify(selection, *AudioFile::open(root / "unknown.flac"), exportBits(format));
        auto corrupt = original; corrupt[26] ^= 0xff; save(root / "checksum.flac", corrupt);
        throws([&] { AudioFile::open(root / "checksum.flac"); });
        corrupt = original; corrupt.resize(corrupt.size() - 20); save(root / "truncated.flac", corrupt);
        throws([&] { AudioFile::open(root / "truncated.flac"); });
        corrupt = original; corrupt[21] |= 0x0f; std::fill(corrupt.begin() + 22, corrupt.begin() + 26, 0xff); save(root / "huge-header.flac", corrupt);
        throws([&] { AudioFile::open(root / "huge-header.flac"); });
        std::stop_source stop;
        throws([&] { decodeAudio(original.data(), original.size(), [](const DecodeInfo&) {}, [&](const float*, unsigned) { stop.request_stop(); }, nullptr, stop.get_token()); });
        auto cancelled = root / "cancelled.flac";
        throws([&] { exportAudio(selection, cancelled, format, nullptr, stop.get_token()); }); CHECK(!std::filesystem::exists(cancelled));
        throws([&] { AudioFile::open(file, nullptr, stop.get_token()); });
    }
    Timeline empty; exportAudio(empty, root / "empty.flac", ExportFormat::FLAC24);
    CHECK(AudioFile::open(root / "empty.flac")->frames == 0);
    auto mp3 = AudioFile::open(std::filesystem::path(COLDEDIT_FIXTURES) / "tone.mp3");
    CHECK(mp3->frames == 44100); CHECK(mp3->sampleRate == 44100); CHECK(mp3->channels == 2);
    double energy = 0, error = 0;
    for (Frame i = 2000; i < 42000; ++i) for (unsigned c = 0; c < 2; ++c) {
        double v = mp3->sample(i, c), t = double(i) / 44100;
        double expected = .3 * std::sin(6.283185307179586 * t * (c ? 660 : 440)) + .1 * std::sin(6.283185307179586 * t * 3700);
        CHECK(std::isfinite(v)); energy += v * v; error += (expected - v) * (expected - v);
    }
    CHECK(energy > 1000); CHECK(error / energy < .01); // Channel order, gapless start, and decoded signal.
    Document doc; doc.open(mp3); exportAudio(*doc.timeline(), root / "mp3-converted.flac", ExportFormat::FLAC24);
    verify(*doc.timeline(), *AudioFile::open(root / "mp3-converted.flac"), 24);
    auto truncated = read(std::filesystem::path(COLDEDIT_FIXTURES) / "tone.mp3"); truncated.resize(truncated.size() / 2);
    save(root / "truncated.mp3", truncated); throws([&] { AudioFile::open(root / "truncated.mp3"); });
    save(root / "garbage.mp3", std::vector<std::uint8_t>(128, 0)); throws([&] { AudioFile::open(root / "garbage.mp3"); });
    CHECK(web::audioBytes.load() == 0);
    {
        web::AudioReservation a, b; a.resize(web::audioLimit - 8); b.resize(8);
        throws([&] { b.resize(9); }); CHECK(web::audioBytes.load() == web::audioLimit);
        a.resize(0); b.resize(web::audioLimit); throws([&] { web::checkAudio(UINT64_MAX); });
    }
    CHECK(web::audioBytes.load() == 0);
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--web-fixtures") {
            const auto root = std::filesystem::absolute(argv[2]); std::filesystem::create_directories(root);
            Timeline silence; silence.pieces.push_back({nullptr, 0, Frame(web::audioLimit / 8 + 1)}); silence.rebuild();
            exportAudio(silence, root / "over-budget.flac", ExportFormat::FLAC24);
            auto b = read(root / "over-budget.flac"); unknownLength(b); save(root / "unknown-over-budget.flac", b);
            silence.pieces[0].length = Frame(web::audioLimit / 8); silence.rebuild();
            exportAudio(silence, root / "at-budget.flac", ExportFormat::FLAC24);
            silence.pieces[0].length = 48000; silence.rebuild(); exportAudio(silence, root / "small.flac", ExportFormat::FLAC24);
            std::cout << "Generated web limit fixtures in " << root << '\n'; return 0;
        }
        auto root = std::filesystem::current_path() / ("coldedit-codecs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root); tests(root); std::filesystem::remove_all(root);
        std::cout << "PASS: FLAC PCM/edits/selection round trips, unknown lengths, corruption, cancellation, MP3 signal/gapless conversion, budget accounting.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
