#include "playback.hpp"
#include "processing.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace coldedit;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string("Failed: ") + #x + " line " + std::to_string(__LINE__)); } while (0)
static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(15)); }
static void renderReady(Playback& audio, float* output) {
    // Seeking intentionally invalidates queued audio; a callback may emit silence
    // until the producer can refill the ring. Never accept stale, nonzero audio.
    for (int attempt = 0; attempt < 100; ++attempt) {
        audio.render(output, 128);
        if (output[0] != 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("Audio worker did not produce a buffer in time.");
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("coldedit-playback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    try {
        // Generate a float ramp with exact values through the streaming exporter.
        const auto path = root / "ramp.wav";
        {
            std::ofstream f(path, std::ios::binary);
            auto put16 = [&](unsigned v) { f.put(char(v)); f.put(char(v >> 8)); };
            auto put32 = [&](unsigned v) { put16(v); put16(v >> 16); };
            f.write("RIFF", 4); put32(36 + 4800 * 4); f.write("WAVEfmt ", 8); put32(16); put16(3); put16(1);
            put32(48000); put32(48000 * 4); put16(4); put16(32); f.write("data", 4); put32(4800 * 4);
            for (unsigned i = 0; i < 4800; ++i) { float v = .1f + float(i) / 10000; f.write(reinterpret_cast<const char*>(&v), 4); }
        }
        auto source = AudioFile::open(path); Document doc; doc.open(source);
        {
            Playback audio(true); audio.setVolume(1); audio.setTimeline(doc.timeline());
            float output[256]{};
            audio.render(output, 128); for (float v : output) CHECK(v == 0);
            audio.play(0, 0, 4800, false); settle(); renderReady(audio, output);
            for (unsigned i = 0; i < 128; ++i) { CHECK(std::abs(output[i * 2] - source->sample(i, 0)) < .000001f); CHECK(output[i * 2] == output[i * 2 + 1]); }
            CHECK(audio.position() == 128);
            audio.pause(); audio.render(output, 128); for (float v : output) CHECK(v == 0); CHECK(audio.position() == 128);
            audio.play(400, 0, 4800, false); settle(); renderReady(audio, output);
            CHECK(std::abs(output[0] - source->sample(400, 0)) < .000001f);
            CHECK(audio.position() == 528);
            audio.seek(2000); settle(); renderReady(audio, output); CHECK(std::abs(output[0] - source->sample(2000, 0)) < .000001f);
            audio.play(100, 100, 104, true); settle(); renderReady(audio, output);
            // The old generation can occupy the full ring. First callback clears it;
            // let the producer refill, then verify exact four-frame looping.
            for (unsigned i = 0; i < 128; ++i) CHECK(std::abs(output[i * 2] - source->sample(100 + i % 4, 0)) < .000001f);
            CHECK(audio.playing());
            audio.play(4798, 0, 4800, false); settle(); renderReady(audio, output);
            CHECK(std::abs(output[0] - source->sample(4798, 0)) < .000001f); CHECK(output[4] == 0); CHECK(!audio.playing()); CHECK(audio.position() == 4800);
            auto slowed = std::make_shared<Timeline>(*doc.timeline()); slowed->sampleRate = 24000;
            audio.setTimeline(slowed); audio.play(0, 0, 4800, false); settle(); renderReady(audio, output);
            CHECK(std::abs(output[2] - std::lerp(source->sample(0, 0), source->sample(1, 0), .5f)) < .000001f);
            doc.fade(0, 4800, false, FadeCurve::Smooth); doc.fade(0, 4800, true);
            doc.insertSilence(430, 17);
            audio.setTimeline(doc.timeline()); audio.play(400, 0, doc.timeline()->frames, false); settle(); renderReady(audio, output);
            for (unsigned i = 0; i < 128; ++i) for (unsigned c = 0; c < 2; ++c)
                CHECK(std::abs(output[i * 2 + c] - doc.timeline()->sample(400 + i, 0)) < .000001f);
            doc.open(source);
            auto filtered = processAudio(*doc.timeline(), 0, source->frames, {Cleanup::LowPass, 200});
            audio.setTimeline(filtered); audio.play(400, 0, source->frames, false); settle(); renderReady(audio, output);
            for (unsigned i = 0; i < 128; ++i) CHECK(std::abs(output[i * 2] - filtered->sample(400 + i, 0)) < .000001f);
            filtered.reset();
            // Repeated seeks and timeline replacement must not leak stale buffers.
            for (int i = 0; i < 30; ++i) {
                audio.setTimeline(doc.timeline()); audio.play(i * 10, 0, 4800, false); settle(); renderReady(audio, output);
                CHECK(std::abs(output[0] - source->sample(i * 10, 0)) < .000001f);
            }
            std::atomic<bool> finite{true};
            std::jthread callback([&](std::stop_token stop) {
                float block[256];
                while (!stop.stop_requested()) {
                    audio.render(block, 128);
                    for (float v : block) if (!std::isfinite(v)) finite.store(false);
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });
            for (int i = 0; i < 100; ++i) {
                audio.play(4798, 0, 4800, false);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                audio.play(0, 0, 4800, true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            CHECK(audio.playing()); CHECK(finite.load());
            callback.request_stop(); callback.join();
            auto held = std::make_shared<Timeline>(*doc.timeline());
            std::weak_ptr<const Timeline> released = held;
            audio.setTimeline(held); audio.play(0, 0, 4800, false); settle(); renderReady(audio, output);
            audio.pause(); held.reset(); audio.setTimeline(doc.timeline());
            for (int i = 0; i < 100 && !released.expired(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            CHECK(released.expired()); // A paused worker must release replaced audio.
        }
        doc = Document{}; source.reset(); std::filesystem::remove_all(root);
        std::cout << "PASS: " << checks << " audio checks (seek, pause, loop, EOF, mono, resampling, snapshots).\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << "\nFixtures: " << root << '\n'; return 1; }
}
