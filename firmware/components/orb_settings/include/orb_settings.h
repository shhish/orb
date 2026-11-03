#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ORB_FACE_ORB, ORB_FACE_MERIDIAN, ORB_FACE_ACTIVITY, ORB_FACE_COUNT } orb_face_kind_t;
typedef enum {
    ORB_FACE_CLAUDE, ORB_FACE_CODEX, ORB_FACE_HOME, ORB_FACE_BATTERY,
    ORB_FACE_WEATHER, ORB_FACE_NEXT, ORB_FACE_DATE, ORB_FACE_PROVIDER_COUNT,
} orb_face_provider_t;

/* Preserve the existing durable record byte for byte. Presentation, clocks and
 * private data never belong in this versioned preference record. */
typedef struct {
    uint8_t version, face, colour, slot[3];
    uint8_t meridian_slot[4], activity_slot[3], reserved[3];
} orb_faces_prefs_t;

#define ORB_SETTINGS_FACE_REQUESTS 8

typedef enum {
    ORB_SETTINGS_UNKNOWN,
    ORB_SETTINGS_PENDING,
    ORB_SETTINGS_SAVED,
    ORB_SETTINGS_FAILED,
} orb_settings_result_t;

typedef struct {
    uint64_t revision;
    bool saving, save_error;
} orb_settings_snapshot_t;

/* Main owner only: initialize after orb_fs_mount and before presentation.
 * Unavailable, missing or invalid storage retains defaults without a save error.
 * service processes one copied request in FIFO order, outside the UI lock. */
void orb_settings_init(void);
void orb_settings_service(void);

/* UI-safe, allocation-free reads and commands. Defaults also exist before init
 * so ordinary host presenters do not need a filesystem or service bootstrap. */
bool orb_settings_faces_valid(const orb_faces_prefs_t *prefs);
void orb_settings_faces_current(orb_faces_prefs_t *out);
void orb_settings_snapshot(orb_settings_snapshot_t *out);
/* The supplied external persistence adapter has already succeeded. This updates
 * the shared preference value without enqueueing another storage operation. */
bool orb_settings_faces_accept(const orb_faces_prefs_t *prefs);

/* A nonzero ticket owns a retained result until release. A full queue or invalid
 * record returns zero. Every admitted request is written, without coalescing.
 * Ordinary host builds complete immediately in memory; storage builds complete
 * only from service. Enqueue is never a claim of durable success. */
uint64_t orb_settings_faces_request(const orb_faces_prefs_t *prefs);
orb_settings_result_t orb_settings_faces_result(uint64_t ticket);
/* Releasing a pending ticket detaches its observer; the copied write still runs
 * in FIFO order even if the requesting view has been destroyed. */
void orb_settings_faces_release(uint64_t ticket);

#ifdef __cplusplus
}
#endif
