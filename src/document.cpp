#include "document.hpp"
#include "codecs.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <cstring>
#include <limits>
#ifdef __EMSCRIPTEN__
#include "web_limits.hpp"
#endif
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace coldedit {
double FadeEnvelope::value(Frame local) const noexcept {
    // Both one-sample fades end in silence. Integer phase survives repeated slicing.
    if (length <= 1) return 0;
    const Frame phase = position + (reversed ? -local : local);
    double t = double(std::clamp(phase, Frame(0), length - 1)) / double(length - 1);
    if (out) t = 1 - t;
    return curve == FadeCurve::Smooth ? t * t * (3 - 2 * t) : t;
}
void Timeline::rebuild() {
    std::vector<Piece> compact;
    Frame total = 0;
    for (const auto& p : pieces) {
        if (p.length <= 0) continue;
        if (p.length > std::numeric_limits<Frame>::max() - total)
            throw std::runtime_error("The edited audio is too long.");
        total += p.length;
        if (!compact.empty()) {
            auto& back = compact.back();
            if (p.fades.empty() && back.fades.empty() && p.mix == back.mix && p.source == back.source && p.gain == back.gain && p.reversed == back.reversed &&
                ((!p.source) || (!p.reversed && back.offset + back.length == p.offset) ||
                 (p.reversed && p.offset + p.length == back.offset))) {
                if (p.reversed) back.offset = p.offset;
                back.length += p.length;
                continue;
            }
        }
        compact.push_back(p);
    }
    pieces = std::move(compact);
    ends.clear(); frames = 0;
    for (const auto& p : pieces) { frames += p.length; ends.push_back(frames); }
}
float Timeline::sample(Frame f, unsigned c) const noexcept {
    if (f < 0 || f >= frames || c >= channels) return 0;
    const auto it = std::upper_bound(ends.begin(), ends.end(), f);
    const auto i = std::size_t(it - ends.begin());
    const auto& p = pieces[i];
    const Frame local = f - (i ? ends[i - 1] : 0);
    if (!p.source) return 0;
    double gain = p.gain;
    for (const auto& fade : p.fades) gain *= fade.value(local);
    const Frame sourceFrame = p.offset + (p.reversed ? p.length - 1 - local : local);
    double value = 0;
    for (unsigned channel = 0; channel < p.source->channels; ++channel)
        if (p.mix[c][channel] != 0) value += p.source->sample(sourceFrame, channel) * p.mix[c][channel];
    value *= gain;
    constexpr double limit = std::numeric_limits<float>::max();
    return float(std::clamp(value, -limit, limit));
}
std::vector<Piece> Timeline::slice(Frame first, Frame last) const {
    first = std::clamp(first, Frame(0), frames); last = std::clamp(last, first, frames);
    std::vector<Piece> result;
    std::size_t i = std::size_t(std::upper_bound(ends.begin(), ends.end(), first) - ends.begin());
    for (; i < pieces.size(); ++i) {
        const Frame start = i ? ends[i - 1] : 0;
        if (start >= last) break;
        const auto& p = pieces[i];
        const Frame a = std::max(first, start) - start, b = std::min(last, ends[i]) - start;
        if (b <= a) continue;
        Piece out = p;
        if (p.source) out.offset += p.reversed ? p.length - b : a;
        out.length = b - a;
        for (auto& fade : out.fades) fade.position += fade.reversed ? -a : a;
        result.push_back(std::move(out));
    }
    return result;
}
Timeline Timeline::range(Frame first, Frame last) const {
    Timeline result; result.sampleRate = sampleRate; result.channels = channels;
    result.pieces = slice(first, last); result.rebuild(); return result;
}
Peak Timeline::peak(Frame first, Frame last, unsigned c) const noexcept {
    first = std::clamp(first, Frame(0), frames); last = std::clamp(last, first, frames);
    Peak out{};
    std::size_t i = std::size_t(std::upper_bound(ends.begin(), ends.end(), first) - ends.begin());
    for (; i < pieces.size(); ++i) {
        const Frame start = i ? ends[i - 1] : 0;
        if (start >= last) break;
        const auto& p = pieces[i];
        const Frame a = std::max(first, start) - start, b = std::min(last, ends[i]) - start;
        if (!p.source || a >= b) continue;
        if (c >= channels) return out;
        const auto mixed = p.source->mixPeak(p.offset + (p.reversed ? p.length - b : a), p.offset + (p.reversed ? p.length - a : b), p.mix[c][0], p.mix[c][1]);
        // Bound each monotonic envelope over this pixel's range. This preserves
        // source peak caching and never hides a transient between the endpoints.
        double gain = p.gain;
        for (const auto& fade : p.fades) gain *= std::max(fade.value(a), fade.value(b - 1));
        constexpr double limit = std::numeric_limits<float>::max();
        Peak v{float(std::clamp(mixed.lo * gain, -limit, limit)), float(std::clamp(mixed.hi * gain, -limit, limit))};
        if (v.lo > v.hi) std::swap(v.lo, v.hi);
        out.lo = std::min(out.lo, v.lo); out.hi = std::max(out.hi, v.hi);
    }
    return out;
}
void Document::open(std::shared_ptr<AudioFile> file) {
    auto t = std::make_shared<Timeline>();
    t->sampleRate = file->sampleRate; t->channels = file->channels;
    t->pieces.push_back({file, 0, file->frames}); t->rebuild();
    current_ = saved_ = t; history_ = {{t, "Open"}}; historyIndex_ = 0;
}
void Document::commit(std::shared_ptr<Timeline> t, std::string label) {
    t->rebuild();
    history_.resize(historyIndex_ + 1);
    history_.push_back({t, std::move(label)});
    if (history_.size() > 129) history_.erase(history_.begin());
    historyIndex_ = history_.size() - 1; current_ = std::move(t);
}
static void fadeBoundary(Timeline& t, Frame at, Frame length) {
    if (length <= 0) return;
    fadeRange(t, std::max<Frame>(0, at - length), at, true, FadeCurve::Smooth);
    fadeRange(t, at, at + std::min(length, t.frames - at), false, FadeCurve::Smooth);
}
void Document::replace(Frame first, Frame last, const std::vector<Piece>& replacement, std::string label, Frame edgeFrames) {
    if (!current_) return;
    first = std::clamp(first, Frame(0), current_->frames); last = std::clamp(last, first, current_->frames);
    if (first == last && replacement.empty()) return;
    auto t = std::make_shared<Timeline>(); t->sampleRate = current_->sampleRate; t->channels = current_->channels;
    t->pieces = current_->slice(0, first);
    t->pieces.insert(t->pieces.end(), replacement.begin(), replacement.end());
    auto tail = current_->slice(last, current_->frames);
    t->pieces.insert(t->pieces.end(), tail.begin(), tail.end());
    if (edgeFrames > 0) {
        t->rebuild(); const Frame inserted = t->frames - (current_->frames - (last - first));
        const Frame n = inserted ? std::min(edgeFrames, std::max<Frame>(1, inserted / 2)) : edgeFrames;
        fadeBoundary(*t, first, n); if (inserted) fadeBoundary(*t, first + inserted, n);
    }
    commit(std::move(t), std::move(label));
}
void Document::trim(Frame first, Frame last, Frame edgeFrames) {
    if (!current_ || first >= last) return;
    auto t = std::make_shared<Timeline>(); t->sampleRate = current_->sampleRate; t->channels = current_->channels;
    t->pieces = current_->slice(first, last); t->rebuild();
    const Frame n = std::min(edgeFrames, std::max<Frame>(1, t->frames / 2));
    fadeBoundary(*t, 0, n); fadeBoundary(*t, t->frames, n); commit(std::move(t), "Trim");
}
void Document::effect(Frame first, Frame last, Effect effect, float gain) {
    if (!current_ || first >= last) return;
    first = std::clamp(first, Frame(0), current_->frames); last = std::clamp(last, first, current_->frames);
    auto p = current_->slice(first, last);
    std::string label;
    if (effect == Effect::Silence) { p = {{nullptr, 0, last - first}}; label = "Silence"; }
    if (effect == Effect::Reverse) {
        std::reverse(p.begin(), p.end());
        for (auto& s : p) {
            s.reversed = !s.reversed;
            for (auto& fade : s.fades) {
                fade.position += fade.reversed ? -(s.length - 1) : s.length - 1;
                fade.reversed = !fade.reversed;
            }
        }
        label = "Reverse";
    }
    if (effect == Effect::Gain) { if (!std::isfinite(gain)) return; for (auto& s : p) s.gain = std::clamp(s.gain * gain, -1e100, 1e100); label = "Gain"; }
    replace(first, last, p, label);
}
void fadeRange(Timeline& t, Frame first, Frame last, bool out, FadeCurve curve) {
    first = std::clamp(first, Frame(0), t.frames); last = std::clamp(last, first, t.frames);
    if (first == last) return;
    auto pieces = t.slice(first, last);
    Frame position = 0;
    for (auto& p : pieces) {
        if (p.source) p.fades.push_back({position, last - first, false, out, curve});
        position += p.length;
    }
    auto head = t.slice(0, first), tail = t.slice(last, t.frames);
    head.insert(head.end(), pieces.begin(), pieces.end()); head.insert(head.end(), tail.begin(), tail.end());
    t.pieces = std::move(head); t.rebuild();
}
void Document::fade(Frame first, Frame last, bool out, FadeCurve curve) {
    if (!current_ || first >= last) return;
    auto t = std::make_shared<Timeline>(*current_); fadeRange(*t, first, last, out, curve);
    commit(std::move(t), out ? "Fade out" : "Fade in");
}
void Document::fadeEdges(Frame first, Frame last, Frame length) {
    if (!current_ || first >= last || length <= 0) return;
    first = std::clamp(first, Frame(0), current_->frames); last = std::clamp(last, first, current_->frames);
    const Frame n = std::min(length, std::max<Frame>(1, (last - first) / 2));
    auto t = std::make_shared<Timeline>(*current_);
    fadeRange(*t, first, first + n, false, FadeCurve::Smooth); fadeRange(*t, last - n, last, true, FadeCurve::Smooth);
    commit(std::move(t), "Fade edges");
}
void Document::channelTool(ChannelTool tool, Frame first, Frame last) {
    if (!current_) return;
    const bool invert = tool == ChannelTool::Invert || tool == ChannelTool::InvertLeft || tool == ChannelTool::InvertRight;
    if (current_->channels != 2 && tool != ChannelTool::Invert && tool != ChannelTool::InvertLeft) return;
    if (!invert) { first = 0; last = current_->frames; }
    auto t = std::make_shared<Timeline>(*current_);
    auto pieces = t->slice(first, last);
    for (auto& p : pieces) {
        if (tool == ChannelTool::Swap) std::swap(p.mix[0], p.mix[1]);
        else if (invert) {
            for (unsigned c = 0; c < t->channels; ++c)
                if (tool == ChannelTool::Invert || (c == 0 && tool == ChannelTool::InvertLeft) || (c == 1 && tool == ChannelTool::InvertRight))
                    for (auto& weight : p.mix[c]) weight = -weight;
        } else {
            if (tool == ChannelTool::ExtractRight) p.mix[0] = p.mix[1];
            if (tool == ChannelTool::Mono) for (unsigned c = 0; c < 2; ++c) p.mix[0][c] = (p.mix[0][c] + p.mix[1][c]) * .5;
            p.mix[1] = {0, 0};
        }
    }
    if (invert) {
        auto head = t->slice(0, first), tail = t->slice(last, t->frames);
        head.insert(head.end(), pieces.begin(), pieces.end()); head.insert(head.end(), tail.begin(), tail.end()); t->pieces = std::move(head);
    } else t->pieces = std::move(pieces);
    if (!invert && tool != ChannelTool::Swap) t->channels = 1;
    const char* names[] = {"Extract left", "Extract right", "Mix to mono", "Swap channels", "Invert polarity", "Invert left polarity", "Invert right polarity"};
    commit(std::move(t), names[int(tool)]);
}
void Document::apply(std::shared_ptr<Timeline> t, std::string label) {
    if (t) commit(std::move(t), std::move(label));
}
Frame zeroCrossing(const Timeline& t, Frame frame, Frame radius) {
    frame = std::clamp(frame, Frame(0), t.frames);
    if (frame == 0 || frame == t.frames || radius <= 0) return frame;
    Frame best = frame; double score = std::numeric_limits<double>::infinity();
    const Frame end = frame + std::min(radius, t.frames - 1 - frame);
    for (Frame f = std::max<Frame>(1, frame - radius); f <= end; ++f) {
        bool crossing = false; double energy = 0;
        for (unsigned c = 0; c < t.channels; ++c) {
            const double a = t.sample(f - 1, c), b = t.sample(f, c);
            crossing |= a == 0 || b == 0 || std::signbit(a) != std::signbit(b);
            energy += a * a + b * b;
        }
        const double candidate = t.channels == 1 ? double(std::abs(f - frame)) : energy;
        if (crossing && (candidate < score || (candidate == score && std::abs(f - frame) < std::abs(best - frame)))) { best = f; score = candidate; }
    }
    return best;
}
void Document::insertSilence(Frame at, Frame length, Frame edgeFrames) {
    if (!current_ || length <= 0) return;
    if (length > std::numeric_limits<Frame>::max() - current_->frames)
        throw std::runtime_error("The inserted silence would make the audio too long.");
    at = std::clamp(at, Frame(0), current_->frames);
    replace(at, at, {{nullptr, 0, length}}, "Insert silence", edgeFrames);
}
bool Document::undo() { if (!canUndo()) return false; current_ = history_[--historyIndex_].timeline; return true; }
bool Document::redo() { if (!canRedo()) return false; current_ = history_[++historyIndex_].timeline; return true; }
std::string Document::undoLabel() const { return canUndo() ? history_[historyIndex_].label : ""; }
std::string Document::redoLabel() const { return canRedo() ? history_[historyIndex_ + 1].label : ""; }

static void put16(std::ostream& f, std::uint16_t v) { char b[2] = {char(v), char(v >> 8)}; f.write(b, 2); }
static void put32(std::ostream& f, std::uint32_t v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, 4); }
void exportAudio(const Timeline& t, const std::filesystem::path& path, ExportFormat format, std::atomic<float>* progress, std::stop_token stop) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Choose a new file name. ColdEdit never overwrites an existing audio file.");
    const unsigned bits = exportBits(format);
    const bool flac = isFlac(format);
    const bool wav = isWav(format);
