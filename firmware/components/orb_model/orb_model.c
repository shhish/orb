#include "orb_model.h"
#include "orb_model_voice_notify.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <pthread.h>
#endif

typedef struct {
    orb_stream_row_t rows[ORB_MODEL_STREAM_ROWS];
    size_t head, count;
    uint64_t revision;
    bool baseline;
} stream_ring_t;

typedef struct {
    orb_model_snapshot_t view;
    stream_ring_t stream_logs[ORB_MODEL_MAX_FOCUS];
    uint64_t message_sequence;
    bool voice_receipt_awaiting;
    orb_prefs_patch_t prefs_request;
    bool prefs_queued;
    bool media_queued;
    orb_media_request_t media_request;
    orb_idle_page_t *idle;
    uint32_t idle_generation;
    bool idle_queued;
    orb_answer_t answers[ORB_MODEL_MAX_ANSWERS];
    size_t answer_count;
    bool answer_claimed[ORB_MODEL_MAX_ANSWERS];
    orb_session_t staged_sessions[ORB_MODEL_MAX_SESSIONS];
    size_t staged_count;
    bool staging;
    orb_message_t messages[ORB_MODEL_MAX_MESSAGES];
    size_t message_count;
    orb_voice_request_t voice_requests[ORB_MODEL_MAX_VOICE_REQUESTS];
    size_t voice_request_count;
    char voice_target[ORB_MODEL_ID_MAX + 1], voice_card[ORB_MODEL_CARD_ID_MAX + 1];
} model_store_t;

/* Keep the bounded payloads out of scarce internal DMA-capable memory. */
static model_store_t *s_store;
#ifdef ESP_PLATFORM
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex;
static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }
#else
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_mutex); }
static void unlock(void) { pthread_mutex_unlock(&s_mutex); }
#endif

/* Validate complete UTF-8 rather than quietly splitting a multibyte glyph at a
 * wire-field boundary. The decoder must reject oversize strings before this API. */
static bool valid_text(const char *text, size_t capacity, bool required)
{
    if (!text || capacity == 0) return false;
    const unsigned char *s = (const unsigned char *)text;
    size_t n = 0;
    while (n < capacity && s[n]) ++n;
    if (n == capacity || (required && n == 0)) return false;
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        unsigned need;
        uint32_t value;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1; value = c & 0x1f; minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2; value = c & 0x0f; minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3; value = c & 0x07; minimum = 0x10000;
        } else return false;
        if (n - i < need) return false;
        for (unsigned j = 0; j < need; ++j) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

#define VALID_FIELD(value, field, required) \
    valid_text((value)->field, sizeof((value)->field), (required))
#define COPY_FIELD(to, from, field) \
    memcpy((to)->field, (from)->field, strlen((from)->field) + 1)

static bool normalize_session(const orb_session_t *in, orb_session_t *out)
{
    if (!in || !VALID_FIELD(in, id, true) || !VALID_FIELD(in, title, false) ||
        !VALID_FIELD(in, project, false) || !VALID_FIELD(in, reason, false) ||
        !VALID_FIELD(in, summary, false) || in->agent < ORB_AGENT_CLAUDE ||
        in->agent > ORB_AGENT_DOT || in->state < ORB_SESSION_BUSY ||
        in->state > ORB_SESSION_ERROR) return false;
    memset(out, 0, sizeof(*out));
    COPY_FIELD(out, in, id);
    COPY_FIELD(out, in, title);
    COPY_FIELD(out, in, project);
    COPY_FIELD(out, in, reason);
    COPY_FIELD(out, in, summary);
    out->agent = in->agent;
    out->state = in->state;
    out->updated = in->updated;
    out->can_message = in->can_message;
    return true;
}

static bool normalize_card(const orb_card_t *in, orb_card_t *out)
{
    if (!in || !VALID_FIELD(in, id, true) || !VALID_FIELD(in, session, false) ||
        !VALID_FIELD(in, title, false) || !VALID_FIELD(in, body, false) ||
        !VALID_FIELD(in, detail, false) || in->kind < ORB_CARD_APPROVAL ||
        in->kind > ORB_CARD_ATTENTION || in->priority < ORB_PRIORITY_LOW ||
        in->priority > ORB_PRIORITY_HIGH || in->options_count > ORB_MODEL_MAX_OPTIONS ||
        in->scope < ORB_APPROVAL_UNSPECIFIED || in->scope > ORB_APPROVAL_TURN ||
        (in->kind != ORB_CARD_APPROVAL && in->scope != ORB_APPROVAL_UNSPECIFIED))
        return false;
    memset(out, 0, sizeof(*out));
    COPY_FIELD(out, in, id);
    COPY_FIELD(out, in, session);
    COPY_FIELD(out, in, title);
    COPY_FIELD(out, in, body);
    COPY_FIELD(out, in, detail);
    out->kind = in->kind;
    out->scope = in->scope;
    out->priority = in->priority;
    out->reply = in->reply;
    out->expires = in->expires;
    out->has_expiry = in->has_expiry || in->expires != 0;
    out->options_count = in->options_count;
    for (size_t i = 0; i < in->options_count; ++i) {
        const orb_option_t *option = &in->options[i];
        if (!VALID_FIELD(option, id, true) || !VALID_FIELD(option, label, true) ||
            option->role < ORB_OPTION_DEFAULT || option->role > ORB_OPTION_DESTRUCTIVE)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(option->id, in->options[j].id)) return false;
        COPY_FIELD(&out->options[i], option, id);
        COPY_FIELD(&out->options[i], option, label);
        out->options[i].role = option->role;
    }
    if (in->kind == ORB_CARD_APPROVAL) {
        bool deny = false;
        for (size_t i = 0; i < in->options_count; ++i)
            if (in->options[i].role == ORB_OPTION_CANCEL ||
                !strcmp(in->options[i].id, "deny")) deny = true;
        /* HIG §7: an approval must always offer a way to decline. Reject an
         * incomplete card rather than fabricate an answer unknown to the hub. */
        if (in->options_count < 2 || !deny) return false;
    }
    return true;
}

static size_t find_session(const char *id)
{
    for (size_t i = 0; i < s_store->view.session_count; ++i)
        if (!strcmp(s_store->view.sessions[i].id, id)) return i;
    return s_store->view.session_count;
}

static size_t find_card(const char *id)
{
    for (size_t i = 0; i < s_store->view.card_count; ++i)
        if (!strcmp(s_store->view.cards[i].id, id)) return i;
    return s_store->view.card_count;
}

static size_t find_stream(const char *session)
{
    for (size_t i = 0; i < s_store->view.stream_count; ++i)
        if (!strcmp(s_store->view.streams[i].session, session)) return i;
    return s_store->view.stream_count;
}

static void remove_stream(size_t i)
{
    if (i >= s_store->view.stream_count) return;
    --s_store->view.stream_count;
    memmove(&s_store->view.streams[i], &s_store->view.streams[i + 1],
            (s_store->view.stream_count - i) * sizeof(orb_agent_stream_t));
    memmove(&s_store->stream_logs[i], &s_store->stream_logs[i + 1],
            (s_store->view.stream_count - i) * sizeof(stream_ring_t));
    memset(&s_store->view.streams[s_store->view.stream_count], 0, sizeof(orb_agent_stream_t));
    memset(&s_store->stream_logs[s_store->view.stream_count], 0, sizeof(stream_ring_t));
}

