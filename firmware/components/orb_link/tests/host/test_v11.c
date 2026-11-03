#include "test.h"
#include "link_proto.h"
#include "link_v11.h"

#include <math.h>
#include <stdlib.h>

int g_checks, g_failures;
static orb_model_snapshot_t s_view;

static cJSON *fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return NULL;
    char bytes[4097];
    size_t n = fread(bytes, 1, sizeof(bytes) - 1, file);
    fclose(file);
    bytes[n] = '\0';
    link_msg_t message;
    cJSON *root = link_parse(bytes, n, &message);
    CHECK(root != NULL);
    CHECK(message.kind == LINK_EVT);
    return root;
}

static cJSON *body(cJSON *root) { return cJSON_GetObjectItemCaseSensitive(root, "p"); }
static cJSON *session(cJSON *root) { return cJSON_GetObjectItemCaseSensitive(body(root), "session"); }

static void set_string(cJSON *object, const char *key, const char *text)
{
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(object, key, cJSON_CreateString(text)));
}

static link_v11_result_t event(cJSON *root)
{
    return link_v11_event(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "m")), body(root));
}

static void empty_snapshot(void)
{
    cJSON *value = cJSON_Parse("{\"sessions\":[],\"reset\":true,\"more\":false}");
    CHECK(value != NULL);
    CHECK(link_v11_event("sessions.snapshot", value) == LINK_V11_APPLIED);
    cJSON_Delete(value);
}

static void fresh(void)
{
    orb_model_reset();
    link_v11_reset();
    orb_model_set_connection(true, false);
    empty_snapshot();
}

static cJSON *snapshot_chunk(cJSON *s, bool reset, bool more)
{
    cJSON *p = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(p, "sessions");
    if (s) CHECK(cJSON_AddItemToArray(array, cJSON_Duplicate(s, true)));
    CHECK(cJSON_AddBoolToObject(p, "reset", reset));
    CHECK(cJSON_AddBoolToObject(p, "more", more));
    return p;
}

static void test_fixtures(void)
{
    fresh();
    cJSON *s = fixture("fixtures/v11-session.json"), *c = fixture("fixtures/v11-card.json");
    CHECK(event(s) == LINK_V11_APPLIED);
    CHECK(event(c) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 1 && s_view.card_count == 1);
    CHECK_STR(s_view.sessions[0].id, "s");
    CHECK_STR(s_view.sessions[0].summary, "Ready");
    CHECK(s_view.sessions[0].agent == ORB_AGENT_CODEX);
    CHECK(!s_view.sessions[0].can_message);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(session(s), "updated", cJSON_CreateNumber(0.5)));
    set_string(session(s), "summary", "Clock corrected; latest event wins");
    CHECK(event(s) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK_STR(s_view.sessions[0].summary, "Clock corrected; latest event wins");
    CHECK(s_view.cards[0].kind == ORB_CARD_CHOICE);
    CHECK(s_view.cards[0].options_count == 2);
    CHECK(s_view.cards[0].options[0].role == ORB_OPTION_PRIMARY);
    uint64_t rev = orb_model_revision();
    CHECK(event(s) == LINK_V11_APPLIED && event(c) == LINK_V11_APPLIED);
    CHECK(orb_model_revision() == rev);
    CHECK(link_v11_event("future.event", NULL) == LINK_V11_IGNORED);
    CHECK(link_v11_event(NULL, NULL) == LINK_V11_IGNORED);
    cJSON *p = cJSON_Parse("{\"id\":\"s\"}");
    CHECK(link_v11_event("session.remove", p) == LINK_V11_APPLIED);
    CHECK(link_v11_event("session.remove", p) == LINK_V11_APPLIED);
    cJSON_Delete(p);
    p = cJSON_Parse("{\"card\":\"c\"}");
    CHECK(link_v11_event("card.cancel", p) == LINK_V11_APPLIED);
    CHECK(link_v11_event("card.cancel", p) == LINK_V11_APPLIED);
    cJSON_Delete(p);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 0 && s_view.card_count == 0);
    cJSON_Delete(s);
    cJSON_Delete(c);
}

static void test_zero_expiry(void)
{
    fresh();
    cJSON *c = fixture("fixtures/v11-card.json");
    CHECK(cJSON_AddNumberToObject(body(c), "expires", 0));
    CHECK(event(c) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.cards[0].has_expiry && s_view.cards[0].expires == 0);
    CHECK(orb_model_expire(0) == 1);
    CHECK(orb_model_snapshot(&s_view) && s_view.card_count == 0);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(body(c), "expires", cJSON_CreateNumber(0.5)));
    CHECK(event(c) == LINK_V11_APPLIED);
    CHECK(orb_model_expire(1) == 1);
    cJSON_Delete(c);
}

