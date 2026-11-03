#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPEECH_RING_SAMPLES 12000u
#define SPEECH_CAPTION_BYTES 1025u

typedef enum {
    SPEECH_COMPLETE, SPEECH_STOP, SPEECH_DOUBLE_TAP, SPEECH_TALK, SPEECH_SILENT,
    SPEECH_INTERRUPTED, SPEECH_UNDERRUN, SPEECH_OVERFLOW, SPEECH_BAD_AUDIO,
} speech_stop_t;

typedef struct {
    int16_t pcm[SPEECH_RING_SAMPLES];
    char caption[SPEECH_CAPTION_BYTES];
    uint32_t rate, next_sequence, phase;
    uint16_t stream, last_stream;
    size_t head, count;
    uint64_t progress_ms;
    bool active, playing, ended, stopped;
    speech_stop_t reason;
    uint32_t underruns;
} speech_core_t;

void speech_core_init(speech_core_t *s);
/* Nonempty, complete UTF-8 for bounded untrusted caption/voice fields. */
bool speech_text_valid(const char *text, size_t max_bytes);
bool speech_core_begin(speech_core_t *s, uint16_t stream, uint32_t rate,
                       const char *caption, uint64_t now);
bool speech_core_packet(speech_core_t *s, const uint8_t *frame, size_t bytes, uint64_t now);
bool speech_core_end(speech_core_t *s, uint16_t stream, uint32_t chunks);
void speech_core_stop(speech_core_t *s, speech_stop_t reason);
/* The service polls deadlines even when codec failure prevents audio pulls. */
void speech_core_tick(speech_core_t *s, uint64_t now);
/* Single-owner core; caller serializes packets and audio pulls. No allocation. */
size_t speech_core_read(speech_core_t *s, int16_t *out, size_t samples,
                        uint32_t output_rate, uint64_t now);
const char *speech_stop_name(speech_stop_t reason);
