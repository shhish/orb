#pragma once

/* Bounded, LVGL-free device state. Call init before starting link/UI tasks.
 * Writers never call the UI: a reader pulls a locked copy on revision change.
 * All lengths are UTF-8 bytes, excluding the trailing NUL. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "orb_units.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_MODEL_MAX_FOCUS 8
#define ORB_MODEL_MAX_IDLE 8
#define ORB_MODEL_CURSOR_MAX 80
#define ORB_MODEL_MAX_SESSIONS 24
#define ORB_MODEL_MAX_CARDS 16
#define ORB_MODEL_MAX_OPTIONS 4
#define ORB_MODEL_MAX_ANSWERS 16
#define ORB_MODEL_ID_MAX 48 /* Session IDs; retained for source compatibility. */
#define ORB_MODEL_CARD_ID_MAX 64
#define ORB_MODEL_OPTION_ID_MAX 32
#define ORB_MODEL_MAX_MESSAGES 4
#define ORB_MODEL_MAX_VOICE_REQUESTS 8
#define ORB_MODEL_TEXT_MAX 280
#define ORB_MODEL_MAX_STEPS 3
#define ORB_MODEL_MAX_USAGE 10
#define ORB_MODEL_STREAM_ROWS 24
#define ORB_MODEL_MAX_WEATHER_HOURS 6

typedef enum { ORB_AGENT_CLAUDE, ORB_AGENT_CODEX, ORB_AGENT_DOT } orb_agent_t;
typedef enum {
    ORB_SESSION_BUSY, ORB_SESSION_WAITING, ORB_SESSION_IDLE, ORB_SESSION_DONE, ORB_SESSION_ERROR
} orb_session_state_t;
typedef enum { ORB_CARD_APPROVAL, ORB_CARD_CHOICE, ORB_CARD_MESSAGE, ORB_CARD_ATTENTION } orb_card_kind_t;
typedef enum { ORB_OPTION_DEFAULT, ORB_OPTION_PRIMARY, ORB_OPTION_CANCEL, ORB_OPTION_DESTRUCTIVE } orb_option_role_t;
typedef enum { ORB_PRIORITY_LOW, ORB_PRIORITY_NORMAL, ORB_PRIORITY_HIGH } orb_priority_t;
typedef enum { ORB_APPROVAL_UNSPECIFIED, ORB_APPROVAL_OPERATION, ORB_APPROVAL_TURN } orb_approval_scope_t;

typedef struct {
    char id[ORB_MODEL_ID_MAX + 1];
    orb_agent_t agent;
    char title[65], project[33];
    orb_session_state_t state;
    char reason[65], summary[161];
    uint64_t updated;
    bool can_message;
} orb_session_t;

typedef struct {
    char id[ORB_MODEL_OPTION_ID_MAX + 1], label[21];
    orb_option_role_t role;
} orb_option_t;

typedef struct {
    char id[ORB_MODEL_CARD_ID_MAX + 1], session[ORB_MODEL_ID_MAX + 1];
    orb_card_kind_t kind;
    orb_approval_scope_t scope; /* Approval grant scope; unspecified means one operation. */
    char title[65], body[281], detail[201];
    orb_option_t options[ORB_MODEL_MAX_OPTIONS];
    uint8_t options_count;
    bool reply;
    orb_priority_t priority;
    uint64_t expires;
    bool has_expiry; /* Explicit epoch zero is expired; legacy nonzero values imply true. */
    bool answered;    /* Local only: queued for delivery, not hub acceptance. */
} orb_card_t;

typedef struct {
    char card[ORB_MODEL_CARD_ID_MAX + 1];
    char option[ORB_MODEL_OPTION_ID_MAX + 1];
    char text[ORB_MODEL_TEXT_MAX + 1]; /* Exactly one of option/text is nonempty. */
} orb_answer_t;

typedef struct {
    orb_answer_t answer; /* The hub's recorded first answer can differ from ours. */
    bool accepted, delivered;
    char error[33];
} orb_answer_result_t;

typedef struct {
    char target[ORB_MODEL_ID_MAX + 1];
    char text[ORB_MODEL_TEXT_MAX + 1];
    uint16_t stream; /* Zero omits stream; stream confirmations belong to this link. */
    uint32_t generation; /* Local source connection; required when stream is nonzero. */
} orb_message_t;

typedef enum { ORB_SEND_UNKNOWN, ORB_SEND_QUEUED, ORB_SEND_DELIVERED, ORB_SEND_FAILED } orb_send_status_t;

typedef struct {
    orb_message_t message;
    bool accepted;
    char error[33]; /* timeout means delivery is unknown; never auto-resubmit. */
    orb_send_status_t status;
    char reason[161];
} orb_message_result_t;

