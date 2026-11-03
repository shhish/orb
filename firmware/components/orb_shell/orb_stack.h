#pragma once

#include "orb_home.h"
#include "orb_model.h"
#include "orb_time.h"

/* A derived, allocation-free order: no placeholder telemetry is retained. */
#define ORB_STACK_MAX (ORB_MODEL_MAX_CARDS + 7u)
typedef enum {
    ORB_STACK_NONE, ORB_STACK_CARD, ORB_STACK_TIME_ACTIVE, ORB_STACK_TIME_ALARM,
    ORB_STACK_HOME, ORB_STACK_NOWPLAYING, ORB_STACK_USAGE, ORB_STACK_TIME_FLOOR,
} orb_stack_kind_t;
typedef struct {
    orb_stack_kind_t kind;
    unsigned index; /* Card index, or the relevant provider's usage window. */
} orb_stack_entry_t;

bool orb_stack_time_active(const orb_time_snapshot_t *time);
bool orb_stack_alarm_today(const orb_time_snapshot_t *time);
/* clock_hour < 0 means unavailable. date_known refers to the shell's supplied
 * local date; time.clock_valid is the wall-clock authority for reset epochs. */
unsigned orb_stack_build(const orb_model_snapshot_t *model, const orb_time_snapshot_t *time,
                         const orb_home_snapshot_t *home, int clock_hour, bool date_known,
                         orb_stack_entry_t *entries, unsigned capacity);

/* Compact metadata remains text, with source identity supplied by the view. */
void orb_stack_time_caption(const orb_time_snapshot_t *time, char *text, size_t capacity, bool compact);
void orb_stack_preview_copy(const orb_model_snapshot_t *model, const orb_time_snapshot_t *time,
                            const char *room_name, orb_stack_entry_t entry, char *text, size_t capacity);