static void test_snapshot_transaction(void)
{
    fresh();
    cJSON *s = fixture("fixtures/v11-session.json");
    CHECK(event(s) == LINK_V11_APPLIED);
    set_string(session(s), "id", "replacement");
    cJSON *p = snapshot_chunk(session(s), true, true);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 1);
    CHECK_STR(s_view.sessions[0].id, "s");
    cJSON_Delete(p);
    link_v11_reset();
    /* A disconnect invalidates an incomplete snapshot and its continuation. */
    p = snapshot_chunk(NULL, false, false);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_INVALID);
    cJSON_Delete(p);
    p = snapshot_chunk(session(s), true, false);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_APPLIED);
    cJSON_Delete(p);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 1);
    CHECK_STR(s_view.sessions[0].id, "replacement");
    p = snapshot_chunk(session(s), true, true);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_APPLIED);
    CHECK(event(s) == LINK_V11_INVALID); /* Updates cannot interleave chunks. */
    cJSON_Delete(p);
    p = snapshot_chunk(NULL, false, false);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_INVALID);
    cJSON_Delete(p);
    empty_snapshot();
    CHECK(orb_model_snapshot(&s_view) && s_view.session_count == 0);
    /* Hello auto-subscription followed by an explicit refresh yields another
     * ordered complete snapshot. Each replaces sessions/cards atomically. */
    for (size_t round = 0; round < 2; ++round) {
        for (size_t i = 0; i < ORB_MODEL_MAX_FOCUS; ++i) {
            char id[20];
            snprintf(id, sizeof(id), "s-%zu", i);
            set_string(session(s), "id", id);
            p = snapshot_chunk(session(s), i == 0, i != ORB_MODEL_MAX_FOCUS - 1);
            CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_APPLIED);
            cJSON_Delete(p);
        }
        CHECK(orb_model_snapshot(&s_view) && s_view.session_count == ORB_MODEL_MAX_FOCUS);
        CHECK_STR(s_view.sessions[ORB_MODEL_MAX_FOCUS - 1].id, "s-7");
    }
    /* A duplicate identity is invalid rather than silently dropping one row. */
    p = snapshot_chunk(session(s), true, true);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_APPLIED);
    cJSON_Delete(p);
    p = snapshot_chunk(session(s), false, false);
    CHECK(link_v11_event("sessions.snapshot", p) == LINK_V11_INVALID);
    cJSON_Delete(p);
    cJSON_Delete(s);
}

static void test_limits(void)
{
    const char *session_fields[] = {"id", "title", "project", "reason", "summary"};
    const size_t session_limits[] = {48, 64, 32, 64, 160};
    char text[282];
    for (size_t f = 0; f < 5; ++f) {
        fresh();
        cJSON *s = fixture("fixtures/v11-session.json");
        memset(text, 'x', session_limits[f]); text[session_limits[f]] = '\0';
        set_string(session(s), session_fields[f], text);
        CHECK(event(s) == LINK_V11_APPLIED);
        text[session_limits[f]] = 'x'; text[session_limits[f] + 1] = '\0';
        set_string(session(s), session_fields[f], text);
        CHECK(event(s) == LINK_V11_INVALID);
        cJSON_Delete(s);
    }
    const char *card_fields[] = {"id", "title", "body", "detail", "session"};
    const size_t card_limits[] = {64, 64, 280, 200, 48};
    for (size_t f = 0; f < 5; ++f) {
        fresh();
        cJSON *c = fixture("fixtures/v11-card.json");
        cJSON *p = body(c);
        if (!cJSON_HasObjectItem(p, card_fields[f])) CHECK(cJSON_AddStringToObject(p, card_fields[f], ""));
        memset(text, 'x', card_limits[f]); text[card_limits[f]] = '\0';
        set_string(p, card_fields[f], text);
        CHECK(event(c) == LINK_V11_APPLIED);
        text[card_limits[f]] = 'x'; text[card_limits[f] + 1] = '\0';
        set_string(p, card_fields[f], text);
        CHECK(event(c) == LINK_V11_INVALID);
        cJSON_Delete(c);
    }
    for (size_t n = 0; n <= ORB_MODEL_MAX_FOCUS; ++n) {
        fresh();
        cJSON *s = fixture("fixtures/v11-session.json");
        for (size_t i = 0; i < n; ++i) {
            snprintf(text, sizeof(text), "session-%zu", i);
            set_string(session(s), "id", text);
            CHECK(event(s) == LINK_V11_APPLIED);
        }
        CHECK(orb_model_snapshot(&s_view) && s_view.session_count == n);
        set_string(session(s), "id", "overflow");
        CHECK(event(s) == (n == ORB_MODEL_MAX_FOCUS ? LINK_V11_INVALID : LINK_V11_APPLIED));
        cJSON_Delete(s);
    }
    fresh();
    cJSON *c = fixture("fixtures/v11-card.json");
    for (size_t i = 0; i < ORB_MODEL_MAX_CARDS; ++i) {
        snprintf(text, sizeof(text), "card-%zu", i);
        set_string(body(c), "id", text);
        CHECK(event(c) == LINK_V11_APPLIED);
    }
    set_string(body(c), "id", "overflow");
    CHECK(event(c) == LINK_V11_INVALID);
    CHECK(orb_model_snapshot(&s_view) && s_view.card_count == ORB_MODEL_MAX_CARDS);
    cJSON_Delete(c);
}

