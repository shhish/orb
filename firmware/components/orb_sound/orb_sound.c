#include "orb_sound.h"
#include "orb_sound_core.h"
#include "orb_sound_save.h"
#include <limits.h>
#include <math.h>
#include <time.h>

/* Capture may acquire before the UI starts this service. Never reset a live
 * lease at init: static atomics are valid for the entire process lifetime. */
static orb_sound_core_t s_core = {
    .pending = {ATOMIC_VAR_INIT(-1), ATOMIC_VAR_INIT(-1)},
};
static atomic_int s_started;
static atomic_uint s_preferences = ATOMIC_VAR_INIT(60u | (22u << 8) | (7u << 13));
static atomic_uint s_preference_generation, s_saved_generation, s_failed_generation;
static atomic_bool s_silent, s_face_down, s_always_on, s_speech_active;
static _Atomic(orb_sound_speech_source_t) s_speech_source;
static _Atomic(orb_sound_mic_sink_t) s_mic_sink;
static _Atomic(void *) s_speech_context, s_mic_context;
static atomic_uint s_queued_count, s_started_count, s_dropped_count, s_last_start_us, s_max_start_us;
static atomic_uint s_asset_bytes;
static atomic_bool s_output_ready, s_quiet;

#define PREFERENCE_SILENT (1u << 19)

bool orb_sound_quiet_at(bool enabled, unsigned start, unsigned end, int hour)
{
    if (!enabled || hour < 0 || hour > 23 || start > 23 || end > 23) return false;
    if (start == end) return true;
    return start < end ? (unsigned)hour >= start && (unsigned)hour < end :
                        (unsigned)hour >= start || (unsigned)hour < end;
}

static bool quiet(void)
{
    unsigned p = atomic_load(&s_preferences);
    time_t now = time(NULL);
    struct tm local;
    int hour = now >= 1577836800 && localtime_r(&now, &local) ? local.tm_hour : -1;
    return orb_sound_quiet_at(p & 128u, (p >> 8) & 31u, (p >> 13) & 31u, hour);
}

static void refresh_mute(void)
{
#ifndef ESP_PLATFORM
    atomic_store(&s_quiet, quiet());
#endif
    /* Face-down and Always On are quiet states, not "never ring": like Silent
     * and Quiet Hours they drop ordinary cues but still let a timer or alarm
     * through (requirement 10.1). Only volume zero silences everything. */
    orb_sound_core_mute(&s_core, atomic_load(&s_silent) || atomic_load(&s_quiet) ||
                                 atomic_load(&s_face_down) || atomic_load(&s_always_on));
    bool hard = !orb_sound_volume();
    atomic_store(&s_core.hard_muted, hard);
    if (hard) orb_sound_stop_all();
}

void orb_sound_preferences(orb_sound_preferences_t *p)
{
    if (!p) return;
    unsigned packed = atomic_load(&s_preferences);
    unsigned generation = atomic_load(&s_preference_generation);
    *p = (orb_sound_preferences_t){.volume = packed & 127u, .quiet_enabled = packed & 128u,
        .spoken_replies = !(packed & (1u << 18)),
        .silent = (packed & PREFERENCE_SILENT) != 0,
        .quiet_start = (packed >> 8) & 31u, .quiet_end = (packed >> 13) & 31u,
        .saving = generation != atomic_load(&s_saved_generation),
        .save_error = generation != 0 && generation == atomic_load(&s_failed_generation)};
}

void orb_sound_save_snapshot(orb_sound_save_snapshot_t *out)
{
    out->generation = atomic_load(&s_preference_generation);
    out->completed = atomic_load(&s_saved_generation);
    out->packed = atomic_load(&s_preferences);
}

void orb_sound_save_complete(unsigned generation, bool saved)
{
    /* A completion for an older edit must not attach its failure to a newer
     * choice. Failed generations finish Saving but retain Not saved. */
    atomic_store(&s_failed_generation, saved ? 0 : generation);
    atomic_store(&s_saved_generation, generation);
}

