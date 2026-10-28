#include "pitch_time.hpp"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace coldedit {
namespace {
void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Pitch/time processing cancelled.");
}
}
void PitchSettings::validate(unsigned sampleRate) const {
    if (!std::isfinite(semitones) || semitones < -24 || semitones > 24)
        throw std::runtime_error("Transpose must be between -24 and +24 semitones.");
    if (!std::isfinite(formantSemitones) || formantSemitones < -12 || formantSemitones > 12)
        throw std::runtime_error("Formant shift must be between -12 and +12 semitones.");
    if (!std::isfinite(formantBaseHz) || formantBaseHz < 0 || formantBaseHz > 2000 || formantBaseHz >= sampleRate * .5)
        throw std::runtime_error("Formant base must be 0 (automatic) or up to 2000 Hz, below half the sample rate.");
}
PitchTimeState PitchTimeState::identity(std::shared_ptr<const Timeline> input, double bpm) {
    if (!input) throw std::runtime_error("Open some audio before changing pitch or timing.");
    PitchTimeState state;
    state.time = StretchMap::identity(input->frames, input->sampleRate, bpm);
    auto base = std::make_shared<Timeline>(*input); base->pitchTime.reset();
    state.source = std::move(base); return state;
}
std::size_t PitchTimeState::noteCount() const {
    std::size_t count = notes.size(); for (const auto& voice : harmonies) count += voice.notes.size(); return count;
}
const std::vector<PitchNote>& PitchTimeState::voiceNotes(unsigned voice) const { return voice ? harmonies.at(voice - 1).notes : notes; }
std::vector<PitchNote>& PitchTimeState::voiceNotes(unsigned voice) { return voice ? harmonies.at(voice - 1).notes : notes; }
std::pair<unsigned, std::size_t> PitchTimeState::locateNote(std::size_t index) const {
    for (unsigned voice = 0; voice <= harmonies.size(); ++voice) {
        if (index < voiceNotes(voice).size()) return {voice, index};
        index -= voiceNotes(voice).size();
    }
    throw std::out_of_range("Pitch note no longer exists.");
}
const PitchNote& PitchTimeState::noteAt(std::size_t index) const { const auto [voice, i] = locateNote(index); return voiceNotes(voice)[i]; }
PitchNote& PitchTimeState::noteAt(std::size_t index) { const auto [voice, i] = locateNote(index); return voiceNotes(voice)[i]; }
PitchTimeState::NoteKey PitchTimeState::noteKey(std::size_t index) const { const auto [voice, i] = locateNote(index); return {voice, voiceNotes(voice)[i].first}; }
unsigned PitchTimeState::addHarmony(const std::vector<std::size_t>& indices, double semitones) {
    if (!analysis || !std::isfinite(semitones) || std::abs(semitones) > 24) throw std::runtime_error("Choose a harmony interval from -24 to +24 semitones.");
    unsigned destination = 1;
    while (destination <= harmonies.size() && !voiceNotes(destination).empty()) ++destination;
    std::vector<std::size_t> selection = indices;
    if (selection.empty()) for (std::size_t i = 0; i < notes.size(); ++i) selection.push_back(i);
    std::sort(selection.begin(), selection.end()); selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    if (selection.empty()) throw std::runtime_error("Detect or select notes first.");
    if (noteCount() + selection.size() > PitchAnalysis::maxNotes) throw std::runtime_error("Maximum 8192 notes, including harmony voices.");
    const unsigned sourceVoice = locateNote(selection.front()).first;
    std::vector<PitchNote> copies; copies.reserve(selection.size());
    for (auto index : selection) {
        if (locateNote(index).first != sourceVoice) throw std::runtime_error("Select notes from one voice to duplicate.");
        auto note = noteAt(index); note.shift += semitones;
        if (std::abs(note.shift) > 24) throw std::runtime_error("This interval would exceed the +/-24 semitone note range.");
        copies.push_back(note);
    }
    HarmonyVoice added{std::move(copies), sourceVoice ? harmonies[sourceVoice - 1].gain : .35};
    if (destination > harmonies.size()) harmonies.push_back(std::move(added));
    else harmonies[destination - 1] = std::move(added);
    return destination;
}
void PitchTimeState::validate() const {
    if (!source || source->pitchTime || source->channels < 1 || source->channels > 2 ||
        source->frames != time.sourceFrames || source->sampleRate != time.sampleRate)
        throw std::runtime_error("Invalid pitch/time source.");
    time.validate(); pitch.validate(source->sampleRate);
    if (harmonies.size() > PitchAnalysis::maxNotes || noteCount() > PitchAnalysis::maxNotes || (!analysis && noteCount() != 0) ||
        (analysis && (analysis->sourceFrames != source->frames || analysis->sampleRate != source->sampleRate ||
            analysis->contour.size() > PitchAnalysis::maxFrames)))
        throw std::runtime_error("Invalid pitch analysis.");
    for (unsigned voice = 0; voice <= harmonies.size(); ++voice) {
        if (voice && (!std::isfinite(harmonies[voice - 1].gain) || harmonies[voice - 1].gain < 0 || harmonies[voice - 1].gain > 1))
          throw std::runtime_error("Harmony level must be between 0% and 100%.");
        Frame end = 0;
        for (const auto& note : voiceNotes(voice)) {
          if (note.first < end || note.last <= note.first || note.last > source->frames ||
              !std::isfinite(note.midi) || note.midi < 0 || note.midi > 127 ||
              !std::isfinite(note.shift) || std::abs(note.shift) > 24 ||
              !std::isfinite(note.confidence) || note.confidence < 0 || note.confidence > 1 ||
              !std::isfinite(note.inferredFraction) || note.inferredFraction < 0 || note.inferredFraction > 1 ||
              !std::isfinite(note.vibrato) || note.vibrato < 0 || note.vibrato > 2 ||
              ((note.fadeFirst != -1 || note.fadeLast != -1) &&
                  (note.fadeFirst < 0 || note.fadeFirst > note.first || note.fadeLast < note.last || note.fadeLast > source->frames)))
              throw std::runtime_error("Invalid pitch note or correction (maximum +/-24 semitones per note).");
          end = note.last;
        }
    }
}
bool PitchTimeState::hasNoteEdits() const {
    for (const auto& voice : harmonies) if (!voice.notes.empty()) return true;
    return std::any_of(notes.begin(), notes.end(), [](const auto& note) { return note.shift != 0 || note.vibrato != 1; });
}
double PitchTimeState::noteShiftAt(double sourceFrame, unsigned voice) const {
    const auto& voiceNotesList = voiceNotes(voice);
    auto it = std::upper_bound(voiceNotesList.begin(), voiceNotesList.end(), sourceFrame,
        [](double frame, const PitchNote& note) { return frame < note.first; });
    if (it == voiceNotesList.begin()) return 0;
    --it; if (sourceFrame >= it->last) return 0;
    const auto correction = [&](const PitchNote& note) {
        double variation = 0;
        if (note.vibrato != 1 && analysis && !analysis->contour.empty()) {
            const auto& curve = analysis->contour;
            auto next = std::lower_bound(curve.begin(), curve.end(), sourceFrame,
                [](const PitchFrame& f, double frame) { return f.sourceFrame < frame; });
            const auto deviation = [&](const PitchFrame& f) {
                const Frame first = note.fadeFirst < 0 ? note.first : note.fadeFirst, last = note.fadeLast < 0 ? note.last : note.fadeLast;
                return f.confidence && !f.inferred && f.sourceFrame >= first && f.sourceFrame < last ? f.midi - note.midi : 0.;
            };
            if (next == curve.begin()) variation = deviation(*next);
            else if (next == curve.end()) variation = deviation(curve.back());
            else {
                const auto& before = *(next - 1);
                const double mix = (sourceFrame - before.sourceFrame) / (next->sourceFrame - before.sourceFrame);
                variation = deviation(before) + (deviation(*next) - deviation(before)) * mix;
            }
        }
        return std::clamp(note.shift + (note.vibrato - 1) * variation, -24., 24.) * note.envelopeAt(sourceFrame, source->sampleRate);
    };
    const auto sharedEnvelope = [](const PitchNote& a, const PitchNote& b) {
        return a.fadeFirst >= 0 && a.fadeFirst == b.fadeFirst && a.fadeLast == b.fadeLast && a.last == b.first;
    };
    const auto blend = [&](const PitchNote& a, const PitchNote& b) {
        const double radius = std::min({source->sampleRate * .005, (a.last - a.first) * .25, (b.last - b.first) * .25});
        const double mix = std::clamp((sourceFrame - b.first + radius) / (2 * radius), 0., 1.);
        const double first = correction(a);
        return first + (correction(b) - first) * mix * mix * (3 - 2 * mix);
    };
    // Equal split siblings are unchanged; later edits crossfade at the seam.
    if (it != voiceNotesList.begin() && sharedEnvelope(*(it - 1), *it) && sourceFrame < it->first + source->sampleRate * .005)
        return blend(*(it - 1), *it);
    if (it + 1 != voiceNotesList.end() && sharedEnvelope(*it, *(it + 1)) && sourceFrame > it->last - source->sampleRate * .005)
        return blend(*it, *(it + 1));
    return correction(*it);
}
bool PitchTimeState::splitNote(std::size_t index, Frame at) {
    if (!analysis || index >= noteCount() || noteCount() >= PitchAnalysis::maxNotes) return false;
    const auto location = locateNote(index); auto& voiceNotesList = voiceNotes(location.first); index = location.second;
    const auto original = voiceNotesList[index];
    const Frame minimum = std::max<Frame>(1, source->sampleRate / 50);
    if (at <= original.first || at >= original.last || at - original.first < minimum || original.last - at < minimum) return false;
    auto left = original, right = original;
    left.fadeFirst = right.fadeFirst = original.fadeFirst < 0 ? original.first : original.fadeFirst;
    left.fadeLast = right.fadeLast = original.fadeLast < 0 ? original.last : original.fadeLast;
    left.last = right.first = at;
    const auto quality = [&](PitchNote& note) {
        double confidence = 0; std::size_t count = 0, inferred = 0;
        auto f = std::lower_bound(analysis->contour.begin(), analysis->contour.end(), note.first,
            [](const PitchFrame& point, Frame frame) { return point.sourceFrame < frame; });
        for (; f != analysis->contour.end() && f->sourceFrame < note.last; ++f) {
            ++count; confidence += f->confidence; inferred += f->inferred;
        }
        if (count) { note.confidence = float(confidence / count); note.inferredFraction = float(inferred) / float(count); }
    };
    quality(left); quality(right);
    voiceNotesList[index] = left; voiceNotesList.insert(voiceNotesList.begin() + index + 1, right); return true;
}

