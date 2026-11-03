#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "orb_sound.h"

#define ORB_SOUND_SAMPLE_RATE 32000u
#define ORB_SOUND_CHUNK_SAMPLES 160u
#define ORB_SOUND_VOICES 2u

typedef struct {
    orb_sound_cue_t cue;
    size_t offset;
    uint64_t age;
    bool active, began_this_block, preview;
    uint32_t queued_at_us;
    unsigned variation;
} orb_sound_voice_t;

typedef struct {
    atomic_bool muted, hard_muted, lease, capture_waiting, capture_active;
    atomic_int pending[ORB_SOUND_VOICES];
    atomic_uint pending_at_us[ORB_SOUND_VOICES];
    atomic_uint stop_mask, sequence;
    orb_sound_voice_t voices[ORB_SOUND_VOICES]; /* audio worker only */
} orb_sound_core_t;

void orb_sound_core_init(orb_sound_core_t *core);
void orb_sound_core_mute(orb_sound_core_t *core, bool muted);
bool orb_sound_core_queue(orb_sound_core_t *core, orb_sound_cue_t cue);
bool orb_sound_core_queue_at(orb_sound_core_t *core, orb_sound_cue_t cue, uint32_t now_us);
bool orb_sound_core_preview_at(orb_sound_core_t *core, orb_sound_cue_t cue, uint32_t now_us);
void orb_sound_core_stop(orb_sound_core_t *core, orb_sound_cue_t cue);
size_t orb_sound_core_render(orb_sound_core_t *core, int16_t *pcm, size_t samples,
                             bool duck);
bool orb_sound_core_try_lease(orb_sound_core_t *core);
void orb_sound_core_release(orb_sound_core_t *core);
bool orb_sound_is_alert(orb_sound_cue_t cue);

/* Immutable authenticated PCM section. Bind validates every record first.
 * Host tests may bind a generated fixture; production copies signed PSRAM. */
bool orb_sound_pcm_init(void);
size_t orb_sound_pcm_bytes(void);
bool orb_sound_pcm_bind(const void *data, size_t bytes);
size_t orb_sound_pcm_count(orb_sound_cue_t cue);
size_t orb_sound_pcm(orb_sound_cue_t cue, size_t offset, int16_t *pcm, size_t capacity);
size_t orb_sound_pcm_variant(orb_sound_cue_t cue, size_t offset, int16_t *pcm,
                             size_t capacity, unsigned sequence);
