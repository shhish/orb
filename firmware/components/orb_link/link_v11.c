#include "link_v11.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static bool s_snapshot_open;
static bool s_have_snapshot;
static size_t s_snapshot_count;

static const cJSON *item(const cJSON *object, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

/* Events may gain additive fields; repeated names are always ambiguous. */
static bool object_ok(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next) {
        if (!a->string) return false;
        for (const cJSON *b = a->next; b; b = b->next)
            if (!b->string || !strcmp(a->string, b->string)) return false;
    }
    return true;
}

static bool text_ok(const char *text, size_t capacity, bool required)
{
    if (!text) return false;
    size_t n = 0;
    while (n < capacity && text[n]) ++n;
    if (n == capacity || (required && !n)) return false;
    const unsigned char *bytes = (const unsigned char *)text;
    for (size_t i = 0; i < n;) {
        unsigned char c = bytes[i++];
        if (c < 0x80) continue;
        uint32_t value, minimum;
        size_t need;
        if (c >= 0xc2 && c <= 0xdf) {
            value = c & 0x1f; minimum = 0x80; need = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            value = c & 0x0f; minimum = 0x800; need = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            value = c & 7; minimum = 0x10000; need = 3;
        } else return false;
        if (need > n - i) return false;
        while (need--) {
            c = bytes[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool read_text(const cJSON *object, const char *key, char *out,
                      size_t capacity, bool required, bool optional)
{
    const cJSON *value = item(object, key);
    if (!value && optional) {
        out[0] = '\0';
        return true;
    }
    const char *text = cJSON_GetStringValue(value);
    if (!text_ok(text, capacity, required)) return false;
    memcpy(out, text, strlen(text) + 1);
    return true;
}

#define READ(object, out, field, required, optional) \
    read_text((object), #field, (out)->field, sizeof((out)->field), (required), (optional))

static bool read_bool(const cJSON *object, const char *key, bool *out)
{
    const cJSON *value = item(object, key);
    if (!cJSON_IsBool(value)) return false;
    *out = cJSON_IsTrue(value);
    return true;
}

static bool read_time(const cJSON *object, const char *key, uint64_t *out, bool optional)
{
    const cJSON *value = item(object, key);
    if (!value && optional) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0) return false;
    /* The wire allows every finite future timestamp. Saturate dates beyond
     * the native range before casting; a double rounds UINT64_MAX to 2^64.
     * Such an expiry remains far in the future, and updates follow wire order. */
    *out = value->valuedouble >= 18446744073709551616.0 ? UINT64_MAX : (uint64_t)value->valuedouble;
    return true;
}

static bool read_count(const cJSON *object, const char *key, uint32_t *out, uint32_t max)
{
    const cJSON *value = item(object, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > max ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint32_t)value->valuedouble;
    return true;
}

static int read_enum(const cJSON *object, const char *key, const char *const *names, size_t count)
{
    const char *text = cJSON_GetStringValue(item(object, key));
    if (text) for (size_t i = 0; i < count; ++i)
        if (!strcmp(text, names[i])) return (int)i;
    return -1;
}

static bool read_session(const cJSON *object, orb_session_t *out)
{
    static const char *const agents[] = {"claude", "codex", "dot"};
    static const char *const states[] = {"busy", "waiting", "idle", "done", "error"};
    memset(out, 0, sizeof(*out));
    int agent = read_enum(object, "agent", agents, 3);
    int state = read_enum(object, "state", states, 5);
    if (!object_ok(object) || agent < 0 || state < 0 ||
        !READ(object, out, id, true, false) || !READ(object, out, title, false, false) ||
        !READ(object, out, project, false, false) || !READ(object, out, reason, false, false) ||
        !READ(object, out, summary, false, false) || !read_time(object, "updated", &out->updated, false) ||
        !read_bool(object, "can_message", &out->can_message)) return false;
    out->agent = (orb_agent_t)agent;
    out->state = (orb_session_state_t)state;
    return true;
}

static bool read_card(const cJSON *object, orb_card_t *out)
{
    static const char *const kinds[] = {"approval", "choice", "message", "attention"};
    static const char *const priorities[] = {"low", "normal", "high"};
    static const char *const roles[] = {"default", "primary", "cancel", "destructive"};
    memset(out, 0, sizeof(*out));
    int kind = read_enum(object, "kind", kinds, 4);
    int priority = read_enum(object, "priority", priorities, 3);
    const cJSON *options = item(object, "options");
    int count = cJSON_GetArraySize(options);
    if (!object_ok(object) || kind < 0 || priority < 0 || !cJSON_IsArray(options) ||
        count > ORB_MODEL_MAX_OPTIONS || !READ(object, out, id, true, false) ||
        !READ(object, out, session, true, true) || !READ(object, out, title, false, false) ||
        !READ(object, out, body, false, false) || !READ(object, out, detail, false, true) ||
        !read_time(object, "expires", &out->expires, true) || !read_bool(object, "reply", &out->reply))
        return false;
    out->kind = (orb_card_kind_t)kind;
    out->priority = (orb_priority_t)priority;
    out->options_count = (uint8_t)count;
    out->has_expiry = item(object, "expires") != NULL;
    const cJSON *scope = item(object, "scope");
    if (scope) {
        const char *name = cJSON_GetStringValue(scope);
        if (kind != ORB_CARD_APPROVAL || !name) return false;
        if (!strcmp(name, "operation")) out->scope = ORB_APPROVAL_OPERATION;
        else if (!strcmp(name, "turn")) out->scope = ORB_APPROVAL_TURN;
        else return false; /* Never turn an unknown grant into an approval. */
    }
    for (int i = 0; i < count; ++i) {
        const cJSON *option = cJSON_GetArrayItem(options, i);
        orb_option_t *to = &out->options[i];
        int role = read_enum(option, "role", roles, 4);
        if (!object_ok(option) || role < 0 || !READ(option, to, id, true, false) ||
            !READ(option, to, label, true, false)) return false;
        to->role = (orb_option_role_t)role;
        for (int j = 0; j < i; ++j)
            if (!strcmp(to->id, out->options[j].id)) return false;
    }
    return true;
}

/* Only the link owner maps these events. Fixed typed scratch avoids a second
 * allocation per pushed replacement; retained rows live in the model PSRAM ring. */
static orb_agent_stream_t s_stream_scratch;
static orb_usage_snapshot_t s_usage_scratch;

static bool read_sequence(const cJSON *object, uint64_t *out)
{
    const cJSON *value = item(object, "seq");
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > 9007199254740991.0 ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (uint64_t)value->valuedouble;
    return true;
}

static bool read_stream(const cJSON *params, orb_agent_stream_t *out)
{
    static const char *const states[] = {"busy", "waiting", "idle", "done", "error"};
    static const char *const kinds[] = {"tool", "reasoning"};
    static const char *const step_states[] = {"running", "done", "failed"};
    memset(out, 0, sizeof(*out));
    const cJSON *steps = item(params, "steps"), *assistant = item(params, "assistant");
    int count = cJSON_GetArraySize(steps), state = read_enum(params, "state", states, 5);
    if (!object_ok(params) || state < 0 || !cJSON_IsArray(steps) || count > ORB_MODEL_MAX_STEPS ||
        !READ(params, out, session, true, false) || !read_sequence(params, &out->seq) ||
        !read_time(params, "at", &out->at, false) || !read_bool(params, "stale", &out->stale) ||
        !READ(params, out, activity, false, false) || !object_ok(assistant) ||
        !READ(assistant, &out->assistant, id, false, false) ||
        !READ(assistant, &out->assistant, text, false, false) ||
        !read_bool(assistant, "final", &out->assistant.final)) return false;
    out->state = (orb_session_state_t)state;
    out->steps_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        const cJSON *step = cJSON_GetArrayItem(steps, i);
        orb_agent_step_t *to = &out->steps[i];
        int kind = read_enum(step, "kind", kinds, 2), status = read_enum(step, "state", step_states, 3);
        if (!object_ok(step) || kind < 0 || status < 0 || !READ(step, to, id, true, false) ||
            !READ(step, to, label, false, false)) return false;
        to->kind = (orb_step_kind_t)kind;
        to->state = (orb_step_state_t)status;
        const cJSON *added = item(step, "added"), *removed = item(step, "removed");
        if ((added && (!cJSON_IsNumber(added) || !isfinite(added->valuedouble) ||
                       added->valuedouble < 0 || added->valuedouble > INT32_MAX ||
                       floor(added->valuedouble) != added->valuedouble)) ||
            (removed && (!cJSON_IsNumber(removed) || !isfinite(removed->valuedouble) ||
                         removed->valuedouble < 0 || removed->valuedouble > INT32_MAX ||
                         floor(removed->valuedouble) != removed->valuedouble))) return false;
        to->has_added = added != NULL; to->has_removed = removed != NULL;
        to->added = added ? (uint32_t)added->valuedouble : 0;
        to->removed = removed ? (uint32_t)removed->valuedouble : 0;
        for (int j = 0; j < i; ++j) if (!strcmp(to->id, out->steps[j].id)) return false;
    }
    return true;
}

static bool read_usage(const cJSON *params, orb_usage_snapshot_t *out)
{
    static const char *const providers[] = {"claude", "codex"};
    static const char *const sources[] = {"statusline", "usage", "rollout", "app-server", "unknown"};
    memset(out, 0, sizeof(*out));
    const cJSON *windows = item(params, "windows"), *unknown = item(params, "unknown");
    int count = cJSON_GetArraySize(windows), missing = cJSON_GetArraySize(unknown);
    if (!object_ok(params) || !cJSON_IsArray(windows) || count > ORB_MODEL_MAX_USAGE ||
        !cJSON_IsArray(unknown) || missing > 2) return false;
    for (int i = 0; i < missing; ++i) {
        const char *provider = cJSON_GetStringValue(cJSON_GetArrayItem(unknown, i));
        int index = provider && !strcmp(provider, "claude") ? 0 : provider && !strcmp(provider, "codex") ? 1 : -1;
        if (index < 0 || out->unknown[index]) return false;
        out->unknown[index] = true;
    }
    out->count = (size_t)count;
    size_t provider_count[2] = {0};
    for (int i = 0; i < count; ++i) {
        const cJSON *window = cJSON_GetArrayItem(windows, i), *percent = item(window, "used_percent");
        orb_usage_window_t *to = &out->windows[i];
        int provider = read_enum(window, "provider", providers, 2);
        int source = item(window, "source") ? read_enum(window, "source", sources, 5) : ORB_USAGE_UNKNOWN;
        const cJSON *on_credits = item(window, "on_credits");
        if (!object_ok(window) || provider < 0 || source < 0 || out->unknown[provider] ||
            ++provider_count[provider] > (provider == 0 ? 8u : 2u) ||
            !READ(window, to, label, true, false) || !cJSON_IsNumber(percent) ||
            !isfinite(percent->valuedouble) || percent->valuedouble < 0 || percent->valuedouble > 100 ||
            !read_time(window, "resets_at", &to->resets_at, false) ||
            !read_time(window, "observed_at", &to->observed_at, false) ||
            !read_bool(window, "stale", &to->stale) ||
            (on_credits && !cJSON_IsBool(on_credits))) return false;
        to->provider = (orb_agent_t)provider;
        to->source = (orb_usage_source_t)source;
        to->used_percent = percent->valuedouble;
        to->on_credits = cJSON_IsTrue(on_credits);
    }
    return true;
}

bool orb_link_read_message_receipt(const cJSON *result, const cJSON *error,
                                    orb_message_result_t *out)
{
    if (!out) return false;
    orb_message_result_t receipt = {0};
    if (error) {
        if (!object_ok(error)) return false;
        const cJSON *status = item(error, "status");
        if (status && (!cJSON_IsString(status) || strcmp(status->valuestring, "failed"))) return false;
        if (!read_text(error, "code", receipt.error, sizeof(receipt.error), true, false) ||
            !read_text(error, "msg", receipt.reason, sizeof(receipt.reason), false, false)) return false;
        receipt.status = ORB_SEND_FAILED;
    } else {
        bool accepted;
        if (!object_ok(result) || !read_bool(result, "accepted", &accepted) || !accepted) return false;
        receipt.accepted = true;
        const cJSON *status = item(result, "status");
        if (status) {
            const char *name = cJSON_GetStringValue(status);
            if (!name) return false;
            if (!strcmp(name, "queued")) receipt.status = ORB_SEND_QUEUED;
            else if (!strcmp(name, "delivered")) receipt.status = ORB_SEND_DELIVERED;
            else return false;
            if (!READ(result, &receipt, reason, false, false)) return false;
        } /* A legacy accepted-only reply is deliberately UNKNOWN. */
    }
    *out = receipt;
    return true;
}

static bool snapshot(const cJSON *params)
{
    const cJSON *sessions = item(params, "sessions");
    bool reset, more;
    int count = cJSON_GetArraySize(sessions);
    if (!object_ok(params) || !cJSON_IsArray(sessions) || count > ORB_MODEL_MAX_FOCUS ||
        !read_bool(params, "reset", &reset) || !read_bool(params, "more", &more)) return false;
    size_t total = reset ? (size_t)count : s_snapshot_count + (size_t)count;
    if (total > ORB_MODEL_MAX_FOCUS) return false;
    /* Keep even a maximally dense short-session chunk off the link stack. */
#ifdef ESP_PLATFORM
    orb_session_t *chunk = heap_caps_calloc(count ? (size_t)count : 1, sizeof(*chunk),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    orb_session_t *chunk = calloc(count ? (size_t)count : 1, sizeof(*chunk));
#endif
    if (!chunk) return false;
    bool ok = true;
    for (int i = 0; i < count && ok; ++i)
        ok = read_session(cJSON_GetArrayItem(sessions, i), &chunk[i]);
    if (ok) ok = orb_model_sessions_chunk(chunk, (size_t)count, reset, more);
    if (ok) {
        s_snapshot_count = total;
        s_snapshot_open = more;
        if (!more) s_have_snapshot = true;
    }
    free(chunk);
    return ok;
}

void link_v11_reset(void)
{
    orb_model_sessions_discard();
    orb_model_set_connection(false, false);
    s_snapshot_open = false;
    s_have_snapshot = false;
    s_snapshot_count = 0;
}

link_v11_result_t link_v11_event(const char *name, const cJSON *params)
{
    if (!name) return LINK_V11_IGNORED;
    bool ok;
    if (!strcmp(name, "agent.stream")) {
        if (!s_have_snapshot || s_snapshot_open || !read_stream(params, &s_stream_scratch))
            return LINK_V11_INVALID;
        return orb_model_apply_stream(&s_stream_scratch) ? LINK_V11_APPLIED : LINK_V11_RESYNC;
    } else if (!strcmp(name, "weather.state")) {
        orb_weather_snapshot_t weather;
        ok = link_v11_read_weather(params, &weather) && orb_model_set_weather(&weather);
    } else if (!strcmp(name, "nowplaying.snapshot")) {
        orb_nowplaying_t media;
        ok = link_v11_read_nowplaying(params, &media) && orb_model_set_nowplaying(&media);
    } else if (!strcmp(name, "usage.snapshot")) {
        ok = read_usage(params, &s_usage_scratch) && orb_model_set_usage(&s_usage_scratch);
    } else if (!strcmp(name, "prefs.update")) {
        orb_prefs_t prefs;
        ok = params && link_v11_read_prefs(params, &prefs) && orb_model_set_prefs(&prefs);
    } else if (!strcmp(name, "sessions.snapshot")) {
        ok = snapshot(params);
    } else if (!strcmp(name, "sessions.summary")) {
        uint32_t idle, hidden[3];
        const cJSON *sources = item(params, "hidden_by_source");
        ok = s_have_snapshot && !s_snapshot_open && object_ok(params) && object_ok(sources) &&
             read_count(params, "idle", &idle, ORB_MODEL_MAX_SESSIONS) &&
             read_count(sources, "claude", &hidden[ORB_AGENT_CLAUDE], ORB_MODEL_MAX_SESSIONS) &&
             read_count(sources, "codex", &hidden[ORB_AGENT_CODEX], ORB_MODEL_MAX_SESSIONS) &&
             read_count(sources, "dot", &hidden[ORB_AGENT_DOT], ORB_MODEL_MAX_SESSIONS) &&
             orb_model_set_summary(idle, hidden);
    } else if (!strcmp(name, "session.update")) {
        orb_session_t session;
        ok = s_have_snapshot && !s_snapshot_open && object_ok(params) &&
             read_session(item(params, "session"), &session) &&
             orb_model_apply_session(&session);
    } else if (!strcmp(name, "session.remove")) {
        char id[ORB_MODEL_ID_MAX + 1];
        ok = s_have_snapshot && !s_snapshot_open && object_ok(params) &&
             read_text(params, "id", id, sizeof(id), true, false);
        if (ok) orb_model_remove_session(id); /* Repeated removal is idempotent. */
    } else if (!strcmp(name, "card.show")) {
        orb_card_t card;
        ok = s_have_snapshot && !s_snapshot_open && read_card(params, &card) && orb_model_upsert_card(&card);
    } else if (!strcmp(name, "card.cancel")) {
        char id[ORB_MODEL_CARD_ID_MAX + 1];
        ok = s_have_snapshot && !s_snapshot_open && object_ok(params) &&
             read_text(params, "card", id, sizeof(id), true, false);
        if (ok) orb_model_cancel_card(id);
    } else return LINK_V11_IGNORED;
    if (!ok) orb_model_sessions_discard();
    return ok ? LINK_V11_APPLIED : LINK_V11_INVALID;
}

static cJSON *checked(cJSON *object, bool ok)
{
    if (!ok) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

cJSON *link_v11_answer_params(const orb_answer_t *answer)
{
    if (!answer || !text_ok(answer->card, sizeof(answer->card), true) ||
        !text_ok(answer->option, sizeof(answer->option), false) ||
        !text_ok(answer->text, sizeof(answer->text), false) ||
        (!!answer->option[0] == !!answer->text[0])) return NULL;
    cJSON *object = cJSON_CreateObject();
    bool ok = object && cJSON_AddStringToObject(object, "card", answer->card) &&
              cJSON_AddStringToObject(object, answer->option[0] ? "option" : "text",
                                      answer->option[0] ? answer->option : answer->text);
    return checked(object, ok);
}

cJSON *link_v11_message_params(const char *target, const char *text, uint16_t stream)
{
    if (!text_ok(target, ORB_MODEL_ID_MAX + 1, true) ||
        !text_ok(text, ORB_MODEL_TEXT_MAX + 1, true)) return NULL;
    cJSON *object = cJSON_CreateObject();
    bool ok = object && cJSON_AddStringToObject(object, "target", target) &&
              cJSON_AddStringToObject(object, "text", text) &&
              (!stream || cJSON_AddNumberToObject(object, "stream", stream));
    return checked(object, ok);
}

bool link_v11_read_answer(const cJSON *result, const char *expected_card,
                          orb_answer_t *recorded, bool *delivered)
{
    if (!recorded || !delivered || !text_ok(expected_card, ORB_MODEL_CARD_ID_MAX + 1, true)) return false;
    orb_answer_t answer = {0};
    const cJSON *decision = item(result, "answer");
    bool delivery;
    if (!object_ok(result) || !READ(result, &answer, card, true, false) ||
        strcmp(answer.card, expected_card) || !object_ok(decision) ||
        cJSON_GetArraySize(decision) != 1 || !read_bool(result, "delivered", &delivery)) return false;
    bool has_option = item(decision, "option") != NULL;
    bool has_text = item(decision, "text") != NULL;
    if (has_option == has_text ||
        !READ(decision, &answer, option, true, !has_option) ||
        !READ(decision, &answer, text, true, !has_text)) return false;
    *recorded = answer;
    *delivered = delivery;
    return true;
}

bool link_v11_read_prefs(const cJSON *value, orb_prefs_t *out)
{
    if (!out) return false;
    if (!value) {
        orb_model_prefs_defaults(out);
        return true;
    }
    const cJSON *sources = item(value, "sources");
    orb_prefs_t prefs = {0};
    uint32_t minutes;
    if (!object_ok(value) || !object_ok(sources) ||
        !read_bool(sources, "claude", &prefs.sources[ORB_AGENT_CLAUDE]) ||
        !read_bool(sources, "codex", &prefs.sources[ORB_AGENT_CODEX]) ||
        !read_bool(sources, "dot", &prefs.sources[ORB_AGENT_DOT]) ||
        !read_count(value, "idle_minutes", &minutes, 1440)) return false;
    const cJSON *units = item(value, "units");
    if (units) {
        static const char *const temperatures[] = {"C", "F"};
        static const char *const sources[] = {"auto", "owner"};
        int temperature = read_enum(units, "temperature", temperatures, 2);
        int source = read_enum(units, "source", sources, 2);
        if (!object_ok(units) || temperature < 0 || source < 0) return false;
        prefs.temperature = temperature == 0 ? ORB_TEMPERATURE_C : ORB_TEMPERATURE_F;
        prefs.temperature_owner = source == 1;
    }
    prefs.idle_minutes = (uint16_t)minutes;
    *out = prefs;
    return true;
}

cJSON *link_v11_prefs_params(const orb_prefs_patch_t *patch)
{
    if (!patch || patch->sources_mask > 7 ||
        (!patch->sources_mask && !patch->has_idle_minutes && !patch->has_temperature) ||
        (patch->has_idle_minutes && patch->idle_minutes > 1440) ||
        (patch->has_temperature && (unsigned)patch->temperature > ORB_TEMPERATURE_F)) return NULL;
    static const char *const names[] = {"claude", "codex", "dot"};
    cJSON *object = cJSON_CreateObject();
    bool ok = object != NULL;
    if (ok && patch->sources_mask) {
        cJSON *sources = cJSON_AddObjectToObject(object, "sources");
        ok = sources != NULL;
        for (size_t i = 0; ok && i < 3; ++i)
            if (patch->sources_mask & (1u << i))
                ok = cJSON_AddBoolToObject(sources, names[i], patch->sources[i]) != NULL;
    }
    if (ok && patch->has_idle_minutes)
        ok = cJSON_AddNumberToObject(object, "idle_minutes", patch->idle_minutes) != NULL;
    if (ok && patch->has_temperature) {
        cJSON *units = cJSON_AddObjectToObject(object, "units");
        const char *temperature = patch->temperature == ORB_TEMPERATURE_F ? "F" :
                                  patch->temperature == ORB_TEMPERATURE_C ? "C" : "auto";
        ok = units && cJSON_AddStringToObject(units, "temperature", temperature);
    }
    return checked(object, ok);
}

cJSON *link_v11_idle_params(const orb_idle_request_t *request)
{
    if (!request || !text_ok(request->cursor, sizeof(request->cursor), false)) return NULL;
    cJSON *object = cJSON_CreateObject();
    bool ok = object && cJSON_AddStringToObject(object, "include", "idle") &&
              cJSON_AddNumberToObject(object, "limit", ORB_MODEL_MAX_IDLE) &&
              (!request->cursor[0] || cJSON_AddStringToObject(object, "cursor", request->cursor));
    return checked(object, ok);
}

bool link_v11_apply_idle(const cJSON *result, const orb_idle_request_t *request)
{
    const cJSON *sessions = item(result, "sessions"), *next = item(result, "cursor");
    int count = cJSON_GetArraySize(sessions);
    const char *cursor = cJSON_IsNull(next) ? NULL : cJSON_GetStringValue(next);
    if (!request || !object_ok(result) || !cJSON_IsArray(sessions) ||
        count > ORB_MODEL_MAX_IDLE || (!cJSON_IsNull(next) &&
        !text_ok(cursor, ORB_MODEL_CURSOR_MAX + 1, true))) return false;
#ifdef ESP_PLATFORM
    orb_session_t *page = heap_caps_calloc(count ? (size_t)count : 1, sizeof(*page),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    orb_session_t *page = calloc(count ? (size_t)count : 1, sizeof(*page));
#endif
    if (!page) return false;
    bool ok = true;
    for (int i = 0; ok && i < count; ++i) {
        ok = read_session(cJSON_GetArrayItem(sessions, i), &page[i]);
        for (int j = 0; ok && j < i; ++j)
            if (!strcmp(page[i].id, page[j].id)) ok = false;
    }
    /* A closed view is a normal race, not a protocol failure. Completion will
     * ignore its generation; validated network content may still be dropped. */
    if (ok) orb_model_complete_idle(request, page, (size_t)count, cursor, NULL);
    free(page);
    return ok;
}

static const char *const s_media_actions[] = {"play", "pause", "playpause", "next", "previous", "volume"};

static bool read_weather_temperature(const cJSON *object, const char *key, float *out)
{
    const cJSON *value = item(object, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < -250 || value->valuedouble > 350) return false;
    *out = (float)value->valuedouble;
    return true;
}

static bool read_weather_condition(const cJSON *object, orb_weather_condition_t *out)
{
    static const char *const names[] = {"clear", "partly_cloudy", "cloudy", "fog", "drizzle", "rain", "snow", "thunder"};
    int value = read_enum(object, "condition", names, sizeof(names) / sizeof(*names));
    if (value < 0) return false;
    *out = (orb_weather_condition_t)(value + ORB_WEATHER_CLEAR);
    return true;
}

bool link_v11_read_weather(const cJSON *value, orb_weather_snapshot_t *out)
{
    if (!out || !object_ok(value)) return false;
    orb_weather_snapshot_t weather = {0};
    const char *unit = cJSON_GetStringValue(item(value, "unit"));
    const cJSON *hourly = item(value, "hourly");
    int count = cJSON_GetArraySize(hourly);
    if (!unit || (strcmp(unit, "C") && strcmp(unit, "F")) ||
        !read_bool(value, "configured", &weather.configured) ||
        !read_bool(value, "available", &weather.available) ||
        !read_bool(value, "stale", &weather.stale) || !read_bool(value, "is_day", &weather.is_day) ||
        !READ(value, &weather, place, weather.configured, false) || !READ(value, &weather, label, true, false) ||
        !read_count(value, "updated_at", &weather.updated_at, UINT32_MAX) ||
        !cJSON_IsArray(hourly) || count > ORB_MODEL_MAX_WEATHER_HOURS) return false;
    weather.unit = !strcmp(unit, "F") ? ORB_TEMPERATURE_F : ORB_TEMPERATURE_C;
    if (!weather.available) {
        if (!cJSON_IsNull(item(value, "temperature")) || !cJSON_IsNull(item(value, "high")) ||
            !cJSON_IsNull(item(value, "low")) || !cJSON_IsNull(item(value, "condition")) ||
            weather.is_day || weather.updated_at || count) return false;
        if (!weather.configured) {
            if (weather.place[0] || weather.stale || strcmp(weather.label, "Set a location in orb-hub")) return false;
        } else if (strcmp(weather.label, weather.stale ? "Weather unavailable" : "Loading weather")) return false;
    } else {
        if (!weather.configured || !weather.updated_at || !read_weather_condition(value, &weather.condition) ||
            !read_weather_temperature(value, "temperature", &weather.temperature) ||
            !read_weather_temperature(value, "high", &weather.high) ||
            !read_weather_temperature(value, "low", &weather.low) || weather.high < weather.low ||
            strcmp(weather.label, orb_weather_condition_label(weather.condition, weather.is_day))) return false;
        weather.hourly_count = (size_t)count;
        uint32_t previous = weather.updated_at;
        for (int i = 0; i < count; ++i) {
            const cJSON *entry = cJSON_GetArrayItem(hourly, i);
            orb_weather_hour_t *hour = &weather.hourly[i];
            if (!object_ok(entry) || !read_count(entry, "hour", &hour->hour, UINT32_MAX) ||
                hour->hour <= previous || !read_weather_temperature(entry, "temperature", &hour->temperature) ||
                !read_weather_condition(entry, &hour->condition) || !read_bool(entry, "is_day", &hour->is_day)) return false;
            previous = hour->hour;
        }
    }
    *out = weather;
    return true;
}

/* Metadata can exceed a small display's useful content. Validate the complete
 * UTF-8 field, then retain a codepoint-safe prefix rather than severing a glyph
 * or forcing a reconnect for a long song title. */
static bool read_media_text(const cJSON *object, const char *key, char *out, size_t capacity)
{
    const char *text = cJSON_GetStringValue(item(object, key));
    if (!text_ok(text, LINK_V11_MAX_MSG, false)) return false;
    size_t n = strlen(text);
    if (n >= capacity) {
        n = capacity - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    }
    memcpy(out, text, n); out[n] = '\0';
    return true;
}

bool link_v11_read_nowplaying(const cJSON *value, orb_nowplaying_t *out)
{
    if (!out || !object_ok(value)) return false;
    memset(out, 0, sizeof(*out));
    static const char *const states[] = {"playing", "paused", "stopped"};
    int state = read_enum(value, "state", states, 3);
    const cJSON *duration = item(value, "duration"), *position = item(value, "position");
    const char *album = cJSON_GetStringValue(item(value, "album"));
    uint32_t volume;
    if (state < 0 || !read_media_text(value, "title", out->title, sizeof(out->title)) ||
        !read_media_text(value, "artist", out->artist, sizeof(out->artist)) ||
        !read_media_text(value, "app", out->app, sizeof(out->app)) ||
        !text_ok(album, LINK_V11_MAX_MSG, false) ||
        !cJSON_IsNumber(duration) || !isfinite(duration->valuedouble) || duration->valuedouble < 0 ||
        !cJSON_IsNumber(position) || !isfinite(position->valuedouble) || position->valuedouble < 0 ||
        !read_count(value, "volume", &volume, 100) ||
        !read_bool(value, "playing", &out->playing) || out->playing != (state == ORB_MEDIA_PLAYING)) return false;
    out->state = (orb_media_state_t)state;
    out->duration = duration->valuedouble;
    out->position = position->valuedouble;
    out->volume = (int)volume;
    return true;
}

cJSON *link_v11_nowplaying_params(const orb_media_request_t *request)
{
    if (!request || request->action < ORB_MEDIA_PLAY || request->action > ORB_MEDIA_VOLUME ||
        (request->action == ORB_MEDIA_VOLUME && (request->value < 0 || request->value > 100))) return NULL;
    cJSON *object = cJSON_CreateObject();
    bool ok = object && cJSON_AddStringToObject(object, "action", s_media_actions[request->action]) &&
        (request->action != ORB_MEDIA_VOLUME || cJSON_AddNumberToObject(object, "value", request->value));
    return checked(object, ok);
}

bool link_v11_apply_nowplaying_reply(const cJSON *result, const orb_media_request_t *request)
{
    if (!request || request->action < ORB_MEDIA_PLAY || request->action > ORB_MEDIA_VOLUME ||
        !object_ok(result) || !cJSON_IsTrue(item(result, "accepted"))) return false;
    const char *action = cJSON_GetStringValue(item(result, "action"));
    orb_nowplaying_t media;
    return action && !strcmp(action, s_media_actions[request->action]) &&
        link_v11_read_nowplaying(item(result, "snapshot"), &media) && orb_model_set_nowplaying(&media);
}
