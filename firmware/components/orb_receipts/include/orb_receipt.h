#pragma once

#include "orb_model.h"
#include "orb_notification_history.h"

/* Review #14: a selected card that has left the model. Expired means its own
 * deadline passed (nothing was approved), not "waiting for a refresh". */
typedef struct {
    const char *title, *detail, *glyph;
} orb_card_gone_t;
orb_card_gone_t orb_card_gone(const orb_model_snapshot_t *model, const char *card, bool ready);

/* Review #24/#25: the one result state model. Pending and successful
 * outcomes end with Done; failed and unknown ones also offer Try again. */
typedef enum {
    ORB_RESULT_PENDING, ORB_RESULT_SENT, ORB_RESULT_DONE, ORB_RESULT_FAILED, ORB_RESULT_UNKNOWN,
} orb_result_outcome_t;
typedef struct {
    const char *title, *detail, *glyph;
    orb_result_outcome_t outcome;
} orb_result_copy_t;
orb_result_copy_t orb_answer_result(bool queued, bool resolved, const orb_answer_result_t *answer);
/* reason is the hub's own explanation, when it gave one. */
orb_result_copy_t orb_send_result(bool sending, bool time, bool timeout, bool error,
                                  orb_send_status_t status, const char *reason);

/* Home events retain their existing receipt vocabulary. The decision is owned
 * by this value; answer borrows the record until history mutation/destruction.
 * Titles/explanations are static. Projection never changes or sends an answer. */
typedef struct {
    const char *title, *explanation, *answer;
    char decision[96];
    bool delivered;
} orb_home_receipt_copy_t;
orb_home_receipt_copy_t orb_home_event_receipt(const orb_notification_record_t *record);

