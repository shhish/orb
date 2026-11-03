#include "test.h"
#include "link_proto.h"
#include "link_v11.h"
#include "orb_link_memory.h"
#include "esp_heap_caps.h"

#include <stdlib.h>

int g_checks, g_failures;

typedef struct {
    void *ptr;
    size_t size;
} allocation_t;

static allocation_t s_allocations[2048];
static size_t s_live, s_live_bytes, s_peak_bytes, s_calls, s_small_calls, s_fail_at;
static bool s_psram;
static size_t s_retained;
static orb_model_snapshot_t s_view;

static void *track_alloc(size_t size)
{
    ++s_calls;
    if (size <= 1024) ++s_small_calls;
    if (s_fail_at && s_calls == s_fail_at) return NULL;
    void *ptr = malloc(size);
    CHECK(ptr != NULL);
    if (!ptr) return NULL;
    size_t i;
    for (i = 0; i < sizeof(s_allocations) / sizeof(*s_allocations); ++i)
        if (!s_allocations[i].ptr) break;
    CHECK(i < sizeof(s_allocations) / sizeof(*s_allocations));
    s_allocations[i] = (allocation_t){ ptr, size };
    ++s_live;
    s_live_bytes += size;
    if (s_live_bytes > s_peak_bytes) s_peak_bytes = s_live_bytes;
    return ptr;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    CHECK(s_psram && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return track_alloc(size);
}

void heap_caps_free(void *ptr)
{
    if (!ptr) return;
    size_t i;
    for (i = 0; i < sizeof(s_allocations) / sizeof(*s_allocations); ++i)
        if (s_allocations[i].ptr == ptr) break;
    CHECK(i < sizeof(s_allocations) / sizeof(*s_allocations));
    if (i == sizeof(s_allocations) / sizeof(*s_allocations)) return;
    --s_live;
    s_live_bytes -= s_allocations[i].size;
    s_allocations[i] = (allocation_t){ 0 };
    free(ptr);
}

static void reset_counts(void)
{
    CHECK(s_live == (s_retained ? 2u : 0u) && s_live_bytes == s_retained);
    s_calls = s_small_calls = s_peak_bytes = s_fail_at = 0;
}

/* Six full-field sessions fit the negotiated 4 KiB envelope. Two chunks
 * reproduce the 8-session focus workload without retaining trees across chunks. */
static size_t snapshot(char text[4097], unsigned first, bool reset, bool more)
{
    char summary[161], reason[65];
    memset(summary, 's', sizeof(summary) - 1); summary[sizeof(summary) - 1] = 0;
    memset(reason, 'r', sizeof(reason) - 1); reason[sizeof(reason) - 1] = 0;
    size_t n = (size_t)snprintf(text, 4097,
        "{\"t\":\"evt\",\"m\":\"sessions.snapshot\",\"p\":{\"reset\":%s,\"more\":%s,\"sessions\":[",
        reset ? "true" : "false", more ? "true" : "false");
    for (unsigned i = 0; i < 6 && first + i < ORB_MODEL_MAX_FOCUS; ++i) {
        n += (size_t)snprintf(text + n, 4097 - n,
            "%s{\"id\":\"session-%u\",\"title\":\"A live coding session\",\"project\":\"orb\","
            "\"agent\":\"codex\",\"state\":\"busy\",\"reason\":\"%s\",\"summary\":\"%s\","
            "\"updated\":1234,\"can_message\":true}", i ? "," : "", first + i, reason, summary);
        CHECK(n < 4097);
    }
    n += (size_t)snprintf(text + n, 4097 - n, "]}}");
    CHECK(n <= 4096);
    return n;
}

int main(void)
{
    char text[4097];
    size_t len = snapshot(text, 0, true, true);
    cJSON_Hooks before = { .malloc_fn = track_alloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&before);
    link_msg_t message;
    cJSON *root = link_parse(text, len, &message);
    CHECK(root != NULL);
    size_t generic_calls = s_calls, generic_small = s_small_calls, generic_peak = s_peak_bytes;
    CHECK(generic_calls > 150 && generic_small == generic_calls);
    cJSON_Delete(root);
    reset_counts();

    s_psram = true;
    orb_link_memory_init();
    s_retained = ORB_LINK_PARSE_ARENA_BYTES + ORB_LINK_RX_POOL_BYTES;
    CHECK(s_live == 2 && s_live_bytes == s_retained);
    reset_counts();
    root = link_parse(text, len, &message);
    CHECK(root != NULL && s_calls == generic_calls && s_small_calls == generic_small);
    CHECK(s_peak_bytes == generic_peak + s_retained);
    /* Repeated init cannot mutate hooks while an earlier tree is live. */
    orb_link_memory_init();
    cJSON_Delete(root);
    reset_counts();
    printf("JSON attribution: %zu-byte snapshot chunk, %zu allocations <=1 KiB, %zu peak payload bytes moved to PSRAM\n",
           len, generic_small, generic_peak);

    /* Decode and apply repeated live envelopes without a single allocator
     * call. Even tiny nested input and malformed exhaustion stay in the pool. */
    CHECK(orb_link_memory_begin_parse());
    root = link_parse(text, len, &message);
    orb_link_memory_end_parse();
    CHECK(root && s_calls == 0 && orb_link_memory_parse_used() < ORB_LINK_PARSE_ARENA_BYTES);
    cJSON_Delete(root);
    CHECK(s_calls == 0);
    char dense[4097];
    size_t dense_len = 0;
    strcpy(dense, "{\"t\":\"evt\",\"m\":\"unknown\",\"p\":{\"a\":[");
    dense_len = strlen("{\"t\":\"evt\",\"m\":\"unknown\",\"p\":{\"a\":[");
    while (dense_len + 5 < sizeof(dense)) {
        dense[dense_len++] = '0'; dense[dense_len++] = ',';
    }
    --dense_len;
    memcpy(dense + dense_len, "]}}", 4); dense_len += 3;
    CHECK(orb_link_memory_begin_parse());
    root = link_parse(dense, dense_len, &message);
    orb_link_memory_end_parse();
    CHECK(root && s_calls == 0);
    cJSON_Delete(root);
    reset_counts();

    char *slots[ORB_LINK_RX_SMALL_SLOTS + ORB_LINK_RX_LARGE_SLOTS];
    for (size_t i = 0; i < sizeof(slots)/sizeof(*slots); ++i) {
        slots[i] = orb_link_memory_rx_resize(NULL, ORB_LINK_RX_SMALL_BYTES);
        CHECK(slots[i] != NULL && s_calls == 0);
    }
    CHECK(!orb_link_memory_rx_resize(NULL, 1) && s_calls == 0);
    for (size_t i = 0; i < sizeof(slots)/sizeof(*slots); ++i) orb_link_memory_rx_free(slots[i]);
    char *small = orb_link_memory_rx_resize(NULL, 20);
    strcpy(small, "fragmented frame");
    char *large = orb_link_memory_rx_resize(small, ORB_LINK_RX_LARGE_BYTES);
    CHECK(large && large != small && !strcmp(large, "fragmented frame"));
    CHECK(s_calls == 0 && !orb_link_memory_rx_resize(large, ORB_LINK_RX_LARGE_BYTES + 1));
    orb_link_memory_rx_free(large);
    reset_counts();

    /* Every parser allocation can fail without leaking a partial DOM or
     * falling back to the host/default allocator. Recovery uses the same hooks. */
    for (size_t i = 1; i <= generic_calls; ++i) {
        s_fail_at = i;
        root = link_parse(text, len, &message);
        CHECK(root == NULL);
        cJSON_Delete(root);
        reset_counts();
    }

    CHECK(orb_model_init());
    link_v11_reset();
    orb_model_set_connection(true, false);
    for (unsigned i = 0; i < 2; ++i) {
        len = snapshot(text, i * 6, i == 0, i != 1);
        root = link_parse(text, len, &message);
        CHECK(root != NULL);
        CHECK(link_v11_event(message.name, message.body) == LINK_V11_APPLIED);
        cJSON_Delete(root);
        CHECK(orb_model_snapshot(&s_view));
        CHECK(s_view.session_count == (i == 1 ? 8 : 0));
        reset_counts();
    }
    CHECK(s_view.online && s_view.synced && !strcmp(s_view.sessions[7].id, "session-7"));

    for (unsigned seq = 0; seq < 500; ++seq) {
        len = (size_t)snprintf(text, sizeof(text),
            "{\"t\":\"evt\",\"m\":\"agent.stream\",\"p\":{\"session\":\"session-0\",\"seq\":%u,\"at\":1234,"
            "\"state\":\"busy\",\"stale\":false,\"activity\":\"Build\",\"steps\":[{\"id\":\"build\",\"kind\":\"tool\","
            "\"label\":\"Build\",\"state\":\"running\"}],\"assistant\":{\"id\":\"a\",\"text\":\"Testing next.\",\"final\":false}}}", seq);
        CHECK(orb_link_memory_begin_parse());
        root = link_parse(text, len, &message);
        orb_link_memory_end_parse();
        CHECK(root && link_v11_event(message.name, message.body) == LINK_V11_APPLIED);
        cJSON_Delete(root);
        CHECK(s_calls == 0);
    }
    const char *pushed[] = {
        "{\"t\":\"evt\",\"m\":\"usage.snapshot\",\"p\":{\"windows\":[{\"provider\":\"codex\",\"label\":\"5 hours\","
        "\"used_percent\":38,\"resets_at\":2000,\"observed_at\":1234,\"stale\":false,\"source\":\"rollout\"}],\"unknown\":[\"claude\"]}}",
        "{\"t\":\"res\",\"id\":10,\"ok\":true,\"r\":{\"accepted\":true,\"status\":\"queued\",\"reason\":\"Queued for Codex\"}}"
    };
    for (size_t i = 0; i < sizeof(pushed)/sizeof(*pushed); ++i) {
        CHECK(orb_link_memory_begin_parse());
        root = link_parse(pushed[i], strlen(pushed[i]), &message);
        orb_link_memory_end_parse();
        CHECK(root != NULL);
        if (!i) CHECK(link_v11_event(message.name, message.body) == LINK_V11_APPLIED);
        else {
            orb_message_result_t receipt;
            CHECK(orb_link_read_message_receipt(message.body, NULL, &receipt));
            CHECK(receipt.status == ORB_SEND_QUEUED);
        }
        cJSON_Delete(root);
        CHECK(s_calls == 0);
    }
    reset_counts();

    /* The same global hooks serve non-link JSON builders and printed buffers. */
    cJSON *params = cJSON_CreateObject();
    CHECK(params && cJSON_AddNumberToObject(params, "stream", 7));
    char *encoded = link_encode_req(1, "voice.start", params);
    CHECK(encoded != NULL && s_live == 3);
    cJSON_free(encoded);
    reset_counts();
    s_fail_at = 1;
    CHECK(cJSON_CreateObject() == NULL);
    reset_counts();
    cJSON_InitHooks(NULL);
    printf("link memory: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