void orb_sound_save_restore(uint32_t packed)
{
    atomic_store(&s_preferences, packed);
    atomic_store(&s_silent, (packed & PREFERENCE_SILENT) != 0);
}

#ifdef ESP_PLATFORM
#include "esp_codec_dev.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_board.h"

static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_speaker, s_mic;

static bool audio_open(void)
{
    if (orb_board_audio_new(ORB_SOUND_SAMPLE_RATE, &s_speaker, &s_mic) != ESP_OK) return false;
    esp_codec_dev_sample_info_t output = {.sample_rate = ORB_SOUND_SAMPLE_RATE,
        .bits_per_sample = 16, .channel = 1, .channel_mask = 1};
    esp_codec_dev_sample_info_t input = {.sample_rate = ORB_SOUND_SAMPLE_RATE,
        .bits_per_sample = 16, .channel = 2, .channel_mask = 3};
    if (esp_codec_dev_open(s_speaker, &output) == ESP_CODEC_DEV_OK &&
        esp_codec_dev_open(s_mic, &input) == ESP_CODEC_DEV_OK &&
        orb_board_audio_set_mic_gain() == ESP_OK &&
        orb_board_audio_set_volume(orb_sound_volume()) == ESP_OK) return true;
    orb_board_audio_delete(s_speaker, s_mic);
    s_speaker = s_mic = NULL;
    return false;
}

static void audio_close(void)
{
    if (s_speaker || s_mic) orb_board_audio_delete(s_speaker, s_mic);
    s_speaker = s_mic = NULL;
}

static bool audio_work(void)
{
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        if (atomic_load(&s_core.pending[i]) != -1 || s_core.voices[i].active) return true;
    }
    return false;
}

static void audio_recover(void)
{
    ESP_LOGW("sound", "audio unavailable; retrying in 100 ms");
    audio_close();
    atomic_store(&s_output_ready, false);
    orb_sound_core_release(&s_core);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
}

static uint32_t audio_park(int64_t last_audio_us, bool *suspended)
{
    if (orb_board_audio_idle(true) != ESP_OK) return 0;
    int64_t remaining = last_audio_us + 10000000 - esp_timer_get_time();
    if (remaining <= 0 && !*suspended) {
        if (orb_board_audio_suspend() != ESP_OK) return 0;
        *suspended = true;
    }
    return *suspended ? UINT32_MAX : (uint32_t)((remaining + 999) / 1000);
}

static float audio_mix(int16_t *pcm, bool speaking)
{
    int16_t speech[ORB_SOUND_CHUNK_SAMPLES];
    orb_sound_core_render(&s_core, pcm, ORB_SOUND_CHUNK_SAMPLES, speaking);
    orb_sound_speech_source_t read = atomic_load(&s_speech_source);
    size_t n = speaking && read ? read(atomic_load(&s_speech_context), speech,
                                      ORB_SOUND_CHUNK_SAMPLES, ORB_SOUND_SAMPLE_RATE) : 0;
    if (n > ORB_SOUND_CHUNK_SAMPLES) n = ORB_SOUND_CHUNK_SAMPLES;
    double energy = 0;
    for (size_t i = 0; i < ORB_SOUND_CHUNK_SAMPLES; i++) {
        int32_t mixed = pcm[i] + (i < n ? (int32_t)speech[i] * 3 / 4 : 0);
        pcm[i] = mixed > INT16_MAX ? INT16_MAX : mixed < INT16_MIN ? INT16_MIN : (int16_t)mixed;
        energy += (double)pcm[i] * pcm[i];
    }
    return (float)(sqrt(energy / ORB_SOUND_CHUNK_SAMPLES) / 32768.0);
}