static void test_malformed(void)
{
    const char *bad_agent[] = {"unknown", "CODEX", "", "codex "};
    for (size_t i = 0; i < 4; ++i) {
        fresh();
        cJSON *s = fixture("fixtures/v11-session.json");
        set_string(session(s), "agent", bad_agent[i]);
        CHECK(event(s) == LINK_V11_INVALID);
        cJSON_Delete(s);
    }
    const char *invalid_utf8[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"};
    for (size_t i = 0; i < 5; ++i) {
        fresh();
        cJSON *s = fixture("fixtures/v11-session.json");
        set_string(session(s), "title", invalid_utf8[i]);
        CHECK(event(s) == LINK_V11_INVALID);
        cJSON_Delete(s);
    }
    fresh();
    cJSON *s = fixture("fixtures/v11-session.json");
    set_string(session(s), "title", "\xf0\x9f\x8c\x95 Round");
    CHECK(event(s) == LINK_V11_APPLIED);
    cJSON_DeleteItemFromObjectCaseSensitive(session(s), "can_message");
    CHECK(cJSON_AddNumberToObject(session(s), "can_message", 1));
    CHECK(event(s) == LINK_V11_INVALID);
    cJSON_Delete(s);
    const double times[] = {-1, NAN, INFINITY};
    for (size_t i = 0; i < 3; ++i) {
        s = fixture("fixtures/v11-session.json");
        /* Avoid a third-party CreateNumber(NAN) float-to-int conversion: the
         * mapper must still reject a malformed injected numeric node. */
        cJSON_GetObjectItemCaseSensitive(session(s), "updated")->valuedouble = times[i];
        CHECK(event(s) == LINK_V11_INVALID);
        cJSON_Delete(s);
    }
    s = fixture("fixtures/v11-session.json");
    cJSON_GetObjectItemCaseSensitive(session(s), "updated")->valuedouble = 1e300;
    CHECK(event(s) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.sessions[0].updated == UINT64_MAX);
    cJSON_Delete(s);
    s = fixture("fixtures/v11-session.json");
    CHECK(cJSON_AddStringToObject(session(s), "id", "duplicate"));
    CHECK(event(s) == LINK_V11_INVALID);
    cJSON_Delete(s);
    cJSON *c = fixture("fixtures/v11-card.json");
    cJSON *options = cJSON_GetObjectItemCaseSensitive(body(c), "options");
    char identifier[34];
    memset(identifier, 'o', 32); identifier[32] = '\0';
    set_string(cJSON_GetArrayItem(options, 0), "id", identifier);
    CHECK(event(c) == LINK_V11_APPLIED);
    identifier[32] = 'o'; identifier[33] = '\0';
    set_string(cJSON_GetArrayItem(options, 0), "id", identifier);
    CHECK(event(c) == LINK_V11_INVALID);
    set_string(cJSON_GetArrayItem(options, 0), "id", "yes");
    char label[22];
    memset(label, 'l', 20); label[20] = '\0';
    set_string(cJSON_GetArrayItem(options, 0), "label", label);
    CHECK(event(c) == LINK_V11_APPLIED);
    label[20] = 'l'; label[21] = '\0';
    set_string(cJSON_GetArrayItem(options, 0), "label", label);
    CHECK(event(c) == LINK_V11_INVALID);
    set_string(cJSON_GetArrayItem(options, 0), "label", "Continue");
    set_string(cJSON_GetArrayItem(options, 1), "id", "yes");
    CHECK(event(c) == LINK_V11_INVALID);
    set_string(cJSON_GetArrayItem(options, 1), "id", "no");
    CHECK(cJSON_AddStringToObject(body(c), "scope", "turn"));
    CHECK(event(c) == LINK_V11_INVALID); /* Scope belongs only on approval. */
    set_string(body(c), "kind", "approval");
    CHECK(event(c) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.cards[0].scope == ORB_APPROVAL_TURN);
    set_string(body(c), "scope", "account");
    CHECK(event(c) == LINK_V11_INVALID);
    cJSON_Delete(c);
}

static void test_answers(void)
{
    fresh();
    cJSON *c = fixture("fixtures/v11-card.json");
    CHECK(event(c) == LINK_V11_APPLIED);
    orb_answer_t answer = {.card = "c", .option = "yes"}, retry, recorded;
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_peek_answer(&retry));
    cJSON *p = link_v11_answer_params(&retry);
    CHECK(p != NULL);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "option")), "yes");
    cJSON_Delete(p);
    link_v11_reset();
    empty_snapshot();
    CHECK(orb_model_peek_answer(&retry));
    CHECK_STR(retry.card, "c");
    CHECK_STR(retry.option, "yes");
    bool delivered = true;
    p = cJSON_Parse("{\"card\":\"c\",\"answer\":{\"option\":\"no\"},\"delivered\":false}");
    CHECK(link_v11_read_answer(p, "c", &recorded, &delivered));
    CHECK(!delivered);
    CHECK_STR(recorded.option, "no"); /* The first decision can differ. */
    CHECK(orb_model_ack_answer(recorded.card));
    CHECK(!orb_model_peek_answer(&retry));
    CHECK(!link_v11_read_answer(p, "other-card", &recorded, &delivered));
    cJSON *decision = cJSON_GetObjectItemCaseSensitive(p, "answer");
    CHECK(cJSON_AddStringToObject(decision, "text", "Ambiguous"));
    CHECK(!link_v11_read_answer(p, "c", &recorded, &delivered));
    cJSON_Delete(p);
    answer.text[0] = 'x';
    CHECK(link_v11_answer_params(&answer) == NULL);
    memset(&answer, 0, sizeof(answer));
    CHECK(link_v11_answer_params(&answer) == NULL);
    CHECK(link_v11_message_params("dot", "", 0) == NULL);
    p = link_v11_message_params("dot", "Hello", 65535);
    CHECK(p != NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "stream")->valueint == 65535);
    cJSON_Delete(p);
    cJSON_Delete(c);
}

