#include "voice_core.h"
#include "speech_core.h"
#include "cJSON.h"
#include "link_v11.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT -2
#include "voice_events_types.inc"

static unsigned s_checks, s_posts, s_wakes;
static bool s_events = true;
static voice_event_t s_last;
static speech_core_t s_speech_storage, *s_speech = &s_speech_storage;
static uint32_t s_speech_generation;
typedef struct { uint16_t stream; speech_stop_t reason; uint32_t generation; } stopped_receipt_t;
static stopped_receipt_t s_stopped[4];
static unsigned s_stopped_head, s_stopped_count;
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static uint32_t orb_link_generation(void) { return 42; }
static bool orb_link_supports_voice(void) { return true; }
static uint64_t now_ms(void) { return 0; }
static void event_post(const voice_event_t *event) { s_last = *event; ++s_posts; }
static void orb_voice_wake(void) { s_wakes++; }
#include "voice_events.inc"

static void event(const char *name, const char *json)
{
    cJSON *params = cJSON_Parse(json);
    CHECK(params != NULL);
    on_voice(name, params, NULL);
    cJSON_Delete(params);
}

static void reply(voice_request_kind_t kind, esp_err_t err, const char *json)
{
    reply_context_t *ctx = malloc(sizeof(*ctx));
    CHECK(ctx != NULL);
    *ctx = (reply_context_t){kind, 7, 41};
    cJSON *params = json ? cJSON_Parse(json) : NULL;
    CHECK(!json || params != NULL);
    on_reply(err, err == ESP_OK ? params : NULL, err == ESP_OK ? NULL : params, ctx);
    cJSON_Delete(params);
}

static void speech_event(const char *name, const char *json)
{
    cJSON *params = cJSON_Parse(json);
    CHECK(params != NULL);
    on_speech(name, params, NULL);
    cJSON_Delete(params);
}

static void test_speech_replacement(void)
{
    CHECK(orb_model_init());
    speech_event("voice.speak", "{\"stream\":1,\"rate\":16000,\"format\":\"pcm_s16le\",\"text\":\"Valid caption\",\"voice\":\"Ava\"}");
    CHECK(s_speech->active && s_speech->stream == 1 && !s_stopped_count);
    CHECK(s_wakes == 1); /* A parked voice worker observes the first speech event. */
    /* cJSON deliberately leaves byte validation to the protocol consumer. A
     * malformed new caption must not interrupt an otherwise valid reply. */
    speech_event("voice.speak", "{\"stream\":2,\"rate\":16000,\"format\":\"pcm_s16le\",\"text\":\"\xc0\x80\"}");
    CHECK(s_speech->active && s_speech->stream == 1 && !s_stopped_count);
    speech_event("voice.speak", "{\"stream\":2,\"rate\":16000,\"format\":\"pcm_s16le\",\"text\":\"New caption\",\"voice\":\"\xc0\x80\"}");
    CHECK(s_speech->active && s_speech->stream == 1 && !s_stopped_count);
    speech_event("voice.speak", "{\"stream\":2,\"rate\":24000,\"format\":\"pcm_s16le\",\"text\":\"Replacement\"}");
    CHECK(s_speech->active && s_speech->stream == 2 && s_stopped_count == 1);
    CHECK(s_stopped[0].stream == 1 && s_stopped[0].reason == SPEECH_INTERRUPTED);
    orb_model_snapshot_t *view = malloc(sizeof(*view)); CHECK(view != NULL);
    CHECK(orb_model_snapshot(view) && !view->speech_voice[0]);
    free(view);
}

int main(void)
{
    test_speech_replacement();
    event("voice.error", "{\"stream\":7,\"code\":\"no_speech\",\"msg\":\"private provider text\"}");
    CHECK(s_posts == 1 && s_last.kind == EVENT_ERROR && s_last.stream == 7);
    CHECK(s_last.generation == 42 && !strcmp(s_last.code, "no_speech") && !s_last.text[0]);
    event("voice.error", "{\"stream\":65536,\"code\":\"no_speech\"}");
    event("voice.error", "{\"stream\":0.5,\"code\":\"no_speech\"}");
    CHECK(s_posts == 1);
    event("voice.error", "{\"stream\":7,\"code\":\"abcdefghijklmnopqrstuvwxyzabcdefg\"}");
    CHECK(s_posts == 2 && s_last.kind == EVENT_ERROR && !s_last.code[0]);
    event("voice.result", "{\"stream\":7,\"text\":\"recognized speech\"}");
    CHECK(s_posts == 3 && s_last.kind == EVENT_RESULT && !strcmp(s_last.text, "recognized speech"));
    CHECK(!s_last.code[0]);
    event("voice.result", "{\"stream\":7,\"text\":\"\"}");
    CHECK(s_posts == 4 && s_last.kind == EVENT_ERROR && !strcmp(s_last.code, "bad_request"));
    s_events = false;
    event("voice.error", "{\"stream\":7,\"code\":\"unsupported\"}");
    CHECK(s_posts == 4);
    s_events = true;
    reply(VOICE_REQ_BEGIN, ESP_FAIL, "{\"code\":\"unsupported\",\"msg\":\"private path\"}");
    CHECK(s_posts == 5 && s_last.kind == EVENT_REPLY && !s_last.ok);
    CHECK(s_last.stream == 7 && s_last.generation == 41 && !strcmp(s_last.code, "unsupported"));
    CHECK(!s_last.text[0]);
    reply(VOICE_REQ_END, ESP_ERR_TIMEOUT, NULL);
    CHECK(s_posts == 6 && !s_last.ok && !strcmp(s_last.code, "timeout"));
    reply(VOICE_REQ_CANCEL, ESP_FAIL, "{\"code\":\"not_found\"}");
    CHECK(s_posts == 7 && s_last.ok);
    reply(VOICE_REQ_SEND, ESP_OK, "{\"accepted\":false}");
    CHECK(s_posts == 8 && !s_last.ok);
    reply(VOICE_REQ_SEND, ESP_OK, "{\"accepted\":true}");
    CHECK(s_posts == 9 && s_last.ok);
    reply(VOICE_REQ_SEND, ESP_OK, "{\"accepted\":true,\"status\":\"queued\",\"reason\":\"Queued for Codex\"}");
    CHECK(s_last.ok && s_last.status == ORB_SEND_QUEUED && !strcmp(s_last.text, "Queued for Codex"));
    reply(VOICE_REQ_SEND, ESP_OK, "{\"accepted\":true,\"status\":\"delivered\",\"reason\":\"Transport received it\"}");
    CHECK(s_last.ok && s_last.status == ORB_SEND_DELIVERED);
    reply(VOICE_REQ_SEND, ESP_FAIL, "{\"status\":\"failed\",\"code\":\"unsupported\",\"msg\":\"Session read only\"}");
    CHECK(!s_last.ok && s_last.status == ORB_SEND_FAILED && !strcmp(s_last.text, "Session read only"));
    reply(VOICE_REQ_SEND, ESP_ERR_TIMEOUT, NULL);
    CHECK(!s_last.ok && s_last.status == ORB_SEND_FAILED && !strcmp(s_last.code, "timeout"));
    printf("orb_voice callbacks: %u checks passed\n", s_checks);
    return 0;
}