static void changed(void)
{
    /* Zero is reserved for readers that have never obtained a snapshot. */
    if (++s_store->view.revision == 0) s_store->view.revision = 1;
}

bool orb_model_init(void)
{
#ifdef ESP_PLATFORM
    /* The application initializes once before it starts link and UI tasks. */
    if (s_store) return true;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (!s_mutex) return false;
#endif
    lock();
    if (!s_store) {
#ifdef ESP_PLATFORM
        s_store = heap_caps_calloc(1, sizeof(*s_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_store = calloc(1, sizeof(*s_store));
#endif
        if (s_store) {
            s_store->view.revision = 1;
            orb_model_prefs_defaults(&s_store->view.prefs);
            strcpy(s_store->voice_target, "dot");
            s_store->view.usage.unknown[0] = s_store->view.usage.unknown[1] = true;
            strcpy(s_store->view.weather.label, "Set a location in orb-hub");
        }
    }
    bool ok = s_store != NULL;
    unlock();
    return ok;
}

void orb_model_reset(void)
{
    if (!s_store) return;
    lock();
    uint64_t revision = s_store->view.revision;
    uint32_t generation = s_store->idle_generation + 1;
    free(s_store->idle);
    memset(s_store, 0, sizeof(*s_store));
    s_store->idle_generation = generation;
    orb_model_prefs_defaults(&s_store->view.prefs);
    s_store->view.revision = revision;
    strcpy(s_store->voice_target, "dot");
    s_store->view.usage.unknown[0] = s_store->view.usage.unknown[1] = true;
    strcpy(s_store->view.weather.label, "Set a location in orb-hub");
    changed();
    unlock();
}

uint64_t orb_model_revision(void)
{
    if (!s_store) return 0;
    lock();
    uint64_t revision = s_store->view.revision;
    unlock();
    return revision;
}

size_t orb_model_memory_bytes(void)
{
    if (!s_store) return 0;
    lock();
    size_t bytes = sizeof(*s_store) + (s_store->idle ? sizeof(*s_store->idle) : 0);
    unlock();
    return bytes;
}

bool orb_model_snapshot(orb_model_snapshot_t *out)
{
    if (!s_store || !out) return false;
    lock();
    memcpy(out, &s_store->view, sizeof(*out));
    unlock();
    return true;
}

bool orb_model_snapshot_if_changed(uint64_t revision, orb_model_snapshot_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool different = revision != s_store->view.revision;
    if (different) memcpy(out, &s_store->view, sizeof(*out));
    unlock();
    return different;
}

void orb_model_set_connection(bool online, bool synced)
{
    if (!s_store) return;
    lock();
    synced = online && synced;
    if (s_store->view.online != online || s_store->view.synced != synced) {
        s_store->view.online = online;
        s_store->view.synced = synced;
        if (!online) {
            s_store->view.speech_voice[0] = '\0';
            s_store->view.nowplaying_supported = false;
            s_store->view.nowplaying_valid = false;
            s_store->view.nowplaying_pending = false;
            s_store->media_queued = false;
            memset(&s_store->view.nowplaying, 0, sizeof(s_store->view.nowplaying));
            s_store->view.weather_supported = false;
            if (s_store->view.weather_valid && s_store->view.weather.available && !s_store->view.weather.stale) {
                s_store->view.weather.stale = true;
                ++s_store->view.weather.revision;
            }
            for (size_t i = 0; i < s_store->view.stream_count; ++i) {
                s_store->view.streams[i].stale = true;
                s_store->stream_logs[i].baseline = false;
            }
            for (size_t i = 0; i < s_store->view.usage.count; ++i)
                s_store->view.usage.windows[i].stale = true;
            if (s_store->view.prefs_pending) {
                s_store->view.prefs_pending = false;
                strcpy(s_store->view.prefs_error, "disconnected");
                s_store->prefs_queued = false;
            }
            if (s_store->idle) {
                s_store->idle->pending = false;
                strcpy(s_store->idle->error, "disconnected");
                s_store->idle_queued = false;
                ++s_store->idle_generation;
            }
        }
        changed();
    }
    unlock();
}

void orb_model_sessions_discard(void)
{
    if (!s_store) return;
    lock();
    s_store->staging = false;
    s_store->staged_count = 0;
    memset(s_store->staged_sessions, 0, sizeof(s_store->staged_sessions));
    unlock();
}

bool orb_model_sessions_chunk(const orb_session_t *sessions, size_t count, bool reset, bool more)
{
    if (!s_store || (count && !sessions) || count > ORB_MODEL_MAX_SESSIONS) return false;
    lock();
    if (reset) {
        s_store->staging = true;
        s_store->staged_count = 0;
        memset(s_store->staged_sessions, 0, sizeof(s_store->staged_sessions));
    }
    bool ok = s_store->staging && count <= ORB_MODEL_MAX_SESSIONS - s_store->staged_count;
    for (size_t i = 0; ok && i < count; ++i) {
        orb_session_t normalized;
        ok = normalize_session(&sessions[i], &normalized);
        for (size_t j = 0; ok && j < s_store->staged_count; ++j)
            if (!strcmp(normalized.id, s_store->staged_sessions[j].id)) ok = false;
        if (ok) s_store->staged_sessions[s_store->staged_count++] = normalized;
    }
    if (ok && !more) {
        memcpy(s_store->view.sessions, s_store->staged_sessions, sizeof(s_store->view.sessions));
        s_store->view.session_count = s_store->staged_count;
        for (size_t i = 0; i < s_store->view.stream_count;) {
            if (find_session(s_store->view.streams[i].session) == s_store->view.session_count)
                remove_stream(i);
            else {
                /* Snapshot establishes a new sequence baseline after a gap. */
                s_store->stream_logs[i].baseline = false;
                s_store->view.streams[i].stale = true;
                ++i;
            }
        }
        /* Old cards are stale after reconnect. Pending answers are independent
         * records and survive until a response; card replay re-disables them. */
        memset(s_store->view.cards, 0, sizeof(s_store->view.cards));
        s_store->view.card_count = 0;
        s_store->view.synced = s_store->view.online;
        s_store->staging = false;
        s_store->staged_count = 0;
        memset(s_store->staged_sessions, 0, sizeof(s_store->staged_sessions));
        changed();
    } else if (!ok) {
        s_store->staging = false;
        s_store->staged_count = 0;
        memset(s_store->staged_sessions, 0, sizeof(s_store->staged_sessions));
    }
    unlock();
    return ok;
}

static bool upsert_session(const orb_session_t *session, bool ordered)
{
    orb_session_t normalized;
    if (!s_store || !normalize_session(session, &normalized)) return false;
    lock();
    size_t i = find_session(normalized.id);
    bool found = i < s_store->view.session_count;
    bool ok = (found && (ordered || normalized.updated >= s_store->view.sessions[i].updated)) ||
              (!found && i < (ordered ? ORB_MODEL_MAX_FOCUS : ORB_MODEL_MAX_SESSIONS));
    if (ok && (!found || memcmp(&normalized, &s_store->view.sessions[i], sizeof(normalized)))) {
        s_store->view.sessions[i] = normalized;
        if (!found) ++s_store->view.session_count;
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_upsert_session(const orb_session_t *session)
{
    return upsert_session(session, false);
}

bool orb_model_apply_session(const orb_session_t *session)
{
    return upsert_session(session, true);
}

bool orb_model_remove_session(const char *id)
{
    if (!s_store || !valid_text(id, ORB_MODEL_ID_MAX + 1, true)) return false;
    lock();
    size_t i = find_session(id);
    bool found = i < s_store->view.session_count;
    if (found) {
        --s_store->view.session_count;
        memmove(&s_store->view.sessions[i], &s_store->view.sessions[i + 1],
                (s_store->view.session_count - i) * sizeof(orb_session_t));
        memset(&s_store->view.sessions[s_store->view.session_count], 0, sizeof(orb_session_t));
        remove_stream(find_stream(id));
        /* Cards have independent lifetimes; only card.cancel withdraws approval. */
        changed();
    }
    unlock();
    return found;
}

bool orb_model_upsert_card(const orb_card_t *card)
{
    orb_card_t normalized;
    if (!s_store || !normalize_card(card, &normalized)) return false;
    lock();
    size_t i = find_card(normalized.id);
    bool found = i < s_store->view.card_count;
    bool ok = found || i < ORB_MODEL_MAX_CARDS;
    if (ok) {
        /* Retransmission must never reactivate an already sent approval. A hub
         * wanting a fresh decision cancels the old card and uses a new id. */
        if (found) normalized.answered = s_store->view.cards[i].answered;
        for (size_t j = 0; j < s_store->answer_count; ++j)
            if (!strcmp(s_store->answers[j].card, normalized.id)) normalized.answered = true;
        if (!found || memcmp(&normalized, &s_store->view.cards[i], sizeof(normalized))) {
            s_store->view.cards[i] = normalized;
            if (!found) ++s_store->view.card_count;
            changed();
        }
    }
    unlock();
    return ok;
}

static void withdraw_answers(const char *id)
{
    size_t write = 0;
    for (size_t i = 0; i < s_store->answer_count; ++i) {
        if (strcmp(s_store->answers[i].card, id) || s_store->answer_claimed[i]) {
            if (write != i) {
                s_store->answers[write] = s_store->answers[i];
                s_store->answer_claimed[write] = s_store->answer_claimed[i];
            }
            ++write;
        } else {
            /* A locally queued choice can be cancelled before the link claims
             * it. Publish a terminal receipt instead of leaving the UI waiting
             * forever for an acknowledgment of an answer we never sent. */
            orb_answer_result_t *result = &s_store->view.last_answer;
            memset(result, 0, sizeof(*result));
            result->answer = s_store->answers[i];
            strcpy(result->error, "cancelled");
        }
    }
    memset(&s_store->answers[write], 0,
           (s_store->answer_count - write) * sizeof(orb_answer_t));
    memset(&s_store->answer_claimed[write], 0,
           (s_store->answer_count - write) * sizeof(bool));
    s_store->answer_count = write;
}

static void remove_card(size_t i)
{
    withdraw_answers(s_store->view.cards[i].id);
    --s_store->view.card_count;
    memmove(&s_store->view.cards[i], &s_store->view.cards[i + 1],
            (s_store->view.card_count - i) * sizeof(orb_card_t));
    memset(&s_store->view.cards[s_store->view.card_count], 0, sizeof(orb_card_t));
}

bool orb_model_cancel_card(const char *id)
{
    if (!s_store || !valid_text(id, ORB_MODEL_CARD_ID_MAX + 1, true)) return false;
    lock();
    size_t i = find_card(id);
    bool found = i < s_store->view.card_count;
    if (found) {
        strcpy(s_store->view.last_cancelled, id);
        remove_card(i);
        changed();
    }
    unlock();
    return found;
}

size_t orb_model_expire(uint64_t now)
{
    if (!s_store) return 0;
    lock();
    size_t removed = 0;
    for (size_t i = 0; i < s_store->view.card_count;) {
        if (s_store->view.cards[i].has_expiry && s_store->view.cards[i].expires <= now) {
            strcpy(s_store->view.last_expired, s_store->view.cards[i].id); remove_card(i); ++removed;
        } else ++i;
    }
    if (removed) changed();
    unlock();
    return removed;
}

bool orb_model_answer(const orb_answer_t *answer)
{
    if (!s_store || !answer || !VALID_FIELD(answer, card, true) ||
        !VALID_FIELD(answer, option, false) || !VALID_FIELD(answer, text, false) ||
        (!!answer->option[0] == !!answer->text[0])) return false;
    lock();
    size_t i = find_card(answer->card);
    bool ok = i < s_store->view.card_count &&
              s_store->answer_count < ORB_MODEL_MAX_ANSWERS;
    if (ok) {
        orb_card_t *card = &s_store->view.cards[i];
        ok = !card->answered;
        if (answer->option[0]) {
            bool found = false;
            for (size_t j = 0; j < card->options_count; ++j)
                if (!strcmp(card->options[j].id, answer->option)) found = true;
            ok = ok && found;
        } else ok = ok && card->reply;
        if (ok) {
            s_store->answer_claimed[s_store->answer_count] = false;
            orb_answer_t *queued = &s_store->answers[s_store->answer_count++];
            memset(queued, 0, sizeof(*queued));
            COPY_FIELD(queued, answer, card);
            COPY_FIELD(queued, answer, option);
            COPY_FIELD(queued, answer, text);
            card->answered = true;
            changed();
        }
    }
    unlock();
    return ok;
}

bool orb_model_take_answer(orb_answer_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool found = s_store->answer_count != 0;
    if (found) {
        *out = s_store->answers[0];
        --s_store->answer_count;
        memmove(&s_store->answers[0], &s_store->answers[1],
                s_store->answer_count * sizeof(orb_answer_t));
        memset(&s_store->answers[s_store->answer_count], 0, sizeof(orb_answer_t));
        memmove(&s_store->answer_claimed[0], &s_store->answer_claimed[1],
                s_store->answer_count * sizeof(bool));
        s_store->answer_claimed[s_store->answer_count] = false;
    }
    unlock();
    return found;
}

size_t orb_model_pending_answers(void)
{
    if (!s_store) return 0;
    lock();
    size_t count = s_store->answer_count;
    unlock();
    return count;
}

bool orb_model_peek_answer(orb_answer_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool found = s_store->answer_count != 0;
    if (found) {
        *out = s_store->answers[0];
        s_store->answer_claimed[0] = true;
    }
    unlock();
    return found;
}

static bool ack_answer_locked(const char *card)
{
    for (size_t i = 0; i < s_store->answer_count; ++i) {
        if (strcmp(s_store->answers[i].card, card)) continue;
        --s_store->answer_count;
        memmove(&s_store->answers[i], &s_store->answers[i + 1],
                (s_store->answer_count - i) * sizeof(orb_answer_t));
        memmove(&s_store->answer_claimed[i], &s_store->answer_claimed[i + 1],
                (s_store->answer_count - i) * sizeof(bool));
        memset(&s_store->answers[s_store->answer_count], 0, sizeof(orb_answer_t));
        s_store->answer_claimed[s_store->answer_count] = false;
        return true;
    }
    return false;
}

bool orb_model_ack_answer(const char *card)
{
    if (!s_store || !valid_text(card, ORB_MODEL_CARD_ID_MAX + 1, true)) return false;
    lock();
    bool found = ack_answer_locked(card);
    unlock();
    return found;
}

bool orb_model_complete_answer(const char *card, const orb_answer_t *recorded,
                               bool delivered, const char *error)
{
    if (!s_store || !valid_text(card, ORB_MODEL_CARD_ID_MAX + 1, true) ||
        (error && !valid_text(error, sizeof(s_store->view.last_answer.error), false))) return false;
    if (recorded && (!VALID_FIELD(recorded, card, true) || strcmp(recorded->card, card) ||
        !VALID_FIELD(recorded, option, false) || !VALID_FIELD(recorded, text, false) ||
        (!!recorded->option[0] == !!recorded->text[0]))) return false;
    orb_answer_result_t result = {0};
    strcpy(result.answer.card, card);
    if (recorded) {
        COPY_FIELD(&result.answer, recorded, option);
        COPY_FIELD(&result.answer, recorded, text);
    }
    result.accepted = recorded != NULL;
    result.delivered = recorded && delivered;
    if (error) strcpy(result.error, error);
    lock();
    bool found = ack_answer_locked(card);
    if (found) {
        s_store->view.last_answer = result;
        changed();
    }
    unlock();
    return found;
}

bool orb_model_message(const orb_message_t *message)
{
    if (!s_store || !message || !VALID_FIELD(message, target, true) ||
        !VALID_FIELD(message, text, true) || (message->stream && !message->generation)) return false;
    lock();
    bool supported = !strcmp(message->target, "dot");
    size_t i = find_session(message->target);
    if (i < s_store->view.session_count) supported = s_store->view.sessions[i].can_message;
    bool ok = supported && s_store->message_count < ORB_MODEL_MAX_MESSAGES;
    if (ok) {
        orb_message_t *queued = &s_store->messages[s_store->message_count++];
        memset(queued, 0, sizeof(*queued));
        COPY_FIELD(queued, message, target);
        COPY_FIELD(queued, message, text);
        queued->stream = message->stream;
        queued->generation = message->generation;
    }
    unlock();
    return ok;
}

bool orb_model_take_message(orb_message_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool found = s_store->message_count != 0;
    if (found) {
        *out = s_store->messages[0];
        --s_store->message_count;
        memmove(s_store->messages, &s_store->messages[1], s_store->message_count * sizeof(*out));
        memset(&s_store->messages[s_store->message_count], 0, sizeof(*out));
    }
    unlock();
    return found;
}

static void stream_row(stream_ring_t *ring, orb_stream_row_kind_t kind, const char *id,
                       const char *text, orb_step_state_t state, uint64_t at)
{
    if (!id[0] || !text[0]) return;
    size_t slot = ORB_MODEL_STREAM_ROWS;
    for (size_t i = 0; i < ring->count; ++i) {
        size_t p = (ring->head + i) % ORB_MODEL_STREAM_ROWS;
        if (ring->rows[p].kind == kind && !strcmp(ring->rows[p].id, id)) { slot = p; break; }
    }
    if (slot == ORB_MODEL_STREAM_ROWS) {
        slot = (ring->head + ring->count) % ORB_MODEL_STREAM_ROWS;
        if (ring->count == ORB_MODEL_STREAM_ROWS) ring->head = (ring->head + 1) % ORB_MODEL_STREAM_ROWS;
        else ++ring->count;
    } else if (!strcmp(ring->rows[slot].text, text) && ring->rows[slot].state == state) return;
    orb_stream_row_t *row = &ring->rows[slot];
    memset(row, 0, sizeof(*row));
    row->revision = ++ring->revision;
    row->at = at;
    row->kind = kind;
    row->state = state;
    strcpy(row->id, id);
    strcpy(row->text, text);
}

void orb_model_complete_message_receipt(const orb_message_t *message, orb_send_status_t status,
                                        const char *reason, const char *error)
{
    if (!s_store || !message || !VALID_FIELD(message, target, true) ||
        !VALID_FIELD(message, text, true) || (unsigned)status > ORB_SEND_FAILED ||
        (reason && !valid_text(reason, sizeof(s_store->view.last_message.reason), false)) ||
        (error && !valid_text(error, sizeof(s_store->view.last_message.error), false))) return;
    orb_message_result_t result = {0};
    COPY_FIELD(&result.message, message, target);
    COPY_FIELD(&result.message, message, text);
    result.message.stream = message->stream;
    result.message.generation = message->generation;
    result.status = status;
    result.accepted = status != ORB_SEND_FAILED;
    if (error) strcpy(result.error, error);
    if (reason) strcpy(result.reason, reason);
    lock();
    if (s_store->view.voice.phase == ORB_VOICE_SENDING &&
        message->stream == s_store->view.voice.stream &&
        !strcmp(message->target, s_store->view.voice.target) &&
        !strcmp(message->text, s_store->view.voice.text)) s_store->voice_receipt_awaiting = false;
    if (result.accepted) {
        size_t i = find_stream(message->target);
        if (i == s_store->view.stream_count && i < ORB_MODEL_MAX_FOCUS &&
            (find_session(message->target) < s_store->view.session_count || !strcmp(message->target, "dot"))) {
            strcpy(s_store->view.streams[i].session, message->target);
            ++s_store->view.stream_count;
        }
        if (i < s_store->view.stream_count) {
            char id[65];
            snprintf(id, sizeof(id), "user-%llu", (unsigned long long)++s_store->message_sequence);
            stream_row(&s_store->stream_logs[i], ORB_STREAM_USER, id, message->text, ORB_STEP_DONE, 0);
        }
    }
    s_store->view.last_message = result;
    changed();
    unlock();
}

void orb_model_complete_message(const orb_message_t *message, bool accepted, const char *error)
{
    /* Older hubs only acknowledge acceptance. Never fabricate delivery. */
    orb_model_complete_message_receipt(message, accepted ? ORB_SEND_UNKNOWN : ORB_SEND_FAILED,
                                       NULL, error);
}

bool orb_model_apply_stream(const orb_agent_stream_t *stream)
{
    if (!s_store || !stream || !VALID_FIELD(stream, session, true) ||
        !VALID_FIELD(stream, activity, false) || stream->steps_count > ORB_MODEL_MAX_STEPS ||
        (unsigned)stream->state > ORB_SESSION_ERROR ||
        !VALID_FIELD(&stream->assistant, id, false) || !VALID_FIELD(&stream->assistant, text, false)) return false;
    orb_agent_stream_t normalized = {0};
    COPY_FIELD(&normalized, stream, session);
    COPY_FIELD(&normalized, stream, activity);
    normalized.seq = stream->seq;
    normalized.at = stream->at;
    normalized.state = stream->state;
    normalized.stale = stream->stale;
    normalized.steps_count = stream->steps_count;
    COPY_FIELD(&normalized.assistant, &stream->assistant, id);
    COPY_FIELD(&normalized.assistant, &stream->assistant, text);
    normalized.assistant.final = stream->assistant.final;
    for (size_t i = 0; i < stream->steps_count; ++i) {
        const orb_agent_step_t *step = &stream->steps[i];
        if (!VALID_FIELD(step, id, true) || !VALID_FIELD(step, label, false) ||
            (unsigned)step->kind > ORB_STEP_REASONING || (unsigned)step->state > ORB_STEP_FAILED) return false;
        for (size_t j = 0; j < i; ++j) if (!strcmp(step->id, stream->steps[j].id)) return false;
        COPY_FIELD(&normalized.steps[i], step, id);
        COPY_FIELD(&normalized.steps[i], step, label);
        normalized.steps[i].kind = step->kind;
        normalized.steps[i].state = step->state;
        if ((step->has_added && step->added > INT32_MAX) ||
            (step->has_removed && step->removed > INT32_MAX)) return false;
        normalized.steps[i].has_added = step->has_added;
        normalized.steps[i].has_removed = step->has_removed;
        normalized.steps[i].added = step->has_added ? step->added : 0;
        normalized.steps[i].removed = step->has_removed ? step->removed : 0;
    }
    lock();
    size_t i = find_stream(stream->session);
    bool found = i < s_store->view.stream_count;
    bool ok = find_session(stream->session) < s_store->view.session_count &&
              (found || i < ORB_MODEL_MAX_FOCUS);
    if (ok && found && s_store->stream_logs[i].baseline) {
        uint64_t previous = s_store->view.streams[i].seq;
        ok = (stream->seq == previous && !memcmp(&normalized, &s_store->view.streams[i], sizeof(normalized))) ||
             (previous != UINT64_MAX && stream->seq == previous + 1);
    }
    if (ok) {
        if (!found) ++s_store->view.stream_count;
        stream_ring_t *ring = &s_store->stream_logs[i];
        ring->baseline = true;
        for (size_t j = 0; j < stream->steps_count; ++j)
            stream_row(ring, ORB_STREAM_STEP, stream->steps[j].id, stream->steps[j].label,
                       stream->steps[j].state, stream->at);
        stream_row(ring, ORB_STREAM_ASSISTANT, stream->assistant.id, stream->assistant.text,
                   stream->assistant.final ? ORB_STEP_DONE : ORB_STEP_RUNNING, stream->at);
        if (!found || memcmp(&normalized, &s_store->view.streams[i], sizeof(normalized))) {
            s_store->view.streams[i] = normalized;
            changed();
        }
    }
    unlock();
    return ok;
}

bool orb_model_get_stream(const char *session, orb_agent_stream_t *out)
{
    if (!s_store || !out || !valid_text(session, ORB_MODEL_ID_MAX + 1, true)) return false;
    lock();
    size_t i = find_stream(session);
    bool found = i < s_store->view.stream_count;
    if (found) *out = s_store->view.streams[i];
    unlock();
    return found;
}

bool orb_model_get_stream_log(const char *session, orb_stream_log_t *out)
{
    if (!s_store || !out || !valid_text(session, ORB_MODEL_ID_MAX + 1, true)) return false;
    lock();
    size_t i = find_stream(session);
    bool found = i < s_store->view.stream_count;
    if (found) {
        stream_ring_t *ring = &s_store->stream_logs[i];
        memset(out, 0, sizeof(*out));
        out->revision = ring->revision;
        out->count = ring->count;
        for (size_t j = 0; j < ring->count; ++j)
            out->rows[j] = ring->rows[(ring->head + j) % ORB_MODEL_STREAM_ROWS];
    }
    unlock();
    return found;
}

bool orb_model_set_usage(const orb_usage_snapshot_t *usage)
{
    if (!s_store || !usage || usage->count > ORB_MODEL_MAX_USAGE) return false;
    orb_usage_snapshot_t normalized = {0};
    normalized.count = usage->count;
    normalized.unknown[0] = usage->unknown[0];
    normalized.unknown[1] = usage->unknown[1];
    size_t counts[2] = {0};
    for (size_t i = 0; i < usage->count; ++i) {
        const orb_usage_window_t *window = &usage->windows[i];
        if ((unsigned)window->provider > ORB_AGENT_CODEX || !VALID_FIELD(window, label, true) ||
            !isfinite(window->used_percent) || window->used_percent < 0 || window->used_percent > 100 ||
            (unsigned)window->source > ORB_USAGE_UNKNOWN || usage->unknown[window->provider] ||
            ++counts[window->provider] > (window->provider == ORB_AGENT_CLAUDE ? 8 : 2)) return false;
        orb_usage_window_t *to = &normalized.windows[i];
        to->provider = window->provider;
        COPY_FIELD(to, window, label);
        to->used_percent = window->used_percent;
        to->resets_at = window->resets_at;
        to->observed_at = window->observed_at;
        to->stale = window->stale;
        to->source = window->source; to->on_credits = window->on_credits;
    }
    lock();
    if (memcmp(&s_store->view.usage, &normalized, sizeof(normalized))) {
        s_store->view.usage = normalized;
        changed();
    }
    unlock();
    return true;
}

bool orb_model_voice_focus(const char *target, const char *card)
{
    if (!s_store || !valid_text(target, ORB_MODEL_ID_MAX + 1, true) ||
        (card && !valid_text(card, ORB_MODEL_CARD_ID_MAX + 1, false))) return false;
    lock();
    strcpy(s_store->voice_target, target);
    strcpy(s_store->voice_card, card ? card : "");
    unlock();
    return true;
}

bool orb_model_get_voice_focus(char target[ORB_MODEL_ID_MAX + 1], char card[ORB_MODEL_CARD_ID_MAX + 1])
{
    if (!s_store || !target || !card) return false;
    lock();
    strcpy(target, s_store->voice_target);
    strcpy(card, s_store->voice_card);
    unlock();
    return true;
}

bool orb_model_voice_request(orb_voice_action_t action, const char *target, const char *card)
{
    if (!s_store || action < ORB_VOICE_START || action > ORB_VOICE_CONFIRM ||
        (target && !valid_text(target, ORB_MODEL_ID_MAX + 1, true)) ||
        (card && !valid_text(card, ORB_MODEL_CARD_ID_MAX + 1, false))) return false;
    lock();
    /* Cancel supersedes commands that have not reached the worker. Reserve its
     * effect even if a noisy producer filled the bounded command queue. */
    if (action == ORB_VOICE_CANCEL) {
        memset(s_store->voice_requests, 0, sizeof(s_store->voice_requests));
        s_store->voice_request_count = 0;
    }
    bool ok = s_store->voice_request_count < ORB_MODEL_MAX_VOICE_REQUESTS;
    if (ok) {
        orb_voice_request_t *queued = &s_store->voice_requests[s_store->voice_request_count++];
        memset(queued, 0, sizeof(*queued));
        queued->action = action;
        strcpy(queued->target, target ? target : s_store->voice_target);
        strcpy(queued->card, card ? card : (target ? "" : s_store->voice_card));
    }
    unlock();
    return orb_model_voice_notify(ok);
}

bool orb_model_take_voice_request(orb_voice_request_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool found = s_store->voice_request_count != 0;
    if (found) {
        *out = s_store->voice_requests[0];
        --s_store->voice_request_count;
        memmove(s_store->voice_requests, &s_store->voice_requests[1],
                s_store->voice_request_count * sizeof(*out));
        memset(&s_store->voice_requests[s_store->voice_request_count], 0, sizeof(*out));
    }
    unlock();
    return found;
}

bool orb_model_set_speech_voice(const char *name)
{
    if (!s_store || !valid_text(name, sizeof(s_store->view.speech_voice), false)) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return false;
    lock();
    if (strcmp(s_store->view.speech_voice, name)) {
        strcpy(s_store->view.speech_voice, name);
        changed();
    }
    unlock();
    return true;
}

bool orb_model_set_voice(const orb_voice_state_t *state)
{
    if (!s_store || !state || state->phase < ORB_VOICE_IDLE || state->phase > ORB_VOICE_ERROR ||
        state->level > 100 || !VALID_FIELD(state, target, false) || !VALID_FIELD(state, card, false) ||
        !VALID_FIELD(state, text, false) || !VALID_FIELD(state, error, false) ||
        !VALID_FIELD(state, error_code, false)) return false;
    orb_voice_state_t normalized = {0};
    normalized.phase = state->phase;
    normalized.stream = state->stream;
    normalized.level = state->level;
    COPY_FIELD(&normalized, state, target);
    COPY_FIELD(&normalized, state, card);
    COPY_FIELD(&normalized, state, text);
    COPY_FIELD(&normalized, state, error);
    COPY_FIELD(&normalized, state, error_code);
    lock();
    if (memcmp(&s_store->view.voice, &normalized, sizeof(normalized))) {
        const orb_voice_state_t *previous = &s_store->view.voice;
        if (normalized.phase == ORB_VOICE_SENDING &&
            (previous->phase != ORB_VOICE_SENDING || previous->stream != normalized.stream ||
             strcmp(previous->target, normalized.target) || strcmp(previous->text, normalized.text)))
            s_store->voice_receipt_awaiting = true;
        if (previous->phase == ORB_VOICE_SENDING && !previous->card[0] &&
            previous->target[0] && previous->text[0] &&
            (normalized.phase == ORB_VOICE_IDLE || normalized.phase == ORB_VOICE_ERROR)) {
            /* The voice worker leaves SENDING for IDLE only after message.send
             * acceptance. Record this here: a UI poll may miss SENDING entirely.
             * Failures remain uncertain and never create another send command. */
            orb_message_result_t *result = &s_store->view.last_message;
            /* A connection can reuse stream numbers. An old same-text receipt
             * is not evidence that this attempt reached its callback. */
            bool have_receipt = !s_store->voice_receipt_awaiting &&
                                result->message.stream == previous->stream &&
                                !strcmp(result->message.target, previous->target) &&
                                !strcmp(result->message.text, previous->text) &&
                                (result->accepted || result->status == ORB_SEND_FAILED);
            if (!have_receipt) {
                memset(result, 0, sizeof(*result));
                COPY_FIELD(&result->message, previous, target);
                COPY_FIELD(&result->message, previous, text);
                result->message.stream = previous->stream;
                /* Voice state has no connection generation. This is a receipt, not
                 * a replayable stream confirmation; generation intentionally stays 0. */
                result->accepted = normalized.phase == ORB_VOICE_IDLE;
                if (!result->accepted) {
                    result->status = ORB_SEND_FAILED;
                    strcpy(result->error, "delivery_unconfirmed");
                }
            }
        }
        s_store->view.voice = normalized;
        changed();
    }
    unlock();
    return true;
}

bool orb_model_get_voice(orb_voice_state_t *out)
{
    if (!s_store || !out) return false;
    lock();
    *out = s_store->view.voice;
    unlock();
    return true;
}

void orb_model_prefs_defaults(orb_prefs_t *out)
{
    if (out) *out = (orb_prefs_t){.sources = {true, true, true}, .idle_minutes = 10};
}

orb_temperature_t orb_model_temperature(void)
{
    if (!s_store) return ORB_TEMPERATURE_C;
    lock();
    orb_temperature_t unit = s_store->view.prefs.temperature;
    unlock();
    return unit == ORB_TEMPERATURE_F ? unit : ORB_TEMPERATURE_C;
}

/* Additive prefs from an older hub must not erase the device's known unit. */
static orb_prefs_t resolved_prefs(const orb_prefs_t *prefs)
{
    orb_prefs_t resolved = *prefs;
    if (resolved.temperature == ORB_TEMPERATURE_UNKNOWN) {
        resolved.temperature = s_store->view.prefs.temperature;
        resolved.temperature_owner = s_store->view.prefs.temperature_owner;
    }
    return resolved;
}

bool orb_model_set_prefs(const orb_prefs_t *prefs)
{
    if (!s_store || !prefs || prefs->idle_minutes > 1440 ||
        (unsigned)prefs->temperature > ORB_TEMPERATURE_F) return false;
    lock();
    orb_prefs_t resolved = resolved_prefs(prefs);
    prefs = &resolved;
    bool different = s_store->view.prefs.temperature != prefs->temperature ||
                     s_store->view.prefs.temperature_owner != prefs->temperature_owner ||
                     s_store->view.prefs.idle_minutes != prefs->idle_minutes ||
                     s_store->view.prefs_error[0] != '\0';
    for (size_t i = 0; i < 3; ++i)
        if (s_store->view.prefs.sources[i] != prefs->sources[i]) different = true;
    if (different || !s_store->view.prefs_known) {
        s_store->view.prefs = *prefs; s_store->view.prefs_known = true;
        s_store->view.prefs_error[0] = '\0';
        changed();
    }
    unlock();
    return true;
}

bool orb_model_prefs_request(const orb_prefs_patch_t *patch)
{
    if (!s_store || !patch || patch->sources_mask > 7 ||
        (!patch->sources_mask && !patch->has_idle_minutes && !patch->has_temperature) ||
        (patch->has_idle_minutes && patch->idle_minutes > 1440) ||
        (patch->has_temperature && (unsigned)patch->temperature > ORB_TEMPERATURE_F)) return false;
    lock();
    bool ok = s_store->view.online && s_store->view.synced && !s_store->view.prefs_pending;
    if (ok) {
        s_store->prefs_request = *patch;
        s_store->prefs_queued = true;
        s_store->view.prefs_pending = true;
        s_store->view.prefs_error[0] = '\0';
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_take_prefs_request(orb_prefs_patch_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool ok = s_store->prefs_queued;
    if (ok) {
        *out = s_store->prefs_request;
        s_store->prefs_queued = false;
    }
    unlock();
    return ok;
}

void orb_model_complete_prefs(const orb_prefs_t *prefs, const char *error)
{
    if (!s_store || (prefs && (prefs->idle_minutes > 1440 || (unsigned)prefs->temperature > ORB_TEMPERATURE_F)) ||
        (error && !valid_text(error, sizeof(s_store->view.prefs_error), false))) return;
    lock();
    if (s_store->view.prefs_pending) {
        if (prefs) { s_store->view.prefs = resolved_prefs(prefs); s_store->view.prefs_known = true; }
        s_store->view.prefs_pending = false;
        s_store->prefs_queued = false;
        strcpy(s_store->view.prefs_error, error ? error : "");
        /* Preference changes invalidate the hub's remainder cursor. */
        if (prefs && s_store->idle) {
            memset(s_store->idle, 0, sizeof(*s_store->idle));
            s_store->idle->pending = true;
            s_store->idle_queued = true;
            ++s_store->idle_generation;
        }
        changed();
    }
    unlock();
}

bool orb_model_set_summary(uint32_t idle, const uint32_t hidden_by_source[3])
{
    if (!s_store || !hidden_by_source) return false;
    lock();
    if (s_store->view.idle_count != idle ||
        memcmp(s_store->view.hidden_by_source, hidden_by_source, sizeof(s_store->view.hidden_by_source))) {
        s_store->view.idle_count = idle;
        memcpy(s_store->view.hidden_by_source, hidden_by_source, sizeof(s_store->view.hidden_by_source));
        changed();
    }
    unlock();
    return true;
}

bool orb_model_idle_open(void)
{
    if (!s_store) return false;
    lock();
    if (!s_store->idle) {
#ifdef ESP_PLATFORM
        s_store->idle = heap_caps_calloc(1, sizeof(*s_store->idle), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_store->idle = calloc(1, sizeof(*s_store->idle));
#endif
        if (s_store->idle) {
            ++s_store->idle_generation;
            s_store->idle->pending = true;
            s_store->idle_queued = true;
            changed();
        }
    }
    bool ok = s_store->idle != NULL;
    unlock();
    return ok;
}

void orb_model_idle_close(void)
{
    if (!s_store) return;
    lock();
    if (s_store->idle) {
        free(s_store->idle);
        s_store->idle = NULL;
        s_store->idle_queued = false;
        ++s_store->idle_generation;
        changed();
    }
    unlock();
}

bool orb_model_idle_more(void)
{
    if (!s_store) return false;
    lock();
    bool ok = s_store->idle && !s_store->idle->pending &&
              (s_store->idle->cursor[0] || s_store->idle->error[0]);
    if (ok) {
        s_store->idle->pending = true;
        s_store->idle->error[0] = '\0';
        s_store->idle_queued = true;
        ++s_store->idle_generation;
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_idle_snapshot(orb_idle_page_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool ok = s_store->idle != NULL;
    if (ok) *out = *s_store->idle;
    unlock();
    return ok;
}

bool orb_model_take_idle_request(orb_idle_request_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool ok = s_store->idle && s_store->idle_queued &&
              s_store->view.online && s_store->view.synced;
    if (ok) {
        memset(out, 0, sizeof(*out));
        out->generation = s_store->idle_generation;
        strcpy(out->cursor, s_store->idle->cursor);
        s_store->idle_queued = false;
    }
    unlock();
    return ok;
}

bool orb_model_complete_idle(const orb_idle_request_t *request,
                             const orb_session_t *sessions, size_t count,
                             const char *cursor, const char *error)
{
    if (!s_store || !request || (count && !sessions) || count > ORB_MODEL_MAX_IDLE ||
        (cursor && !valid_text(cursor, ORB_MODEL_CURSOR_MAX + 1, false)) ||
        (error && !valid_text(error, sizeof(s_store->view.prefs_error), false))) return false;
    /* Validate the whole page before replacing anything; no partial page can
     * accidentally turn a malformed response into selectable agent context. */
    for (size_t i = 0; i < count; ++i) {
        orb_session_t normalized;
        if (!normalize_session(&sessions[i], &normalized)) return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(sessions[i].id, sessions[j].id)) return false;
    }
    lock();
    bool ok = s_store->idle && s_store->idle->pending &&
              request->generation == s_store->idle_generation;
    if (ok) {
        orb_idle_page_t *page = s_store->idle;
        if (error && !strcmp(error, "conflict")) {
            memset(page, 0, sizeof(*page));
            page->pending = true;
            s_store->idle_queued = true;
            ++s_store->idle_generation;
        } else if (error) {
            page->pending = false;
            strcpy(page->error, error);
        } else {
            memset(page, 0, sizeof(*page));
            page->count = count;
            for (size_t i = 0; i < count; ++i) normalize_session(&sessions[i], &page->sessions[i]);
            strcpy(page->cursor, cursor ? cursor : "");
        }
        changed();
    }
    unlock();
    return ok;
}

void orb_model_load_demo(void)
{
    if (!orb_model_init()) return;
    orb_model_reset();
    static const orb_session_t sessions[] = {
        {.id = "claude-firmware", .agent = ORB_AGENT_CLAUDE, .title = "Firmware build",
         .project = "esp32s3-firmware", .state = ORB_SESSION_WAITING,
         .reason = "Approve Bash: idf.py build",
         .summary = "Ready to build the round shell. The command runs locally on the Mac mini.",
         .updated = 1793102400, .can_message = true},
        {.id = "codex-shell", .agent = ORB_AGENT_CODEX, .title = "Round shell",
         .project = "orb/w2-shell", .state = ORB_SESSION_BUSY,
         .reason = "Checking simulator layouts",
         .summary = "Reviewing the Agents list and approval actions at larger text sizes.",
         .updated = 1793102390, .can_message = true},
        {.id = "dot-evening", .agent = ORB_AGENT_DOT, .title = "Evening plans",
         .project = "Dot", .state = ORB_SESSION_IDLE, .reason = "Ready for your reply",
         .summary = "Your afternoon is clear. Ask me to help plan the evening.",
         .updated = 1793102300, .can_message = true},
        {.id = "codex-tests", .agent = ORB_AGENT_CODEX, .title = "Hub checks",
         .project = "orb/w2-hub", .state = ORB_SESSION_DONE, .reason = "Tests passed",
         .summary = "The bounded card queue and cancellation checks passed.",
         .updated = 1793102200},
        {.id = "claude-assets", .agent = ORB_AGENT_CLAUDE, .title = "Font assets",
         .project = "esp32s3-firmware", .state = ORB_SESSION_ERROR, .reason = "Asset pack missing",
         .summary = "The simulator needs the local font pack before this check can continue.",
         .updated = 1793102100, .can_message = true},
    };
    static const orb_card_t cards[] = {
        {.id = "approval-build", .session = "claude-firmware", .kind = ORB_CARD_APPROVAL,
         .title = "Allow build?", .body = "Mac mini · Allow once.",
         .detail = "Bash: idf.py build", .options = {
             {.id = "allow", .label = "Allow", .role = ORB_OPTION_DEFAULT},
             {.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL}},
         .options_count = 2, .priority = ORB_PRIORITY_HIGH},
        {.id = "choice-next", .session = "codex-shell", .kind = ORB_CARD_CHOICE,
         .title = "What next?", .body = "Choose the next surface to review.", .options = {
             {.id = "agents", .label = "Agents", .role = ORB_OPTION_PRIMARY},
             {.id = "stack", .label = "Smart Stack", .role = ORB_OPTION_DEFAULT},
             {.id = "settings", .label = "Settings", .role = ORB_OPTION_DEFAULT},
             {.id = "later", .label = "Later", .role = ORB_OPTION_CANCEL}},
         .options_count = 4, .priority = ORB_PRIORITY_NORMAL},
        {.id = "message-dot", .session = "dot-evening", .kind = ORB_CARD_MESSAGE,
         .title = "From Dot", .body = "A quiet evening ahead. What’s your plan?",
         .reply = true, .priority = ORB_PRIORITY_NORMAL},
        {.id = "attention-tests", .session = "codex-tests", .kind = ORB_CARD_ATTENTION,
         .title = "Build complete", .body = "Build checks passed. Ready to review.",
         .priority = ORB_PRIORITY_LOW},
    };
    for (size_t i = 0; i < sizeof(sessions) / sizeof(sessions[0]); ++i)
        orb_model_upsert_session(&sessions[i]);
    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i)
        orb_model_upsert_card(&cards[i]);
}

const char *orb_weather_condition_name(orb_weather_condition_t condition)
{
    static const char *const names[] = {"unknown", "clear", "partly_cloudy", "cloudy",
        "fog", "drizzle", "rain", "snow", "thunder"};
    return condition >= ORB_WEATHER_UNKNOWN && condition <= ORB_WEATHER_THUNDER ? names[condition] : "unknown";
}

const char *orb_weather_condition_label(orb_weather_condition_t condition, bool is_day)
{
    static const char *const labels[] = {"Weather unavailable", "Clear", "Partly cloudy", "Cloudy",
        "Fog", "Drizzle", "Rain", "Snow", "Thunder"};
    if (condition == ORB_WEATHER_CLEAR && is_day) return "Sunny";
    return condition >= ORB_WEATHER_UNKNOWN && condition <= ORB_WEATHER_THUNDER ? labels[condition] : labels[0];
}

static bool weather_temperature(float value)
{
    return isfinite(value) && value >= -250 && value <= 350;
}

static bool normalize_weather(const orb_weather_snapshot_t *in, orb_weather_snapshot_t *out)
{
    if (!in || !VALID_FIELD(in, place, in->configured) || !VALID_FIELD(in, label, true) ||
        (in->unit != ORB_TEMPERATURE_C && in->unit != ORB_TEMPERATURE_F) ||
        in->hourly_count > ORB_MODEL_MAX_WEATHER_HOURS) return false;
    memset(out, 0, sizeof(*out));
    out->configured = in->configured;
    out->available = in->available;
    out->stale = in->stale;
    out->unit = in->unit;
    COPY_FIELD(out, in, place);
    COPY_FIELD(out, in, label);
    if (!in->available) {
        if (in->temperature != 0 || in->high != 0 || in->low != 0 || in->hourly_count ||
            in->condition != ORB_WEATHER_UNKNOWN || in->is_day || in->updated_at) return false;
        if (!in->configured)
            return !in->place[0] && !in->stale && !strcmp(in->label, "Set a location in orb-hub");
        return !strcmp(in->label, in->stale ? "Weather unavailable" : "Loading weather");
    }
    if (!in->configured || !in->updated_at || in->condition < ORB_WEATHER_CLEAR ||
        in->condition > ORB_WEATHER_THUNDER || !weather_temperature(in->temperature) ||
        !weather_temperature(in->high) || !weather_temperature(in->low) || in->high < in->low ||
        strcmp(in->label, orb_weather_condition_label(in->condition, in->is_day))) return false;
    out->temperature = in->temperature;
    out->high = in->high;
    out->low = in->low;
    out->condition = in->condition;
    out->is_day = in->is_day;
    out->updated_at = in->updated_at;
    out->hourly_count = in->hourly_count;
    uint32_t previous = in->updated_at;
    for (size_t i = 0; i < in->hourly_count; ++i) {
        const orb_weather_hour_t *hour = &in->hourly[i];
        if (hour->hour <= previous || !weather_temperature(hour->temperature) ||
            hour->condition < ORB_WEATHER_CLEAR || hour->condition > ORB_WEATHER_THUNDER) return false;
        out->hourly[i].hour = hour->hour;
        out->hourly[i].temperature = hour->temperature;
        out->hourly[i].condition = hour->condition;
        out->hourly[i].is_day = hour->is_day;
        previous = hour->hour;
    }
    return true;
}

void orb_model_weather_capability(bool supported)
{
    if (!s_store) return;
    lock();
    bool update = s_store->view.weather_supported != supported;
    s_store->view.weather_supported = supported;
    if (!supported && s_store->view.weather_valid && s_store->view.weather.available && !s_store->view.weather.stale) {
        s_store->view.weather.stale = true;
        ++s_store->view.weather.revision;
        update = true;
    }
    if (update) changed();
    unlock();
}

bool orb_model_set_weather(const orb_weather_snapshot_t *snapshot)
{
    if (!s_store) return false;
    orb_weather_snapshot_t normalized;
    if (!normalize_weather(snapshot, &normalized)) return false;
    lock();
    bool ok = s_store->view.online && s_store->view.weather_supported;
    normalized.revision = s_store->view.weather.revision;
    if (ok && (!s_store->view.weather_valid || memcmp(&normalized, &s_store->view.weather, sizeof(normalized)))) {
        ++normalized.revision;
        s_store->view.weather = normalized;
        s_store->view.weather_valid = true;
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_weather_snapshot(orb_weather_snapshot_t *out)
{
    if (!s_store || !out) return false;
    lock();
    *out = s_store->view.weather;
    unlock();
    return true;
}

void orb_model_nowplaying_capability(bool supported)
{
    if (!s_store) return;
    lock();
    if (s_store->view.nowplaying_supported != supported || !supported) {
        s_store->view.nowplaying_supported = supported;
        s_store->view.nowplaying_valid = false;
        s_store->view.nowplaying_pending = false;
        s_store->view.nowplaying_error[0] = '\0';
        memset(&s_store->view.nowplaying, 0, sizeof(s_store->view.nowplaying));
        s_store->media_queued = false;
        changed();
    }
    unlock();
}

bool orb_model_set_nowplaying(const orb_nowplaying_t *snapshot)
{
    if (!s_store || !snapshot || !VALID_FIELD(snapshot, title, false) ||
        !VALID_FIELD(snapshot, artist, false) || !VALID_FIELD(snapshot, app, false) ||
        !isfinite(snapshot->duration) || snapshot->duration < 0 ||
        !isfinite(snapshot->position) || snapshot->position < 0 ||
        snapshot->volume < 0 || snapshot->volume > 100 ||
        snapshot->state < ORB_MEDIA_PLAYING || snapshot->state > ORB_MEDIA_STOPPED ||
        snapshot->playing != (snapshot->state == ORB_MEDIA_PLAYING)) return false;
    orb_nowplaying_t normalized = {0};
    COPY_FIELD(&normalized, snapshot, title);
    COPY_FIELD(&normalized, snapshot, artist);
    COPY_FIELD(&normalized, snapshot, app);
    normalized.duration = snapshot->duration;
    normalized.position = snapshot->position;
    normalized.volume = snapshot->volume;
    normalized.state = snapshot->state;
    normalized.playing = snapshot->playing;
    lock();
    bool ok = s_store->view.online && s_store->view.nowplaying_supported;
    if (ok && (!s_store->view.nowplaying_valid ||
        memcmp(&normalized, &s_store->view.nowplaying, sizeof(normalized)))) {
        s_store->view.nowplaying = normalized;
        s_store->view.nowplaying_valid = true;
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_nowplaying_request(orb_media_action_t action, int value)
{
    if (!s_store || action < ORB_MEDIA_PLAY || action > ORB_MEDIA_VOLUME ||
        (action == ORB_MEDIA_VOLUME && (value < 0 || value > 100))) return false;
    lock();
    bool ok = s_store->view.online && s_store->view.nowplaying_supported &&
        s_store->view.nowplaying_valid && !s_store->view.nowplaying_pending &&
        (action == ORB_MEDIA_VOLUME || s_store->view.nowplaying.app[0]);
    if (ok) {
        s_store->media_request = (orb_media_request_t){action, value};
        s_store->media_queued = true;
        s_store->view.nowplaying_pending = true;
        s_store->view.nowplaying_error[0] = '\0';
        changed();
    }
    unlock();
    return ok;
}

bool orb_model_take_nowplaying_request(orb_media_request_t *out)
{
    if (!s_store || !out) return false;
    lock();
    bool ok = s_store->media_queued && s_store->view.nowplaying_supported && s_store->view.online;
    if (ok) { *out = s_store->media_request; s_store->media_queued = false; }
    unlock();
    return ok;
}

void orb_model_complete_nowplaying(const char *error)
{
    if (!s_store) return;
    lock();
    if (s_store->view.nowplaying_pending) {
        s_store->view.nowplaying_pending = false;
        s_store->media_queued = false;
        snprintf(s_store->view.nowplaying_error, sizeof(s_store->view.nowplaying_error), "%s", error ? error : "");
        changed();
    }
    unlock();
}
