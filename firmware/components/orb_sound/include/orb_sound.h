#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stable palette IDs are also the signed ORBSOUND section directory IDs.
 * Every cue accompanies visible feedback (hig.md §8). Calls never allocate,
 * perform I2C/NVS/flash access, or wait for the audio worker. */
typedef enum {
    ORB_SOUND_DETENT, ORB_SOUND_TAP, ORB_SOUND_OPEN, ORB_SOUND_CLOSE,
    ORB_SOUND_NEEDS_YOU, ORB_SOUND_DONE, ORB_SOUND_SENT,
    ORB_SOUND_LISTENING_START, ORB_SOUND_LISTENING_STOP, ORB_SOUND_FAILED,
    ORB_SOUND_HOME_ON, ORB_SOUND_HOME_OFF, ORB_SOUND_TIMER, ORB_SOUND_ALARM,
    ORB_SOUND_GAME_ROLL, ORB_SOUND_GAME_BEAD, ORB_SOUND_GAME_POP,
    ORB_SOUND_GAME_SWITCH, ORB_SOUND_GAME_COLLECT,
    ORB_SOUND_CUE_COUNT,
    /* Source-compatible names for the existing shell's semantic cues. */
    ORB_SOUND_APPROVE = ORB_SOUND_DONE, ORB_SOUND_DENY = ORB_SOUND_FAILED,
    ORB_SOUND_NOTIFICATION = ORB_SOUND_NEEDS_YOU,
    ORB_SOUND_START = ORB_SOUND_LISTENING_START, ORB_SOUND_STOP = ORB_SOUND_LISTENING_STOP,
    ORB_SOUND_SEND = ORB_SOUND_SENT, ORB_SOUND_ALL_CLEAR = ORB_SOUND_DONE,
    ORB_SOUND_TICK = ORB_SOUND_DETENT, ORB_SOUND_NAV_OPEN = ORB_SOUND_OPEN,
} orb_sound_cue_t;

bool orb_sound_init(void);
/* Main service owner only, before UI startup. Flash writes require its internal
 * stack; the audio/UI request paths never perform persistence work. */
void orb_sound_preferences_init(void);
/* Debounced persistence; returns the next absolute monotonic deadline, or
 * INT64_MAX when idle. Call outside the LVGL lock on the same main owner. */
int64_t orb_sound_preferences_service(int64_t now_us);
bool orb_sound_play(orb_sound_cue_t cue);
/* A user-requested single sample at current volume. Alerts do not loop/ramp;
 * previews respect Silent/Quiet and never replace a real ringing alert. */
bool orb_sound_preview(orb_sound_cue_t cue);
void orb_sound_stop(orb_sound_cue_t cue); /* timer/alarm continue until stopped */
void orb_sound_stop_all(void);
void orb_sound_set_volume(unsigned volume); /* clamps 0..100, persisted asynchronously */
unsigned orb_sound_volume(void);
void orb_sound_set_silent(bool silent); /* owner's choice, persisted asynchronously */
void orb_sound_set_always_on(bool always_on); /* every sound suppressed */
void orb_sound_set_muted(bool silent, bool face_down); /* transient shell/power policy */

typedef struct {
    unsigned volume;
    bool quiet_enabled;
    bool spoken_replies; /* Local speaker opt-out; captions remain available. */
    bool silent;
    unsigned quiet_start, quiet_end;
    bool saving, save_error;
} orb_sound_preferences_t;
void orb_sound_preferences(orb_sound_preferences_t *preferences);
bool orb_sound_set_preferences(const orb_sound_preferences_t *preferences);
bool orb_sound_quiet_at(bool enabled, unsigned start, unsigned end, int local_hour);

/* One bounded pull per 5 ms; the speech service resamples to output_rate.
 * Register once at service initialization, before setting active. */
typedef size_t (*orb_sound_speech_source_t)(void *ctx, int16_t *pcm, size_t capacity,
                                           uint32_t output_rate);
typedef void (*orb_sound_mic_sink_t)(void *ctx, const int16_t *stereo, size_t frames,
                                    uint32_t rate, float playback_envelope);
void orb_sound_set_speech_source(orb_sound_speech_source_t source, void *ctx);
void orb_sound_set_mic_sink(orb_sound_mic_sink_t sink, void *ctx);
void orb_sound_set_speech_active(bool active);
bool orb_sound_speech_allowed(void);

/* Recording still has exclusive priority over playback. Worker closes duplex
 * before handing over the lease; capture must release on every failure path. */
bool orb_sound_audio_acquire(void);
void orb_sound_audio_release(void);

typedef struct {
    bool ready, silent, face_down, always_on;
    uint32_t asset_bytes, queued_count, started_count, dropped_count;
    uint32_t last_start_us, max_start_us;
} orb_sound_stats_t;
/* Start measures accepted call -> first codec submission, not acoustic onset.
 * Capture an electrical/acoustic trace on the board to validate total latency. */
void orb_sound_stats(orb_sound_stats_t *stats);
