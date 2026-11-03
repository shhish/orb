#include "orb_voice.h"
#include "voice_core.h"
#include "voice_wait.h"
#include "speech_core.h"
#include "mic_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "orb_board.h"
#include "orb_link.h"
#include "orb_mem.h"
#include "orb_sound.h"
#include "orb_time_voice.h"

static const char *TAG = "orb_voice";

typedef enum { EVENT_REPLY, EVENT_RESULT, EVENT_ERROR } event_kind_t;
typedef struct {
    event_kind_t kind;
    voice_request_kind_t request;
    uint16_t stream;
    uint32_t generation;
    bool ok;
    orb_send_status_t status;
    char text[281], code[33];
} voice_event_t;
typedef struct {
    voice_request_kind_t request;
    uint16_t stream;
    uint32_t generation;
} reply_context_t;

#define VOICE_EVENTS 8
#define VOICE_STACK 4096
static TaskHandle_t s_task;
static voice_core_t *s_core;
static QueueHandle_t s_events;
static StaticQueue_t s_event_queue;
static uint8_t *s_event_storage;
static esp_codec_dev_handle_t s_speaker, s_mic;
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_boot_held;
static bool s_events_lost;
static bool s_sleep_eligible;

static speech_core_t *s_speech;
static uint32_t s_speech_generation;
static portMUX_TYPE s_speech_lock = portMUX_INITIALIZER_UNLOCKED;
static mic_dsp_t s_capture_dsp, s_interrupt_dsp;
typedef struct { uint16_t stream; speech_stop_t reason; uint32_t generation; } stopped_receipt_t;
static stopped_receipt_t s_stopped[4];
static unsigned s_stopped_head, s_stopped_count;
static void speech_poll(void);

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

