#include <assert.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_PLATFORM 1
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
#define ORB_SOUND_SAMPLE_RATE 32000u
#define ORB_SOUND_CHUNK_SAMPLES 160u
#define ORB_SOUND_VOICES 2u
#define ESP_LOGW(...) (s_logs++)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
#define ALARM 2u

typedef void *esp_codec_dev_handle_t;
typedef struct { unsigned sample_rate, bits_per_sample, channel, channel_mask; } esp_codec_dev_sample_info_t;
typedef unsigned orb_sound_cue_t;
typedef size_t (*orb_sound_speech_source_t)(void *, int16_t *, size_t, unsigned);
typedef void (*orb_sound_mic_sink_t)(void *, const int16_t *, size_t, unsigned, float);
typedef struct {
    atomic_bool capture_waiting, capture_active;
    atomic_int pending[2];
    struct { bool active, began_this_block; uint32_t queued_at_us; } voices[2];
} orb_sound_core_t;
static orb_sound_core_t s_core;
static void *s_task = (void *)1;
static esp_codec_dev_handle_t s_speaker, s_mic;
static atomic_int s_started;
static atomic_uint s_asset_bytes, s_started_count, s_last_start_us, s_max_start_us, s_dropped_count, s_queued_count;
static atomic_bool s_output_ready, s_quiet, s_speech_active;
static _Atomic(orb_sound_speech_source_t) s_speech_source;
static _Atomic(orb_sound_mic_sink_t) s_mic_sink;
static _Atomic(void *) s_speech_context, s_mic_context;
static bool s_lease, s_board_idle, s_board_suspended, s_capture_yield;
static bool s_early_sent;
static bool s_fail_idle, s_fail_suspend, s_fail_resume, s_fail_write, s_fail_open, s_fail_volume;
static unsigned s_opens, s_closes, s_writes, s_parks, s_notifications, s_recovery_waits;
static unsigned s_suspends, s_idle_waits, s_logs, s_cue_renders, s_audible_writes, s_speech_reads;
static unsigned s_volume, s_applied_volume, s_wakes, s_wakes_at_request;
static uint64_t s_clock, s_origin, s_last_work;
static jmp_buf s_done;
enum { IDLE, IDLE_NOTIFY, EARLY_CUE, CUE, SPEECH, CAPTURE, IDLE_FAIL, SUSPEND_FAIL,
       RESUME_CUE_FAIL, RESUME_SPEECH_FAIL, WRITE_FAIL, VOLUME_FAIL, OPEN_FAIL_ALARM };
