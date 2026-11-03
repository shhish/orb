#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float dc[2], low[2], noise, gain, echo_coupling;
    uint32_t playback_ms, excess_ms;
    uint32_t warm_samples; /* output samples since init, for the start fade */
} mic_dsp_t;

void mic_dsp_init(mic_dsp_t *s);
/* Light adaptive noise suppression after both channels' DC/treble filtering.
 * Output has frames samples, preserving the caller's 16 kHz upstream clock. */
void mic_dsp_mono(mic_dsp_t *s, const int16_t *stereo, int16_t *mono, size_t frames, uint32_t rate);
/* Fallback only: sustained energy above learned playback-envelope coupling.
 * Short typing transients do not interrupt, and quiet echo learns the coupling. */
bool mic_dsp_interrupt(mic_dsp_t *s, const int16_t *stereo, size_t frames,
                       uint32_t rate, float playback_envelope, bool playing);