std::shared_ptr<Timeline> renderPitchTime(const PitchTimeState& state, std::atomic<float>* progress, std::stop_token stop) {
    state.validate(); checkStop(stop); resolvePitchTime(*state.source, progress, stop);
    const auto& source = *state.source; const Frame total = state.time.frames();
    auto result = std::make_shared<Timeline>(); result->sampleRate = source.sampleRate; result->channels = source.channels;
    if (state.identity()) {
        result->pieces = source.pieces; result->rebuild();
    } else {
        // Continuous processors share the timing map and retain phase across anchors.
        // Feed source samples ahead by inputLatency, discard outputLatency of
        // pre-roll, and continue with zero input to retain the end of the file.
        using Processor = signalsmith::stretch::SignalsmithStretch<float>;
        const auto voices = 1 + state.harmonies.size();
        std::vector<std::unique_ptr<Processor>> processors(voices);
        std::vector<std::vector<std::pair<Frame, Frame>>> regions(voices);
        const bool dryLead = state.time.identity() && state.pitch.identity() &&
            std::none_of(state.notes.begin(), state.notes.end(), [](const auto& n) { return n.shift != 0 || n.vibrato != 1; });
        for (unsigned voice = 0; voice < processors.size(); ++voice) {
            checkStop(stop);
            if (voice && (state.voiceNotes(voice).empty() || state.harmonies[voice - 1].gain == 0)) continue;
            auto& processor = processors[voice]; processor = std::make_unique<Processor>(0);
            processor->presetDefault(int(source.channels), float(source.sampleRate));
            processor->setTransposeSemitones(float(state.pitch.semitones));
            processor->setFormantSemitones(float(state.pitch.formantSemitones), state.pitch.preserveFormants);
            processor->setFormantBase(float(state.pitch.formantBaseHz / source.sampleRate));
            // Adjacent notes share an amplitude envelope, including split siblings.
            if (voice) for (const auto& note : state.voiceNotes(voice)) {
                auto& spans = regions[voice];
                if (!spans.empty() && spans.back().second == note.first) spans.back().second = note.last;
                else spans.emplace_back(note.first, note.last);
            }
        }
        const auto& stretch = *processors[0];
        constexpr int blockSize = 256;
        std::array<std::vector<float>, 2> input;
        std::vector<std::array<std::vector<float>, 2>> outputs(voices);
        std::vector<double> gain(voices);
        for (unsigned c = 0; c < source.channels; ++c) {
            input[c].resize(std::max(4096, stretch.inputLatency()));
            for (auto& output : outputs) output[c].resize(blockSize);
            for (int i = 0; i < stretch.inputLatency(); ++i) input[c][i] = source.sample(i, c);
        }
        std::vector<double> boundaries{0};
        for (const auto& a : state.time.anchors) if (a.sourceFrame > 0 && a.sourceFrame < source.frames) boundaries.push_back(state.time.outputAt(a.sourceFrame));
        const double mappedEnd = state.time.outputAt(double(source.frames));
        boundaries.push_back(mappedEnd);
        const double firstRate = state.time.sourceAt(boundaries[1]) / boundaries[1];
        for (auto& processor : processors) if (processor) processor->seek(input, stretch.inputLatency(), float(firstRate));
        Frame inputPosition = stretch.inputLatency(), processingPosition = 0;
        const double tailStart = boundaries[boundaries.size() - 2];
        const double endRate = (source.frames - state.time.sourceAt(tailStart)) / (mappedEnd - tailStart);
        std::size_t nextAnchor = 1;
        auto process = [&](int requested) {
            checkStop(stop);
            while (nextAnchor < boundaries.size() &&
                std::llround(boundaries[nextAnchor]) <= processingPosition) ++nextAnchor;
            int count = std::min(blockSize, requested);
            if (nextAnchor < boundaries.size()) count = int(std::min<Frame>(count,
                Frame(std::llround(boundaries[nextAnchor])) - processingPosition));
            const Frame end = processingPosition + count;
            const double sourceEnd = end <= total ? state.time.sourceAt(double(end)) : source.frames + (end - total) * endRate;
            const Frame inputEnd = Frame(std::llround(sourceEnd)) + stretch.inputLatency();
            const int inCount = int(std::max<Frame>(0, inputEnd - inputPosition));
            for (unsigned c = 0; c < source.channels; ++c) {
                input[c].resize(std::size_t(inCount));
                for (int i = 0; i < inCount; ++i) input[c][i] = source.sample(inputPosition + i, c);
            }
            // processingPosition is the processor's current time, ahead of the
            // samples it emits by outputLatency(). Source-coordinate notes
            // therefore stay aligned through time warps and compensated pre-roll.
            const double at = state.time.sourceAt(processingPosition + count * .5);
            for (unsigned voice = 0; voice < processors.size(); ++voice) if (processors[voice] && (voice || !dryLead)) {
                processors[voice]->setTransposeSemitones(float(state.pitch.semitones + state.noteShiftAt(at, voice)));
                processors[voice]->process(input, inCount, outputs[voice], count);
            }
            processingPosition = end; inputPosition = inputEnd;
            return count;
        };
        int remaining = stretch.outputLatency();
        while (remaining > 0) remaining -= process(remaining);
        auto rendered = AudioFile::render(total, source.sampleRate, source.channels,
            [&](Frame first, Frame count, float* data) {
                Frame written = 0;
                while (written < count) {
                    const int n = process(int(std::min<Frame>(blockSize, count - written)));
                    for (int i = 0; i < n; ++i) {
                        const Frame frame = first + written + i;
                        const double at = state.time.sourceAt(double(frame));
                        std::fill(gain.begin(), gain.end(), 0.);
                        for (unsigned voice = 1; voice < processors.size(); ++voice) if (processors[voice]) {
                            const auto& spans = regions[voice];
                            auto span = std::upper_bound(spans.begin(), spans.end(), at,
                                [](double f, const auto& region) { return f < region.first; });
                            if (span == spans.begin()) continue;
                            --span; if (at >= span->second) continue;
                            const double fade = std::min(source.sampleRate * .01, (span->second - span->first) * .25);
                            const double ramp = std::clamp(std::min(at - span->first, span->second - at) / fade, 0., 1.);
                            gain[voice] = state.harmonies[voice - 1].gain * ramp * ramp * (3 - 2 * ramp);
                        }
                        for (unsigned c = 0; c < source.channels; ++c) {
                            double value = dryLead ? source.sample(frame, c) : outputs[0][c][i];
                            for (unsigned voice = 1; voice < processors.size(); ++voice) if (gain[voice]) value += outputs[voice][c][i] * gain[voice];
                            data[std::size_t(written + i) * source.channels + c] = std::isfinite(value) ? float(value) : 0;
                        }
                    }
                    written += n;
                }
            }, progress, stop);
        result->pieces = {{rendered, 0, total}}; result->rebuild();
    }
    result->pitchTime = std::make_shared<PitchTimeState>(state);
    checkStop(stop); if (progress) progress->store(1); return result;
}