static void audio_submitted(uint32_t submit_at_us, float envelope)
{
    for (unsigned i = 0; i < ORB_SOUND_VOICES; i++) {
        if (!s_core.voices[i].began_this_block) continue;
        uint32_t latency = submit_at_us - s_core.voices[i].queued_at_us;
        atomic_fetch_add(&s_started_count, 1);
        atomic_store(&s_last_start_us, latency);
        unsigned maximum = atomic_load(&s_max_start_us);
        while (latency > maximum && !atomic_compare_exchange_weak(&s_max_start_us, &maximum, latency)) {}
    }
    /* TX DMA paces the owner. A failed/missing RX must never stall cues or
     * capture takeover; only complete stereo frames reach the sink. */
    int16_t stereo[ORB_SOUND_CHUNK_SAMPLES * 2];
    size_t read_bytes = 0;
    orb_board_audio_read(s_mic, stereo, sizeof(stereo), &read_bytes, 0);
    orb_sound_mic_sink_t sink = atomic_load(&s_mic_sink);
    if (sink && read_bytes >= 4) sink(atomic_load(&s_mic_context), stereo, read_bytes / 4,
        ORB_SOUND_SAMPLE_RATE, envelope);
}

static void sound_task(void *ctx)
{
    (void)ctx;
    if (!orb_sound_pcm_init()) {
        ESP_LOGW("sound", "signed sampled palette unavailable; speech remains available");
    }
    atomic_store(&s_asset_bytes, (unsigned)orb_sound_pcm_bytes());
    atomic_store(&s_started, 2);
    unsigned applied_volume = UINT_MAX;
    int16_t pcm[ORB_SOUND_CHUNK_SAMPLES];
    bool opened = false, suspended = false;
    float envelope = 0;
    int64_t last_audio_us = esp_timer_get_time();
    for (;;) {
        if (atomic_load(&s_core.capture_waiting) || atomic_load(&s_core.capture_active)) {
            if (opened) {
                audio_close(); opened = false; atomic_store(&s_output_ready, false);
                orb_sound_core_release(&s_core);
            }
            last_audio_us = esp_timer_get_time();
            orb_sound_core_render(&s_core, pcm, ORB_SOUND_CHUNK_SAMPLES, false);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
            continue;
        }
        if (!opened) {
            if (!orb_sound_core_try_lease(&s_core)) { vTaskDelay(1); continue; }
            if (!audio_open()) { audio_recover(); continue; }
            opened = true; suspended = false; applied_volume = orb_sound_volume();
            atomic_store(&s_output_ready, true);
        }
        atomic_store(&s_quiet, quiet());
        refresh_mute();
        bool speaking = atomic_load(&s_speech_active) && orb_sound_speech_allowed();
        bool work = speaking || audio_work();
        if (!work && esp_timer_get_time() - last_audio_us >= 100000) {
            uint32_t wait = audio_park(last_audio_us, &suspended);
            if (!wait) { audio_recover(); opened = false; continue; }
            /* One deadline closes analog bias after 10 s. Notifications stay
             * latched across this decision, including a racing cue or mic. */
            ulTaskNotifyTake(pdTRUE, wait == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait));
            continue;
        }
        if (applied_volume != orb_sound_volume()) {
            unsigned desired = orb_sound_volume();
            if (orb_board_audio_set_volume(desired) != ESP_OK) {
                audio_recover(); opened = false; continue;
            }
            applied_volume = desired;
        }
        /* Do not dequeue a cue or consume speech until clocks and both codec
         * analog paths have resumed; failed wake keeps the first block intact. */
        if (orb_board_audio_idle(false) != ESP_OK) {
            audio_recover(); opened = false; continue;
        }
        suspended = false;
        if (work) last_audio_us = esp_timer_get_time();
        envelope = audio_mix(pcm, speaking);
        uint32_t submit_at_us = (uint32_t)esp_timer_get_time();
        size_t written_bytes = 0;
        bool ok = orb_board_audio_write(s_speaker, pcm, sizeof(pcm), &written_bytes, 10) == ESP_OK &&
                  written_bytes == sizeof(pcm);
        if (!ok) {
            audio_recover(); opened = false; continue;
        }
        audio_submitted(submit_at_us, envelope);
    }
}

