#pragma once
#include "document.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace coldedit {
class Playback {
public:
    // Manual mode exposes the same device callback to deterministic offline tests.
    explicit Playback(bool manualRender = false);
    ~Playback();
    bool available() const { return available_; }
    const std::string& error() const { return error_; }
    void setTimeline(std::shared_ptr<const Timeline> timeline, Frame position = 0);
    void play(Frame position, Frame loopStart, Frame loopEnd, bool loop);
    void pause();
    void seek(Frame position);
    bool playing() const { return (transport_.load() & 1) != 0; }
    Frame position() const {
        const auto generation = transport_.load(std::memory_order_acquire) >> 1;
        return renderedGeneration_.load(std::memory_order_acquire) == generation ? position_.load() : requestedView_.load();
    }
    void setVolume(float volume) { volume_.store(volume); }
    float meter(unsigned channel) const { return meters_[channel].load(); }
    std::uint64_t underruns() const { return underruns_.load(); }
    void render(float* output, unsigned frameCount) noexcept;
private:
    struct Device;
    std::unique_ptr<Device> device_;
    bool available_ = false;
    std::string error_;
    static constexpr std::uint64_t capacity = 16384;
    struct Sample { float left = 0, right = 0; Frame position = 0; std::uint64_t generation = 0; bool last = false; };
    std::array<Sample, capacity> ring_{};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    // Generation and run state share an atomic so an old EOF cannot stop a new play.
    std::atomic<std::uint64_t> transport_{2}, underruns_{0}, renderedGeneration_{0};
    std::uint64_t audibleGeneration_ = 0; // callback-owned
    std::atomic<Frame> position_{0}, requestedView_{0};
    std::atomic<float> volume_{.8f}, meters_[2]{};
    std::mutex commandMutex_;
    std::condition_variable_any wake_;
    std::shared_ptr<const Timeline> timeline_;
    Frame requestedPosition_ = 0, loopStart_ = 0, loopEnd_ = 0;
    bool loop_ = false;
    std::jthread worker_;
    void produce(std::stop_token stop);
};
}