void orb_voice_wake(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void orb_voice_set_sleep_eligible(bool eligible)
{
    portENTER_CRITICAL(&s_input_lock);
    bool changed = s_sleep_eligible != eligible;
    s_sleep_eligible = eligible;
    portEXIT_CRITICAL(&s_input_lock);
    if (changed) orb_voice_wake();
}

static void model_request_wake(void *ctx)
{
    (void)ctx;
    orb_voice_wake();
}

static void event_post(const voice_event_t *event)
{
    if (!s_events) return;
    if (xQueueSend(s_events, event, 0) != pdTRUE) {
        /* Callback delivery cannot block the link task. Losing a recording
         * result fails closed instead of leaving a plausible pending UI. */
        portENTER_CRITICAL(&s_input_lock);
        s_events_lost = true;
        portEXIT_CRITICAL(&s_input_lock);
    }
    orb_voice_wake();
}

static bool audio_open(void *ctx)
{
    (void)ctx;
    orb_voice_speech_stop(ORB_SPEECH_STOP);
    mic_dsp_init(&s_capture_dsp);
    orb_mem_begin(ORB_MEM_VOICE_START);
    /* The board's duplex codec pair has one owner. Capture preempts short UI
     * ticks; this lease also protects construction and all failure unwind. */
    if (!orb_sound_audio_acquire()) {
        orb_mem_end(ORB_MEM_VOICE_START);
        return false;
    }
    if (orb_board_audio_new(16000, &s_speaker, &s_mic) != ESP_OK) {
        orb_sound_audio_release();
        orb_mem_end(ORB_MEM_VOICE_START);
        return false;
    }
    esp_codec_dev_sample_info_t fmt = {.sample_rate = 16000, .bits_per_sample = 16,
                                      .channel = 2, .channel_mask = 3};
    /* Both physical ES7210 slots are read; upstream remains mono PCM16. */
    if (esp_codec_dev_open(s_mic, &fmt) == ESP_CODEC_DEV_OK &&
        orb_board_audio_set_mic_gain() == ESP_OK) {
        orb_mem_end(ORB_MEM_VOICE_START);
        return true;
    }
    orb_board_audio_delete(s_speaker, s_mic);
    orb_sound_audio_release();
    s_speaker = s_mic = NULL;
    orb_mem_end(ORB_MEM_VOICE_START);
    return false;
}

static void audio_close(void *ctx)
{
    (void)ctx;
    if (s_mic) {
        esp_err_t err = orb_board_audio_delete(s_speaker, s_mic);
        if (err != ESP_OK) ESP_LOGW(TAG, "audio teardown failed: %s", esp_err_to_name(err));
        orb_sound_audio_release();
    }
    s_speaker = s_mic = NULL;
}

static size_t audio_read(void *ctx, uint8_t *pcm, size_t capacity)
{
    (void)ctx;
    _Alignas(4) int16_t stereo[VOICE_PCM_BYTES];
    size_t bytes = 0;
    if (capacity > VOICE_PCM_BYTES || (capacity & 1)) return SIZE_MAX;
    esp_err_t err = orb_board_audio_read(s_mic, stereo, capacity * 2, &bytes, 40);
    if ((err != ESP_OK && err != ESP_ERR_TIMEOUT) || (bytes & 3)) return SIZE_MAX;
    size_t frames = bytes / 4;
    mic_dsp_mono(&s_capture_dsp, stereo, (int16_t *)pcm, frames, 16000);
    return frames * 2;
}

static bool audio_send(void *ctx, const uint8_t *frame, size_t bytes)
{
    (void)ctx;
    return orb_link_send_audio_on(s_core->generation, frame, bytes) == ESP_OK;
}

static void on_reply(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    reply_context_t *reply = ctx;
    bool ok = err == ESP_OK;
    if (reply->request == VOICE_REQ_CANCEL && err == ESP_FAIL) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
        if (voice_cancel_absent(code)) ok = true;
    }
    voice_event_t event = {.kind = EVENT_REPLY, .request = reply->request,
                           .stream = reply->stream, .generation = reply->generation, .ok = ok};
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
    if (code && strlen(code) < sizeof(event.code)) memcpy(event.code, code, strlen(code) + 1);
    else if (err == ESP_ERR_TIMEOUT) strcpy(event.code, "timeout");
    if (reply->request == VOICE_REQ_SEND) {
        orb_message_result_t receipt;
        if ((err == ESP_OK || err == ESP_FAIL) &&
            orb_link_read_message_receipt(err == ESP_OK ? result : NULL,
                                          err == ESP_FAIL ? error : NULL, &receipt)) {
            event.ok = receipt.accepted;
            event.status = receipt.status;
            strcpy(event.text, receipt.reason);
            strcpy(event.code, receipt.error);
        } else {
            event.ok = false;
            event.status = ORB_SEND_FAILED;
            strcpy(event.text, err == ESP_ERR_TIMEOUT ?
                "Delivery unconfirmed. Check the conversation." : "Message receipt unavailable.");
            if (!event.code[0]) strcpy(event.code, "internal");
        }
    }
    event_post(&event);
    free(reply);
}

static bool request_send(void *ctx, voice_request_kind_t kind, const orb_voice_state_t *state)
{
    (void)ctx;
    if (kind == VOICE_REQ_SEND && !strcmp(state->target, "time")) {
        /* A Time result has only local effects after physical Save. Reuse the
         * bounded receipt path, and let core completion cancel the hub result. */
        voice_event_t event = {.kind = EVENT_REPLY, .request = kind,
            .stream = state->stream, .generation = s_core->generation};
        event.ok = orb_time_reminder_from_voice(state->text, event.text, sizeof(event.text));
        event.status = event.ok ? ORB_SEND_DELIVERED : ORB_SEND_FAILED;
        if (!event.ok) strcpy(event.code, "bad_request");
        orb_sound_play(event.ok ? ORB_SOUND_DONE : ORB_SOUND_FAILED);
        event_post(&event);
        return true;
    }
    static const char *const methods[] = {"voice.begin", "voice.end", "voice.cancel", "message.send"};
    cJSON *params = cJSON_CreateObject();
    if (!params) return false;
    bool ok = cJSON_AddNumberToObject(params, "stream", state->stream) != NULL;
    if (kind == VOICE_REQ_BEGIN || kind == VOICE_REQ_SEND)
        ok = ok && cJSON_AddStringToObject(params, "target", state->target);
    if (kind == VOICE_REQ_SEND)
        ok = ok && cJSON_AddStringToObject(params, "text", state->text);
    /* Replies cross task boundaries but never run with the flash cache off.
     * Keep even this small per-request copy out of the DMA/Wi-Fi reserve. */
    reply_context_t *reply = ok ? heap_caps_malloc(sizeof(*reply), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!reply) { cJSON_Delete(params); return false; }
    *reply = (reply_context_t){kind, state->stream, s_core->generation};
    esp_err_t err = orb_link_request_on(s_core->generation, methods[kind], params, 10000, on_reply, reply);
    if (err != ESP_OK) { free(reply); return false; }
    return true;
}