typedef enum { ORB_STEP_TOOL, ORB_STEP_REASONING } orb_step_kind_t;
typedef enum { ORB_STEP_RUNNING, ORB_STEP_DONE, ORB_STEP_FAILED } orb_step_state_t;
typedef struct {
    char id[65];
    orb_step_kind_t kind;
    char label[65];
    orb_step_state_t state;
    bool has_added, has_removed;
    uint32_t added, removed;
} orb_agent_step_t;
typedef struct {
    char id[65], text[161];
    bool final;
} orb_agent_assistant_t;
typedef struct {
    char session[ORB_MODEL_ID_MAX + 1];
    uint64_t seq, at;
    orb_session_state_t state;
    bool stale;
    char activity[65];
    orb_agent_step_t steps[ORB_MODEL_MAX_STEPS];
    size_t steps_count;
    orb_agent_assistant_t assistant;
} orb_agent_stream_t;

typedef enum { ORB_STREAM_USER, ORB_STREAM_ASSISTANT, ORB_STREAM_STEP } orb_stream_row_kind_t;
typedef struct {
    uint64_t revision, at;
    orb_stream_row_kind_t kind;
    char id[65], text[ORB_MODEL_TEXT_MAX + 1];
    orb_step_state_t state;
} orb_stream_row_t;
typedef struct {
    uint64_t revision;
    size_t count;
    /* Oldest first. Stable row IDs allow the UI to update only changed rows. */
    orb_stream_row_t rows[ORB_MODEL_STREAM_ROWS];
} orb_stream_log_t;

typedef enum { ORB_USAGE_STATUSLINE, ORB_USAGE_USAGE, ORB_USAGE_ROLLOUT,
               ORB_USAGE_APP_SERVER, ORB_USAGE_UNKNOWN } orb_usage_source_t;
typedef struct {
    orb_agent_t provider; /* Claude or Codex only. */
    char label[65];
    double used_percent;
    uint64_t resets_at, observed_at;
    bool stale;
    orb_usage_source_t source;
    bool on_credits;
} orb_usage_window_t;
typedef struct {
    size_t count;
    orb_usage_window_t windows[ORB_MODEL_MAX_USAGE];
    bool unknown[2]; /* Provider indexed; unknown is never represented as 0%. */
} orb_usage_snapshot_t;

typedef enum { ORB_VOICE_START, ORB_VOICE_STOP, ORB_VOICE_CANCEL, ORB_VOICE_CONFIRM } orb_voice_action_t;
typedef enum {
    ORB_VOICE_IDLE, ORB_VOICE_STARTING, ORB_VOICE_RECORDING, ORB_VOICE_TRANSCRIBING,
    ORB_VOICE_CONFIRMING, ORB_VOICE_SENDING, ORB_VOICE_ERROR
} orb_voice_phase_t;

