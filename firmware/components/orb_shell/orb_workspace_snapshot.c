/* Snapshot generations and payload accounting belong together, independently
 * of the shell's LVGL tree, navigation and presenter implementation. */
#include "orb_workspace_snapshot.h"
#include "orb_memory.h"

#include <stddef.h>
#include <stdlib.h>

#ifndef ESP_PLATFORM
/* Match device payload receipts without imposing board admission on host
 * fixtures. The header is allocator overhead, excluded on both platforms. */
typedef union {
    max_align_t alignment;
    size_t size;
} payload_header_t;
#endif

void *orb_workspace_payload_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, size);
#else
    if (size > SIZE_MAX - sizeof(payload_header_t)) return NULL;
    payload_header_t *header = calloc(1, sizeof(*header) + size);
    if (!header) return NULL;
    header->size = size;
    orb_memory_track(ORB_MEMORY_WORKSPACE, size);
    return header + 1;
#endif
}

void orb_workspace_payload_free(void *pointer)
{
#ifdef ESP_PLATFORM
    orb_memory_free(pointer);
#else
    if (!pointer) return;
    payload_header_t *header = (payload_header_t *)pointer - 1;
    orb_memory_untrack(ORB_MEMORY_WORKSPACE, header->size);
    free(header);
#endif
}

void orb_workspace_snapshots_release(workspace_snapshots_t *snapshots)
{
    if (snapshots->model && !--snapshots->model->references) orb_workspace_payload_free(snapshots->model);
    if (snapshots->home && !--snapshots->home->references) orb_workspace_payload_free(snapshots->home);
    *snapshots = (workspace_snapshots_t){0};
}

bool orb_workspace_snapshots_init(workspace_snapshots_t *out, const workspace_snapshots_t *source,
    const orb_model_snapshot_t *model, const orb_home_snapshot_t *home)
{
    *out = (workspace_snapshots_t){0};
    if (source) {
        *out = *source;
        out->model->references++;
        out->home->references++;
        return true;
    }
    out->model = orb_workspace_payload_alloc(sizeof(*out->model));
    if (out->model) out->model->references = 1;
    out->home = orb_workspace_payload_alloc(sizeof(*out->home));
    if (out->home) out->home->references = 1;
    if (!out->model || !out->home || !orb_home_init()) {
        orb_workspace_snapshots_release(out);
        return false;
    }
    if (model && home) {
        out->model->value = *model;
        out->home->value = *home;
    } else orb_home_snapshot(&out->home->value);
    return true;
}

bool orb_workspace_model_refresh(workspace_model_t **current, const orb_model_snapshot_t **view)
{
    workspace_model_t *storage = *current;
    if (storage->value.revision == orb_model_revision()) return false;
    if (storage->references == 1)
        return orb_model_snapshot_if_changed(storage->value.revision, &storage->value);
    workspace_model_t *next = orb_workspace_payload_alloc(sizeof(*next));
    if (!next) return false; /* Retry next poll; the retained generation stays valid. */
    if (!orb_model_snapshot_if_changed(storage->value.revision, &next->value)) {
        orb_workspace_payload_free(next);
        return false;
    }
    next->references = 1;
    storage->references--;
    *current = next;
    *view = &next->value;
    return true;
}

bool orb_workspace_home_refresh(workspace_home_t **current, const orb_home_snapshot_t **view)
{
    workspace_home_t *storage = *current;
    if (storage->value.revision == orb_home_revision()) return false;
    if (storage->references == 1)
        return orb_home_snapshot_if_changed(storage->value.revision, &storage->value);
    workspace_home_t *next = orb_workspace_payload_alloc(sizeof(*next));
    if (!next) return false;
    if (!orb_home_snapshot_if_changed(storage->value.revision, &next->value)) {
        orb_workspace_payload_free(next);
        return false;
    }
    next->references = 1;
    storage->references--;
    *current = next;
    *view = &next->value;
    return true;
}