static void test_temperature_preferences(void)
{
    fresh();
    const char *base = "{\"sources\":{\"claude\":true,\"codex\":true,\"dot\":true},\"idle_minutes\":10}";
    cJSON *p = cJSON_Parse(base);
    cJSON *units = cJSON_AddObjectToObject(p, "units");
    cJSON_AddStringToObject(units, "temperature", "F");
    cJSON_AddStringToObject(units, "source", "auto");
    orb_prefs_t prefs;
    CHECK(link_v11_read_prefs(p, &prefs));
    CHECK(prefs.temperature == ORB_TEMPERATURE_F && !prefs.temperature_owner);
    CHECK(link_v11_event("prefs.update", p) == LINK_V11_APPLIED);
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
    cJSON_ReplaceItemInObjectCaseSensitive(units, "source", cJSON_CreateString("owner"));
    CHECK(link_v11_event("prefs.update", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.prefs.temperature_owner);
    const char *bad[] = {"auto", "fahrenheit", "", "f"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        cJSON_ReplaceItemInObjectCaseSensitive(units, "temperature", cJSON_CreateString(bad[i]));
        CHECK(!link_v11_read_prefs(p, &prefs));
    }
    cJSON_ReplaceItemInObjectCaseSensitive(units, "temperature", cJSON_CreateString("C"));
    cJSON_ReplaceItemInObjectCaseSensitive(units, "source", cJSON_CreateString("hub"));
    CHECK(!link_v11_read_prefs(p, &prefs));
    cJSON_ReplaceItemInObjectCaseSensitive(units, "source", cJSON_CreateString("auto"));
    CHECK(link_v11_event("prefs.update", p) == LINK_V11_APPLIED);
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_C);
    cJSON_AddStringToObject(units, "temperature", "F");
    CHECK(!link_v11_read_prefs(p, &prefs));
    cJSON_Delete(p);
    for (unsigned unit = 0; unit <= ORB_TEMPERATURE_F; ++unit) {
        orb_prefs_patch_t patch = {.has_temperature = true, .temperature = unit};
        p = link_v11_prefs_params(&patch);
        CHECK(p && cJSON_GetArraySize(p) == 1);
        units = cJSON_GetObjectItemCaseSensitive(p, "units");
        const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(units, "temperature"));
        CHECK(value && !strcmp(value, unit == ORB_TEMPERATURE_F ? "F" : unit == ORB_TEMPERATURE_C ? "C" : "auto"));
        CHECK(cJSON_GetArraySize(units) == 1);
        cJSON_Delete(p);
    }
    CHECK(!link_v11_prefs_params(&(orb_prefs_patch_t){.has_temperature = true, .temperature = 99}));
}

