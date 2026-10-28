#include "stretch.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace coldedit {
namespace {
void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Time-stretch cancelled.");
}
void checkBpm(double bpm) {
    if (!std::isfinite(bpm) || bpm < 20 || bpm > 400)
        throw std::runtime_error("BPM must be between 20 and 400.");
}
}
StretchMap StretchMap::identity(Frame frames, unsigned rate, double tempo) {
    checkBpm(tempo);
    if (frames < 2 || !rate) throw std::runtime_error("Open some audio before changing timing.");
    StretchMap map; map.sourceFrames = frames; map.sampleRate = rate;
    map.bpm = map.sourceBpm = tempo; return map;
}
bool StretchMap::identity() const {
    if (bpm != sourceBpm) return false;
    for (const auto& a : anchors) if (std::abs(outputAt(a.sourceFrame) - a.sourceFrame) >= 1e-7) return false;
    return true;
}

double StretchMap::framesPerBeat() const { return 60. * sampleRate / bpm; }
double StretchMap::startBeat() const {
    return anchors.empty() ? 0 : anchors.front().beat - anchors.front().sourceFrame * sourceBpm / (60. * sampleRate);
}
Frame StretchMap::frames() const {
    const double length = outputAt(double(sourceFrames));
    if (!std::isfinite(length) || length < 1 || length >= double(std::numeric_limits<Frame>::max()) / 2)
        throw std::runtime_error("The stretched audio is too long or too short.");
    return Frame(std::llround(length));
}
double StretchMap::sourceAt(double outputFrame) const {
    const double beat = outputFrame / framesPerBeat() + startBeat();
    const double sourceFpb = 60. * sampleRate / sourceBpm;
    if (anchors.empty()) return std::clamp(beat * sourceFpb, 0., double(sourceFrames));
    if (beat <= anchors.front().beat) return std::clamp(anchors.front().sourceFrame + (beat - anchors.front().beat) * sourceFpb, 0., double(sourceFrames));
    auto it = std::upper_bound(anchors.begin(), anchors.end(), beat,
        [](double b, const StretchAnchor& a) { return b < a.beat; });
    if (it == anchors.end()) return std::clamp(anchors.back().sourceFrame + (beat - anchors.back().beat) * sourceFpb, 0., double(sourceFrames));
    const auto& a = *(it - 1); const auto& b = *it;
    return std::lerp(a.sourceFrame, b.sourceFrame, (beat - a.beat) / (b.beat - a.beat));
}
double StretchMap::outputAt(double sourceFrame) const {
    sourceFrame = std::clamp(sourceFrame, 0., double(sourceFrames));
    const double sourceFpb = 60. * sampleRate / sourceBpm;
    if (anchors.empty()) return sourceFrame * sourceBpm / bpm;
    if (sourceFrame <= anchors.front().sourceFrame) return sourceFrame * sourceBpm / bpm;
    auto it = std::upper_bound(anchors.begin(), anchors.end(), sourceFrame,
        [](double f, const StretchAnchor& a) { return f < a.sourceFrame; });
    if (it == anchors.end()) return (anchors.back().beat + (sourceFrame - anchors.back().sourceFrame) / sourceFpb - startBeat()) * framesPerBeat();
    const auto& a = *(it - 1); const auto& b = *it;
    return (std::lerp(a.beat, b.beat, (sourceFrame - a.sourceFrame) / (b.sourceFrame - a.sourceFrame)) - startBeat()) * framesPerBeat();
}
bool StretchMap::add(double outputFrame) {
    if (!std::isfinite(outputFrame) || outputFrame < 0 || outputFrame > frames() || anchors.size() >= maxAnchors) return false;
    const double beat = outputFrame / framesPerBeat() + startBeat(), sourceFrame = sourceAt(outputFrame);
    auto it = std::lower_bound(anchors.begin(), anchors.end(), beat,
        [](const StretchAnchor& a, double b) { return a.beat < b; });
    if ((it != anchors.end() && (it->sourceFrame - sourceFrame < 1 || (it->beat - beat) * framesPerBeat() < 1)) ||
        (it != anchors.begin() && (sourceFrame - (it - 1)->sourceFrame < 1 || (beat - (it - 1)->beat) * framesPerBeat() < 1))) return false;
    anchors.insert(it, {sourceFrame, beat}); return true;
}
bool StretchMap::move(std::size_t i, double beat) {
    if (!std::isfinite(beat) || anchors.size() < 2 || i >= anchors.size()) return false;
    // Keep both adjacent segments ordered and between 1/8x and 8x duration.
    double lower = -std::numeric_limits<double>::infinity(), upper = -lower;
    if (i) {
        const auto& a = anchors[i - 1];
        const double left = (anchors[i].sourceFrame - a.sourceFrame) / framesPerBeat();
        lower = a.beat + std::max(left / 8, 1 / framesPerBeat()); upper = a.beat + left * 8;
    }
    if (i + 1 < anchors.size()) {
        const auto& b = anchors[i + 1];
        const double right = (b.sourceFrame - anchors[i].sourceFrame) / framesPerBeat();
        lower = std::max(lower, b.beat - right * 8); upper = std::min(upper, b.beat - std::max(right / 8, 1 / framesPerBeat()));
    }
    if (lower > upper) return false;
    beat = std::clamp(beat, lower, upper);
    if (std::abs(beat - anchors[i].beat) < 1e-10) return false;
    anchors[i].beat = beat; return true;
}
void StretchMap::validate() const {
    checkBpm(bpm); checkBpm(sourceBpm);
    if (sourceFrames < 2 || sampleRate == 0 || anchors.size() > maxAnchors)
        throw std::runtime_error("Invalid stretch source or anchors.");
    for (const auto& a : anchors) if (!std::isfinite(a.sourceFrame) || !std::isfinite(a.beat) || a.sourceFrame < 0 || a.sourceFrame > sourceFrames)
        throw std::runtime_error("Invalid stretch anchor.");
    for (std::size_t i = 1; i < anchors.size(); ++i) {
        const auto& a = anchors[i - 1]; const auto& b = anchors[i];
        const double input = b.sourceFrame - a.sourceFrame, output = (b.beat - a.beat) * framesPerBeat();
        if (!std::isfinite(b.sourceFrame) || !std::isfinite(b.beat) || input < 1 || output < .999999 ||
            output / input < .125 - 1e-9 || output / input > 8 + 1e-9)
            throw std::runtime_error("Keep each stretch segment between 1/8 and 8 times its original length.");
    }
    (void)frames();
}

