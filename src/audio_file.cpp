#include "audio_file.hpp"
#include "codecs.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <fstream>
#include <array>
#include <chrono>
#ifdef __EMSCRIPTEN__
#include "web_limits.hpp"
#include <fstream>
#endif
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coldedit {
static std::atomic<std::uint64_t> renderedBytes{0};
struct AudioFile::Mapping {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    ~Mapping() { if (bytes) UnmapViewOfFile(bytes); if (map) CloseHandle(map); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
#elif defined(__EMSCRIPTEN__)
    std::unique_ptr<std::uint8_t[]> storage;
    web::AudioReservation reservation;
    std::uint64_t processed = 0;
    ~Mapping() { renderedBytes.fetch_sub(processed); }
#else
    int file = -1;
    ~Mapping() { if (bytes) munmap(const_cast<std::uint8_t*>(bytes), size); if (file >= 0) close(file); }
#endif
};
static std::uint16_t u16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
static std::uint32_t u32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }

std::shared_ptr<AudioFile> AudioFile::open(const std::filesystem::path& p, std::atomic<float>* progress, std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Import cancelled.");
    auto a = std::shared_ptr<AudioFile>(new AudioFile);
    a->path = p;
    a->mapping_ = std::make_unique<Mapping>();
    auto& m = *a->mapping_;
#ifdef _WIN32
    m.file = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m.file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open the file. Check its location and permissions.");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(m.file, &size) || size.QuadPart < 12 || std::uint64_t(size.QuadPart) > SIZE_MAX) throw std::runtime_error("Invalid or unsupported file size.");
    m.size = std::size_t(size.QuadPart);
    m.map = CreateFileMappingW(m.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m.map) m.bytes = static_cast<const std::uint8_t*>(MapViewOfFile(m.map, FILE_MAP_READ, 0, 0, 0));
#elif defined(__EMSCRIPTEN__)
    const auto bytes = std::filesystem::file_size(p);
    web::checkInput(bytes);
    if (bytes < 12) throw std::runtime_error("Invalid audio file size.");
    std::ifstream input(p, std::ios::binary);
    char signature[4]{}; input.read(signature, 4); input.seekg(0);
    if (!std::memcmp(signature, "RIFF", 4)) m.reservation.resize(bytes);
    m.storage = std::make_unique_for_overwrite<std::uint8_t[]>(std::size_t(bytes));
    if (!input.read(reinterpret_cast<char*>(m.storage.get()), std::streamsize(bytes)))
        throw std::runtime_error("Could not read the audio file.");
    m.size = std::size_t(bytes); m.bytes = m.storage.get();
#else
    m.file = ::open(p.c_str(), O_RDONLY);
    struct stat st{};
    if (m.file < 0 || fstat(m.file, &st) || st.st_size < 12 || std::uint64_t(st.st_size) > SIZE_MAX) throw std::runtime_error("Cannot open the audio file.");
    m.size = std::size_t(st.st_size);
    void* v = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.file, 0);
    if (v != MAP_FAILED) m.bytes = static_cast<const std::uint8_t*>(v);
#endif
    if (!m.bytes) throw std::runtime_error("Cannot map this file into memory.");
    a->fileBytes = m.size;
#ifdef __EMSCRIPTEN__
    // The WASM buffer now owns the audio; release the temporary JS filesystem copy.
    if (p.parent_path() == "/imports") std::filesystem::remove(p);
#endif
    if (std::memcmp(m.bytes, "RIFF", 4)) return a->decode(progress, stop);
    a->parse();
#ifdef __EMSCRIPTEN__
    m.reservation.resize(m.size);
#endif
    if (stop.stop_requested()) throw std::runtime_error("Import cancelled.");
    a->peakThread_ = std::jthread([ptr = a.get()](std::stop_token s) { ptr->buildPeaks(s); });
    return a;
}

