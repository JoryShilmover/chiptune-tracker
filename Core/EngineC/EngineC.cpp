#include "tracker_engine.h"

#include <tracker/DemoSong.hpp>
#include <tracker/Engine.hpp>

#include <algorithm>
#include <memory>

struct TEEngine {
    tracker::Engine engine;
};

double te_output_sample_rate(void) { return dsspu::kOutputSampleRate; }

TEEngine* te_engine_create_demo(void) {
    return new TEEngine{tracker::Engine(std::make_unique<tracker::DemoSong>())};
}

void te_engine_destroy(TEEngine* engine) { delete engine; }

void te_engine_play(TEEngine* engine) { engine->engine.play(); }

void te_engine_stop(TEEngine* engine) { engine->engine.stop(); }

void te_engine_set_tick_rate(TEEngine* engine, double hz) { engine->engine.setTickRate(hz); }

void te_engine_reset_stats(TEEngine* engine) { engine->engine.resetStats(); }

void te_engine_status(TEEngine* engine, TEStatus* out) {
    const tracker::EngineStatus s = engine->engine.status();
    out->playing = s.playing;
    out->tick = s.tick;
    out->step = s.step;
    out->tick_rate = s.tickRate;
    out->peak_left = s.peakLeft;
    out->peak_right = s.peakRight;
    std::copy(s.channelLevels.begin(), s.channelLevels.end(), out->channel_levels);
    out->render_calls = s.renderCalls;
    out->frames_rendered = s.framesRendered;
    out->last_frame_count = s.lastFrameCount;
    out->last_render_us = s.lastRenderMicros;
    out->max_render_us = s.maxRenderMicros;
}

void te_engine_render(TEEngine* engine, float* left, float* right, uint32_t frames) {
    engine->engine.render(left, right, frames);
}
