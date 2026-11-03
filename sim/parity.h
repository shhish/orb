#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "orb_shell.h"
#include "orb_model.h"

/* One scenario per process: no durable state and no reset-order dependence. */
typedef struct {
    const char *id;
    const char *family;
    const char *state;
    const char *rim; /* Declared binding hint; each frame also records runtime observation. */
    void (*setup)(const char *id);
} parity_scenario_t;
extern const parity_scenario_t parity_shell_scenarios[];
extern const unsigned parity_shell_count;
extern const parity_scenario_t parity_app_scenarios[];
extern const unsigned parity_app_count;
extern orb_shell_t *parity_shell;
extern lv_display_t *parity_display;
extern orb_shell_snapshot_t parity_status;
extern uint32_t parity_now;
void parity_check(bool ok, const char *why);
void parity_advance(unsigned ms);
void parity_shell_create(bool fixture);
void parity_navigate(orb_shell_surface_t surface);
bool parity_tap_text(const char *text); /* exact visible text, never child indices */
void parity_tap(int x, int y);
void parity_event(const char *kind, const char *detail);
void parity_set_root(lv_obj_t *root);
lv_obj_t *parity_root(void);
void parity_drag(int x1, int y1, int x2, int y2, unsigned ms);
void parity_apps_poll(void);
void parity_sound_event(const char *op, unsigned cue, bool accepted);
const char *parity_rim_observed(void);
bool parity_apps_rim(char *out, size_t capacity);
bool parity_apps_semantic(char *out, size_t capacity);
void parity_sound_storage_state(bool saving, bool error);
void parity_idle_failure(bool fail);
