#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ORB_NAV_ADMISSION_NONE,
    ORB_NAV_ADMISSION_BUSY,
    ORB_NAV_ADMISSION_DRAIN,
    ORB_NAV_ADMISSION_WORKSPACE_HOLD,
    ORB_NAV_ADMISSION_SOURCE_CAPTURE,
    ORB_NAV_ADMISSION_TARGET_CAPTURE,
    ORB_NAV_ADMISSION_COMPOSITOR_BEGIN,
} orb_nav_admission_reason_t;

typedef struct {
    uint32_t sequence;
    orb_nav_admission_reason_t reason;
    size_t psram_free, psram_largest;
} orb_nav_admission_receipt_t;

const char *orb_nav_admission_name(orb_nav_admission_reason_t reason);
/* Sticky until the next failure, including across benchmark restoration. All
 * callers hold the UI lock; no heap allocation or log in the admission path. */
void orb_nav_admission_record(orb_nav_admission_reason_t reason, size_t free_bytes, size_t largest);
void orb_nav_admission_fail(orb_nav_admission_reason_t reason);
orb_nav_admission_receipt_t orb_nav_admission_last(void);