#endif

bool orb_sound_set_preferences(const orb_sound_preferences_t *p)
{
    if (!p || p->volume > 100 || p->quiet_start > 23 || p->quiet_end > 23) return false;
    unsigned packed = p->volume | (p->quiet_enabled ? 128u : 0u) |
                      (p->quiet_start << 8) | (p->quiet_end << 13) | (p->spoken_replies ? 0u : (1u << 18)) |
                      (p->silent ? PREFERENCE_SILENT : 0u);
    atomic_store(&s_preferences, packed);
    atomic_store(&s_silent, p->silent);
    atomic_fetch_add(&s_preference_generation, 1);
    refresh_mute();
#ifdef ESP_PLATFORM
    if (s_task) xTaskNotifyGive(s_task);
#endif
#if defined(ESP_PLATFORM) || defined(ORB_SOUND_TEST_STORAGE)
    orb_sound_save_notify();
#else
    atomic_store(&s_saved_generation, atomic_load(&s_preference_generation));
#endif
    return true;
}

void orb_sound_set_volume(unsigned volume)
{
    orb_sound_preferences_t p; orb_sound_preferences(&p);
    p.volume = volume > 100 ? 100 : volume;
    orb_sound_set_preferences(&p);
}
unsigned orb_sound_volume(void) { return atomic_load(&s_preferences) & 127u; }

bool orb_sound_init(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_started, &expected, 1)) return expected > 0;
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCoreWithCaps(sound_task, "orb_sound", 6144, NULL, 5, &s_task, 0,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&s_started, 0); return false;
    }
#else
    orb_sound_pcm_init();
    atomic_store(&s_asset_bytes, (unsigned)orb_sound_pcm_bytes());
    atomic_store(&s_output_ready, true);
    atomic_store(&s_started, 2);
#endif
    refresh_mute();
    return true;
}

void orb_sound_set_muted(bool silent, bool face_down_now)
{
    atomic_store(&s_silent, silent); atomic_store(&s_face_down, face_down_now); refresh_mute();
}
void orb_sound_set_silent(bool silent)
{
    /* Power/face-down muting uses set_muted and never writes this preference.
     * Change only this bit so a concurrent volume/Quiet Hours edit survives. */
    unsigned previous = silent ? atomic_fetch_or(&s_preferences, PREFERENCE_SILENT) :
                                 atomic_fetch_and(&s_preferences, ~PREFERENCE_SILENT);
    if (((previous & PREFERENCE_SILENT) != 0) != silent) {
        atomic_fetch_add(&s_preference_generation, 1);
#if defined(ESP_PLATFORM) || defined(ORB_SOUND_TEST_STORAGE)
        orb_sound_save_notify();
#else
        atomic_store(&s_saved_generation, atomic_load(&s_preference_generation));
#endif
    }
    atomic_store(&s_silent, silent);
    refresh_mute();
}
void orb_sound_set_always_on(bool always_on) { atomic_store(&s_always_on, always_on); refresh_mute(); }