typedef struct {
    orb_voice_action_t action;
    char target[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
} orb_voice_request_t;

typedef struct {
    orb_voice_phase_t phase;
    uint16_t stream;
    char target[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    char text[ORB_MODEL_TEXT_MAX + 1], error[97];
    char error_code[33]; /* Stable hub/local code; display copy never parses msg. */
    uint8_t level; /* 0..100, shown with the persistent recording indicator. */
} orb_voice_state_t;

typedef struct {
    bool sources[3]; /* Indexed by orb_agent_t. */
    uint16_t idle_minutes;
    orb_temperature_t temperature;
    bool temperature_owner; /* units.source == "owner"; otherwise automatic. */
} orb_prefs_t;

typedef struct {
    uint8_t sources_mask; /* Bit (1 << orb_agent_t) means include that source. */
    bool sources[3];
    bool has_idle_minutes;
    uint16_t idle_minutes;
    bool has_temperature;
    orb_temperature_t temperature; /* UNKNOWN sends "auto". */
} orb_prefs_patch_t;

typedef struct {
    size_t count;
    orb_session_t sessions[ORB_MODEL_MAX_IDLE];
    char cursor[ORB_MODEL_CURSOR_MAX + 1]; /* Empty means cursor:null/exhausted. */
    bool pending;
    char error[33];
} orb_idle_page_t;

typedef struct {
    uint32_t generation;
    char cursor[ORB_MODEL_CURSOR_MAX + 1];
} orb_idle_request_t;

/* Display metadata is bounded; album is validated on the wire but not retained.
 * Values are confirmed Mac data only; no local playback clock is inferred. */
typedef enum { ORB_MEDIA_PLAYING, ORB_MEDIA_PAUSED, ORB_MEDIA_STOPPED } orb_media_state_t;
typedef enum {
    ORB_MEDIA_PLAY, ORB_MEDIA_PAUSE, ORB_MEDIA_PLAYPAUSE,
    ORB_MEDIA_NEXT, ORB_MEDIA_PREVIOUS, ORB_MEDIA_VOLUME
} orb_media_action_t;
typedef struct {
    char title[161], artist[97], app[33];
    double duration, position;
    orb_media_state_t state;
    int volume;
    bool playing;
} orb_nowplaying_t;
typedef struct { orb_media_action_t action; int value; } orb_media_request_t;

typedef enum {
    ORB_WEATHER_UNKNOWN, ORB_WEATHER_CLEAR, ORB_WEATHER_PARTLY_CLOUDY,
    ORB_WEATHER_CLOUDY, ORB_WEATHER_FOG, ORB_WEATHER_DRIZZLE,
    ORB_WEATHER_RAIN, ORB_WEATHER_SNOW, ORB_WEATHER_THUNDER
} orb_weather_condition_t;

typedef struct {
    uint32_t hour; /* Unix seconds, formatted with the device's local clock. */
    float temperature; /* Already in snapshot.unit; do not convert again. */
    orb_weather_condition_t condition;
    bool is_day;
} orb_weather_hour_t;

typedef struct {
    uint64_t revision; /* Local, changes only when the retained value changes. */
    bool configured, available, stale;
    char place[65], label[33];
    orb_temperature_t unit;
    float temperature, high, low; /* Valid only when available. */
    orb_weather_condition_t condition; /* UNKNOWN represents wire null. */
    bool is_day;
    uint32_t updated_at;
    size_t hourly_count;
    orb_weather_hour_t hourly[ORB_MODEL_MAX_WEATHER_HOURS];
} orb_weather_snapshot_t;

typedef struct {
    uint64_t revision;
    bool online, synced;
    orb_prefs_t prefs;
    /* False until the hub has confirmed preferences since reset. Offline UI
     * then shows "unknown" instead of presenting local defaults as fact. */
    bool prefs_known;
    bool prefs_pending;
    char prefs_error[33];
    uint32_t idle_count, hidden_by_source[3];
    bool nowplaying_supported, nowplaying_valid, nowplaying_pending;
    char nowplaying_error[33];
    orb_nowplaying_t nowplaying;
    bool weather_supported, weather_valid;
    orb_weather_snapshot_t weather;
    orb_voice_state_t voice;
    char speech_voice[65]; /* Confirmed Mac voice; empty means unknown. */
    orb_answer_result_t last_answer;
    orb_message_result_t last_message;
    size_t stream_count;
    orb_agent_stream_t streams[ORB_MODEL_MAX_FOCUS];
    orb_usage_snapshot_t usage;
    /* Explicit hub cancellation only. Absence during snapshot replay is not a
     * cancellation; older coalesced removals may remain generically closed. */
    char last_cancelled[ORB_MODEL_CARD_ID_MAX + 1];
    /* Last card removed by its own deadline (orb_model_expire), so a viewer
     * can say "Expired" rather than waiting for a refresh that never comes. */
    char last_expired[ORB_MODEL_CARD_ID_MAX + 1];
    size_t session_count, card_count;
    orb_session_t sessions[ORB_MODEL_MAX_SESSIONS];
    orb_card_t cards[ORB_MODEL_MAX_CARDS];
} orb_model_snapshot_t;

/* ESP payloads live in PSRAM. Failure leaves no partially initialized store.
 * Init is idempotent and is called once by the owning application before tasks. */
bool orb_model_init(void);
void orb_model_reset(void);
uint64_t orb_model_revision(void);
bool orb_model_snapshot(orb_model_snapshot_t *out);
bool orb_model_snapshot_if_changed(uint64_t revision, orb_model_snapshot_t *out);
/* Chunks remain invisible until more=false; reset=false requires a live stage.
 * The final chunk atomically replaces sessions and clears cards for replay.
 * Pending answers survive; replayed cards with pending answers stay disabled. */
bool orb_model_sessions_chunk(const orb_session_t *sessions, size_t count, bool reset, bool more);
void orb_model_sessions_discard(void);
void orb_model_set_connection(bool online, bool synced);
bool orb_model_upsert_session(const orb_session_t *session);
/* Ordered wire updates may legitimately move the hub wall-clock backwards. */
bool orb_model_apply_session(const orb_session_t *session);
bool orb_model_remove_session(const char *id);
bool orb_model_upsert_card(const orb_card_t *card);
bool orb_model_cancel_card(const char *id);
/* Expiry is explicit: use the hub-synchronized unix clock, never LVGL ticks. */
size_t orb_model_expire(uint64_t now);
bool orb_model_answer(const orb_answer_t *answer);
/* Link drains this bounded queue; false means empty. Cancellation/expiry also
 * withdraws answers that have not yet been handed to the link. */
bool orb_model_take_answer(orb_answer_t *out);
size_t orb_model_pending_answers(void);
/* Reliable link path: peek claims the head without removing it. Claimed
 * answers survive cancel, expiry and reconnect until complete/ack. */
bool orb_model_peek_answer(orb_answer_t *out);
bool orb_model_ack_answer(const char *card);
bool orb_model_complete_answer(const char *card, const orb_answer_t *recorded,
                               bool delivered, const char *error);
/* Text messages are handed off once. Ambiguous send timeouts must not silently
 * start duplicate turns; the sender reports the result before another send. */
bool orb_model_message(const orb_message_t *message);
bool orb_model_take_message(orb_message_t *out);
void orb_model_complete_message(const orb_message_t *message, bool accepted, const char *error);
/* Exact bounded receipts preserve queued vs transport-delivered semantics. */
void orb_model_complete_message_receipt(const orb_message_t *message, orb_send_status_t status,
                                        const char *reason, const char *error);
/* A replacement is accepted only for a current focus session. Sequence gaps
 * return false without changing state; a new sessions snapshot resets baseline. */
bool orb_model_apply_stream(const orb_agent_stream_t *stream);
bool orb_model_get_stream(const char *session, orb_agent_stream_t *out);
bool orb_model_get_stream_log(const char *session, orb_stream_log_t *out);
bool orb_model_set_usage(const orb_usage_snapshot_t *usage);
/* UI mic controls queue commands. START with NULL target uses the current
 * focus (default dot). Commands never invoke audio/network code on LVGL. */
bool orb_model_voice_request(orb_voice_action_t action, const char *target, const char *card);
/* One service-owner notifier, invoked outside the model lock after enqueue.
 * Registration also wakes already queued work, so startup cannot lose it. */
void orb_model_set_voice_notify(void (*notify)(void *context), void *context);
bool orb_model_take_voice_request(orb_voice_request_t *out);
bool orb_model_voice_focus(const char *target, const char *card);
bool orb_model_get_voice_focus(char target[ORB_MODEL_ID_MAX + 1], char card[ORB_MODEL_CARD_ID_MAX + 1]);
bool orb_model_set_voice(const orb_voice_state_t *state);
bool orb_model_get_voice(orb_voice_state_t *out);
/* Empty clears unknown metadata; never infer a name from a local default. */
bool orb_model_set_speech_voice(const char *name);
/* Confirmed preferences remain visible until the hub accepts the partial patch.
 * Requests are single-flight and are never silently retried after a timeout. */
void orb_model_prefs_defaults(orb_prefs_t *out);
/* Last confirmed device preference survives old hubs and link loss. */
orb_temperature_t orb_model_temperature(void);
bool orb_model_set_prefs(const orb_prefs_t *prefs);
bool orb_model_prefs_request(const orb_prefs_patch_t *patch);
bool orb_model_take_prefs_request(orb_prefs_patch_t *out);
void orb_model_complete_prefs(const orb_prefs_t *prefs, const char *error);
bool orb_model_set_summary(uint32_t idle, const uint32_t hidden_by_source[3]);
/* Idle payload is separate PSRAM owned only while this view is open. A stale
 * response from a closed/reopened view cannot install a page into the new one. */
bool orb_model_idle_open(void);
void orb_model_idle_close(void);
bool orb_model_idle_more(void);
bool orb_model_idle_snapshot(orb_idle_page_t *out);
bool orb_model_take_idle_request(orb_idle_request_t *out);
bool orb_model_complete_idle(const orb_idle_request_t *request,
                             const orb_session_t *sessions, size_t count,
                             const char *cursor, const char *error);
/* Capability is reset at every disconnect; stale media is never actionable.
 * One command in flight, no automatic retry or optimistic metadata. */
void orb_model_nowplaying_capability(bool supported);
bool orb_model_set_nowplaying(const orb_nowplaying_t *snapshot);
bool orb_model_nowplaying_request(orb_media_action_t action, int value);
bool orb_model_take_nowplaying_request(orb_media_request_t *out);
void orb_model_complete_nowplaying(const char *error);
/* Weather is read-only; disconnect retains the last value marked stale. */
void orb_model_weather_capability(bool supported);
bool orb_model_set_weather(const orb_weather_snapshot_t *snapshot);
bool orb_model_weather_snapshot(orb_weather_snapshot_t *out);
const char *orb_weather_condition_name(orb_weather_condition_t condition);
const char *orb_weather_condition_label(orb_weather_condition_t condition, bool is_day);
/* Explicit test/developer fixture only. Never called automatically by init. */
void orb_model_load_demo(void);
/* Exact live retained-store payload; excludes caller-owned snapshots. */
size_t orb_model_memory_bytes(void);

#ifdef __cplusplus
}
#endif
