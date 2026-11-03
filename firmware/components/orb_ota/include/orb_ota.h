#pragma once

/*
 * Over-the-air updates from the hub (docs/protocol.md, "Firmware downloads"):
 * offers arrive as ota.available (or answer ota.check), images stream over
 * pinned HTTPS into the inactive slot, are checked (SHA-256 and app
 * signature), and boot only once healthy; otherwise the bootloader rolls back.
 */

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    ORB_OTA_IDLE,
    ORB_OTA_OFFERED,       /* an offer is waiting (install "idle" or "ask") */
    ORB_OTA_DOWNLOADING,
    ORB_OTA_VERIFYING,
    ORB_OTA_READY,         /* written and checked; reboots to install */
    ORB_OTA_FAILED,
} orb_ota_state_t;

typedef struct {
    orb_ota_state_t state;
    int progress;          /* 0-100 while downloading */
    char version[32];      /* of the offer */
    char notes[128];
    char error[64];
    bool assets;           /* this status refers to the independent assets channel */
    bool pending_verify;   /* this boot is a new image on probation */
} orb_ota_status_t;

typedef void (*orb_ota_cb_t)(const orb_ota_status_t *status, void *ctx);

/*
 * First thing in app_main: if this image is on probation, starts the 60 s
 * health deadline (missing it marks the image invalid and reboots into the
 * previous one), and records the previous attempt's outcome for hello.
 */
esp_err_t orb_ota_boot_check(void);

/*
 * Call once per boot, after the first hello the hub accepted (with the UI up),
 * whether or not this image is on probation. On probation it keeps the image.
 * Every boot, it also tells orb_ota that the hello carrying last_ota got
 * through, so the report stops being sent.
 */
void orb_ota_mark_healthy(void);

/* Registers with orb_link (ota.available, hello's last_ota) and acts on offers
 * by their install policy. cb on every state change. Call before orb_link_start
 * so the first hello and offer cannot arrive before their handlers exist. */
esp_err_t orb_ota_start(orb_ota_cb_t cb, void *ctx);

esp_err_t orb_ota_status(orb_ota_status_t *out);

/* Asks the hub for an offer now (ota.check). */
esp_err_t orb_ota_check_now(void);

/* Installs the waiting offer (the user tapped "Update"). Reboots when READY. */
esp_err_t orb_ota_install(void);

/* For install "idle": the app tells orb_ota when the device is charging and
 * untouched, and when that stops. */
void orb_ota_set_idle(bool idle);

/* The battery, from the power service on every change. Below 30 % and not
 * charging, "now" and "idle" offers wait (a tap still installs), since losing
 * power on probation rolls a good image back. battery_pct < 0: unknown or no
 * battery. Until called, nothing is held back. */
void orb_ota_set_power(int battery_pct, bool charging);