static unsigned s_case;
static bool orb_sound_is_alert(unsigned cue) { return cue == ALARM; }
static unsigned orb_sound_volume(void) { return s_volume; }
static bool orb_sound_speech_allowed(void) { return true; }
static bool quiet(void) { return false; }
static void refresh_mute(void) {}
static bool orb_sound_pcm_init(void) { return true; }
static unsigned orb_sound_pcm_bytes(void) { return 10; }
static unsigned orb_sound_pcm_count(unsigned cue) { (void)cue; return 10; }
static int64_t esp_timer_get_time(void) { return (int64_t)s_clock; }
static unsigned xTaskNotifyGive(void *task) { assert(task == s_task); s_notifications++; return 1; }
static int orb_board_audio_new(unsigned rate, void **speaker, void **mic)
{
    assert(rate == 32000 && !s_speaker && !s_mic);
    if (s_fail_open) { s_fail_open = false; return -1; }
    *speaker = (void *)2; *mic = (void *)3; s_opens++;
    s_board_idle = s_board_suspended = false;
    return 0;
}
static int esp_codec_dev_open(void *codec, const esp_codec_dev_sample_info_t *format)
{
    assert(codec && format->sample_rate == 32000); return 0;
}
static int orb_board_audio_set_mic_gain(void) { return 0; }
static int orb_board_audio_set_volume(unsigned volume)
{
    assert(volume == s_volume);
    if (s_fail_volume) {
        assert(s_board_suspended && !s_cue_renders && !s_speech_reads);
        s_fail_volume = false; return -1;
    }
    s_applied_volume = volume; return 0;
}
static int orb_board_audio_delete(void *speaker, void *mic)
{
    assert(speaker && mic); s_closes++; return 0;
}
static int orb_board_audio_idle(bool idle)
{
    assert(s_speaker && s_mic);
    if (!idle) { assert(s_applied_volume == s_volume); s_wakes++; }
    if (idle && s_fail_idle) { s_fail_idle = false; return -1; }
    if (!idle && s_fail_resume && s_board_suspended) {
        s_fail_resume = false;
        /* No PCM may be dequeued before successful analog resume. */
        assert(!s_cue_renders && !s_speech_reads);
        return -1;
    }
    s_board_idle = idle;
    if (!idle) s_board_suspended = false;
    return 0;
}
static int orb_board_audio_suspend(void)
{
    assert(s_board_idle && s_clock - s_last_work >= 10000000);
    assert(atomic_load(&s_core.pending[0]) == -1 && !atomic_load(&s_speech_active));
    if (s_fail_suspend) { s_fail_suspend = false; return -1; }
    s_suspends++; s_board_suspended = true; return 0;
}
static void orb_sound_core_release(orb_sound_core_t *core) { assert(core == &s_core); s_lease = false; }
static bool orb_sound_core_try_lease(orb_sound_core_t *core)
{
    assert(core == &s_core);
    if (s_lease) return false;
    s_lease = true; return true;
}
static size_t orb_sound_core_render(orb_sound_core_t *core, int16_t *pcm, unsigned samples, bool speaking)
{
    (void)speaking; assert(core == &s_core && samples == 160);
    memset(pcm, 0, samples * sizeof(*pcm));
    bool pending = atomic_load(&s_core.pending[0]) >= 0;
    s_core.voices[0].began_this_block = pending;
    if (!pending) return 0;
    assert(!s_board_idle && !s_board_suspended);
    atomic_store(&s_core.pending[0], -1);
    s_cue_renders++; pcm[0] = 10; return 1;
}
static bool orb_sound_core_queue_at(orb_sound_core_t *core, unsigned cue, uint32_t now)
{
    assert(core == &s_core && atomic_load(&s_core.pending[0]) == -1);
    if (atomic_load(&s_core.capture_waiting) || atomic_load(&s_core.capture_active)) return false;
    atomic_store(&s_core.pending[0], (int)cue);
    s_core.voices[0].queued_at_us = now; return true;
}
static bool orb_sound_core_preview_at(orb_sound_core_t *core, unsigned cue, uint32_t now)
{
    return orb_sound_core_queue_at(core, cue, now);
}
static void orb_sound_stop_all(void)
{
    atomic_store(&s_core.pending[0], -1);
}
static void orb_sound_set_speech_active(bool active);
static bool orb_sound_play(orb_sound_cue_t cue);
static bool orb_sound_preview(orb_sound_cue_t cue);
static bool orb_sound_audio_acquire(void);
static void orb_sound_audio_release(void);
static size_t speech_read(void *ctx, int16_t *pcm, size_t capacity, unsigned rate)
{
    (void)ctx; assert(capacity == 160 && rate == 32000 && !s_board_suspended && !s_board_idle);
    s_speech_reads++; memset(pcm, 0, capacity * sizeof(*pcm)); pcm[0] = 20; return capacity;
}
static int orb_board_audio_write(void *speaker, const void *buffer, size_t size, size_t *written, unsigned timeout)
{
    const int16_t *pcm = buffer;
    assert(speaker && !s_board_idle && !s_board_suspended && size == 320 && timeout == 10);
    if (pcm[0] && s_fail_write) {
        s_last_work = s_clock; s_fail_write = false; *written = 0; return -1;
    }
    *written = size; s_writes++; s_clock += 5000;
    if (pcm[0]) {
        s_audible_writes++; s_last_work = s_clock - 5000;
        if (s_case == SPEECH || s_case == RESUME_SPEECH_FAIL) orb_sound_set_speech_active(false);
    }
    return 0;
}
static int orb_board_audio_read(void *mic, void *pcm, size_t size, size_t *read, unsigned timeout)
{
    (void)pcm; assert(mic && !s_board_idle && timeout == 0); *read = size; return 0;
}
static void vTaskDelay(unsigned ticks)
{
    s_clock += ticks * 1000;
    if (s_capture_yield) s_lease = false;
}
static unsigned ulTaskNotifyTake(int clear, uint32_t wait)
{
    assert(clear == pdTRUE);
    if (wait == portMAX_DELAY) {
        assert(s_board_idle && s_board_suspended && s_opens > 0 && s_suspends > 0);
        s_parks++;
        if (s_case == IDLE || s_case == IDLE_NOTIFY || s_case == IDLE_FAIL || s_case == SUSPEND_FAIL ||
            s_case == OPEN_FAIL_ALARM || s_case == EARLY_CUE || s_parks > 1) longjmp(s_done, 1);
        unsigned notified = s_notifications;
        if (s_case == VOLUME_FAIL) {
            s_volume = 25; s_fail_volume = true; s_wakes_at_request = s_wakes;
            assert(orb_sound_play(0));
        }
        if (s_case == CUE || s_case == RESUME_CUE_FAIL || s_case == WRITE_FAIL) assert(orb_sound_play(0));
        if (s_case == SPEECH || s_case == RESUME_SPEECH_FAIL) orb_sound_set_speech_active(true);
        if (s_case == CAPTURE) {
            s_capture_yield = true; assert(orb_sound_audio_acquire()); s_capture_yield = false;
        }
        assert(s_notifications > notified);
        return 1;
    }
    if (s_case == CAPTURE && wait == 5) {
        assert(s_closes == 1 && !atomic_load(&s_output_ready) && atomic_load(&s_core.capture_active));
        unsigned notified = s_notifications; orb_sound_audio_release(); s_last_work = s_clock;
        assert(s_notifications > notified); return 1;
    }
    if (wait == 100 && !atomic_load(&s_output_ready)) {
        assert(s_logs && !s_lease);
        s_recovery_waits++;
        if (s_case == RESUME_CUE_FAIL) assert(atomic_load(&s_core.pending[0]) == 0 && !s_cue_renders);
        if (s_case == RESUME_SPEECH_FAIL) assert(!s_speech_reads);
        if (s_case == VOLUME_FAIL) {
            assert(atomic_load(&s_core.pending[0]) == 0 && !s_cue_renders && !s_speech_reads);
            assert(s_wakes == s_wakes_at_request && s_applied_volume == 60);
        }
        if (s_case == OPEN_FAIL_ALARM || s_case == WRITE_FAIL) {
            assert(orb_sound_play(ALARM));
            assert(!orb_sound_play(0) && !orb_sound_preview(ALARM));
        }
        s_clock += wait * 1000; return 0;
    }
    assert(s_board_idle && !s_board_suspended && s_clock - s_last_work >= 100000);
    assert((uint64_t)wait * 1000 + s_clock == s_last_work + 10000000);
    s_idle_waits++;
    if (s_case == EARLY_CUE && !s_early_sent) {
        s_early_sent = true; s_clock += 1000000; assert(orb_sound_play(0)); return 1;
    }
    if (s_case == IDLE_NOTIFY && s_idle_waits == 1) {
        /* A preferences notification must not restart the analog-idle timer. */
        s_clock += 3000000; return 1;
    }
    s_clock += wait * 1000; return 0;
}
#include "sound_idle.inc"

