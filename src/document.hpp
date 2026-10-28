#pragma once
#include "audio_file.hpp"
#include <functional>
#include <vector>
#include <array>

namespace coldedit {
enum class FadeCurve { Linear, Smooth };
struct FadeEnvelope {
    Frame position = 0, length = 0;
    bool reversed = false, out = false;
    FadeCurve curve = FadeCurve::Linear;
    double value(Frame local) const noexcept;
};
struct Piece {
    std::shared_ptr<AudioFile> source; // null means digital silence
    Frame offset = 0, length = 0;
    double gain = 1;
    bool reversed = false;
    std::vector<FadeEnvelope> fades{};
    std::array<std::array<double, 2>, 2> mix{{{1, 0}, {0, 1}}};
};
struct PitchTimeState;
struct Timeline {
    std::vector<Piece> pieces;
    std::vector<Frame> ends;
    Frame frames = 0;
    unsigned sampleRate = 48000, channels = 2;
    // Immutable pitch/time controls travel with rendered audio through undo/redo.
    std::shared_ptr<const PitchTimeState> pitchTime;
    void rebuild();
    float sample(Frame frame, unsigned channel) const noexcept;
    Peak peak(Frame first, Frame last, unsigned channel) const noexcept;
    Peak mixPeak(Frame first, Frame last, double left, double right) const noexcept;
    std::vector<Piece> slice(Frame first, Frame last) const;
    Timeline range(Frame first, Frame last) const;
};
void visitAudioSources(const Timeline& timeline, const std::function<void(const std::shared_ptr<AudioFile>&)>& visit);
enum class Effect { Silence, Reverse, Gain };
enum class ChannelTool { ExtractLeft, ExtractRight, Mono, Swap, Invert, InvertLeft, InvertRight };
void fadeRange(Timeline& timeline, Frame first, Frame last, bool out, FadeCurve curve = FadeCurve::Linear);
Frame zeroCrossing(const Timeline& timeline, Frame frame, Frame radius);
class Document {
public:
    void open(std::shared_ptr<AudioFile> file, bool saved = true);
    void replace(Frame first, Frame last, const std::vector<Piece>& replacement, std::string label, Frame edgeFrames = 0);
    void trim(Frame first, Frame last, Frame edgeFrames = 0);
    void effect(Frame first, Frame last, Effect effect, float gain = 1.f);
    void fade(Frame first, Frame last, bool out, FadeCurve curve = FadeCurve::Linear);
    void insertSilence(Frame at, Frame length, Frame edgeFrames = 0);
    void channelTool(ChannelTool tool, Frame first, Frame last);
    void fadeEdges(Frame first, Frame last, Frame length);
    void apply(std::shared_ptr<Timeline> timeline, std::string label);
    bool undo();
    bool redo();
    bool canUndo() const { return historyIndex_ > 0; }
    bool canRedo() const { return historyIndex_ + 1 < history_.size(); }
    bool dirty() const { return current_ && current_ != saved_.lock(); }
    void markSaved() { saved_ = current_; }
    std::shared_ptr<const Timeline> timeline() const { return current_; }
    std::string undoLabel() const;
    std::string redoLabel() const;
    std::size_t historySize() const { return history_.size(); }
    void clearHistory();
    std::uint64_t reclaimableAudioBytes(const std::vector<Piece>& retained = {}) const;
private:
    struct State { std::shared_ptr<const Timeline> timeline; std::string label; };
    std::shared_ptr<const Timeline> current_;
    std::weak_ptr<const Timeline> saved_;
    std::vector<State> history_;
    std::size_t historyIndex_ = 0;
    void commit(std::shared_ptr<Timeline> timeline, std::string label);
};
enum class ExportFormat { PCM16, PCM24, Float32, FLAC16, FLAC24, MP3VBR, MP3192, MP3320 };
inline bool isFlac(ExportFormat format) { return format == ExportFormat::FLAC16 || format == ExportFormat::FLAC24; }
inline bool isMp3(ExportFormat format) { return format == ExportFormat::MP3VBR || format == ExportFormat::MP3192 || format == ExportFormat::MP3320; }
inline bool isWav(ExportFormat format) { return !isFlac(format) && !isMp3(format); }
inline const char* exportExtension(ExportFormat format) { return isMp3(format) ? ".mp3" : isFlac(format) ? ".flac" : ".wav"; }
inline unsigned mp3Bitrate(ExportFormat format) { return format == ExportFormat::MP3192 ? 192 : format == ExportFormat::MP3320 ? 320 : 0; }
unsigned mp3SampleRate(unsigned sourceRate, unsigned bitrate);
inline unsigned exportBits(ExportFormat format) {
    return format == ExportFormat::PCM16 || format == ExportFormat::FLAC16 ? 16 : format == ExportFormat::Float32 ? 32 : 24;
}
// Writes a new destination only. Existing files are rejected; source files stay intact.
void exportAudio(const Timeline& timeline, const std::filesystem::path& path,
               ExportFormat format, std::atomic<float>* progress = nullptr,
               std::stop_token stop = {});
inline void exportWav(const Timeline& timeline, const std::filesystem::path& path,
    ExportFormat format, std::atomic<float>* progress = nullptr, std::stop_token stop = {}) {
    exportAudio(timeline, path, format, progress, stop);
}
float exactPeak(const Timeline& timeline, Frame first, Frame last, std::stop_token stop = {});
}