static void test_focus_preferences_and_idle(void)
{
    fresh();
    orb_prefs_t prefs;
    CHECK(link_v11_read_prefs(NULL, &prefs));
    CHECK(prefs.sources[0] && prefs.sources[1] && prefs.sources[2] && prefs.idle_minutes == 10);
    cJSON *p = cJSON_Parse("{\"sources\":{\"claude\":false,\"codex\":true,\"dot\":true},\"idle_minutes\":0}");
    CHECK(link_v11_read_prefs(p, &prefs));
    CHECK(!prefs.sources[0] && prefs.idle_minutes == 0);
    CHECK(orb_model_set_prefs(&prefs));
    cJSON_DeleteItemFromObjectCaseSensitive(p, "idle_minutes");
    CHECK(!link_v11_read_prefs(p, &prefs));
    cJSON_Delete(p);
    const char *bad[] = {
        "{\"sources\":{\"claude\":true,\"codex\":true,\"dot\":1},\"idle_minutes\":10}",
        "{\"sources\":{\"claude\":true,\"codex\":true,\"dot\":true},\"idle_minutes\":1.5}",
        "{\"sources\":{\"claude\":true,\"codex\":true,\"dot\":true},\"idle_minutes\":1441}",
        "{\"sources\":{\"claude\":true,\"claude\":false,\"codex\":true,\"dot\":true},\"idle_minutes\":10}"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        p = cJSON_Parse(bad[i]); CHECK(!link_v11_read_prefs(p, &prefs)); cJSON_Delete(p);
    }
    orb_prefs_patch_t patch = {.sources_mask = 1u << ORB_AGENT_CODEX};
    p = link_v11_prefs_params(&patch);
    CHECK(p != NULL && cJSON_GetArraySize(p) == 1);
    cJSON *sources = cJSON_GetObjectItemCaseSensitive(p, "sources");
    CHECK(cJSON_GetArraySize(sources) == 1 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(sources, "codex")));
    CHECK(!cJSON_HasObjectItem(p, "idle_minutes")); cJSON_Delete(p);
    patch = (orb_prefs_patch_t){.has_idle_minutes = true, .idle_minutes = 30};
    p = link_v11_prefs_params(&patch);
    CHECK(p != NULL && cJSON_GetArraySize(p) == 1);
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "idle_minutes")->valueint == 30); cJSON_Delete(p);
    patch = (orb_prefs_patch_t){0}; CHECK(link_v11_prefs_params(&patch) == NULL);
    p = cJSON_Parse("{\"idle\":9,\"hidden_by_source\":{\"claude\":2,\"codex\":0,\"dot\":1}}");
    CHECK(link_v11_event("sessions.summary", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.idle_count == 9 && s_view.hidden_by_source[0] == 2);
    cJSON_GetObjectItemCaseSensitive(p, "idle")->valuedouble = 1.5;
    CHECK(link_v11_event("sessions.summary", p) == LINK_V11_INVALID); cJSON_Delete(p);
    empty_snapshot();
    CHECK(orb_model_idle_open());
    orb_idle_request_t request;
    CHECK(orb_model_take_idle_request(&request));
    p = link_v11_idle_params(&request);
    CHECK(p && !cJSON_HasObjectItem(p, "cursor"));
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "limit")->valueint == 8); cJSON_Delete(p);
    p = cJSON_Parse("{\"sessions\":[],\"cursor\":\"opaque-next\"}");
    CHECK(link_v11_apply_idle(p, &request)); cJSON_Delete(p);
    CHECK(orb_model_idle_more() && orb_model_take_idle_request(&request));
    p = link_v11_idle_params(&request);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "cursor")), "opaque-next"); cJSON_Delete(p);
    p = cJSON_Parse("{\"sessions\":[],\"cursor\":null}");
    CHECK(link_v11_apply_idle(p, &request));
    CHECK(!orb_model_idle_more());
    orb_model_idle_close();
    CHECK(link_v11_apply_idle(p, &request)); /* Closed-view reply is harmless. */
    cJSON_Delete(p);
    p = cJSON_Parse("{\"sessions\":[]}"); CHECK(!link_v11_apply_idle(p, &request)); cJSON_Delete(p);
    p = cJSON_Parse("{\"sessions\":[],\"cursor\":\"\"}"); CHECK(!link_v11_apply_idle(p, &request)); cJSON_Delete(p);
    char cursor[82]; memset(cursor, 'x', 81); cursor[81] = '\0';
    p = cJSON_Parse("{\"sessions\":[],\"cursor\":null}");
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(p, "cursor", cJSON_CreateString(cursor)));
    CHECK(!link_v11_apply_idle(p, &request)); cJSON_Delete(p);
    cJSON *source = fixture("fixtures/v11-session.json");
    p = cJSON_Parse("{\"sessions\":[],\"cursor\":null}");
    cJSON *array = cJSON_GetObjectItemCaseSensitive(p, "sessions");
    CHECK(cJSON_AddItemToArray(array, cJSON_Duplicate(session(source), true)));
    CHECK(cJSON_AddItemToArray(array, cJSON_Duplicate(session(source), true)));
    CHECK(!link_v11_apply_idle(p, &request));
    cJSON_Delete(p); cJSON_Delete(source);
}