AudioFile::~AudioFile() {
    peakThread_.request_stop();
    if (peakThread_.joinable()) peakThread_.join();
    mapping_.reset();
    if (!temporaryDirectory_.empty()) { std::error_code ec; std::filesystem::remove(temporaryDirectory_ / "audio.wav", ec); std::filesystem::remove(temporaryDirectory_, ec); }
}
std::uint64_t AudioFile::processedBytes() { return renderedBytes.load(); }
std::uint64_t AudioFile::audioBytes() {
#ifdef __EMSCRIPTEN__
    return web::audioBytes.load();
#else
    return 0;
#endif
}
std::shared_ptr<AudioFile> AudioFile::decode(std::atomic<float>* progress, std::stop_token stop) {
    auto result = std::shared_ptr<AudioFile>(new AudioFile);
    DecodeInfo info;
    std::uint64_t written = 0;
    // The native sink is bounded by RIFF, not by RAM. The browser sink has a
    // reservation shared across all source/effect buffers, history and clipboard.
    constexpr std::uint64_t diskLimit = UINT32_MAX - 44ull;
#ifdef __EMSCRIPTEN__
    result->mapping_ = std::make_unique<Mapping>();
    auto& m = *result->mapping_;
    auto capacity = [&](std::uint64_t bytes) {
        if (bytes <= m.size) return;
        web::checkAudio(bytes, m.reservation.bytes);
        const auto available = web::audioLimit - (web::audioBytes.load() - m.reservation.bytes);
        const auto grow = !m.size && info.frames ? bytes : std::min(available, std::max<std::uint64_t>(65536, m.size + m.size / 2));
        const auto next = std::max(bytes, grow);
        m.reservation.resize(next);
        auto storage = std::make_unique_for_overwrite<std::uint8_t[]>(std::size_t(next));
        if (written) std::memcpy(storage.get(), m.storage.get(), std::size_t(written));
        m.storage = std::move(storage); m.bytes = m.storage.get(); m.size = std::size_t(next);
    };
#else
    struct Temporary {
        std::filesystem::path directory;
        ~Temporary() { if (!directory.empty()) { std::error_code ec; std::filesystem::remove(directory / "audio.wav", ec); std::filesystem::remove(directory, ec); } }
    } temp;
    static std::atomic<unsigned> serial{0};
    for (int attempt = 0; attempt < 32 && temp.directory.empty(); ++attempt) {
        auto candidate = std::filesystem::temp_directory_path() / ("coldedit-decode-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++));
        if (std::filesystem::create_directory(candidate)) temp.directory = candidate;
    }
    if (temp.directory.empty()) throw std::runtime_error("Cannot create temporary decoded audio storage.");
    std::ofstream output(temp.directory / "audio.wav", std::ios::binary);
    auto put16 = [&](unsigned v) { output.put(char(v)); output.put(char(v >> 8)); };
    auto put32 = [&](unsigned v) { put16(v); put16(v >> 16); };
    auto capacity = [&](std::uint64_t bytes) {
        if (bytes > diskLimit) throw std::runtime_error("Decoded audio exceeds the 4 GiB temporary audio limit. Open a shorter recording.");
    };
#endif
    decodeAudio(mapping_->bytes, mapping_->size, [&](const DecodeInfo& metadata) {
        info = metadata;
        if (info.frames > diskLimit / (info.channels * 4)) {
#ifdef __EMSCRIPTEN__
            web::checkAudio(web::audioLimit + 1);
#endif
            throw std::runtime_error("Decoded audio exceeds the 4 GiB temporary audio limit. Open a shorter recording.");
        }
        if (info.frames) capacity(info.frames * info.channels * 4);
#ifndef __EMSCRIPTEN__
        output.write("RIFF", 4); put32(0); output.write("WAVEfmt ", 8); put32(16);
        put16(3); put16(info.channels); put32(info.rate); put32(info.rate * info.channels * 4);
        put16(info.channels * 4); put16(32); output.write("data", 4); put32(0);
#endif
    }, [&](const float* samples, unsigned count) {
        const auto bytes = std::uint64_t(count) * info.channels * 4;
        if (bytes > diskLimit - written) throw std::runtime_error("Decoded audio exceeds the 4 GiB temporary audio limit.");
        capacity(written + bytes);
#ifdef __EMSCRIPTEN__
        std::memcpy(m.storage.get() + written, samples, std::size_t(bytes));
#else
        output.write(reinterpret_cast<const char*>(samples), std::streamsize(bytes));
        if (!output) throw std::runtime_error("Cannot write decoded audio. Check temporary disk space.");
#endif
        written += bytes;
    }, progress, stop);
#ifdef __EMSCRIPTEN__
    result->data_ = m.bytes; result->frames = Frame(written / (info.channels * 4));
    result->channels = info.channels; result->sampleRate = info.rate;
    result->bits = 32; result->floating = true; result->blockAlign_ = info.channels * 4;
    result->peakThread_ = std::jthread([ptr = result.get()](std::stop_token s) { ptr->buildPeaks(s); });
#else
    output.seekp(4); put32(unsigned(written + 36)); output.seekp(40); put32(unsigned(written));
    output.flush(); if (!output) throw std::runtime_error("Cannot flush decoded audio."); output.close();
    result = AudioFile::open(temp.directory / "audio.wav", nullptr, stop);
    result->temporaryDirectory_ = temp.directory; temp.directory.clear();
#endif
    result->path = path; result->fileBytes = fileBytes;
    result->format = !std::memcmp(mapping_->bytes, "fLaC", 4) ? "FLAC / " + std::to_string(info.bits) + "-bit" : "MP3";
    return result;
}
std::shared_ptr<AudioFile> AudioFile::render(Frame count, unsigned rate, unsigned channels,
    const std::function<void(Frame, Frame, float*)>& fill, std::atomic<float>* progress, std::stop_token stop) {
    if (count < 0 || channels < 1 || channels > 2 || rate < 1000 || rate > 768000 || std::uint64_t(count) > (UINT32_MAX - 44) / (channels * 4))
        throw std::runtime_error("This processing range is too large. Select a shorter range.");
    const auto bytes = std::uint64_t(count) * channels * 4;
    auto cancelled = [&] { if (stop.stop_requested()) throw std::runtime_error("Processing cancelled."); };
    cancelled();
#ifdef __EMSCRIPTEN__
    auto a = std::shared_ptr<AudioFile>(new AudioFile); a->mapping_ = std::make_unique<Mapping>();
    a->mapping_->reservation.resize(bytes);
    renderedBytes.fetch_add(bytes); a->mapping_->processed = bytes;
    a->mapping_->storage = std::make_unique<std::uint8_t[]>(std::size_t(bytes));
    a->mapping_->bytes = a->mapping_->storage.get(); a->mapping_->size = std::size_t(bytes);
    a->data_ = a->mapping_->bytes; a->frames = count; a->channels = channels; a->sampleRate = rate;
    a->bits = 32; a->floating = true; a->blockAlign_ = channels * 4; a->fileBytes = bytes;
#else
    struct Temporary {
        std::filesystem::path directory, file;
        ~Temporary() { if (!directory.empty()) { std::error_code ec; std::filesystem::remove(file, ec); std::filesystem::remove(directory, ec); } }
    } temp;
    static std::atomic<unsigned> serial{0};
    for (int attempt = 0; attempt < 32 && temp.directory.empty(); ++attempt) {
        const auto candidate = std::filesystem::temp_directory_path() / ("coldedit-process-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++));
        if (std::filesystem::create_directory(candidate)) temp.directory = candidate;
    }
    if (temp.directory.empty()) throw std::runtime_error("Cannot create temporary audio storage.");
    temp.file = temp.directory / "audio.wav";
    std::ofstream output(temp.file, std::ios::binary);
    auto u16 = [&](unsigned v) { output.put(char(v)); output.put(char(v >> 8)); };
    auto u32 = [&](unsigned v) { u16(v); u16(v >> 16); };
    output.write("RIFF", 4); u32(unsigned(bytes + 36)); output.write("WAVEfmt ", 8); u32(16);
    u16(3); u16(channels); u32(rate); u32(rate * channels * 4); u16(channels * 4); u16(32);
    output.write("data", 4); u32(unsigned(bytes));
#endif
    std::array<float, 4096 * 2> block{};
    for (Frame at = 0; at < count; at += 4096) {
        cancelled(); const Frame n = std::min<Frame>(4096, count - at); fill(at, n, block.data());
        for (Frame i = 0; i < n * channels; ++i) if (!std::isfinite(block[std::size_t(i)])) throw std::runtime_error("Processing produced a non-finite sample.");
#ifdef __EMSCRIPTEN__
        std::memcpy(a->mapping_->storage.get() + std::size_t(at) * channels * 4, block.data(), std::size_t(n) * channels * 4);
#else
        output.write(reinterpret_cast<const char*>(block.data()), std::streamsize(n * channels * 4));
        if (!output) throw std::runtime_error("Cannot write processed audio. Check temporary disk space.");
#endif
        if (progress) progress->store(float(double(at + n) / double(std::max<Frame>(1, count))));
    }
    cancelled();
#ifdef __EMSCRIPTEN__
    a->buildPeaks(stop); cancelled();
#else
    output.flush(); if (!output) throw std::runtime_error("Cannot flush processed audio."); output.close();
    auto a = AudioFile::open(temp.file); a->temporaryDirectory_ = temp.directory; temp.directory.clear();
    while (!a->peaksReady() && a->peakProgress() >= 0) {
        if (stop.stop_requested()) { a->peakThread_.request_stop(); cancelled(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    a->waitForPeaks(); cancelled();
#endif
    return a;
}

void AudioFile::parse() {
    const auto* b = mapping_->bytes;
    if (std::memcmp(b, "RIFF", 4) || std::memcmp(b + 8, "WAVE", 4)) throw std::runtime_error("Open an uncompressed RIFF/WAVE file. RF64 and compressed formats are not supported yet.");
    const std::uint64_t end = std::uint64_t(u32(b + 4)) + 8;
    if (end < 12 || end > mapping_->size) throw std::runtime_error("Truncated WAV container.");
    bool formatFound = false, dataFound = false;
    std::uint64_t dataBytes = 0;
    for (std::uint64_t pos = 12; pos + 8 <= end;) {
        const auto* chunk = b + pos;
        const std::uint64_t len = u32(chunk + 4), start = pos + 8;
        if (len > end - start) throw std::runtime_error("Truncated WAV chunk.");
        if (!std::memcmp(chunk, "fmt ", 4) && !formatFound) {
            if (len < 16) throw std::runtime_error("Invalid WAV format chunk.");
            auto tag = u16(b + start);
            channels = u16(b + start + 2);
            sampleRate = u32(b + start + 4);
            blockAlign_ = u16(b + start + 12);
            bits = u16(b + start + 14);
            if (tag == 0xfffe) {
                static constexpr std::uint8_t guidTail[] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                if (len < 40 || u16(b + start + 16) < 22 || std::memcmp(b + start + 26, guidTail, 14)) throw std::runtime_error("Unsupported extensible WAV format.");
                tag = u16(b + start + 24);
                const auto valid = u16(b + start + 18);
                if (valid != 0 && valid != bits) throw std::runtime_error("WAV valid-bit packing is not supported.");
            }
            floating = tag == 3;
            if ((tag != 1 && tag != 3) || (floating ? bits != 32 : bits != 8 && bits != 16 && bits != 24 && bits != 32)) throw std::runtime_error("Use 8/16/24/32-bit PCM or 32-bit float WAV.");
            if (channels < 1 || channels > 2) throw std::runtime_error("This version supports mono and stereo WAV files.");
            if (sampleRate < 1000 || sampleRate > 768000 || blockAlign_ != channels * (bits / 8)) throw std::runtime_error("Invalid WAV sample rate or block alignment.");
            formatFound = true;
        } else if (!std::memcmp(chunk, "data", 4) && !dataFound) {
            data_ = b + start;
            dataBytes = len;
            dataFound = true;
        }
        pos = start + len + (len & 1);
    }
    if (!formatFound || !dataFound) throw std::runtime_error("WAV is missing its format or audio chunk.");
    if (dataBytes % blockAlign_) throw std::runtime_error("Incomplete audio frame in WAV data.");
    frames = Frame(dataBytes / blockAlign_);
}

float AudioFile::sample(Frame f, unsigned c) const noexcept {
    if (f < 0 || f >= frames || c >= channels) return 0;
    const auto* p = data_ + std::size_t(f) * blockAlign_ + c * (bits / 8);
    if (floating) { float v; std::memcpy(&v, p, 4); return std::isfinite(v) ? v : 0.f; }
    if (bits == 8) return (float(p[0]) - 128.f) / 128.f;
    if (bits == 16) return float(std::int16_t(u16(p))) / 32768.f;
    if (bits == 24) { std::int32_t v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v -= 0x1000000; return float(v) / 8388608.f; }
    return float(std::int32_t(u32(p))) / 2147483648.f;
}

void AudioFile::buildPeaks(std::stop_token stop) {
    try {
        const unsigned cacheChannels = peakChannels();
        constexpr std::uint64_t budget = 32 * 1024 * 1024;
        Frame stride = 256;
        while ((std::uint64_t(frames / stride + 1) * cacheChannels * sizeof(PackedPeak) * 16 / 15) > budget) stride *= 2;
        Level base{stride, {}};
        base.values.resize(std::size_t((frames + stride - 1) / stride) * cacheChannels);
        for (Frame f = 0; f < frames; f += stride) {
            if (stop.stop_requested()) return;
            for (unsigned c = 0; c < cacheChannels; ++c) {
                float lo = 0, hi = 0;
                for (Frame j = f; j < std::min(f + stride, frames); ++j) { float v = peakSample(j, c); lo = std::min(lo, v); hi = std::max(hi, v); }
                base.values[std::size_t(f / stride) * cacheChannels + c] = {lo, hi};
            }
            if ((f / stride % 256) == 0) progress_.store(float(double(f) / double(std::max<Frame>(1, frames))));
        }
        levels_.push_back(std::move(base));
        while (levels_.back().values.size() > cacheChannels) {
            if (stop.stop_requested()) return;
            const auto& prev = levels_.back();
            const auto blocks = prev.values.size() / cacheChannels;
            Level next{prev.stride * 16, {}};
            next.values.resize(((blocks + 15) / 16) * cacheChannels);
            for (std::size_t b = 0; b < blocks; b += 16) for (unsigned c = 0; c < cacheChannels; ++c) {
                PackedPeak p{0, 0};
                for (std::size_t j = b; j < std::min(b + 16, blocks); ++j) { const auto v = prev.values[j * cacheChannels + c]; p.lo = std::min(p.lo, v.lo); p.hi = std::max(p.hi, v.hi); }
                next.values[(b / 16) * cacheChannels + c] = p;
            }
            levels_.push_back(std::move(next));
        }
        std::size_t bytes = 0;
        for (const auto& l : levels_) bytes += l.values.size() * sizeof(PackedPeak);
        peakBytes_.store(bytes);
        progress_.store(1);
        ready_.store(true, std::memory_order_release);
    } catch (...) { progress_.store(-1); } // Raw-detail view remains usable if cache allocation fails.
}

void AudioFile::waitForPeaks() { if (peakThread_.joinable()) peakThread_.join(); }

Peak AudioFile::peak(Frame first, Frame last, unsigned c) const noexcept {
    return c < channels ? cachedPeak(first, last, c) : Peak{};
}
float AudioFile::peakSample(Frame f, unsigned c) const noexcept {
    if (c < 2) return sample(f, c);
    return float((double(sample(f, 0)) + (c == 2 ? 1 : -1) * double(sample(f, 1))) * .5);
}
Peak AudioFile::mixPeak(Frame first, Frame last, double left, double right) const noexcept {
    if (channels == 1) right = 0;
    unsigned c = 0; double gain = left;
    if (left == 0) { c = 1; gain = right; }
    else if (right != 0 && std::abs(left) == std::abs(right)) { c = left == right ? 2 : 3; gain = left * 2; }
    else if (right != 0) {
        auto a = mixPeak(first, last, left, 0), b = mixPeak(first, last, 0, right);
        return {a.lo + b.lo, a.hi + b.hi};
    }
    if (!gain) return {};
    auto p = cachedPeak(first, last, c); if (gain < 0) std::swap(p.lo, p.hi);
    return {float(p.lo * gain), float(p.hi * gain)};
}
Peak AudioFile::cachedPeak(Frame first, Frame last, unsigned c) const noexcept {
    first = std::clamp(first, Frame(0), frames); last = std::clamp(last, first, frames);
    Peak result{};
    if (c >= peakChannels() || first == last) return result;
    if (peaksReady() && !levels_.empty() && last - first >= levels_.front().stride) {
        const Level* level = &levels_.front();
        for (const auto& l : levels_) { if (l.stride > last - first) break; level = &l; }
        for (Frame b = first / level->stride; b <= (last - 1) / level->stride; ++b) {
            const auto p = level->values[std::size_t(b) * peakChannels() + c];
            result.lo = std::min(result.lo, p.lo); result.hi = std::max(result.hi, p.hi);
        }
    } else if (last - first <= 4096) {
        for (Frame f = first; f < last; ++f) { float v = peakSample(f, c); result.lo = std::min(result.lo, v); result.hi = std::max(result.hi, v); }
    }
    // Never scan an entire large file on the UI thread while peaks are building.
    return result;
}
}
