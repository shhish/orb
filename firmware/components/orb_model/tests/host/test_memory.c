/* Compile the actual ESP allocation branch; normal model tests use pthreads. */
#include "orb_model.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks, s_allocations;
static bool s_fail_alloc = true;
static void *s_payload;
static orb_model_snapshot_t s_view;

/* This fixture isolates the ESP allocation branch; notification delivery has
 * its own production registry coverage in test_model.c. */
bool orb_model_voice_notify(bool queued) { return queued; }

#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++s_allocations;
    if (s_fail_alloc) return NULL;
    void *payload = calloc(count, size);
    CHECK(payload != NULL);
    if (!s_payload) s_payload = payload;
    return payload;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks)
{
    CHECK(semaphore != NULL && ticks == portMAX_DELAY);
    return 1;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore != NULL);
    return 1;
}

int main(void)
{
    /* Exhausted PSRAM must leave the store absent, without an internal retry. */
    CHECK(!orb_model_init());
    CHECK(s_allocations == 1);
    CHECK(!orb_model_snapshot(&s_view));
    CHECK(orb_model_revision() == 0);
    s_fail_alloc = false;
    CHECK(orb_model_init());
    CHECK(orb_model_init());
    CHECK(s_allocations == 2);

    orb_model_set_connection(true, false);
    for (size_t i = 0; i < ORB_MODEL_MAX_SESSIONS; ++i) {
        orb_session_t session = {.agent = ORB_AGENT_CODEX, .state = ORB_SESSION_BUSY};
        snprintf(session.id, sizeof(session.id), "session-%zu", i);
        CHECK(orb_model_sessions_chunk(&session, 1, i == 0, i + 1 < ORB_MODEL_MAX_SESSIONS));
    }
    for (size_t i = 0; i < ORB_MODEL_MAX_CARDS; ++i) {
        orb_card_t card = {.kind = ORB_CARD_ATTENTION, .priority = ORB_PRIORITY_NORMAL};
        snprintf(card.id, sizeof(card.id), "card-%zu", i);
        CHECK(orb_model_upsert_card(&card));
    }
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 24 && s_view.card_count == 16 && s_view.synced);
    CHECK(!strcmp(s_view.sessions[23].id, "session-23"));
    CHECK(!strcmp(s_view.cards[15].id, "card-15"));
    /* Chunk staging, the final commit, cards and snapshot copies allocate no
     * extra payloads: all retained copies use the one PSRAM store. */
    CHECK(s_allocations == 2);
    orb_agent_stream_t stream = {.session = "session-0", .state = ORB_SESSION_BUSY};
    strcpy(stream.activity, "Building");
    static orb_stream_log_t log;
    for (size_t n = 0; n < 1000; ++n) {
        stream.seq = n;
        snprintf(stream.assistant.id, sizeof(stream.assistant.id), "assistant-%zu", n);
        snprintf(stream.assistant.text, sizeof(stream.assistant.text), "Sentence %zu", n);
        stream.assistant.final = true;
        CHECK(orb_model_apply_stream(&stream));
        CHECK(orb_model_get_stream_log("session-0", &log));
        CHECK(log.count == (n < ORB_MODEL_STREAM_ROWS ? n + 1 : ORB_MODEL_STREAM_ROWS));
    }
    CHECK(!strcmp(log.rows[0].id, "assistant-976") && !strcmp(log.rows[23].id, "assistant-999"));
    orb_usage_snapshot_t usage = {.count = 1, .unknown = {false, true}};
    usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .used_percent = 62,
        .label = "5 hours", .source = ORB_USAGE_USAGE};
    CHECK(orb_model_set_usage(&usage));
    orb_message_t message = {.target = "session-0", .text = "Run tests"};
    orb_model_complete_message_receipt(&message, ORB_SEND_QUEUED, "Queued for Codex", NULL);
    CHECK(orb_model_snapshot(&s_view) && s_view.last_message.accepted &&
          s_view.last_message.status == ORB_SEND_QUEUED);
    CHECK(orb_model_get_stream_log("session-0", &log) && log.rows[23].kind == ORB_STREAM_USER);
    CHECK(s_allocations == 2); /* Every stream/usage/receipt used the startup PSRAM rings. */
    orb_model_weather_capability(true);
    orb_weather_snapshot_t weather = {.configured = true, .available = true, .place = "New York",
        .label = "Sunny", .unit = ORB_TEMPERATURE_F, .temperature = 72, .high = 75, .low = 58,
        .condition = ORB_WEATHER_CLEAR, .is_day = true, .updated_at = 1793221200};
    CHECK(orb_model_set_weather(&weather));
    CHECK(orb_model_weather_snapshot(&weather) && weather.revision == 1 && weather.temperature == 72);
    CHECK(s_allocations == 2); /* Weather shares the one startup store too. */
    CHECK(s_allocations == 2);
    orb_model_reset();
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 0 && s_view.card_count == 0);
    CHECK(s_allocations == 2);
    static orb_idle_page_t idle;
    s_fail_alloc = true;
    CHECK(!orb_model_idle_open());
    CHECK(s_allocations == 3 && !orb_model_idle_snapshot(&idle));
    s_fail_alloc = false;
    CHECK(orb_model_idle_open());
    CHECK(s_allocations == 4 && orb_model_idle_snapshot(&idle) && idle.pending);
    CHECK(orb_model_idle_open() && s_allocations == 4);
    orb_model_idle_close();
    CHECK(!orb_model_idle_snapshot(&idle));
    CHECK(orb_model_idle_open() && s_allocations == 5);
    orb_model_reset(); /* Reset also frees a currently open page. */
    CHECK(!orb_model_idle_snapshot(&idle));
    free(s_payload);
    printf("orb_model memory: %u checks passed\n", s_checks);
    return 0;
}