RhythmAnalysis analyzeRhythm(const Timeline& source, std::atomic<float>* progress, std::stop_token stop) {
    RhythmAnalysis result;
    if (source.frames < 2 || !source.sampleRate) return result;
    // Linked-channel energy avoids cancelling opposite-polarity stereo attacks.
    // Bound the analysis envelope even for hours-long native recordings.
    const Frame hop = std::max<Frame>(std::max(1u, source.sampleRate / 200), (source.frames + 499999) / 500000);
    const auto bins = std::size_t((source.frames - 1) / hop + 1);
    std::vector<double> novelty(bins);
    double previous = 0, peak = 0;
    for (std::size_t i = 0; i < bins; ++i) {
        checkStop(stop); double energy = 0;
        const Frame at = Frame(i) * hop, end = std::min(source.frames, at + hop);
        for (Frame f = at; f < end; ++f) {
            if ((f & 4095) == 0) checkStop(stop);
            for (unsigned c = 0; c < source.channels; ++c) { const double v = source.sample(f, c); energy += v * v; }
        }
        const double rms = std::sqrt(energy / double((end - at) * source.channels));
        novelty[i] = std::max(0., rms - previous); previous = rms; peak = std::max(peak, novelty[i]);
        if (progress) progress->store(float(double(i + 1) / bins) * .8f);
    }
    if (peak < 1e-6) { if (progress) progress->store(1); return result; }
    const double binRate = double(source.sampleRate) / hop;
    const auto radius = std::max<std::size_t>(1, std::size_t(binRate * .08));
    std::vector<std::pair<double, Frame>> candidates;
    for (std::size_t i = 0; i < bins; ++i) {
        if (novelty[i] < peak * .08) continue;
        const auto a = i > radius ? i - radius : 0, b = std::min(bins, i + radius + 1);
        bool maximum = true; double mean = 0;
        for (auto j = a; j < b; ++j) {
            mean += novelty[j];
            if (novelty[j] > novelty[i] || (j < i && novelty[j] == novelty[i])) maximum = false;
        }
        if (maximum && novelty[i] > mean / double(b - a) * 1.5) candidates.push_back({novelty[i], Frame(i) * hop});
    }
    if (candidates.size() > StretchMap::maxAnchors) {
        result.limited = true;
        std::partial_sort(candidates.begin(), candidates.begin() + StretchMap::maxAnchors, candidates.end(), std::greater<>());
        candidates.resize(StretchMap::maxAnchors);
    }
    for (const auto& [strength, frame] : candidates) { (void)strength; result.transients.push_back(frame); }
    std::sort(result.transients.begin(), result.transients.end());
    // Normalised onset autocorrelation, using at most the first three minutes.
    // Prefer a plausible metrical octave (60-200 BPM); report an estimate only
    // when several attacks provide evidence of a repeating pulse.
    const auto n = std::min(bins, std::size_t(binRate * 180));
    double best = 0; std::size_t bestLag = 0;
    const auto minLag = std::max<std::size_t>(1, std::size_t(binRate * 60 / 200));
    const auto maxLag = std::min(n / 3, std::size_t(std::ceil(binRate)));
    for (auto lag = minLag; lag <= maxLag; ++lag) {
        checkStop(stop); double dot = 0, aa = 0, bb = 0;
        for (std::size_t i = lag; i < n; ++i) { dot += novelty[i] * novelty[i - lag]; aa += novelty[i] * novelty[i]; bb += novelty[i - lag] * novelty[i - lag]; }
        const double correlation = aa > 0 && bb > 0 ? dot / std::sqrt(aa * bb) : 0;
        const double bpm = 60 * binRate / lag;
        const double score = correlation * (1 - .08 * std::abs(std::log2(bpm / 120.)));
        if (score > best) { best = score; bestLag = lag; result.confidence = correlation; }
        if (progress) progress->store(.8f + .2f * float(double(lag - minLag + 1) / (maxLag - minLag + 1)));
    }
    if (bestLag && result.transients.size() >= 4 && result.confidence >= .2) result.bpm = 60 * binRate / bestLag;
    else result.confidence = 0;
    checkStop(stop); if (progress) progress->store(1); return result;
}
}
