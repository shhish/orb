#pragma once

#include "orb_screen.h"

/* Compatibility hosting for one single-route app descriptor while the shell
 * still owns outer routes, history and transitions (Home, Time). Contexts
 * stay opaque to the workspace; preview hosts create read-only framework
 * instances that issue no intent, cue or lease. UI owner only. */
typedef struct orb_app_host orb_app_host_t;

#define ORB_APP_HOST_MODEL_MAX 64

typedef struct {
    /* The app's single complete descriptor (also in its registry manifest). */
    const orb_screen_descriptor_t *(*descriptor)(void);
    const char *name;
    /* Zero keeps the small default; larger projections reserve exactly this
     * many bytes in the host, without enlarging every other app host. */
    size_t model_bytes;
    /* Retained presenters keep their own instance across layers; a closed
     * ordinary face starts from current preferences, as before migration. */
    bool forget_on_detach;
    bool (*command)(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent);
    /* The app's own write vocabulary. Runs synchronously so the service's
     * acceptance returns to the presenter in its original order. */
    bool (*intent)(const orb_intent_t *intent);
    /* Optional: app actions whose presenter has no physical-input guard keep
     * accepting synthetic activation, exactly as before migration. */
    bool (*allow_synthetic)(const orb_intent_t *intent);
    /* The presenter's own rotary finish: contact suspension and lifts that
     * did not pass through the lease. */
    void (*rim_finish)(orb_screen_instance_t *instance, bool commit);
    /* Local hierarchy presentation (a navigate intent to the screen itself):
     * before=true precedes the screen's own tree change. */
    void (*transition)(void *context, const lv_area_t *token, bool reverse, bool before);
    /* The shell's navigation and its recogniser's latest pointer, whose
     * provenance a rim contact keeps; NULL for preview copies. */
    orb_nav_t *nav;
    lv_indev_t *const *input;
    void *context;
} orb_app_host_config_t;

orb_app_host_t *orb_app_host_create(bool preview, const orb_app_host_config_t *config);
void orb_app_host_destroy(orb_app_host_t *host);
/* The projection the screen receives at creation and on later updates. */
bool orb_app_host_set_model(orb_app_host_t *host, const void *model, size_t size);
const void *orb_app_host_model(const orb_app_host_t *host, size_t *size);
/* Creates the screen as the page's last child, then restores saved context. */
bool orb_app_host_attach(orb_app_host_t *host, lv_obj_t *page);
/* Saves and closes the screen when it lives under page; never refuses. */
void orb_app_host_detach_page(orb_app_host_t *host, lv_obj_t *page);
orb_screen_instance_t *orb_app_host_current(orb_app_host_t *host);
/* A changed environment projection, and the screen's time-driven poll. */
void orb_app_host_environment(orb_app_host_t *host);
void orb_app_host_poll(orb_app_host_t *host);
/* The screen's rim property as a framework lease on the shell's nav. False
 * when it offers none, so the shell keeps its own list/unbind policy. */
bool orb_app_host_rim(orb_app_host_t *host);
/* One presenter rotary finish per call: a captured lease lift commits through
 * the lease; otherwise (and for cancellation) the presenter finishes directly. */
void orb_app_host_rim_finish(orb_app_host_t *host, bool commit);
/* A case twist while the screen is shown: through the lease when it holds one
 * and no finger is down, else the shell's ordinary step; then the presenter's
 * one safe edit finishes at once. False when the screen is not shown. */
bool orb_app_host_twist(orb_app_host_t *host, int detents, bool *changed);
/* Deep-copies value contexts, including the live screen's current context,
 * so preview/benchmark copies never alias or replay the source. */
bool orb_app_host_clone(orb_app_host_t **destination, orb_app_host_t *source, bool preview);
bool orb_app_host_restore(orb_app_host_t **destination, orb_app_host_t **saved);
