#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <functional>

namespace coldedit {
using Frame = std::int64_t;
struct Peak { float lo = 0, hi = 0; };
// Float extrema preserve headroom in float WAVs even after non-destructive gain.
struct PackedPeak { float lo, hi; };
struct PitchTimeState;

class AudioFile {
public:
    static std::shared_ptr<AudioFile> open(const std::filesystem::path& path,
        std::atomic<float>* progress = nullptr, std::stop_token stop = {});
    // Streaming processing sink; disk-backed on native, budgeted PCM storage on web.
    static std::shared_ptr<AudioFile> render(Frame frames, unsigned rate, unsigned channels,
        const std::function<void(Frame, Frame, float*)>& fill, std::atomic<float>* progress, std::stop_token stop);
    static std::uint64_t processedBytes();
    static std::uint64_t audioBytes();
    std::uint64_t storageBytes() const;
    // Lightweight, editable audio while the spectral render runs. Publishing
    // only upgrades this source; it never replaces a document/history revision.
    static std::shared_ptr<AudioFile> pitchTimePreview(const PitchTimeState& map);
    std::shared_ptr<const PitchTimeState> pitchTimeState() const { return pitchTimeState_; }
    std::shared_ptr<AudioFile> pitchTimeResult() const { return pitchTimeResult_.load(std::memory_order_acquire) ? pitchTimeOwner_ : nullptr; }
    void publishPitchTime(std::shared_ptr<AudioFile> result);
    static std::uint64_t pitchTimeRevision();
    ~AudioFile();
    AudioFile(const AudioFile&) = delete;
    AudioFile& operator=(const AudioFile&) = delete;
    float sample(Frame frame, unsigned channel) const noexcept;
    Peak peak(Frame first, Frame last, unsigned channel) const noexcept;
    Peak mixPeak(Frame first, Frame last, double left, double right) const noexcept;
    void waitForPeaks();
    bool peaksReady() const noexcept;
    float peakProgress() const noexcept;
    std::size_t peakBytes() const noexcept { return peakBytes_.load(); }
    Frame frames = 0;
    unsigned sampleRate = 0, channels = 0, bits = 0;
    bool floating = false;
    std::filesystem::path path;
    std::uint64_t fileBytes = 0;
    std::string format = "WAV";
private:
    friend class AudioRecording;
    AudioFile() = default;
    std::shared_ptr<const PitchTimeState> pitchTimeState_;
    // Published once; the wrapper owns the result for every reader's lifetime.
    // A raw atomic avoids locking/refcounting on every playback sample.
    std::shared_ptr<AudioFile> pitchTimeOwner_;
    std::atomic<AudioFile*> pitchTimeResult_{nullptr};
    std::mutex pitchTimePublishMutex_;
    struct Mapping;
    std::unique_ptr<Mapping> mapping_;
    std::filesystem::path temporaryDirectory_;
    const std::uint8_t* data_ = nullptr;
    unsigned blockAlign_ = 0;
    struct Level { Frame stride; std::vector<PackedPeak> values; };
    std::vector<Level> levels_;
    std::jthread peakThread_;
    std::atomic<bool> ready_{false};
    std::atomic<float> progress_{0};
    std::atomic<std::size_t> peakBytes_{0};
    static std::shared_ptr<AudioFile> map(const std::filesystem::path& path);
    static std::shared_ptr<AudioFile> mapFloat(const std::filesystem::path& path,
        Frame frames, unsigned rate, unsigned channels, std::stop_token stop);
    void parse();
    std::shared_ptr<AudioFile> decode(std::atomic<float>* progress, std::stop_token stop);
    void buildPeaks(std::stop_token stop);
    unsigned peakChannels() const { return channels == 2 ? 4 : 1; }
    float peakSample(Frame frame, unsigned channel) const noexcept;
    Peak cachedPeak(Frame first, Frame last, unsigned channel) const noexcept;
};
// Incremental capture storage. Only append from one non-realtime thread.
// finish transfers ownership to the editor without copying the whole take.
class AudioRecording {
public:
    AudioRecording(unsigned rate, unsigned channels);
    ~AudioRecording();
    void append(const float* samples, unsigned frames);
    std::shared_ptr<AudioFile> finish();
    Frame frames() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
