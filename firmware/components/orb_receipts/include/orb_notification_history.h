#pragma once

#include "orb_model.h"

#define ORB_NOTIFICATION_HISTORY_COUNT 16

typedef struct orb_notification_history orb_notification_history_t;

/* Records own their values, including the winning answer after card removal.
 * Borrowed records remain valid only until the next history mutation or destroy. */
typedef struct {
    char id[ORB_MODEL_CARD_ID_MAX + 1], title[65], state[96], decision[33], attempted[33], source[16];
    uint32_t received_at;
    bool received_known;
    orb_option_t options[ORB_MODEL_MAX_OPTIONS];
    unsigned options_count;
    bool queued, resolved, approval, doorbell_shown;
    orb_answer_result_t result;
} orb_notification_record_t;

typedef enum {
    ORB_NOTIFICATION_BLUE, ORB_NOTIFICATION_ORANGE,
    ORB_NOTIFICATION_YELLOW, ORB_NOTIFICATION_INDIGO,
} orb_notification_color_t;

typedef struct {
    const char *name, *glyph;
    orb_notification_color_t color;
} orb_notification_source_t;

typedef struct { bool new_card, new_generic; } orb_notification_arrivals_t;
typedef enum {
    ORB_NOTIFICATION_CUE_NONE, ORB_NOTIFICATION_CUE_APPROVE, ORB_NOTIFICATION_CUE_DENY,
} orb_notification_cue_t;
typedef struct {
    bool all_clear;
    orb_notification_cue_t approval_cue;
} orb_notification_effects_t;

/* Exact private state reservation, excluding allocator metadata and LVGL descendants. */
size_t orb_notification_history_bytes(void);
orb_notification_history_t *orb_notification_history_create(void);
void orb_notification_history_destroy(orb_notification_history_t *history);
unsigned orb_notification_history_count(const orb_notification_history_t *history);
const orb_notification_record_t *orb_notification_history_at(const orb_notification_history_t *history, unsigned index);
const orb_notification_record_t *orb_notification_history_find(const orb_notification_history_t *history, const char *id);
const orb_notification_record_t *orb_notification_history_receipt(const orb_notification_history_t *history, const char *id);
bool orb_notification_history_cleared(const orb_notification_history_t *history, const char *id);
void orb_notification_history_clear(orb_notification_history_t *history);
void orb_notification_history_undo(orb_notification_history_t *history);
bool orb_notification_history_can_undo(const orb_notification_history_t *history);
void orb_notification_history_dismiss(orb_notification_history_t *history, const char *id);
unsigned orb_notification_history_selected(const orb_notification_history_t *history);
void orb_notification_history_select(orb_notification_history_t *history, unsigned index);
/* The detail route remembers its selected identity across covering layers.
 * This is a value, never an answer or pending command, and previews omit it. */
void orb_notification_history_remember_detail(orb_notification_history_t *history, const char *id);
const char *orb_notification_history_detail(const orb_notification_history_t *history);
void orb_notification_history_mark_answer(orb_notification_history_t *history, const char *id,
    const char *option, const char *label);
void orb_notification_history_retry(orb_notification_history_t *history, const char *id);
void orb_notification_history_mark_doorbell_shown(orb_notification_history_t *history, const char *id);
/* Capture fixtures seed an empty history without replaying live reconciliation. */
void orb_notification_history_seed(orb_notification_history_t *history, const orb_model_snapshot_t *model);
void orb_notification_history_fixture_age(orb_notification_history_t *history, const char *id,
    bool known, unsigned age_ms, uint32_t now);
/* Legacy neighbour previews copy only records/count, retaining their original
 * unmasked presentation. No cleared/undo/selection state or model pointer moves. */
void orb_notification_history_clone_preview(orb_notification_history_t *destination,
    const orb_notification_history_t *source);
orb_notification_arrivals_t orb_notification_history_arrivals(const orb_notification_history_t *history,
    const orb_model_snapshot_t *model);
/* Correlation emits presentation effects once; it never sends, retries, cues or
 * schedules navigation. Resolved receipts live until explicit UI departure and
 * remain in bounded history until FIFO eviction or owner destruction. */
orb_notification_effects_t orb_notification_history_reconcile(orb_notification_history_t *history,
    const orb_model_snapshot_t *model, bool had_history, bool new_card, bool was_synced, uint32_t now);
const orb_notification_source_t *orb_notification_source(const char *name);
const orb_notification_source_t *orb_notification_card_source(const orb_model_snapshot_t *model,
    const orb_card_t *card, const orb_notification_record_t *recent);
