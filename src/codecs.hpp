#pragma once
#include "audio_file.hpp"
#include <ostream>

namespace coldedit {
struct DecodeInfo {
    unsigned rate = 0, channels = 0, bits = 0;
    std::uint64_t frames = 0; // Zero means unknown; never trusted as an output bound.
};
// The input remains mapped; callbacks consume bounded interleaved float blocks.
void decodeAudio(const std::uint8_t* bytes, std::size_t size,
    const std::function<void(const DecodeInfo&)>& begin,
    const std::function<void(const float*, unsigned)>& append,
    std::atomic<float>* progress, std::stop_token stop);
struct Timeline;
void writeFlac(const Timeline& timeline, std::ostream& output, unsigned bits,
    std::atomic<float>* progress, std::stop_token stop);
}