static bool play(orb_sound_cue_t cue, bool preview)
{
    /* Feedback must remain timely; real alarms/timers instead survive codec
     * startup or recovery and remain queued until the output returns. */
    if (atomic_load(&s_started) != 2 ||
        (!atomic_load(&s_output_ready) && (preview || !orb_sound_is_alert(cue))) || !orb_sound_pcm_count(cue)) {
        atomic_fetch_add(&s_dropped_count, 1); return false;
    }
    refresh_mute();
#ifdef ESP_PLATFORM
    uint32_t now = (uint32_t)esp_timer_get_time();
#else
    uint32_t now = 0;
#endif
    bool queued = preview ? orb_sound_core_preview_at(&s_core, cue, now) :
                            orb_sound_core_queue_at(&s_core, cue, now);
    if (!queued) {
        atomic_fetch_add(&s_dropped_count, 1); return false;
    }
    atomic_fetch_add(&s_queued_count, 1);
#ifdef ESP_PLATFORM
    xTaskNotifyGive(s_task);
#else
    int16_t pcm[ORB_SOUND_CHUNK_SAMPLES];
    orb_sound_core_render(&s_core, pcm, ORB_SOUND_CHUNK_SAMPLES, false);
#endif
    return true;
}
bool orb_sound_play(orb_sound_cue_t cue) { return play(cue, false); }
bool orb_sound_preview(orb_sound_cue_t cue) { return play(cue, true); }
void orb_sound_stop(orb_sound_cue_t cue) { orb_sound_core_stop(&s_core, cue); }
void orb_sound_stop_all(void) { atomic_store(&s_core.stop_mask, (1u << ORB_SOUND_CUE_COUNT) - 1); }

void orb_sound_set_speech_source(orb_sound_speech_source_t source, void *ctx)
{
    atomic_store(&s_speech_context, ctx); atomic_store(&s_speech_source, source);
}
void orb_sound_set_mic_sink(orb_sound_mic_sink_t sink, void *ctx)
{
    atomic_store(&s_mic_context, ctx); atomic_store(&s_mic_sink, sink);
}
void orb_sound_set_speech_active(bool active)
{
    atomic_store(&s_speech_active, active);
#ifdef ESP_PLATFORM
    if (s_task) xTaskNotifyGive(s_task);
#endif
}
bool orb_sound_speech_allowed(void)
{
    return !(atomic_load(&s_preferences) & (1u << 18)) && orb_sound_volume() && !atomic_load(&s_silent) && !atomic_load(&s_face_down) &&
           !atomic_load(&s_always_on) && !atomic_load(&s_quiet) && !atomic_load(&s_core.capture_waiting) &&
           !atomic_load(&s_core.capture_active);
}

bool orb_sound_audio_acquire(void)
{
    atomic_store(&s_core.capture_waiting, true);
    orb_sound_stop_all();
#ifdef ESP_PLATFORM
    if (s_task) xTaskNotifyGive(s_task);
    int64_t deadline = esp_timer_get_time() + 160000;
    while (!orb_sound_core_try_lease(&s_core)) {
        if (esp_timer_get_time() >= deadline) {
            atomic_store(&s_core.capture_waiting, false); return false;
        }
        vTaskDelay(1);
    }
#else
    if (!orb_sound_core_try_lease(&s_core)) {
        atomic_store(&s_core.capture_waiting, false); return false;
    }
#endif
    atomic_store(&s_core.capture_active, true);
    atomic_store(&s_core.capture_waiting, false);
    return true;
}

void orb_sound_audio_release(void)
{
    atomic_store(&s_core.capture_active, false);
    orb_sound_core_release(&s_core);
#ifdef ESP_PLATFORM
    if (s_task) xTaskNotifyGive(s_task);
#endif
}

void orb_sound_stats(orb_sound_stats_t *stats)
{
    if (!stats) return;
    *stats = (orb_sound_stats_t){.ready = atomic_load(&s_started) == 2 &&
        atomic_load(&s_asset_bytes) && atomic_load(&s_output_ready),
        .silent = atomic_load(&s_silent), .face_down = atomic_load(&s_face_down),
        .always_on = atomic_load(&s_always_on),
        .asset_bytes = atomic_load(&s_asset_bytes), .queued_count = atomic_load(&s_queued_count),
        .started_count = atomic_load(&s_started_count), .dropped_count = atomic_load(&s_dropped_count),
        .last_start_us = atomic_load(&s_last_start_us), .max_start_us = atomic_load(&s_max_start_us)};
}