static void run(unsigned scenario)
{
    s_case = scenario; s_lease = s_board_idle = s_board_suspended = s_capture_yield = false;
    s_fail_idle = scenario == IDLE_FAIL;
    s_fail_suspend = scenario == SUSPEND_FAIL;
    s_fail_resume = scenario == RESUME_CUE_FAIL || scenario == RESUME_SPEECH_FAIL;
    s_fail_write = scenario == WRITE_FAIL; s_fail_open = scenario == OPEN_FAIL_ALARM;
    s_speaker = s_mic = NULL; s_early_sent = s_fail_volume = false;
    s_volume = 60; s_applied_volume = s_wakes = s_wakes_at_request = 0;
    memset(&s_core, 0, sizeof(s_core));
    atomic_store(&s_core.pending[0], -1); atomic_store(&s_core.pending[1], -1);
    atomic_store(&s_speech_active, false); atomic_store(&s_output_ready, false);
    atomic_store(&s_started_count, 0);
    atomic_store(&s_speech_source, speech_read);
    s_opens = s_closes = s_writes = s_parks = s_notifications = s_recovery_waits = 0;
    s_suspends = s_idle_waits = s_logs = s_cue_renders = s_audible_writes = s_speech_reads = 0;
    s_clock = s_origin = s_last_work = 1000000;
    if (!setjmp(s_done)) sound_task(NULL);
    assert(s_parks >= 1 && s_suspends == s_parks);
    assert(s_idle_waits >= 1 && s_clock - s_origin >= 10000000);
    if (scenario == IDLE_NOTIFY) assert(s_idle_waits == 2 && s_clock == s_origin + 10000000);
    if (scenario == CUE || scenario == SPEECH) assert(s_audible_writes == 1 && s_opens == 1 && s_closes == 0);
    if (scenario == EARLY_CUE || scenario == CUE || scenario == RESUME_CUE_FAIL || scenario == VOLUME_FAIL || scenario == OPEN_FAIL_ALARM)
        assert(s_cue_renders == 1 && s_audible_writes == 1 && atomic_load(&s_started_count) == 1);
    if (scenario == WRITE_FAIL) assert(s_cue_renders == 2 && s_audible_writes == 1 && atomic_load(&s_started_count) == 1);
    if (scenario == RESUME_SPEECH_FAIL) assert(s_speech_reads == 1 && s_audible_writes == 1);
    if (scenario == VOLUME_FAIL) assert(s_applied_volume == 25 && s_volume == 25);
    if (scenario == CAPTURE) assert(s_opens == 2 && s_closes == 1);
    if (scenario >= IDLE_FAIL) assert(s_recovery_waits == 1 && s_logs == 1);
    if (scenario == EARLY_CUE) assert(s_suspends == 1 && s_clock - s_origin == 11100000);
    if (scenario == CUE) assert(atomic_load(&s_last_start_us) < 60000);
    audio_close();
}
int main(void)
{
    for (unsigned scenario = IDLE; scenario <= OPEN_FAIL_ALARM; scenario++) run(scenario);
    /* Preview shares the real producer path and its notification contract. */
    atomic_store(&s_started, 2); atomic_store(&s_output_ready, true);
    unsigned notified = s_notifications; assert(orb_sound_preview(0) && s_notifications > notified);
    puts("sound worker: 10 s analog-idle deadline, event-only suspension, cue/speech/mic wake, dequeue order, TX/volume recovery, alarm recovery and bounded logged retries passed");
}
