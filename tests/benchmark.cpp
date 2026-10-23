#include "document.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif
using namespace coldedit;
using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static std::uint64_t privateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX p{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), sizeof(p)); return p.PrivateUsage;
#else
    return 0;
#endif
}
int main() {
    const auto path = std::filesystem::current_path() / ("coldedit-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()) + ".wav");
    try {
        constexpr std::uint32_t bytes = 256 * 1024 * 1024;
        {
            std::ofstream f(path, std::ios::binary);
            auto u16 = [&](unsigned x) { f.put(char(x)); f.put(char(x >> 8)); };
            auto u32 = [&](unsigned x) { u16(x); u16(x >> 16); };
            f.write("RIFF", 4); u32(bytes + 36); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
            u32(48000); u32(192000); u16(4); u16(16); f.write("data", 4); u32(bytes);
            f.seekp(44LL + bytes - 1); f.put(0);
            for (std::int64_t pos = 0; pos < bytes; pos += 64 * 1024) { f.seekp(44 + pos); u16(24576); u16(8192); }
            if (!f) throw std::runtime_error("Cannot write benchmark fixture.");
        }
        const auto memoryBefore = privateBytes();
        auto start = Clock::now(); auto source = AudioFile::open(path); const auto openMs = elapsed(start);
        source->waitForPeaks(); const auto peakMs = elapsed(start);
        Document doc; doc.open(source);
        for (int i = 0; i < 128; ++i) doc.effect(i * 10000, i * 10000 + 5000, Effect::Reverse);
        const auto memoryAfter = privateBytes();
        std::vector<double> timings; float checksum = 0;
        for (int view = 0; view < 250; ++view) {
            const double span = double(source->frames) * std::pow(.96, view % 230);
            const double left = (source->frames - span) * ((view % 31) / 31.);
            const auto now = Clock::now();
            for (int x = 0; x < 1600; ++x) for (unsigned c = 0; c < 2; ++c) {
                const auto p = doc.timeline()->peak(Frame(left + span * x / 1600), Frame(std::ceil(left + span * (x + 1) / 1600)), c); checksum += p.hi;
            }
            timings.push_back(elapsed(now));
        }
        std::sort(timings.begin(), timings.end());
        std::cout << "{\n  \"source_bytes\": " << bytes
                  << ",\n  \"source_duration_minutes\": " << double(source->frames) / 48000 / 60
                  << ",\n  \"map_and_parse_ms\": " << openMs
                  << ",\n  \"peaks_complete_ms\": " << peakMs
                  << ",\n  \"peak_cache_bytes\": " << source->peakBytes()
                  << ",\n  \"private_bytes_before\": " << memoryBefore
                  << ",\n  \"private_bytes_after_128_edits\": " << memoryAfter
                  << ",\n  \"query_1600_columns_stereo_ms_median\": " << timings[timings.size() / 2]
                  << ",\n  \"query_1600_columns_stereo_ms_p95\": " << timings[std::size_t(timings.size() * .95)]
                  << ",\n  \"checksum\": " << checksum << "\n}\n";
        doc = Document{}; source.reset(); std::filesystem::remove(path); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; std::error_code ec; std::filesystem::remove(path, ec); return 1; }
}
