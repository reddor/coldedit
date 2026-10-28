#include "processing.hpp"
#include "pitch_time.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <limits>

namespace coldedit {
std::shared_ptr<Timeline> mixAudio(const Timeline& source, const Timeline& clipboard,
    Frame first, Frame last, std::atomic<float>* progress, std::stop_token stop) {
    if (source.sampleRate != clipboard.sampleRate || source.channels != clipboard.channels)
        throw std::runtime_error("Clipboard audio must have the same sample rate and channel count.");
    if (first < 0 || last <= first || clipboard.frames <= 0)
        throw std::runtime_error("Select a range and copy some audio before pasting into it.");
    const Frame length = std::min(last - first, clipboard.frames), end = first + length;
    resolvePitchTime(source, progress, stop); resolvePitchTime(clipboard, progress, stop);
    auto rendered = AudioFile::render(length, source.sampleRate, source.channels,
        [&](Frame at, Frame count, float* block) {
            for (Frame i = 0; i < count; ++i) for (unsigned c = 0; c < source.channels; ++c) {
                const double value = double(source.sample(first + at + i, c)) + clipboard.sample(at + i, c);
                constexpr double limit = std::numeric_limits<float>::max();
                block[std::size_t(i) * source.channels + c] = float(std::clamp(value, -limit, limit));
            }
        }, progress, stop);
    auto result = std::make_shared<Timeline>(); result->sampleRate = source.sampleRate; result->channels = source.channels;
    result->pieces = source.slice(0, first);
    if (first > source.frames) result->pieces.push_back({nullptr, 0, first - source.frames});
    result->pieces.push_back({rendered, 0, length});
    auto tail = source.slice(end, source.frames); result->pieces.insert(result->pieces.end(), tail.begin(), tail.end());
    result->rebuild(); return result;
}
const char* cleanupName(Cleanup kind) {
    switch (kind) {
    case Cleanup::Normalize: return "Normalize";
    case Cleanup::RemoveDC: return "Remove DC offset";
    case Cleanup::HighPass: return "High-pass filter";
    case Cleanup::LowPass: return "Low-pass filter";
    }
    return "Cleanup";
}
std::shared_ptr<Timeline> processAudio(const Timeline& source, Frame first, Frame last,
    CleanupSettings settings, std::atomic<float>* progress, std::stop_token stop) {
    first = std::clamp(first, Frame(0), source.frames); last = std::clamp(last, first, source.frames);
    if (first == last) throw std::runtime_error("Select some audio to process.");
    auto cancelled = [&] { if (stop.stop_requested()) throw std::runtime_error("Processing cancelled."); };
    cancelled();
    resolvePitchTime(source, progress, stop);
    auto result = std::make_shared<Timeline>(source);
    result->pitchTime.reset();
    auto head = source.slice(0, first), tail = source.slice(last, source.frames);
    std::array<double, 2> mean{}, compensation{};
    if (settings.kind == Cleanup::Normalize || settings.kind == Cleanup::RemoveDC) {
        double peak = 0;
        for (Frame f = first; f < last; ++f) {
            if ((f & 4095) == 0) { cancelled(); if (progress) progress->store(float(double(f - first) / double(last - first)) * .5f); }
            for (unsigned c = 0; c < source.channels; ++c) {
                const double value = source.sample(f, c); peak = std::max(peak, std::abs(value));
                const double y = value - compensation[c], sum = mean[c] + y;
                compensation[c] = (sum - mean[c]) - y; mean[c] = sum;
            }
        }
        if (settings.kind == Cleanup::Normalize) {
            if (!std::isfinite(settings.value) || settings.value < -60 || settings.value > 0) throw std::runtime_error("Normalization target must be between -60 and 0 dBFS.");
            const double gain = peak > 0 ? std::pow(10., settings.value / 20) / peak : 1;
            auto middle = source.slice(first, last); for (auto& p : middle) p.gain = std::clamp(p.gain * gain, -1e100, 1e100);
            head.insert(head.end(), middle.begin(), middle.end());
            head.insert(head.end(), tail.begin(), tail.end()); result->pieces = std::move(head); result->rebuild();
            cancelled(); if (progress) progress->store(1); return result;
        }
        for (auto& value : mean) value /= double(last - first);
    }
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    if (settings.kind == Cleanup::HighPass || settings.kind == Cleanup::LowPass) {
        if (!std::isfinite(settings.value) || settings.value < 1 || settings.value >= source.sampleRate * .5)
            throw std::runtime_error("Filter cutoff must be positive and below half the sample rate.");
        // RBJ/W3C Audio EQ Cookbook, Butterworth Q=1/sqrt(2), 12 dB/octave.
        // https://www.w3.org/TR/audio-eq-cookbook/
        const double w = 2 * std::numbers::pi * settings.value / source.sampleRate;
        const double cosine = std::cos(w), alpha = std::sin(w) / std::sqrt(2.), a0 = 1 + alpha;
        const bool high = settings.kind == Cleanup::HighPass;
        b0 = (1 + (high ? cosine : -cosine)) / (2 * a0); b2 = b0; b1 = (high ? -2 : 2) * b0;
        a1 = -2 * cosine / a0; a2 = (1 - alpha) / a0;
    }
    std::array<double, 2> z1{}, z2{};
    // Initialize at the first sample's DC equilibrium to avoid an artificial startup impulse.
    for (unsigned c = 0; c < source.channels; ++c) {
        const double x = source.sample(first, c), y = settings.kind == Cleanup::HighPass ? 0 : x;
        z1[c] = y - b0 * x; z2[c] = b2 * x - a2 * y;
    }
    auto rendered = AudioFile::render(last - first, source.sampleRate, source.channels,
        [&](Frame at, Frame count, float* block) {
            for (Frame i = 0; i < count; ++i) for (unsigned c = 0; c < source.channels; ++c) {
                const double x = source.sample(first + at + i, c);
                double y;
                if (settings.kind == Cleanup::RemoveDC) y = x - mean[c];
                else { y = b0 * x + z1[c]; z1[c] = b1 * x - a1 * y + z2[c]; z2[c] = b2 * x - a2 * y; }
                constexpr double limit = std::numeric_limits<float>::max();
                block[std::size_t(i) * source.channels + c] = float(std::clamp(y, -limit, limit));
            }
        }, progress, stop);
    head.push_back({rendered, 0, rendered->frames}); head.insert(head.end(), tail.begin(), tail.end());
    result->pieces = std::move(head); result->rebuild(); cancelled(); return result;
}
}
