#include "pitch_analysis.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace coldedit {
namespace {
void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Pitch detection cancelled.");
}
// Analysis-only anti-alias filter. Two biquads make a fourth-order Butterworth
// low-pass before integer decimation; no downsampled audio enters the output.
struct LowPass {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    LowPass(double rate, double cutoff, double q) {
        const double w = 2 * std::numbers::pi * cutoff / rate, c = std::cos(w), a = std::sin(w) / (2 * q);
        b0 = (1 - c) / (2 * (1 + a)); b1 = 2 * b0; b2 = b0;
        a1 = -2 * c / (1 + a); a2 = (1 - a) / (1 + a);
    }
    double process(double x) {
        const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y;
    }
};
double median(std::vector<double> values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end()); return *middle;
}
}
double PitchNote::envelopeAt(double sourceFrame, unsigned sampleRate) const {
    const Frame begin = fadeFirst < 0 ? first : fadeFirst, end = fadeLast < 0 ? last : fadeLast;
    if (sourceFrame < begin || sourceFrame >= end) return 0;
    const double ramp = std::min(sampleRate * .01, (end - begin) * .25);
    const double blend = std::clamp(std::min(sourceFrame - begin, end - sourceFrame) / ramp, 0., 1.);
    return blend * blend * (3 - 2 * blend);
}
double PitchNote::shiftAt(double sourceFrame, unsigned sampleRate) const {
    return sourceFrame >= first && sourceFrame < last ? shift * envelopeAt(sourceFrame, sampleRate) : 0;
}
PitchAnalysis analyzePitch(const Timeline& source, std::atomic<float>* progress, std::stop_token stop) {
    checkStop(stop);
    if (!source.sampleRate || source.channels < 1 || source.channels > 2)
        throw std::runtime_error("Invalid audio for pitch detection.");
    PitchAnalysis result; result.sourceFrames = source.frames; result.sampleRate = source.sampleRate;
    const unsigned decimation = std::max(1u, source.sampleRate / 8000);
    const double rate = double(source.sampleRate) / decimation;
    const int hop = std::max(1, int(std::llround(rate * .01)));
    result.hop = Frame(hop) * decimation;
    if (source.frames < 2 || rate < 3000) return result;
    const Frame count = (source.frames - 1) / result.hop + 1;
    if (count > Frame(PitchAnalysis::maxFrames)) throw std::runtime_error("Pitch detection supports up to about 2.7 hours. Trim a shorter passage first.");
    result.contour.reserve(std::size_t(count));
    const int minLag = std::max(2, int(std::floor(rate / 1200))), maxLag = int(std::ceil(rate / 50));
    const int window = maxLag * 2, length = window + maxLag + 2, capacity = length + hop + 1;
    const double cutoff = std::min(2200., rate * .35);
    std::array<std::array<LowPass, 2>, 2> filters{{
        {LowPass(source.sampleRate, cutoff, .5411961001), LowPass(source.sampleRate, cutoff, 1.306562965)},
        {LowPass(source.sampleRate, cutoff, .5411961001), LowPass(source.sampleRate, cutoff, 1.306562965)}}};
    std::array<std::vector<double>, 2> ring;
    for (auto& channel : ring) channel.resize(capacity);
    std::vector<double> samples(length), difference(maxLag + 2);
    Frame produced = 0;
    for (Frame bin = 0; bin < count; ++bin) {
        checkStop(stop);
        const Frame centre = bin * hop, start = centre - length / 2, required = start + length;
        while (produced < required) {
            if ((produced & 1023) == 0) checkStop(stop);
            for (unsigned c = 0; c < source.channels; ++c) {
                double value = 0;
                for (unsigned j = 0; j < decimation; ++j)
                    value = filters[c][1].process(filters[c][0].process(source.sample(produced * decimation + j, c)));
                ring[c][std::size_t(produced % capacity)] = value;
            }
            ++produced;
        }
        // Choose the channel with more AC energy. L+R could cancel a valid
        // opposite-polarity stereo melody; channels still render together.
        unsigned channel = 0; double energy = 0;
        for (unsigned c = 0; c < source.channels; ++c) {
            double sum = 0, squares = 0;
            for (int j = 0; j < length; ++j) {
                const Frame at = start + j;
                const double v = at < 0 ? 0 : ring[c][std::size_t(at % capacity)];
                sum += v; squares += v * v;
            }
            const double ac = std::max(0., squares / length - std::pow(sum / length, 2));
            if (ac > energy) { energy = ac; channel = c; }
        }
        PitchFrame frame{bin * result.hop};
        // The period detector needs ~60 ms of audio. Its energy would smear
        // pitch across short silences, so measure activity in this 10 ms bin
        // separately, at full bandwidth (including breath/consonant energy).
        const Frame energyFirst = std::max<Frame>(0, frame.sourceFrame - result.hop / 2);
        const Frame energyLast = std::min(source.frames, frame.sourceFrame + result.hop / 2);
        for (unsigned c = 0; c < source.channels; ++c) {
            double sum = 0, squares = 0;
            for (Frame at = energyFirst; at < energyLast; ++at) {
                const double sample = source.sample(at, c); sum += sample; squares += sample * sample;
            }
            const double n = double(std::max<Frame>(1, energyLast - energyFirst));
            frame.rms = std::max(frame.rms, float(std::sqrt(std::max(0., squares / n - std::pow(sum / n, 2)))));
        }
        if (energy > 1e-8) {
            for (int j = 0; j < length; ++j) samples[j] = start + j < 0 ? 0 : ring[channel][std::size_t((start + j) % capacity)];
            // YIN's cumulative mean normalised difference, absolute threshold,
            // then parabolic period refinement (de Cheveigne & Kawahara, 2002).
            double running = 0; difference[0] = 1;
            for (int lag = 1; lag <= maxLag + 1; ++lag) {
                double sum = 0;
                for (int j = 0; j < window; ++j) { const double d = samples[j] - samples[j + lag]; sum += d * d; }
                running += sum; difference[lag] = running > 1e-20 ? sum * lag / running : 1;
            }
            int lag = minLag;
            while (lag <= maxLag && difference[lag] >= .15) ++lag;
            if (lag <= maxLag) {
                while (lag < maxLag && difference[lag + 1] < difference[lag]) ++lag;
                const double a = difference[lag - 1], b = difference[lag], c = difference[lag + 1];
                const double denominator = a - 2 * b + c;
                const double offset = denominator > 1e-12 ? std::clamp(.5 * (a - c) / denominator, -.5, .5) : 0;
                const double hz = rate / (lag + offset);
                if (hz >= 50 && hz <= 1200) {
                    frame.midi = float(69 + 12 * std::log2(hz / 440)); frame.confidence = float(1 - b);
                }
            }
        }
        result.contour.push_back(frame);
        if (progress) progress->store(float(double(bin + 1) / count) * .9f);
    }
    refinePitchAnalysis(result, stop);
    checkStop(stop); if (progress) progress->store(1); return result;
}
void refinePitchAnalysis(PitchAnalysis& result, std::stop_token stop) {
    checkStop(stop);
    auto& frames = result.contour; const auto count = frames.size();
    result.notes.clear();
    if (count == 0) return;
    if (result.hop <= 0 || result.sourceFrames <= 0 || !result.sampleRate || count > PitchAnalysis::maxFrames)
        throw std::runtime_error("Invalid pitch contour.");
    float peak = 0;
    for (const auto& f : frames) peak = std::max(peak, f.rms);
    const float silence = std::max(1e-5f, peak * .001f);
    for (auto& f : frames) {
        if (f.inferred) f.confidence = 0;
        f.inferred = false;
        if (f.rms <= silence) { f.confidence = 0; f.midi = 0; }
    }
    // A seven-frame robust neighbourhood removes brief octave/subharmonic
    // excursions, including multi-frame errors. Leave the good contour intact
    // so vibrato and sustained real register changes are preserved.
    std::vector<unsigned char> stable(count);
    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 1023) == 0) checkStop(stop);
        if (!frames[i].confidence) continue;
        std::array<double, 7> neighbours{}; std::size_t n = 0;
        neighbours[n++] = frames[i].midi;
        for (std::size_t j = i; j > 0 && i - j < 3;) {
            --j; if (frames[j].rms <= silence) break;
            if (frames[j].confidence) neighbours[n++] = frames[j].midi;
        }
        for (std::size_t j = i + 1; j < count && j <= i + 3; ++j) {
            if (frames[j].rms <= silence) break;
            if (frames[j].confidence) neighbours[n++] = frames[j].midi;
        }
        std::sort(neighbours.begin(), neighbours.begin() + n);
        stable[i] = n >= 3 && std::abs(frames[i].midi - neighbours[n / 2]) <= 2.5;
    }
    // Do not use a tiny isolated pitch fragment as the reference for a whole
    // uncertain passage. At least ~60 ms must support each reference run.
    const std::size_t minimum = std::max<std::size_t>(3, std::size_t(std::ceil(.06 * result.sampleRate / result.hop)));
    for (std::size_t first = 0; first < count;) {
        checkStop(stop);
        if (!stable[first]) { ++first; continue; }
        std::size_t end = first + 1;
        while (end < count && stable[end] && std::abs(frames[end].midi - frames[end - 1].midi) < 2.5) ++end;
        if (end - first < minimum) std::fill(stable.begin() + first, stable.begin() + end, 0);
        first = end;
    }
    // Fill from the original stable references only, never from earlier guesses.
    // Similar neighbours interpolate; different notes switch at the midpoint
    // instead of inventing a long slide. References can cross silence, but the
    // silent frames themselves remain empty (including isolated audible islands).
    std::size_t left = count, right = 0;
    while (right < count && !stable[right]) ++right;
    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 1023) == 0) checkStop(stop);
        auto& f = frames[i];
        if (stable[i]) {
            left = i; right = i + 1; while (right < count && !stable[right]) ++right;
            continue;
        }
        f.confidence = 0; f.midi = 0;
        if (f.rms <= silence || (left == count && right == count)) continue;
        const auto reference = left == count ? right : right == count ? left : i - left <= right - i ? left : right;
        f.midi = frames[reference].midi;
        if (left != count && right != count && std::abs(frames[left].midi - frames[right].midi) < 1)
            f.midi = float(frames[left].midi + (frames[right].midi - frames[left].midi) * double(i - left) / double(right - left));
        f.confidence = .25f * frames[reference].confidence; f.inferred = true;
    }
    // Stable note regions; require a sustained change, so ordinary vibrato
    // does not turn into dozens of notes. Silent gaps remain uncorrected.
    std::size_t first = 0;
    const auto append = [&](std::size_t begin, std::size_t end) {
        if (end - begin < minimum) return;
        std::vector<double> pitches; float confidence = 0; std::size_t inferred = 0;
        for (auto i = begin; i < end; ++i) {
            const auto& f = frames[i]; confidence += f.confidence;
            if (f.inferred) ++inferred; else pitches.push_back(f.midi);
        }
        if (pitches.empty()) for (auto i = begin; i < end; ++i) pitches.push_back(frames[i].midi);
        if (result.notes.size() >= PitchAnalysis::maxNotes) throw std::runtime_error("Too many detected notes. Trim a shorter passage first.");
        result.notes.push_back({std::max<Frame>(0, result.contour[begin].sourceFrame - result.hop / 2),
            std::min(result.sourceFrames, result.contour[end - 1].sourceFrame + result.hop / 2), median(std::move(pitches)), 0,
            confidence / float(end - begin), float(inferred) / float(end - begin)});
    };
    while (first < result.contour.size()) {
        checkStop(stop);
        if (!result.contour[first].confidence) { ++first; continue; }
        std::size_t end = first + 1;
        double centre = result.contour[first].midi;
        for (; end < result.contour.size() && result.contour[end].confidence; ++end) {
            if (end + 2 < result.contour.size() && result.contour[end + 1].confidence && result.contour[end + 2].confidence) {
                const double next = median({result.contour[end].midi, result.contour[end + 1].midi, result.contour[end + 2].midi});
                if (std::abs(next - centre) > .75 && std::abs(result.contour[end + 2].midi - result.contour[end].midi) < .5) break;
            }
            centre += (result.contour[end].midi - centre) * .08;
        }
        append(first, end); first = end;
    }
    checkStop(stop);
}
}
