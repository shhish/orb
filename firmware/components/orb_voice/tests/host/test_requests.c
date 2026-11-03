#include "voice_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
typedef struct { int unused; } cJSON;
typedef struct {
    voice_request_kind_t request;
    uint16_t stream;
    uint32_t generation;
} reply_context_t;

static unsigned s_checks, s_allocations, s_requests;
static bool s_fail_alloc, s_fail_json, s_json_live;
static esp_err_t s_request_result;
static void *s_allocation;
static cJSON s_json;
static voice_core_t s_test_core = {.generation = 41};
static voice_core_t *s_core = &s_test_core;
static const char *s_expected_method;
typedef enum { EVENT_REPLY } event_kind_t;
typedef struct {
    event_kind_t kind;
    voice_request_kind_t request;
    uint16_t stream;
    uint32_t generation;
    bool ok;
    orb_send_status_t status;
    char text[281], code[33];
} voice_event_t;
static voice_event_t s_event;
static bool s_reminder_ok;
static unsigned s_reminders;
typedef enum { ORB_SOUND_DONE, ORB_SOUND_FAILED } orb_sound_cue_t;
static orb_sound_cue_t s_cue;
static bool orb_sound_play(orb_sound_cue_t cue) { s_cue = cue; return true; }

#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static bool orb_time_reminder_from_voice(const char *text, char *reason, size_t capacity)
{
    CHECK(!strcmp(text, "test transcript"));
    ++s_reminders;
    snprintf(reason, capacity, "%s", s_reminder_ok ? "Saved locally" : "Invalid reminder");
    return s_reminder_ok;
}

static void event_post(const voice_event_t *event) { s_event = *event; }

static void *heap_caps_malloc(size_t size, unsigned caps)
{
    CHECK(size == sizeof(reply_context_t));
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_allocations;
    if (s_fail_alloc) return NULL;
    CHECK(s_allocation == NULL);
    s_allocation = malloc(size);
    CHECK(s_allocation != NULL);
    return s_allocation;
}

static void test_free(void *ptr)
{
    CHECK(ptr == s_allocation && ptr != NULL);
    free(ptr);
    s_allocation = NULL;
}

static cJSON *cJSON_CreateObject(void)
{
    CHECK(!s_json_live);
    s_json_live = true;
    return &s_json;
}

static cJSON *cJSON_AddNumberToObject(cJSON *object, const char *key, double value)
{
    CHECK(object == &s_json && !strcmp(key, "stream") && value == 7);
    return s_fail_json ? NULL : object;
}

static cJSON *cJSON_AddStringToObject(cJSON *object, const char *key, const char *value)
{
    CHECK(object == &s_json);
    CHECK((!strcmp(key, "target") && !strcmp(value, "dot")) ||
          (!strcmp(key, "text") && !strcmp(value, "test transcript")));
    return object;
}

static void cJSON_Delete(cJSON *object)
{
    CHECK(object == &s_json && s_json_live);
    s_json_live = false;
}

static void on_reply(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)err; (void)result; (void)error; (void)ctx;
}

static esp_err_t orb_link_request_on(uint32_t generation, const char *method, cJSON *params,
                                   uint32_t timeout,
                                   void (*callback)(esp_err_t, const cJSON *, const cJSON *, void *),
                                   void *ctx)
{
    CHECK(generation == 41 && timeout == 10000 && callback == on_reply);
    CHECK(!strcmp(method, s_expected_method));
    const reply_context_t *reply = ctx;
    CHECK(reply == s_allocation && reply->generation == 41 && reply->stream == 7);
    CHECK(reply->request <= VOICE_REQ_SEND);
    ++s_requests;
    cJSON_Delete(params); /* The link consumes params even when enqueue fails. */
    return s_request_result;
}

#define free test_free
#include "voice_requests.inc"
#undef free

int main(void)
{
    orb_voice_state_t state = {.stream = 7, .target = "dot", .text = "test transcript"};
    s_fail_alloc = true;
    CHECK(!request_send(NULL, VOICE_REQ_BEGIN, &state));
    CHECK(s_allocations == 1 && s_requests == 0 && !s_json_live && !s_allocation);

    s_fail_alloc = false;
    s_fail_json = true;
    CHECK(!request_send(NULL, VOICE_REQ_BEGIN, &state));
    CHECK(s_allocations == 1 && s_requests == 0 && !s_json_live);
    s_fail_json = false;

    s_expected_method = "voice.begin";
    s_request_result = ESP_FAIL;
    CHECK(!request_send(NULL, VOICE_REQ_BEGIN, &state));
    CHECK(!s_allocation && !s_json_live && s_requests == 1);

    static const char *const methods[] = {"voice.begin", "voice.end", "voice.cancel", "message.send"};
    s_request_result = ESP_OK;
    for (unsigned kind = VOICE_REQ_BEGIN; kind <= VOICE_REQ_SEND; ++kind) {
        s_expected_method = methods[kind];
        CHECK(request_send(NULL, (voice_request_kind_t)kind, &state));
        CHECK(s_allocation != NULL && !s_json_live);
        CHECK(((reply_context_t *)s_allocation)->request == (voice_request_kind_t)kind);
        /* An accepted request retains its PSRAM context until its callback. */
        test_free(s_allocation);
    }
    CHECK(s_requests == 5 && s_allocations == 6);
    strcpy(state.target, "time");
    CHECK(request_send(NULL, VOICE_REQ_SEND, &state));
    CHECK(s_reminders == 1 && s_requests == 5 && s_allocations == 6);
    CHECK(s_event.kind == EVENT_REPLY && s_event.request == VOICE_REQ_SEND && s_event.stream == 7);
    CHECK(s_event.generation == 41 && !s_event.ok && s_event.status == ORB_SEND_FAILED);
    CHECK(s_cue == ORB_SOUND_FAILED);
    CHECK(!strcmp(s_event.text, "Invalid reminder") && !strcmp(s_event.code, "bad_request"));
    s_reminder_ok = true;
    CHECK(request_send(NULL, VOICE_REQ_SEND, &state));
    CHECK(s_reminders == 2 && s_requests == 5 && s_allocations == 6);
    CHECK(s_event.ok && s_event.status == ORB_SEND_DELIVERED && !s_event.code[0]);
    CHECK(s_cue == ORB_SOUND_DONE);
    printf("orb_voice request memory: %u checks passed\n", s_checks);
    return 0;
}