static bool answer_send(void *ctx, const orb_voice_state_t *state)
{
    (void)ctx;
    orb_answer_t answer = {0};
    memcpy(answer.card, state->card, sizeof(answer.card));
    memcpy(answer.text, state->text, sizeof(answer.text));
    return orb_model_answer(&answer);
}

static void model_publish(void *ctx, const orb_voice_state_t *state)
{
    (void)ctx;
    if (!orb_model_set_voice(state)) ESP_LOGE(TAG, "invalid voice state");
    /* Never print transcripts, targets, cards or provider errors. */
    static orb_voice_phase_t previous;
    if (previous != state->phase) {
        ESP_LOGI(TAG, "phase=%u stream=%u", (unsigned)state->phase, state->stream);
        previous = state->phase;
    }
}

static bool model_focus(void *ctx, char target[49], char card[65])
{
    (void)ctx;
    return orb_model_get_voice_focus(target, card);
}

static bool stream_id(const cJSON *params, uint16_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "stream");
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 1 || value->valuedouble > UINT16_MAX ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint16_t)value->valuedouble;
    return true;
}

static void on_voice(const char *name, const cJSON *params, void *ctx)
{
    (void)ctx;
    if (!s_events) return;
    voice_event_t event = {.kind = EVENT_ERROR, .generation = orb_link_generation()};
    if (!stream_id(params, &event.stream)) return;
    if (!strcmp(name, "voice.result")) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "text"));
        size_t n = text ? strlen(text) : 0;
        if (n && n <= 280) { memcpy(event.text, text, n + 1); event.kind = EVENT_RESULT; }
        else strcpy(event.code, "bad_request");
    } else {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "code"));
        if (code && strlen(code) < sizeof(event.code)) memcpy(event.code, code, strlen(code) + 1);
    }
    event_post(&event);
}

static void voice_task(void *ctx)
{
    (void)ctx;
    for (;;) {
        uint64_t now = now_ms();
        speech_poll();
        voice_core_connection(s_core, orb_link_supports_voice(), orb_link_generation(), now);
        portENTER_CRITICAL(&s_input_lock);
        bool held = s_boot_held;
        bool lost = s_events_lost;
        s_events_lost = false;
        portEXIT_CRITICAL(&s_input_lock);
        if (lost) voice_core_lost(s_core, now);
        voice_core_boot(s_core, held, now);
        voice_event_t event;
        while (xQueueReceive(s_events, &event, 0) == pdTRUE) {
            if (event.generation != s_core->generation) continue;
            if (event.kind == EVENT_REPLY) {
                if (event.request == VOICE_REQ_SEND && event.stream == s_core->view.stream &&
                    s_core->view.phase == ORB_VOICE_SENDING) {
                    orb_message_t message = {.stream = event.stream, .generation = event.generation};
                    strcpy(message.target, s_core->view.target);
                    strcpy(message.text, s_core->view.text);
                    if (event.status == ORB_SEND_UNKNOWN)
                        orb_model_complete_message(&message, event.ok, event.code);
                    else orb_model_complete_message_receipt(&message, event.status, event.text, event.code);
                }
                voice_core_reply(s_core, event.request, event.stream, event.generation, event.ok, event.code, now);
            } else if (event.kind == EVENT_RESULT) voice_core_result(s_core, event.stream, event.text, now);
            else voice_core_error(s_core, event.stream, event.code, now);
        }
        orb_voice_request_t request;
        while (orb_model_take_voice_request(&request)) voice_core_action(s_core, &request, now);
        voice_core_tick(s_core, now);
        portENTER_CRITICAL(&s_input_lock);
        bool eligible = s_sleep_eligible;
        portEXIT_CRITICAL(&s_input_lock);
        portENTER_CRITICAL(&s_speech_lock);
        bool speech = s_speech && (s_speech->active || s_stopped_count);
        portEXIT_CRITICAL(&s_speech_lock);
        uint32_t wait = voice_wait_ms(s_core, speech, eligible);
        /* Notifications are latched even if a producer runs between draining
         * its queue and this wait. Eligible idle has no periodic voice wake. */
        ulTaskNotifyTake(pdTRUE, wait ? pdMS_TO_TICKS(wait) : portMAX_DELAY);
    }
}

