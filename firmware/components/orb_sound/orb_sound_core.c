#include "orb_sound_core.h"

#include <limits.h>
#include <string.h>

/* Stop previewed alerts without cancelling real timers when Silent changes. */
#define STOP_PREVIEWS (1u << ORB_SOUND_CUE_COUNT)

bool orb_sound_is_alert(orb_sound_cue_t cue)
{
    return cue == ORB_SOUND_TIMER || cue == ORB_SOUND_ALARM;
}

void orb_sound_core_init(orb_sound_core_t *core)
{
    memset(core, 0, sizeof(*core));
    atomic_init(&core->muted, false); atomic_init(&core->hard_muted, false);
    atomic_init(&core->lease, false); atomic_init(&core->capture_waiting, false);
    atomic_init(&core->capture_active, false);
    atomic_init(&core->stop_mask, 0); atomic_init(&core->sequence, 0);
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        atomic_init(&core->pending[i], -1);
        atomic_init(&core->pending_at_us[i], 0);
    }
}

void orb_sound_core_mute(orb_sound_core_t *core, bool muted)
{
    atomic_store(&core->muted, muted);
    if (muted) {
        unsigned ordinary = ((1u << ORB_SOUND_CUE_COUNT) - 1) &
                            ~((1u << ORB_SOUND_TIMER) | (1u << ORB_SOUND_ALARM));
        atomic_fetch_or(&core->stop_mask, ordinary | STOP_PREVIEWS);
    }
}

bool orb_sound_core_try_lease(orb_sound_core_t *core)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&core->lease, &expected, true);
}

void orb_sound_core_release(orb_sound_core_t *core)
{
    atomic_store(&core->lease, false);
}

bool orb_sound_core_queue(orb_sound_core_t *core, orb_sound_cue_t cue)
{
    return orb_sound_core_queue_at(core, cue, 0);
}

static bool queue_at(orb_sound_core_t *core, orb_sound_cue_t cue, uint32_t now_us, bool preview)
{
    if (cue < 0 || cue >= ORB_SOUND_CUE_COUNT || atomic_load(&core->hard_muted) ||
        (atomic_load(&core->muted) && (preview || !orb_sound_is_alert(cue))) ||
        atomic_load(&core->capture_waiting) || atomic_load(&core->capture_active)) return false;
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        int expected = -1;
        if (atomic_compare_exchange_strong(&core->pending[i], &expected, -2)) {
            atomic_store(&core->pending_at_us[i], now_us);
            atomic_store(&core->pending[i], cue + (preview ? ORB_SOUND_CUE_COUNT : 0));
            return true;
        }
    }
    return false;
}

bool orb_sound_core_queue_at(orb_sound_core_t *core, orb_sound_cue_t cue, uint32_t now_us)
{
    return queue_at(core, cue, now_us, false);
}

bool orb_sound_core_preview_at(orb_sound_core_t *core, orb_sound_cue_t cue, uint32_t now_us)
{
    return queue_at(core, cue, now_us, true);
}

void orb_sound_core_stop(orb_sound_core_t *core, orb_sound_cue_t cue)
{
    if (cue >= 0 && cue < ORB_SOUND_CUE_COUNT) atomic_fetch_or(&core->stop_mask, 1u << cue);
}

size_t orb_sound_core_render(orb_sound_core_t *core, int16_t *pcm, size_t samples, bool duck)
{
    if (!pcm || samples > ORB_SOUND_CHUNK_SAMPLES) return 0;
    memset(pcm, 0, samples * sizeof(*pcm));
    unsigned stopped = atomic_exchange(&core->stop_mask, 0);
    bool hard = atomic_load(&core->hard_muted) || atomic_load(&core->capture_waiting) ||
                atomic_load(&core->capture_active);
    bool muted = atomic_load(&core->muted);
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        orb_sound_voice_t *voice = &core->voices[i];
        voice->began_this_block = false;
        if (hard || (voice->active && ((stopped & (1u << voice->cue)) ||
            (voice->preview && (stopped & STOP_PREVIEWS)) ||
            (muted && (voice->preview || !orb_sound_is_alert(voice->cue)))))) voice->active = false;
    }
    for (unsigned request = 0; request < ORB_SOUND_VOICES; request++) {
        int cue = atomic_load(&core->pending[request]);
        if (cue < 0) continue; /* -2 is a producer publishing its timestamp. */
        bool preview = cue >= ORB_SOUND_CUE_COUNT;
        if (preview) cue -= ORB_SOUND_CUE_COUNT;
        uint32_t queued_at_us = atomic_load(&core->pending_at_us[request]);
        atomic_store(&core->pending[request], -1);
        if (cue < 0 || hard || (stopped & (1u << cue)) ||
            (preview && (stopped & STOP_PREVIEWS)) || (muted && (preview || !orb_sound_is_alert(cue)))) continue;
        if (preview) {
            for (unsigned i = 0; i < ORB_SOUND_VOICES; i++)
                if (core->voices[i].preview) core->voices[i].active = false;
        }
        unsigned slot = 0;
        if (core->voices[0].active && !core->voices[1].active) slot = 1;
        else if (core->voices[0].active && core->voices[1].active) {
            /* Detents never evict a ringing timer. An alert may replace a cue. */
            if (orb_sound_is_alert(core->voices[0].cue) && !core->voices[0].preview) slot = 1;
            if (orb_sound_is_alert(core->voices[slot].cue) && !core->voices[slot].preview &&
                (preview || !orb_sound_is_alert(cue))) continue;
        }
        core->voices[slot] = (orb_sound_voice_t){.cue = cue, .active = true,
            .began_this_block = true, .preview = preview, .queued_at_us = queued_at_us,
            .variation = atomic_fetch_add(&core->sequence, 1) % 3};
    }
    int32_t mix[ORB_SOUND_CHUNK_SAMPLES] = {0};
    int16_t scratch[ORB_SOUND_CHUNK_SAMPLES];
    size_t active = 0;
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        orb_sound_voice_t *voice = &core->voices[i];
        if (!voice->active) continue;
        active++;
        size_t written = 0;
        while (written < samples) {
            size_t n = orb_sound_pcm_variant(voice->cue, voice->offset, scratch,
                                              samples - written, voice->variation);
            if (!n) { voice->active = false; break; }
            for (size_t j = 0; j < n; j++) {
                /* Two full-peak palette sources have 6 dB headroom each.
                 * Alerts fade from -40 dB to their desk level over 20 seconds. */
                int32_t gain = 16384;
                if (orb_sound_is_alert(voice->cue) && !voice->preview) {
                    uint64_t age = voice->age + j;
                    uint64_t ramp = 20u * ORB_SOUND_SAMPLE_RATE;
                    gain = age >= ramp ? gain : (int32_t)(164 + age * 16220 / ramp);
                }
                if (duck) gain = gain * 3277 / 16384; /* -14 dB, including alerts */
                mix[written + j] += (int32_t)scratch[j] * gain / 32768;
            }
            written += n; voice->offset += n; voice->age += n;
            if (voice->offset == orb_sound_pcm_count(voice->cue)) {
                if (orb_sound_is_alert(voice->cue) && !voice->preview) voice->offset = 0;
                else { voice->active = false; break; }
            }
        }
    }
    for (size_t i = 0; i < samples; i++)
        pcm[i] = mix[i] > INT16_MAX ? INT16_MAX : mix[i] < INT16_MIN ? INT16_MIN : (int16_t)mix[i];
    return active;
}
