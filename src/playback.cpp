#include "playback.hpp"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace coldedit {
struct Playback::Device { ma_device handle{}; bool initialized = false; };
Playback::Playback(bool manualRender) : device_(std::make_unique<Device>()) {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<float>::is_always_lock_free);
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32; cfg.playback.channels = 2; cfg.sampleRate = 48000;
    cfg.periodSizeInMilliseconds = 10;
    cfg.pUserData = this;
    cfg.dataCallback = [](ma_device* dev, void* out, const void*, ma_uint32 count) { static_cast<Playback*>(dev->pUserData)->render(static_cast<float*>(out), count); };
    if (manualRender) available_ = true;
    else if (ma_device_init(nullptr, &cfg, &device_->handle) == MA_SUCCESS) {
        device_->initialized = true;
        available_ = ma_device_start(&device_->handle) == MA_SUCCESS;
    }
    if (!available_) error_ = "Audio output unavailable. Editing and export still work.";
    worker_ = std::jthread([this](std::stop_token s) { produce(s); });
}
Playback::~Playback() {
    pause(); worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (device_->initialized) ma_device_uninit(&device_->handle);
}
void Playback::setTimeline(std::shared_ptr<const Timeline> t, Frame p) {
    pause();
    std::lock_guard lock(commandMutex_);
    timeline_ = std::move(t); requestedPosition_ = p; requestedView_.store(p);
    loop_ = false; transport_.fetch_add(2, std::memory_order_release);
    wake_.notify_all();
}
void Playback::play(Frame p, Frame first, Frame last, bool loop) {
    if (!available_) return;
    std::lock_guard lock(commandMutex_);
    if (!timeline_ || !timeline_->frames) return;
    requestedPosition_ = std::clamp(p, Frame(0), timeline_->frames - 1);
    loopStart_ = std::clamp(first, Frame(0), timeline_->frames);
    loopEnd_ = std::clamp(last, loopStart_, timeline_->frames);
    loop_ = loop && loopEnd_ > loopStart_;
    if (loop_ && (requestedPosition_ < loopStart_ || requestedPosition_ >= loopEnd_)) requestedPosition_ = loopStart_;
    requestedView_.store(requestedPosition_);
    transport_.store(((transport_.load() >> 1) + 1) * 2 + 1, std::memory_order_release);
    wake_.notify_all();
}
void Playback::pause() { transport_.fetch_and(~std::uint64_t(1)); }
void Playback::seek(Frame p) {
    std::lock_guard lock(commandMutex_);
    requestedPosition_ = timeline_ ? std::clamp(p, Frame(0), timeline_->frames) : 0;
    if (loop_ && playing() && loopEnd_ > loopStart_) requestedPosition_ = std::clamp(requestedPosition_, loopStart_, loopEnd_ - 1);
    requestedView_.store(requestedPosition_); transport_.fetch_add(2, std::memory_order_release);
    wake_.notify_all();
}
void Playback::produce(std::stop_token stop) {
    std::shared_ptr<const Timeline> t;
    std::uint64_t gen = 0;
    double cursor = 0;
    Frame first = 0, end = 0;
    bool loop = false, exhausted = true;
    while (!stop.stop_requested()) {
        if (gen != (transport_.load(std::memory_order_acquire) >> 1)) {
            std::lock_guard lock(commandMutex_);
            t = timeline_; cursor = double(requestedPosition_); first = loopStart_;
            loop = loop_; end = t ? (loop ? loopEnd_ : t->frames) : 0;
            gen = transport_.load() >> 1; exhausted = !t || end == 0;
        }
        if (!playing() || exhausted || !t) {
            std::unique_lock lock(commandMutex_);
            // Adopt replaced timelines even while paused, releasing old source audio.
            wake_.wait(lock, stop, [&] { return (transport_.load() >> 1) != gen || (playing() && !exhausted); });
            continue;
        }
        auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        const auto free = capacity - (w - r);
        const auto count = std::min<std::uint64_t>(free, 1024);
        if (!count) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        const double step = double(t->sampleRate) / 48000.0;
        for (std::uint64_t n = 0; n < count; ++n) {
            if (cursor >= double(end)) { if (loop) cursor = double(first) + std::fmod(cursor - double(first), double(end - first)); else { exhausted = true; break; } }
            const auto f = Frame(cursor); const float mix = float(cursor - double(f));
            const Frame next = f + 1 < end ? f + 1 : (loop ? first : f);
            auto& s = ring_[w % capacity];
            auto interpolate = [&](unsigned c) { return std::lerp(t->sample(f, c), t->sample(next, c), mix); };
            s.left = interpolate(0); s.right = interpolate(t->channels == 1 ? 0 : 1);
            cursor += step; s.position = std::min(Frame(cursor), end); s.generation = gen;
            s.last = !loop && cursor >= double(end); ++w;
            if (s.last) { exhausted = true; break; }
        }
        write_.store(w, std::memory_order_release);
    }
}
void Playback::render(float* out, unsigned count) noexcept {
    std::fill_n(out, std::size_t(count) * 2, 0.f);
    const auto state = transport_.load(std::memory_order_acquire);
    const auto gen = state >> 1;
    auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    float peaks[2]{}; const float volume = volume_.load();
    bool underflow = false;
    for (unsigned f = 0; f < count; ++f) {
        // Consume only obsolete generations, never a future command's audio.
        while (r < w && ring_[r % capacity].generation < gen) ++r;
        if (!(state & 1) || transport_.load(std::memory_order_acquire) != state) break;
        if (r == w) { underflow = audibleGeneration_ == gen; break; }
        if (ring_[r % capacity].generation != gen) break;
        const auto& s = ring_[r % capacity];
        out[f * 2] = std::clamp(s.left * volume, -1.f, 1.f);
        out[f * 2 + 1] = std::clamp(s.right * volume, -1.f, 1.f);
        peaks[0] = std::max(peaks[0], std::abs(out[f * 2])); peaks[1] = std::max(peaks[1], std::abs(out[f * 2 + 1]));
        position_.store(s.position); renderedGeneration_.store(gen, std::memory_order_release); audibleGeneration_ = gen; ++r;
        if (s.last) { auto expected = state; transport_.compare_exchange_strong(expected, state & ~std::uint64_t(1)); break; }
    }
    read_.store(r, std::memory_order_release);
    if (underflow) underruns_.fetch_add(1);
    const float decay = std::exp(-float(count) / 48000.f * 10.f);
    for (unsigned c = 0; c < 2; ++c) meters_[c].store(std::max(peaks[c], meters_[c].load() * decay));
}
}
