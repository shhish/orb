#pragma once

/* UI owner / LVGL lock only. Handles are zero-initialized, caller-owned and
 * remain valid after parent deletion. Descriptor/model pointers are borrowed;
 * model/environment projections are valid only during their callback. Lifecycle
 * operations cannot recursively destroy their own instance. Host intent handlers
 * enqueue a copy; they must not synchronously route or destroy the sender.
 * Destroy is called after every entered create, including create returning false;
 * the framework alone owns and deletes the root and state allocation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "orb_intent.h"
#include "orb_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_SCREEN_RESOURCE_LIMIT 32
#define ORB_SCREEN_CONTEXT_LIMIT 32
#define ORB_SCREEN_CONTEXT_MAX 4096
#define ORB_SCREEN_FEEDBACK_HISTORY 32

typedef struct orb_screen_host orb_screen_host_t;
typedef struct { void *framework; } orb_screen_instance_t;

typedef struct {
    uint64_t model, home, time, settings, device;
} orb_model_revisions_t;
typedef uint32_t orb_model_domain_mask_t;
enum {
    ORB_DOMAIN_MODEL = 1u, ORB_DOMAIN_HOME = 2u, ORB_DOMAIN_TIME = 4u,
    ORB_DOMAIN_SETTINGS = 8u, ORB_DOMAIN_DEVICE = 16u, ORB_DOMAIN_ALL = 31u,
};
typedef struct { const void *data; size_t size; } orb_screen_model_t;
typedef struct { uint64_t revision; bool reduce_motion, always_on; } orb_ui_environment_t;
typedef struct {
    orb_model_revisions_t revisions;
    const orb_screen_model_t *model;
    const orb_ui_environment_t *environment;
} orb_screen_update_t;

typedef struct {
    uint16_t version, size;
    unsigned char payload[];
} orb_screen_context_t;

/* The schema covers the entire payload (padding must remain zero). There are
 * deliberately no pointer, command, handle or arbitrary-byte field kinds. */
typedef enum {
    ORB_CONTEXT_U32 = 1, ORB_CONTEXT_I32, ORB_CONTEXT_U64,
    ORB_CONTEXT_ID, ORB_CONTEXT_BOOL, ORB_CONTEXT_TEXT,
} orb_context_kind_t;
typedef struct { uint16_t offset, size; orb_context_kind_t kind; } orb_context_field_t;

typedef enum { ORB_BACK_CONSUMED, ORB_BACK_CLOSE_LAYER, ORB_BACK_LEAVE_SCREEN } orb_back_result_t;
typedef enum {
    ORB_VISIBILITY_ACTIVE, ORB_VISIBILITY_COVERED,
    ORB_VISIBILITY_REDACTED, ORB_VISIBILITY_SUSPENDED,
} orb_visibility_t;
typedef enum {
    ORB_PRIVACY_UNDECLARED, ORB_PRIVACY_PUBLIC, ORB_PRIVACY_PRIVATE,
} orb_privacy_policy_t;
typedef enum {
    ORB_AOD_UNDECLARED, ORB_AOD_HIDDEN, ORB_AOD_PUBLIC_FACE, ORB_AOD_PUBLIC_SUMMARY,
} orb_aod_policy_t;
typedef enum {
    ORB_FEEDBACK_UNDECLARED, ORB_FEEDBACK_SILENT, ORB_FEEDBACK_STANDARD,
} orb_feedback_profile_t;
typedef enum {
    ORB_CUE_OPEN = 1, ORB_CUE_CLOSE, ORB_CUE_DETENT,
    ORB_CUE_SUBMIT, ORB_CUE_DONE, ORB_CUE_ERROR,
} orb_feedback_role_t;
typedef struct {
    const char *title;
    uint16_t accent_token, title_role;
    bool inset_ring;
} orb_screen_chrome_t;
typedef struct {
    orb_memory_owner_t owner;
    size_t state_bytes, context_bytes, payload_bytes, ui_bytes;
    bool declared;
} orb_memory_profile_t;

