#pragma once

#include "orb_home.h"
#include "orb_model.h"

/* LVGL/UI-thread ownership: previews retain immutable generations, rather
 * than borrowing pointers whose live workspace may update or disappear. */
typedef struct {
    size_t references;
    orb_model_snapshot_t value;
} workspace_model_t;

typedef struct {
    size_t references;
    orb_home_snapshot_t value;
} workspace_home_t;

typedef struct {
    workspace_model_t *model;
    workspace_home_t *home;
} workspace_snapshots_t;

/* Private workspace payloads stay in PSRAM on ESP; host telemetry preserves
 * unbudgeted allocation behavior. Allocator metadata is outside the receipt. */
void *orb_workspace_payload_alloc(size_t size);
void orb_workspace_payload_free(void *pointer);
bool orb_workspace_snapshots_init(workspace_snapshots_t *out, const workspace_snapshots_t *source,
    const orb_model_snapshot_t *model, const orb_home_snapshot_t *home);
void orb_workspace_snapshots_release(workspace_snapshots_t *snapshots);
bool orb_workspace_model_refresh(workspace_model_t **storage, const orb_model_snapshot_t **view);
bool orb_workspace_home_refresh(workspace_home_t **storage, const orb_home_snapshot_t **view);
