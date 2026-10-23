#include "codecs.hpp"
#include "document.hpp"
#include "web_limits.hpp"
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace coldedit {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled.");
}
void validate(const DecodeInfo& info) {
    if (info.channels < 1 || info.channels > 2) throw std::runtime_error("Open mono or stereo audio. Surround files are not supported.");
    if (info.rate < 1000 || info.rate > 768000 || info.bits < 4 || info.bits > 32)
        throw std::runtime_error("Unsupported audio sample rate or bit depth.");
}
struct FlacInput {
    const std::uint8_t* data;
    std::size_t size, position = 0;
    const std::function<void(const DecodeInfo&)>& begin;
    const std::function<void(const float*, unsigned)>& append;
    std::stop_token stop;
    DecodeInfo info;
    std::uint64_t frames = 0;
    bool metadata = false, corrupt = false;
    std::exception_ptr exception;
    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder*, FLAC__byte* out, std::size_t* count, void* user) {
        auto& s = *static_cast<FlacInput*>(user);
        if (s.stop.stop_requested() || s.corrupt || s.exception) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        *count = std::min(*count, s.size - s.position);
        std::memcpy(out, s.data + s.position, *count); s.position += *count;
        return *count ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    static void meta(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* user) {
        auto& s = *static_cast<FlacInput*>(user);
        if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        try {
            const auto& i = m->data.stream_info;
            s.info = {i.sample_rate, i.channels, i.bits_per_sample, i.total_samples};
            validate(s.info); cancelled(s.stop); s.begin(s.info); s.metadata = true;
        } catch (...) { s.exception = std::current_exception(); }
    }
    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const samples[], void* user) {
        auto& s = *static_cast<FlacInput*>(user);
        try {
            if (s.exception) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            if (!s.metadata || s.corrupt || f->header.channels != s.info.channels ||
                f->header.sample_rate != s.info.rate || f->header.bits_per_sample != s.info.bits)
                throw std::runtime_error("Invalid FLAC audio frame.");
            const double scale = std::ldexp(1., 1 - int(s.info.bits));
            std::array<float, 4096 * 2> block;
            for (unsigned at = 0; at < f->header.blocksize; at += 4096) {
                cancelled(s.stop);
                const unsigned n = std::min(4096u, f->header.blocksize - at);
                for (unsigned i = 0; i < n; ++i) for (unsigned c = 0; c < s.info.channels; ++c)
                    block[i * s.info.channels + c] = float(samples[c][at + i] * scale);
                s.append(block.data(), n); s.frames += n;
            }
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        } catch (...) { s.exception = std::current_exception(); return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    }
    static void error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* user) {
        static_cast<FlacInput*>(user)->corrupt = true;
    }
};
struct FlacOutput {
    std::ostream& stream;
    std::stop_token stop;
    std::uint64_t position = 0;
    bool limit = false;
    static FLAC__StreamEncoderWriteStatus write(const FLAC__StreamEncoder*, const FLAC__byte* bytes, std::size_t n, FLAC__uint32, FLAC__uint32, void* user) {
        auto& s = *static_cast<FlacOutput*>(user);
        if (s.stop.stop_requested()) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
#ifdef __EMSCRIPTEN__
        if (s.position > web::exportLimit || n > web::exportLimit - s.position) {
            s.limit = true; return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        }
#endif
        s.stream.write(reinterpret_cast<const char*>(bytes), std::streamsize(n)); s.position += n;
        return s.stream ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    static FLAC__StreamEncoderSeekStatus seek(const FLAC__StreamEncoder*, FLAC__uint64 at, void* user) {
        auto& s = *static_cast<FlacOutput*>(user);
        s.stream.seekp(std::streamoff(at)); s.position = at;
        return s.stream ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    static FLAC__StreamEncoderTellStatus tell(const FLAC__StreamEncoder*, FLAC__uint64* at, void* user) {
        *at = static_cast<FlacOutput*>(user)->position; return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }
};
}
void decodeAudio(const std::uint8_t* bytes, std::size_t size,
    const std::function<void(const DecodeInfo&)>& begin,
    const std::function<void(const float*, unsigned)>& append,
    std::atomic<float>* progress, std::stop_token stop) {
    cancelled(stop);
    if (size >= 4 && !std::memcmp(bytes, "fLaC", 4)) {
        FlacInput input{bytes, size, 0, begin, append, stop, {}, 0, false, false, {}};
        std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)> decoder(FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
        if (!decoder) throw std::bad_alloc();
        FLAC__stream_decoder_set_md5_checking(decoder.get(), true);
        if (FLAC__stream_decoder_init_stream(decoder.get(), FlacInput::read, nullptr, nullptr, nullptr, nullptr,
            FlacInput::write, FlacInput::meta, FlacInput::error, &input) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
            throw std::runtime_error("Cannot initialize the FLAC decoder.");
        while (FLAC__stream_decoder_get_state(decoder.get()) != FLAC__STREAM_DECODER_END_OF_STREAM) {
            cancelled(stop);
            const bool ok = FLAC__stream_decoder_process_single(decoder.get());
            if (input.exception) std::rethrow_exception(input.exception);
            cancelled(stop);
            if (!ok || input.corrupt) throw std::runtime_error("The FLAC file is damaged or incomplete.");
            if (progress) progress->store(float(double(input.position) / double(size)));
        }
        const bool checksum = FLAC__stream_decoder_finish(decoder.get());
        if (!checksum || !input.metadata || (input.info.frames && input.frames != input.info.frames))
            throw std::runtime_error("The FLAC file is damaged or incomplete (length/checksum mismatch).");
    } else {
        drmp3 mp3{};
        if (!drmp3_init_memory(&mp3, bytes, size, nullptr))
            throw std::runtime_error("Cannot decode this file. Open a WAV, FLAC, or MP3 file.");
        struct Release { drmp3* value; ~Release() { drmp3_uninit(value); } } release{&mp3};
        const auto trim = std::uint64_t(mp3.delayInPCMFrames) + mp3.paddingInPCMFrames;
        const auto expected = mp3.totalPCMFrameCount != DRMP3_UINT64_MAX && mp3.totalPCMFrameCount >= trim ? mp3.totalPCMFrameCount - trim : 0;
        DecodeInfo info{mp3.sampleRate, mp3.channels, 32, expected}; validate(info); begin(info);
        std::array<float, 4096 * 2> block;
        std::uint64_t frames = 0;
        for (;;) {
            cancelled(stop);
            const auto n = drmp3_read_pcm_frames_f32(&mp3, 4096, block.data());
            if (!n) break;
            if (mp3.mp3FrameSampleRate != info.rate || mp3.mp3FrameChannels != info.channels)
                throw std::runtime_error("MP3 files that change sample rate or channel count are not supported.");
            append(block.data(), unsigned(n)); frames += n;
            if (progress) progress->store(float(double(mp3.streamCursor) / double(size)));
        }
        if (!frames || (expected && frames != expected)) throw std::runtime_error("The MP3 file is damaged or incomplete.");
    }
    cancelled(stop);
    if (progress) progress->store(1);
}
void writeFlac(const Timeline& t, std::ostream& output, unsigned bits, std::atomic<float>* progress, std::stop_token stop) {
    cancelled(stop);
    if (t.channels < 1 || t.channels > 2 || t.frames < 0 || std::uint64_t(t.frames) >= (1ull << 36) || (bits != 16 && bits != 24))
        throw std::runtime_error("Unsupported FLAC format or duration.");
    std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)> encoder(FLAC__stream_encoder_new(), FLAC__stream_encoder_delete);
    if (!encoder) throw std::bad_alloc();
    auto* e = encoder.get();
    FLAC__stream_encoder_set_channels(e, t.channels);
    FLAC__stream_encoder_set_bits_per_sample(e, bits);
    FLAC__stream_encoder_set_sample_rate(e, t.sampleRate);
    FLAC__stream_encoder_set_total_samples_estimate(e, std::uint64_t(t.frames));
    FLAC__stream_encoder_set_compression_level(e, 5);
    FlacOutput sink{output, stop};
    if (FLAC__stream_encoder_init_stream(e, FlacOutput::write, FlacOutput::seek, FlacOutput::tell, nullptr, &sink) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        throw std::runtime_error("Cannot encode FLAC at this sample rate.");
    auto check = [&](bool ok) {
        cancelled(stop);
        if (sink.limit) throw std::runtime_error("FLAC export exceeds the web editor's 128 MiB limit. Export a selection or use ColdEdit for Windows.");
        if (!ok) throw std::runtime_error("FLAC export failed. Check available disk space.");
    };
    std::array<FLAC__int32, 4096 * 2> block;
    const double scale = std::ldexp(1., int(bits) - 1);
    for (Frame at = 0; at < t.frames; at += 4096) {
        cancelled(stop);
        const unsigned n = unsigned(std::min<Frame>(4096, t.frames - at));
        for (unsigned i = 0; i < n; ++i) for (unsigned c = 0; c < t.channels; ++c) {
            double value = t.sample(at + i, c); if (!std::isfinite(value)) value = 0;
            block[i * t.channels + c] = FLAC__int32(std::llround(std::clamp(value * scale, -scale, scale - 1)));
        }
        check(FLAC__stream_encoder_process_interleaved(e, block.data(), n));
        if (progress) progress->store(float(double(at + n) / double(std::max<Frame>(1, t.frames))));
    }
    check(FLAC__stream_encoder_finish(e));
}
}