typedef enum { ORB_RIM_SCROLL = 1, ORB_RIM_FOCUS, ORB_RIM_VALUE, ORB_RIM_GAME_LOCAL } orb_rim_kind_t;
typedef struct {
    uint64_t subpage, property;
    orb_rim_kind_t kind;
    float minimum, maximum, value, detent, units_per_turn;
    bool physical_required;
    void (*begin)(orb_screen_instance_t *, orb_input_origin_t, bool physical);
    void (*preview)(orb_screen_instance_t *, float value);
    void (*finish)(orb_screen_instance_t *, float value, bool commit,
                   orb_input_origin_t, bool physical);
} orb_rim_binding_t;

typedef struct {
    bool (*create)(orb_screen_instance_t *, const orb_screen_args_t *);
    void (*update)(orb_screen_instance_t *, const orb_screen_update_t *);
    void (*destroy)(orb_screen_instance_t *);
    bool (*save_context)(const orb_screen_instance_t *, orb_screen_context_t *, size_t);
    bool (*restore_context)(orb_screen_instance_t *, const orb_screen_context_t *);
    orb_back_result_t (*back)(orb_screen_instance_t *);
    bool (*rim_binding)(orb_screen_instance_t *, orb_rim_binding_t *);
    void (*visibility)(orb_screen_instance_t *, orb_visibility_t);
    void (*chrome)(const orb_screen_instance_t *, orb_screen_chrome_t *);
} orb_screen_ops_t;

typedef struct {
    orb_screen_id_t id, parent_id;
    const orb_screen_ops_t *ops;
    size_t state_bytes, context_bytes;
    uint16_t context_version;
    const orb_context_field_t *context_fields;
    size_t context_field_count;
    orb_memory_profile_t memory;
    orb_model_domain_mask_t subscriptions;
    orb_privacy_policy_t privacy;
    orb_aod_policy_t always_on;
    orb_feedback_profile_t feedback;
    orb_nav_role_t nav_role;
} orb_screen_descriptor_t;

typedef bool (*orb_screen_intent_cb_t)(void *, const orb_intent_t *);
typedef void (*orb_screen_cue_cb_t)(void *, orb_feedback_role_t);
/* Optional adapter subaccounts existing LVGL/kit charges, never reserves them a
 * second time. enter/leave must be paired; used excludes shared caches/fonts. */
typedef struct {
    void (*enter)(void *, orb_screen_instance_t *);
    void (*leave)(void *, orb_screen_instance_t *);
    size_t (*used)(void *, const orb_screen_instance_t *);
} orb_screen_ui_scope_t;
typedef struct {
    orb_nav_t *nav;
    orb_screen_intent_cb_t intent;
    orb_screen_cue_cb_t cue;
    void *context;
    orb_screen_ui_scope_t ui_scope;
    /* Optional immutable projection, borrowed through the immediate update. */
    bool (*project)(void *, orb_screen_id_t, orb_screen_update_t *);
    /* Compatibility hosts can retain the existing outer input handoff while
     * descriptors migrate. Read-only previews remain visible and silent. */
    bool defer_rim, preview;
    /* Default denial is unchanged. A migration host can explicitly retain
     * safe synthetic preference actions; preview/silent guards still win. */
    bool (*allow_synthetic)(void *, const orb_intent_t *);
    /* Optional refusal feedback only. This never admits or replays an action;
     * preview, visibility and silence guards run before this callback. */
    void (*synthetic_refused)(void *, const orb_intent_t *);
    /* Optional shared navigation for the rim lease while a compatibility host
     * keeps its own routing value in nav. The shell owns this instance and
     * rebinds it at every place change, so a released lease leaves the
     * binding for that rebind, and an unchanged binding refreshes in place. */
    orb_nav_t *rim_nav;
    /* With rim_nav: whether the shell's recogniser is driving a rim contact,
     * and its provenance. The first change of that contact opens the lease. */
    bool (*rim_contact)(void *, bool *physical);
} orb_screen_host_config_t;

typedef struct {
    orb_screen_instance_t *instance;
    orb_rim_binding_t binding;
    orb_input_origin_t origin;
    bool physical, contact;
    float start;
} orb_rim_lease_t;
struct orb_screen_host {
    orb_screen_host_config_t config;
    orb_rim_lease_t lease;
    uint64_t event_sequence;
    uint64_t event_ids[ORB_SCREEN_FEEDBACK_HISTORY];
    bool event_consumed[ORB_SCREEN_FEEDBACK_HISTORY];
};

