#include "recording.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace coldedit;
static void check(bool value) { if (!value) throw std::runtime_error("Recording check failed"); }
int main() {
    try {
        std::vector<float> signal(60001);
        for (std::size_t i = 0; i < signal.size(); ++i) signal[i] = float(std::sin(double(i) * .13) * .8);
        for (int iteration = 0; iteration < 4; ++iteration) {
            MicrophoneRecording capture(true);
            // Arbitrary device periods, including a short final block.
            for (unsigned at = 0; at < signal.size();) {
                const auto n = std::min(777u, unsigned(signal.size()) - at);
                capture.capture(signal.data() + at, n); at += n;
            }
            auto audio = capture.finish();
            check(!capture.failed()); check(capture.frames() == Frame(signal.size()));
            check(audio->channels == 1 && audio->sampleRate == 48000 && audio->frames == Frame(signal.size()));
            for (Frame i = 0; i < audio->frames; ++i) check(audio->sample(i, 0) == signal[std::size_t(i)]);
        }
        // Overrun ends the take without dropping samples in the retained prefix.
        MicrophoneRecording overflow(true);
        overflow.capture(signal.data(), 1000);
        overflow.capture(signal.data(), 131073); // Rejected before any input access.
        auto prefix = overflow.finish();
        check(overflow.failed() && !overflow.error().empty()); check(prefix->frames == 1000);
        for (Frame i = 0; i < 1000; ++i) check(prefix->sample(i, 0) == signal[std::size_t(i)]);
        // Cancellation joins the writer safely, including with queued samples.
        { MicrophoneRecording cancelled(true); cancelled.capture(signal.data(), unsigned(signal.size())); }
        std::cout << "PASS: capture periods, stop/drain, prefix on overrun, repeated sessions and cancellation\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
