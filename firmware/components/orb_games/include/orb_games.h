#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "orb_game_logic.h"
#include "esp_err.h"

/* Compiled provider for orb_apps. Called by its additive builtins hook, before
 * the display starts. Each manifest is offline and uses a per-game namespace.
 * Shell lists orb_apps_at/count and queues orb_apps_request_enter(id); it never
 * calls main-owned lifecycle callbacks under the LVGL lock. */
bool orb_apps_register_builtins(void);
esp_err_t orb_games_last_save_error(void);

/* UI-thread requests: PWR pauses a playing round; Back/BOOT exits only from
 * its pause sheet. No model mutation or LVGL work happens on the caller. */
bool orb_games_request_pause(void);
bool orb_games_request_back(void);
/* Main's IMU poll uses 100 Hz only while a tilt game needs fresh samples.
 * Atomic observation; no lifecycle/model pointer crosses task boundaries. */
bool orb_games_motion_active(void);

/* UI benchmark reservation serializes with main-owned app entry, including an
 * entry waiting for the LVGL lock. No board input or score storage is borrowed. */
bool orb_games_benchmark_reserve(void);
void orb_games_benchmark_release(void);

/* A bounded, storage-backed progress snapshot. Boot registration and lifecycle
 * exit refresh it outside LVGL; UI reads never touch the filesystem. Unknown
 * or invalid records return an empty line. */
void orb_games_progress(orb_game_kind_t kind, char *text, size_t capacity);