/* Called with s_speech_lock held. Receipts are bounded and connection scoped;
 * no allocation, logging or transport ever runs from the audio callback. */
static void speech_stopped(uint16_t stream, speech_stop_t reason)
{
    if (s_stopped_count < 4) {
        unsigned tail = (s_stopped_head + s_stopped_count++) % 4;
        s_stopped[tail] = (stopped_receipt_t){stream, reason, s_speech_generation};
    }
}

static void speech_receipt(void)
{
    if (!s_speech->stopped) return;
    speech_stopped(s_speech->stream, s_speech->reason);
    s_speech->stopped = false;
}

static void speech_generation(uint32_t generation)
{
    if (generation == s_speech_generation) return;
    speech_core_init(s_speech);
    s_stopped_count = s_stopped_head = 0;
    s_speech_generation = generation;
}

static bool json_uint(const cJSON *params, const char *name, uint32_t max, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > max || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}

static void on_speech(const char *name, const cJSON *params, void *ctx)
{
    (void)ctx;
    if (!s_speech || !orb_link_supports_voice()) return;
    uint16_t stream;
    if (!stream_id(params, &stream)) return;
    uint32_t generation = orb_link_generation(), rate = 0, chunks = 0;
    const char *caption = NULL, *voice = "";
    bool began = false;
    if (!strcmp(name, "voice.speak")) {
        const char *format = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "format"));
        caption = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "text"));
        const cJSON *voice_field = cJSON_GetObjectItemCaseSensitive(params, "voice");
        if (voice_field) {
            voice = cJSON_GetStringValue(voice_field);
            if (!speech_text_valid(voice, 64)) return;
            for (const unsigned char *p = (const unsigned char *)voice; *p; p++)
                if (*p < 0x20 || *p == 0x7f) return;
        }
        if (!json_uint(params, "rate", 24000, &rate) || (rate != 16000 && rate != 24000) ||
            !format || strcmp(format, "pcm_s16le") || !caption || !caption[0] || strlen(caption) > 1024) return;
    } else if (!strcmp(name, "voice.speak.end") && !json_uint(params, "chunks", UINT32_MAX, &chunks)) return;
    portENTER_CRITICAL(&s_speech_lock);
    speech_generation(generation);
    if (caption && stream > s_speech->last_stream) {
        bool interrupted = s_speech->active;
        uint16_t previous = s_speech->stream;
        /* Begin validates the entire caption before replacing the ring. A bad
         * new event must not stop valid audio or fabricate an interruption. */
        began = speech_core_begin(s_speech, stream, rate, caption, now_ms());
        if (began && interrupted) speech_stopped(previous, SPEECH_INTERRUPTED);
    } else if (!strcmp(name, "voice.speak.end")) speech_core_end(s_speech, stream, chunks);
    else if (!strcmp(name, "voice.speak.stop") && stream == s_speech->stream)
        speech_core_stop(s_speech, SPEECH_STOP);
    speech_receipt();
    portEXIT_CRITICAL(&s_speech_lock);
    /* Model mutex work stays outside the audio critical section. */
    if (began) orb_model_set_speech_voice(voice);
    orb_voice_wake();
}