static void test_live_agents(void)
{
    fresh();
    cJSON *f = fixture("fixtures/v11-session.json");
    CHECK(event(f) == LINK_V11_APPLIED);
    cJSON *p = cJSON_Parse("{\"session\":\"s\",\"seq\":4,\"at\":1793102400,\"state\":\"busy\",\"stale\":false,"
        "\"activity\":\"Building firmware\",\"steps\":[{\"id\":\"a\",\"kind\":\"tool\",\"label\":\"Build\",\"state\":\"running\"}],"
        "\"assistant\":{\"id\":\"reply\",\"text\":\"Tests are next.\",\"final\":false}}");
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_APPLIED);
    static orb_stream_log_t log;
    orb_agent_stream_t stream;
    CHECK(orb_model_get_stream("s", &stream) && stream.seq == 4 && stream.steps_count == 1);
    CHECK(!stream.steps[0].has_added && !stream.steps[0].has_removed);
    CHECK(orb_model_get_stream_log("s", &log) && log.count == 2);
    CHECK_STR(log.rows[0].text, "Build");
    uint64_t revision = orb_model_revision(), row_revision = log.rows[0].revision;
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_APPLIED);
    CHECK(revision == orb_model_revision());
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "seq"), 6);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_RESYNC);
    CHECK(orb_model_get_stream("s", &stream) && stream.seq == 4);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "seq"), 5);
    cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "steps"), 0);
    set_string(step, "state", "done");
    cJSON_AddNumberToObject(step, "added", 42); cJSON_AddNumberToObject(step, "removed", 0);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_APPLIED);
    CHECK(orb_model_get_stream_log("s", &log) && log.count == 2);
    CHECK(log.rows[0].revision != row_revision && log.rows[0].state == ORB_STEP_DONE);
    CHECK(orb_model_get_stream("s", &stream));
    CHECK(stream.steps[0].has_added && stream.steps[0].added == 42);
    CHECK(stream.steps[0].has_removed && stream.steps[0].removed == 0);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(step, "added"), -1);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(step, "added"), 1.5);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(step, "added"), 2147483648.0);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(step, "added"), 42);
    /* New snapshot repairs a sequence gap without discarding the bounded log. */
    cJSON *chunk = snapshot_chunk(session(f), true, false);
    CHECK(link_v11_event("sessions.snapshot", chunk) == LINK_V11_APPLIED);
    cJSON_Delete(chunk);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "seq"), 50);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_APPLIED);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "seq"), 50.5);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "seq"), 51);
    set_string(step, "kind", "thinking");
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    set_string(step, "kind", "reasoning");
    char long_label[66]; memset(long_label, 'x', 65); long_label[65] = 0;
    set_string(step, "label", long_label);
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_INVALID);
    set_string(step, "label", ""); /* Empty readable labels are permitted. */
    CHECK(link_v11_event("agent.stream", p) == LINK_V11_APPLIED);
    cJSON_Delete(p); cJSON_Delete(f);

    p = cJSON_Parse("{\"windows\":[{\"provider\":\"claude\",\"label\":\"5 hours\",\"used_percent\":62.5,"
        "\"resets_at\":1793102401,\"observed_at\":1793102300,\"stale\":true,\"source\":\"usage\"}],\"unknown\":[\"codex\"]}");
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.usage.count == 1 && s_view.usage.unknown[1]);
    CHECK(s_view.usage.windows[0].used_percent == 62.5 && s_view.usage.windows[0].stale);
    CHECK(!s_view.usage.windows[0].on_credits);
    revision = orb_model_revision();
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED && revision == orb_model_revision());
    cJSON *window = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "windows"), 0);
    cJSON_GetObjectItemCaseSensitive(window, "used_percent")->valuedouble = NAN;
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(window, "used_percent"), 101);
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_INVALID);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(window, "used_percent"), 0);
    cJSON_DeleteItemFromObjectCaseSensitive(window, "source");
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.usage.windows[0].source == ORB_USAGE_UNKNOWN);
    cJSON_Delete(p);
    p = cJSON_Parse("{\"windows\":[{\"provider\":\"codex\",\"label\":\"week\",\"used_percent\":100,"
        "\"resets_at\":1793102401,\"observed_at\":1793102300,\"stale\":false,\"source\":\"rollout\",\"on_credits\":true}],"
        "\"unknown\":[\"claude\"]}");
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.usage.count == 1);
    CHECK(s_view.usage.windows[0].used_percent == 100 && s_view.usage.windows[0].on_credits);
    cJSON *win0 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "windows"), 0);
    cJSON_ReplaceItemInObjectCaseSensitive(win0, "on_credits", cJSON_CreateString("true"));
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_INVALID);
    cJSON_ReplaceItemInObjectCaseSensitive(win0, "on_credits", cJSON_CreateBool(false));
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && !s_view.usage.windows[0].on_credits);
    cJSON_Delete(p);
    p = cJSON_Parse("{\"windows\":[],\"unknown\":[]}");
    CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && s_view.usage.count == 0 && !s_view.usage.unknown[0]);
    cJSON_Delete(p);
    const char *bad[] = {
        "{\"windows\":[],\"unknown\":[\"dot\"]}",
        "{\"windows\":[],\"unknown\":[\"codex\",\"codex\"]}",
        "{\"windows\":[],\"windows\":[],\"unknown\":[]}",
        "{\"windows\":null,\"unknown\":[]}",
        "{\"windows\":[],\"unknown\":false}"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        p = cJSON_Parse(bad[i]); CHECK(link_v11_event("usage.snapshot", p) == LINK_V11_INVALID); cJSON_Delete(p);
    }
}