void orb_screen_host_init(orb_screen_host_t *, const orb_screen_host_config_t *);
bool orb_screen_descriptor_valid(const orb_screen_descriptor_t *, const char **reason);
bool orb_screen_create(orb_screen_instance_t *, orb_screen_host_t *,
                       const orb_screen_descriptor_t *, lv_obj_t *parent,
                       const orb_screen_args_t *, bool preview);
void orb_screen_destroy(orb_screen_instance_t *);
bool orb_screen_alive(const orb_screen_instance_t *);
void *orb_screen_state(const orb_screen_instance_t *);
lv_obj_t *orb_screen_root(const orb_screen_instance_t *);
const orb_screen_descriptor_t *orb_screen_descriptor(const orb_screen_instance_t *);
orb_screen_host_t *orb_screen_host(const orb_screen_instance_t *);
bool orb_screen_update(orb_screen_instance_t *, const orb_screen_update_t *);
void orb_screen_visibility(orb_screen_instance_t *, orb_visibility_t);
orb_visibility_t orb_screen_visibility_get(const orb_screen_instance_t *);
orb_back_result_t orb_screen_back(orb_screen_instance_t *);
void orb_screen_chrome(const orb_screen_instance_t *, orb_screen_chrome_t *);
bool orb_screen_save(const orb_screen_instance_t *, orb_screen_context_t *, size_t);
bool orb_screen_restore(orb_screen_instance_t *, const orb_screen_context_t *, size_t);
bool orb_screen_context_valid(const orb_screen_descriptor_t *, const orb_screen_context_t *, size_t);
/* Missing ID resolves to the first current item, or zero for empty data. */
uint64_t orb_screen_resolve_id(uint64_t requested, const uint64_t *ids, size_t count);
int32_t orb_screen_clamp_scroll(int32_t requested, int32_t maximum);

/* Tracked payloads are automatically released, including failed create. */
void *orb_screen_alloc(orb_screen_instance_t *, size_t bytes);
bool orb_screen_free(orb_screen_instance_t *, void *);
typedef enum {
    ORB_RESOURCE_TIMER = 1, ORB_RESOURCE_ANIMATION,
    ORB_RESOURCE_OBSERVER, ORB_RESOURCE_ASYNC,
} orb_screen_resource_kind_t;
typedef void (*orb_screen_cleanup_cb_t)(void *);
typedef void (*orb_screen_pause_cb_t)(void *, bool paused);
bool orb_screen_track(orb_screen_instance_t *, orb_screen_resource_kind_t,
                      void *, orb_screen_cleanup_cb_t, orb_screen_pause_cb_t);
/* Forget after natural completion; release invokes cleanup now. */
bool orb_screen_untrack(orb_screen_instance_t *, void *resource, bool release);
lv_timer_t *orb_screen_timer_create(orb_screen_instance_t *, lv_timer_cb_t,
                                   uint32_t period, void *user_data);
typedef struct { size_t state, context, payload, payload_peak, ui; unsigned resources[4], allocations; } orb_screen_usage_t;
void orb_screen_usage(const orb_screen_instance_t *, orb_screen_usage_t *);

bool orb_screen_emit(orb_screen_instance_t *, const orb_intent_t *);
/* Issue at semantic commitment/result. Zero means the 32-event ledger is full.
 * Every feedback attempt retires its ID, even when policy makes it silent.
 * Out-of-order IDs are accepted once; retired/reused slots reject old replay. */
uint64_t orb_screen_event(orb_screen_host_t *);
bool orb_screen_feedback(orb_screen_instance_t *, uint64_t event,
                         orb_feedback_role_t, orb_input_origin_t, bool committed);

bool orb_screen_rim_acquire(orb_screen_instance_t *);
void orb_screen_rim_release(orb_screen_host_t *);
bool orb_screen_rim_begin(orb_screen_instance_t *, orb_input_origin_t, bool physical);
bool orb_screen_rim_set(orb_screen_instance_t *, float value, orb_input_origin_t);
bool orb_screen_rim_finish(orb_screen_instance_t *, bool commit);
bool orb_screen_rim_step(orb_screen_host_t *, int detents, bool physical);

#ifdef __cplusplus
}
#endif
