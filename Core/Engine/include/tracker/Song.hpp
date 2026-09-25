#pragma once

#include <dsspu/Spu.hpp>

#include <cstdint>
#include <span>

namespace tracker {

// Where song sample data is mapped in the SPU's address space (DS main RAM).
inline constexpr uint32_t kSampleMemoryBase = 0x02000000;

// Something the engine can play. A song drives the SPU only through register
// writes, once per tick, exactly as a DS sound driver would.
//
// reset() and tick() run on the real-time audio thread: they must not
// allocate, lock or block.
class Song {
public:
    virtual ~Song() = default;

    // Sample data, mapped at kSampleMemoryBase. Must stay valid and unchanged
    // for the song's lifetime.
    virtual std::span<const uint8_t> sampleMemory() const = 0;

    // Called when playback starts, after the SPU has been reset.
    virtual void reset(dsspu::Spu& spu) = 0;

    // Called once per tick while playing.
    virtual void tick(dsspu::Spu& spu, uint32_t tick) = 0;

    // Ticks per sequencer step, for position display.
    virtual uint32_t ticksPerStep() const = 0;
};

}  // namespace tracker
