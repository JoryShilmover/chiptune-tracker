#pragma once

#include "tracker/Song.hpp"

#include <vector>

namespace tracker {

// A hardcoded 32-step demo that exercises every channel type: a PSG lead with
// envelope and delayed vibrato, a PSG arpeggio, a looping PCM8 saw bass, an
// ADPCM kick, and noise hi-hats and snare. Stands in for real songs until the
// sequencer exists (Milestone 2).
class DemoSong final : public Song {
public:
    static constexpr uint32_t kTicksPerStep = 7;  // ~128 BPM in 16th notes at 60 Hz
    static constexpr uint32_t kLoopSteps = 32;

    // Which parts play; used by spu-render to solo demos.
    struct Parts {
        bool psg = true;
        bool noise = true;
        bool pcm = true;
        bool adpcm = true;
    };

    DemoSong();
    explicit DemoSong(Parts parts);

    std::span<const uint8_t> sampleMemory() const override { return memory_; }
    void reset(dsspu::Spu& spu) override;
    void tick(dsspu::Spu& spu, uint32_t tick) override;
    uint32_t ticksPerStep() const override { return kTicksPerStep; }

private:
    void lead(dsspu::Spu& spu, uint32_t tick);
    void arpeggio(dsspu::Spu& spu, uint32_t tick);
    void bass(dsspu::Spu& spu, uint32_t tick);
    void drums(dsspu::Spu& spu, uint32_t tick);

    Parts parts_;
    std::vector<uint8_t> memory_;
    uint32_t sawAddress_ = 0;
    uint32_t sawWords_ = 0;
    uint32_t kickAddress_ = 0;
    uint32_t kickWords_ = 0;

    int leadNote_ = 69;
    int leadAge_ = 1000;
    int bassAge_ = 1000;
    int hatAge_ = 1000;
    int snareAge_ = 1000;
};

}  // namespace tracker
