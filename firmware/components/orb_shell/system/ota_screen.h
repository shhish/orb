#pragma once

#include "../orb_app_host.h"
#include "orb_shell.h"
#include "orb_fonts.h"

enum { ORB_OTA_SCREEN_ID = 0xff04, ORB_OTA_LINE_COUNT = 6 };
typedef enum {
    ORB_OTA_UPDATES, ORB_OTA_OFFER, ORB_OTA_DOWNLOAD, ORB_OTA_VERIFY, ORB_OTA_READY, ORB_OTA_FAILED,
} orb_ota_page_t;
typedef struct { uint32_t started; orb_ota_page_t page; bool waiting; } orb_ota_wait_t;

/* Generic shell scaffolding is shared with setup/pairing. Borrow it only for
 * the immediate composition; its label styles deliberately survive OTA. */
typedef struct {
    lv_obj_t *system, *lines[ORB_OTA_LINE_COUNT], *reading, *ring;
    lv_obj_t *art, *wifi, *battery, *power, *settings, *primary, *cancel;
    void (*line)(void *, unsigned, const char *, orb_text_style_t, orb_text_variant_t, unsigned);
    void (*primary_action)(void *, const char *, orb_shell_action_t, bool);
    void *context;
    /* Shell-lifetime UI budget survives complete presenter destruction. */
    orb_ota_wait_t *wait;
} orb_ota_scaffold_t;
typedef struct {
    bool (*read)(void *, orb_ota_scaffold_t *);
    void *context;
} orb_ota_source_t;

const orb_screen_descriptor_t *orb_ota_descriptor(void);
orb_app_host_t *orb_ota_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
bool orb_ota_host_ensure(orb_app_host_t *host, lv_obj_t *parent, const orb_ota_source_t *source);
/* These opportunities stay in the shell's original order: capture before
 * choosing a destination, copy after its generic reset, then full layout.
 * A progress-only delivery never enters either full-render opportunity. */
void orb_ota_screen_capture(orb_screen_instance_t *instance, orb_ota_page_t page);
void orb_ota_screen_content(orb_screen_instance_t *instance, orb_ota_page_t page,
    const orb_shell_snapshot_t *status, bool pending);
void orb_ota_screen_layout(orb_screen_instance_t *instance);
void orb_ota_screen_progress(orb_screen_instance_t *instance, int progress);