static void test_message_receipts(void)
{
    orb_message_result_t receipt;
    cJSON *p = cJSON_Parse("{\"accepted\":true,\"status\":\"queued\",\"reason\":\"Queued for Codex\"}");
    CHECK(orb_link_read_message_receipt(p, NULL, &receipt));
    CHECK(receipt.accepted && receipt.status == ORB_SEND_QUEUED);
    CHECK_STR(receipt.reason, "Queued for Codex");
    set_string(p, "status", "delivered");
    CHECK(orb_link_read_message_receipt(p, NULL, &receipt) && receipt.status == ORB_SEND_DELIVERED);
    set_string(p, "status", "failed");
    CHECK(!orb_link_read_message_receipt(p, NULL, &receipt));
    cJSON_Delete(p);
    p = cJSON_Parse("{\"accepted\":true}");
    CHECK(orb_link_read_message_receipt(p, NULL, &receipt));
    CHECK(receipt.accepted && receipt.status == ORB_SEND_UNKNOWN);
    cJSON_Delete(p);
    p = cJSON_Parse("{\"status\":\"failed\",\"code\":\"unsupported\",\"msg\":\"Session is read only\"}");
    CHECK(orb_link_read_message_receipt(NULL, p, &receipt));
    CHECK(!receipt.accepted && receipt.status == ORB_SEND_FAILED);
    CHECK_STR(receipt.error, "unsupported"); CHECK_STR(receipt.reason, "Session is read only");
    set_string(p, "status", "delivered");
    CHECK(!orb_link_read_message_receipt(NULL, p, &receipt));
    cJSON_Delete(p);
    CHECK(!orb_link_read_message_receipt(NULL, NULL, &receipt));
}