static void on_speech_pcm(const uint8_t *frame, size_t bytes, uint32_t generation, void *ctx)
{
    (void)ctx;
    if (!s_speech) return;
    portENTER_CRITICAL(&s_speech_lock);
    if (generation == s_speech_generation) {
        speech_core_packet(s_speech, frame, bytes, now_ms());
        speech_receipt();
    }
    bool stopped = s_stopped_count != 0;
    portEXIT_CRITICAL(&s_speech_lock);
    if (stopped) orb_voice_wake();
}

static size_t speech_read(void *ctx, int16_t *pcm, size_t capacity, uint32_t rate)
{
    (void)ctx;
    portENTER_CRITICAL(&s_speech_lock);
    size_t n = speech_core_read(s_speech, pcm, capacity, rate, now_ms());
    speech_receipt();
    bool stopped = s_stopped_count != 0;
    portEXIT_CRITICAL(&s_speech_lock);
    if (stopped) orb_voice_wake();
    return n;
}

static void speech_microphones(void *ctx, const int16_t *stereo, size_t frames,
                                uint32_t rate, float playback_envelope)
{
    (void)ctx;
    portENTER_CRITICAL(&s_speech_lock);
    bool playing = s_speech && s_speech->active && s_speech->playing;
    portEXIT_CRITICAL(&s_speech_lock);
    if (mic_dsp_interrupt(&s_interrupt_dsp, stereo, frames, rate, playback_envelope, playing))
        orb_voice_speech_stop(ORB_SPEECH_TALK);
}

static void speech_hello(cJSON *hello, void *ctx)
{
    (void)ctx;
    cJSON *descriptor = cJSON_CreateObject();
    cJSON *formats = cJSON_CreateArray(), *rates = cJSON_CreateArray();
    if (!descriptor || !formats || !rates) goto failed;
    if (!cJSON_AddItemToArray(formats, cJSON_CreateString("pcm_s16le")) ||
        !cJSON_AddItemToArray(rates, cJSON_CreateNumber(16000)) ||
        !cJSON_AddItemToArray(rates, cJSON_CreateNumber(24000))) goto failed;
    cJSON_AddItemToObject(descriptor, "formats", formats); formats = NULL;
    cJSON_AddItemToObject(descriptor, "rates", rates); rates = NULL;
    if (!cJSON_AddNumberToObject(descriptor, "prebuffer_ms", 150)) goto failed;
    cJSON_AddItemToObject(hello, "speech", descriptor);
    return;
failed:
    cJSON_Delete(formats); cJSON_Delete(rates); cJSON_Delete(descriptor);
}

static void speech_poll(void)
{
    if (!s_speech) return;
    uint32_t generation = orb_link_generation();
    bool available = orb_link_supports_voice(), allowed = orb_sound_speech_allowed();
    portENTER_CRITICAL(&s_speech_lock);
    speech_generation(generation);
    if (!available) {
        speech_core_init(s_speech);
        s_stopped_count = 0;
    } else if (!allowed) speech_core_stop(s_speech, SPEECH_SILENT);
    else if (s_core->opened) speech_core_stop(s_speech, SPEECH_INTERRUPTED);
    else speech_core_tick(s_speech, now_ms());
    speech_receipt();
    bool active = s_speech->active;
    stopped_receipt_t receipts[4];
    unsigned count = s_stopped_count;
    for (unsigned i = 0; i < count; ++i) receipts[i] = s_stopped[(s_stopped_head + i) % 4];
    s_stopped_count = s_stopped_head = 0;
    portEXIT_CRITICAL(&s_speech_lock);
    orb_sound_set_speech_active(active);
    for (unsigned i = 0; i < count; ++i) {
        cJSON *p = cJSON_CreateObject();
        if (!p) continue;
        if (!cJSON_AddNumberToObject(p, "stream", receipts[i].stream) ||
            !cJSON_AddStringToObject(p, "reason", speech_stop_name(receipts[i].reason))) {
            cJSON_Delete(p);
            continue;
        }
        orb_link_notify_on(receipts[i].generation, "voice.speak.stopped", p);
    }
}

