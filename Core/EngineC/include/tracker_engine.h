#ifndef TRACKER_ENGINE_H
#define TRACKER_ENGINE_H

// C interface to the tracker engine, for Swift. See tracker/Engine.hpp for the
// threading rules: te_engine_render is for the audio thread only; everything
// else is for a single control thread.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TE_CHANNEL_COUNT 16

typedef struct TEEngine TEEngine;

typedef struct {
    bool playing;
    uint32_t tick;
    uint32_t step;
    double tick_rate;
    float peak_left;   // 0-1 since the previous status call
    float peak_right;
    float channel_levels[TE_CHANNEL_COUNT];
    uint64_t render_calls;
    uint64_t frames_rendered;
    uint32_t last_frame_count;
    double last_render_us;
    double max_render_us;
} TEStatus;

// The engine's native output rate (~32,728.5 Hz).
double te_output_sample_rate(void);

// Creates an engine playing the built-in demo song.
TEEngine* te_engine_create_demo(void);
void te_engine_destroy(TEEngine* engine);

void te_engine_play(TEEngine* engine);
void te_engine_stop(TEEngine* engine);
void te_engine_set_tick_rate(TEEngine* engine, double hz);
void te_engine_reset_stats(TEEngine* engine);
void te_engine_status(TEEngine* engine, TEStatus* out);

// Audio thread only. Writes `frames` samples to each of `left` and `right`.
void te_engine_render(TEEngine* engine, float* left, float* right, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