#ifdef __EMSCRIPTEN__
    if (wav) web::checkExport(std::uint64_t(t.frames), t.channels, bits);
#endif
    const std::uint64_t extra = format == ExportFormat::Float32 ? 48 : 36;
    const auto bytesPerFrame = t.channels * (bits / 8);
    if (!bytesPerFrame || t.frames < 0 || (wav && std::uint64_t(t.frames) > (UINT32_MAX - extra) / bytesPerFrame))
        throw std::runtime_error("Export exceeds the RIFF/WAV 4 GiB limit. Export a selection or use a lower bit depth.");
    const std::uint64_t size = std::uint64_t(t.frames) * bytesPerFrame;
    if (wav && size + (size & 1) + extra > UINT32_MAX) throw std::runtime_error("Export exceeds the RIFF/WAV 4 GiB limit. Trim the audio or use a lower bit depth.");
    auto temp = path; temp += ".coldedit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    try {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot create the export file.");
        if (flac) writeFlac(t, f, bits, progress, stop);
        else if (isMp3(format)) writeMp3(t, f, mp3Bitrate(format), progress, stop);
        else {
        f.write("RIFF", 4); put32(f, std::uint32_t(size + (size & 1) + extra)); f.write("WAVEfmt ", 8); put32(f, 16);
        put16(f, format == ExportFormat::Float32 ? 3 : 1); put16(f, std::uint16_t(t.channels));
        put32(f, t.sampleRate); put32(f, t.sampleRate * t.channels * (bits / 8));
        put16(f, std::uint16_t(t.channels * (bits / 8))); put16(f, std::uint16_t(bits));
        if (format == ExportFormat::Float32) { f.write("fact", 4); put32(f, 4); put32(f, std::uint32_t(t.frames)); }
        f.write("data", 4); put32(f, std::uint32_t(size));
        std::vector<char> buffer(4096 * t.channels * (bits / 8));
        for (Frame base = 0; base < t.frames; base += 4096) {
            if (stop.stop_requested()) throw std::runtime_error("Export cancelled.");
            const Frame count = std::min<Frame>(4096, t.frames - base);
            std::size_t at = 0;
            for (Frame i = 0; i < count; ++i) for (unsigned c = 0; c < t.channels; ++c) {
                float v = t.sample(base + i, c);
                if (!std::isfinite(v)) v = 0;
                std::uint32_t encoded;
                if (format == ExportFormat::Float32) std::memcpy(&encoded, &v, 4);
                else {
                    const double scale = bits == 16 ? 32768.0 : 8388608.0;
                    const auto q = std::int32_t(std::llround(std::clamp(double(v) * scale, -scale, scale - 1)));
                    encoded = std::uint32_t(q);
                }
                for (unsigned b = 0; b < bits / 8; ++b) buffer[at++] = char(encoded >> (8 * b));
            }
            f.write(buffer.data(), std::streamsize(at));
            if (!f) throw std::runtime_error("Export failed. Check available disk space.");
            if (progress) progress->store(float(double(base + count) / double(std::max<Frame>(1, t.frames))));
        }
        if (size & 1) f.put(0);
        }
        if (stop.stop_requested()) throw std::runtime_error("Export cancelled.");
        f.flush(); if (!f) throw std::runtime_error("Could not flush the export file."); f.close();
        // Publish without replacing a file, including one created since our first check.
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish the export (Windows error " + std::to_string(GetLastError()) + "). Choose a new name in a writable folder.");
#elif defined(__EMSCRIPTEN__)
        // MEMFS has no hard links. Only one export job can publish at a time.
        if (std::filesystem::exists(path)) throw std::runtime_error("Export destination already exists.");
        std::filesystem::rename(temp, path);
#else
        std::filesystem::create_hard_link(temp, path);
        std::filesystem::remove(temp);
#endif
        if (progress) progress->store(1);
    } catch (...) { std::error_code ec; std::filesystem::remove(temp, ec); throw; }
}
float exactPeak(const Timeline& t, Frame first, Frame last, std::stop_token stop) {
    float peak = 0;
    first = std::clamp(first, Frame(0), t.frames); last = std::clamp(last, first, t.frames);
    for (Frame f = first; f < last; ++f) {
        if ((f & 4095) == 0 && stop.stop_requested()) return 0;
        for (unsigned c = 0; c < t.channels; ++c) peak = std::max(peak, std::abs(t.sample(f, c)));
    }
    return peak;
}
}