static void test_weather(void)
{
    fresh();
    orb_model_weather_capability(true);
    cJSON *root = fixture("fixtures/weather-state.json"), *p = body(root);
    CHECK(event(root) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.weather_valid && s_view.weather.available && s_view.weather.hourly_count == 6);
    CHECK(s_view.weather.unit == ORB_TEMPERATURE_F && s_view.weather.is_day && !s_view.weather.hourly[0].is_day);
    CHECK(fabsf(s_view.weather.temperature - 72.1f) < .001f && s_view.weather.revision == 1);
    uint64_t revision = orb_model_revision();
    CHECK(event(root) == LINK_V11_APPLIED && orb_model_revision() == revision);
    const char *keys[] = {"configured", "available", "place", "unit", "temperature", "condition", "is_day",
        "label", "high", "low", "hourly", "updated_at", "stale"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
        cJSON *bad = cJSON_Duplicate(p, true);
        cJSON_DeleteItemFromObjectCaseSensitive(bad, keys[i]);
        CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID);
        CHECK(orb_model_revision() == revision);
        cJSON_Delete(bad);
    }
    static const struct { const char *key, *value; } invalid[] = {
        {"configured", "false"}, {"configured", "1"}, {"available", "null"},
        {"place", "\"\""}, {"unit", "\"auto\""}, {"unit", "true"},
        {"temperature", "null"}, {"temperature", "true"}, {"temperature", "351"},
        {"temperature", "-251"}, {"temperature", "1e999"}, {"high", "57"},
        {"low", "null"}, {"condition", "\"unknown\""}, {"condition", "null"},
        {"is_day", "0"}, {"label", "\"Clear\""}, {"stale", "1"},
        {"updated_at", "0"}, {"updated_at", "-1"}, {"updated_at", "1.5"},
        {"updated_at", "4294967296"}, {"hourly", "null"}, {"hourly", "{}"}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        cJSON *bad = cJSON_Duplicate(p, true), *value = cJSON_Parse(invalid[i].value);
        CHECK(value && cJSON_ReplaceItemInObjectCaseSensitive(bad, invalid[i].key, value));
        CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID);
        CHECK(orb_model_revision() == revision);
        cJSON_Delete(bad);
    }
    for (int field = 0; field < 2; ++field) {
        cJSON *bad = cJSON_Duplicate(p, true);
        char oversized[66]; memset(oversized, 'a', sizeof(oversized));
        oversized[field ? 33 : 65] = '\0';
        set_string(bad, field ? "label" : "place", oversized);
        CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID);
        cJSON_Delete(bad);
    }
    cJSON *bad = cJSON_Duplicate(p, true), *hours = cJSON_GetObjectItemCaseSensitive(bad, "hourly");
    CHECK(cJSON_AddItemToArray(hours, cJSON_Duplicate(cJSON_GetArrayItem(hours, 5), true)));
    CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID); cJSON_Delete(bad);
    static const struct { const char *key, *value; } invalid_hour[] = {
        {"hour", "1793221200"}, {"hour", "1.1"}, {"hour", "4294967296"},
        {"temperature", "null"}, {"temperature", "351"}, {"condition", "\"hail\""}, {"is_day", "1"}
    };
    for (size_t i = 0; i < sizeof(invalid_hour) / sizeof(*invalid_hour); ++i) {
        bad = cJSON_Duplicate(p, true);
        hours = cJSON_GetObjectItemCaseSensitive(bad, "hourly");
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(hours, 0),
            invalid_hour[i].key, cJSON_Parse(invalid_hour[i].value)));
        CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID); cJSON_Delete(bad);
    }
    bad = cJSON_Duplicate(p, true); CHECK(cJSON_AddBoolToObject(bad, "stale", false));
    CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID); cJSON_Delete(bad);
    bad = cJSON_Duplicate(p, true); hours = cJSON_GetObjectItemCaseSensitive(bad, "hourly");
    CHECK(cJSON_AddBoolToObject(cJSON_GetArrayItem(hours, 0), "is_day", true));
    CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID); cJSON_Delete(bad);
    bad = cJSON_Duplicate(p, true); hours = cJSON_GetObjectItemCaseSensitive(bad, "hourly");
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(hours, 1), "hour", cJSON_CreateNumber(1793224800)));
    CHECK(link_v11_event("weather.state", bad) == LINK_V11_INVALID); cJSON_Delete(bad);
    CHECK(orb_model_revision() == revision);
    cJSON_Delete(root);
    p = cJSON_Parse("{\"configured\":false,\"available\":false,\"place\":\"\",\"unit\":\"C\",\"temperature\":null,\"condition\":null,\"is_day\":false,\"label\":\"Set a location in orb-hub\",\"high\":null,\"low\":null,\"hourly\":[],\"updated_at\":0,\"stale\":false}");
    CHECK(link_v11_event("weather.state", p) == LINK_V11_APPLIED);
    CHECK(orb_model_snapshot(&s_view) && !s_view.weather.available && !s_view.weather.configured);
    CHECK_STR(s_view.weather.label, "Set a location in orb-hub");
    set_string(p, "place", "Berlin"); set_string(p, "label", "Loading weather");
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(p, "configured", cJSON_CreateTrue()));
    CHECK(link_v11_event("weather.state", p) == LINK_V11_APPLIED);
    set_string(p, "label", "Weather unavailable");
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(p, "stale", cJSON_CreateTrue()));
    CHECK(link_v11_event("weather.state", p) == LINK_V11_APPLIED);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(p, "temperature", cJSON_CreateNumber(0)));
    CHECK(link_v11_event("weather.state", p) == LINK_V11_INVALID);
    cJSON_Delete(p);
}

int main(void)
{
    CHECK(orb_model_init());
    test_fixtures();
    test_snapshot_transaction();
    test_limits();
    test_malformed();
    test_answers();
    test_zero_expiry();
    test_focus_preferences_and_idle();
    test_temperature_preferences();
    test_live_agents();
    test_message_receipts();
    test_weather();
    printf("v1.1 mapping: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