std::shared_ptr<Timeline> previewPitchTime(const PitchTimeState& state) {
    state.validate();
    auto result = std::make_shared<Timeline>();
    result->sampleRate = state.source->sampleRate; result->channels = state.source->channels;
    if (state.identity()) result->pieces = state.source->pieces;
    else result->pieces = {{AudioFile::pitchTimePreview(state), 0, state.time.frames()}};
    result->rebuild(); result->pitchTime = std::make_shared<PitchTimeState>(state);
    return result;
}
bool needsPitchTimeRender(const Timeline& timeline) {
    for (const auto& p : timeline.pieces) if (p.source && p.source->pitchTimeState() && !p.source->pitchTimeResult()) return true;
    return false;
}
void resolvePitchTime(const Timeline& timeline, std::atomic<float>* progress, std::stop_token stop) {
    for (const auto& p : timeline.pieces) {
        checkStop(stop);
        if (!p.source || p.source->pitchTimeResult()) continue;
        if (const auto map = p.source->pitchTimeState()) {
            auto rendered = renderPitchTime(*map, progress, stop);
            checkStop(stop);
            p.source->publishPitchTime(rendered->pieces.front().source);
        }
    }
}
PitchTimeRenderQueue::~PitchTimeRenderQueue() { suspend(); if (worker_.joinable()) worker_.join(); }
void PitchTimeRenderQueue::suspend() { if (worker_.joinable()) worker_.request_stop(); }
void PitchTimeRenderQueue::wait(std::stop_token stop) const {
    while (running_.load(std::memory_order_acquire)) { checkStop(stop); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    checkStop(stop);
}
void PitchTimeRenderQueue::update(std::shared_ptr<const Timeline> timeline, bool suspended) {
    if (failed_ && failed_ != timeline) { failed_.reset(); error_.clear(); }
    if (worker_.joinable() && (suspended || target_ != timeline)) worker_.request_stop();
    if (done_.exchange(false, std::memory_order_acquire)) {
        worker_.join();
        if (!workerError_.empty() && target_ == timeline) { failed_ = target_; error_ = std::move(workerError_); }
        target_.reset();
    }
    if (target_ || suspended || !timeline || !needsPitchTimeRender(*timeline) || failed_ == timeline) return;
    failed_.reset(); error_.clear(); workerError_.clear(); target_ = std::move(timeline);
    progress_.store(0); running_.store(true);
    worker_ = std::jthread([this, target = target_](std::stop_token stop) {
        try { resolvePitchTime(*target, &progress_, stop); }
        catch (const std::exception& e) { if (!stop.stop_requested()) workerError_ = e.what(); }
        running_.store(false, std::memory_order_release); done_.store(true, std::memory_order_release);
    });
}

}
