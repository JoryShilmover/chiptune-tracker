// spu-render: renders the demo song through the engine to a WAV file.
//
// Uses the same engine and song the app plays in real time, rendered offline.

#include "Wav.hpp"

#include <tracker/DemoSong.hpp>
#include <tracker/Engine.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

void usage() {
    std::puts(
        "usage: spu-render [demo] [-o output.wav] [--seconds N] [--16bit]\n"
        "\n"
        "demos: all (default), psg, noise, pcm, adpcm\n"
        "  --16bit  skip the DS's 10-bit output stage for a clean reference render");
}

}  // namespace

int main(int argc, char** argv) {
    std::string demo = "all";
    std::string output;
    // The 32-step loop, twice.
    double seconds = 2.0 * tracker::DemoSong::kLoopSteps * tracker::DemoSong::kTicksPerStep /
                     tracker::Engine::kDefaultTickRate;
    bool degrade = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--16bit") {
            degrade = false;
        } else if (arg[0] != '-') {
            demo = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (output.empty()) output = "demo-" + demo + ".wav";

    const bool all = demo == "all";
    const tracker::DemoSong::Parts parts{
        .psg = all || demo == "psg",
        .noise = all || demo == "noise",
        .pcm = all || demo == "pcm",
        .adpcm = all || demo == "adpcm",
    };
    if (!parts.psg && !parts.noise && !parts.pcm && !parts.adpcm) {
        std::fprintf(stderr, "unknown demo: %s\n", demo.c_str());
        usage();
        return 2;
    }

    tracker::Engine engine(std::make_unique<tracker::DemoSong>(parts));
    engine.setDegradeTo10Bit(degrade);
    engine.play();

    std::vector<int16_t> audio(size_t(seconds * dsspu::kOutputSampleRate) * 2);
    engine.render(audio.data(), audio.size() / 2);

    // WAV needs an integer rate; 32728 Hz vs the true ~32728.5 Hz is a
    // 0.0015% pitch difference.
    if (!writeWav(output, audio, uint32_t(dsspu::kOutputSampleRate))) {
        std::fprintf(stderr, "failed to write %s\n", output.c_str());
        return 1;
    }
    std::printf("wrote %s (%.1f s, %u Hz, %s output)\n", output.c_str(), seconds,
                uint32_t(dsspu::kOutputSampleRate), degrade ? "10-bit" : "16-bit");
    return 0;
}