void orb_voice_speech_stop(orb_speech_stop_t reason)
{
    if (!s_speech) return;
    speech_stop_t why = reason == ORB_SPEECH_DOUBLE_TAP ? SPEECH_DOUBLE_TAP :
                        reason == ORB_SPEECH_TALK ? SPEECH_TALK : SPEECH_STOP;
    portENTER_CRITICAL(&s_speech_lock);
    speech_core_stop(s_speech, why);
    speech_receipt();
    portEXIT_CRITICAL(&s_speech_lock);
    orb_voice_wake();
}

void orb_voice_speech_status(orb_speech_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_speech) return;
    portENTER_CRITICAL(&s_speech_lock);
    status->active = s_speech->active;
    status->buffering = s_speech->active && !s_speech->playing;
    status->stream = s_speech->stream;
    status->rate = s_speech->rate;
    status->underruns = s_speech->underruns;
    memcpy(status->text, s_speech->caption, sizeof(status->text));
    portEXIT_CRITICAL(&s_speech_lock);
}

esp_err_t orb_voice_start(void)
{
    if (s_task) return ESP_OK;
    if (!orb_model_init()) return ESP_ERR_NO_MEM;
    s_core = heap_caps_calloc(1, sizeof(*s_core), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_event_storage = heap_caps_malloc(VOICE_EVENTS * sizeof(voice_event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_speech = heap_caps_calloc(1, sizeof(*s_speech), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_core || !s_event_storage || !s_speech) goto failed;
    mic_dsp_init(&s_capture_dsp);
    mic_dsp_init(&s_interrupt_dsp);
    s_events = xQueueCreateStatic(VOICE_EVENTS, sizeof(voice_event_t), s_event_storage, &s_event_queue);
    if (!s_events) goto failed;
    const voice_ops_t ops = {audio_open, audio_close, audio_read, audio_send, request_send,
                             answer_send, model_publish, model_focus};
    voice_core_init(s_core, &ops, NULL);
    if (orb_link_on_event("voice.result", on_voice, NULL) != ESP_OK ||
        orb_link_on_event("voice.error", on_voice, NULL) != ESP_OK ||
        orb_link_on_event("voice.speak", on_speech, NULL) != ESP_OK ||
        orb_link_on_event("voice.speak.end", on_speech, NULL) != ESP_OK ||
        orb_link_on_event("voice.speak.stop", on_speech, NULL) != ESP_OK ||
        orb_link_on_audio(on_speech_pcm, NULL) != ESP_OK ||
        orb_link_on_hello(speech_hello, NULL) != ESP_OK) goto failed;
    if (xTaskCreatePinnedToCore(voice_task, "orb_voice", VOICE_STACK, NULL, 4, &s_task, 0) != pdPASS)
        goto failed;
    orb_model_set_voice_notify(model_request_wake, NULL);
    orb_sound_set_speech_source(speech_read, NULL);
    orb_sound_set_mic_sink(speech_microphones, NULL);
    orb_link_set_voice_available(true);
    return ESP_OK;
failed:
    orb_link_set_voice_available(false);
    /* Registration may have succeeded already; callbacks are guarded below by
     * the startup ordering (the app starts the link only after this succeeds). */
    if (s_events) vQueueDelete(s_events);
    s_events = NULL;
    free(s_event_storage); s_event_storage = NULL;
    free(s_core); s_core = NULL;
    free(s_speech); s_speech = NULL;
    return ESP_ERR_NO_MEM;
}

void orb_voice_boot(bool held)
{
    portENTER_CRITICAL(&s_input_lock);
    bool changed = s_boot_held != held;
    s_boot_held = held;
    portEXIT_CRITICAL(&s_input_lock);
    if (changed) orb_voice_wake();
}
