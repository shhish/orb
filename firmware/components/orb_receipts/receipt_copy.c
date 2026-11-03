/* Shared receipt projection: correlation facts become copy, never commands. */
#include "orb_receipt.h"

#include <stdio.h>
#include <string.h>
#include "orb_fonts.h"

orb_card_gone_t orb_card_gone(const orb_model_snapshot_t *model, const char *card, bool ready)
{
    if (card[0] && !strcmp(model->last_cancelled, card))
        return (orb_card_gone_t){"Canceled", "The session closed this request. No action is needed.",
                                 ORB_ICON_X_CIRCLE};
    if (card[0] && !strcmp(model->last_expired, card))
        return (orb_card_gone_t){"Expired", "This request timed out. Nothing was approved.", ORB_ICON_HOURGLASS};
    return ready ? (orb_card_gone_t){"Loading…", "Refreshing cards from your Mac", ORB_ICON_ARROWS_CLOCKWISE} :
        (orb_card_gone_t){"Mac offline", "Reconnect your Mac to refresh cards", ORB_ICON_WIFI_SLASH};
}

/* Glossary §2.1: one title, symbol and outcome per delivery state, shared by
 * card answers, session messages and local Time saves. */
static orb_result_copy_t result(const char *title, const char *detail, orb_result_outcome_t outcome)
{
    static const char *const s_glyphs[] = {
        [ORB_RESULT_PENDING] = ORB_ICON_CLOCK, [ORB_RESULT_SENT] = ORB_ICON_CHECK,
        [ORB_RESULT_DONE] = ORB_ICON_CHECK, [ORB_RESULT_FAILED] = ORB_ICON_WARNING,
        [ORB_RESULT_UNKNOWN] = ORB_ICON_QUESTION,
    };
    return (orb_result_copy_t){title, detail, s_glyphs[outcome], outcome};
}

orb_result_copy_t orb_answer_result(bool queued, bool resolved, const orb_answer_result_t *answer)
{
    if (!queued && !resolved)
        return result("Answered", "This was answered. Check the session for the result", ORB_RESULT_DONE);
    if (!resolved) return result("Sending…", "Waiting for your Mac", ORB_RESULT_PENDING);
    if (!strcmp(answer->error, "cancelled"))
        return result("Not sent", "The request closed before your answer was sent", ORB_RESULT_FAILED);
    if (!answer->accepted)
        return result("Not sent", "Your Mac didn’t take this answer", ORB_RESULT_FAILED);
    return answer->delivered ? result("Delivered", "Your answer reached the session", ORB_RESULT_DONE) :
        result("Sent", "Your Mac has it, but the session didn’t. Answer there.", ORB_RESULT_UNKNOWN);
}

orb_result_copy_t orb_send_result(bool sending, bool time, bool timeout, bool error,
                                  orb_send_status_t status, const char *reason)
{
    if (time) return sending ? result("Saving…", "", ORB_RESULT_PENDING) :
        status == ORB_SEND_DELIVERED ? result("Saved", "Find it in Time", ORB_RESULT_DONE) :
        result("Not saved", "Orb couldn’t save the reminder", ORB_RESULT_FAILED);
    if (sending) return result("Sending…", "", ORB_RESULT_PENDING);
    const char *detail = reason && reason[0] ? reason : NULL;
    if (timeout) return result("Unknown", "Your Mac didn’t confirm it. Check the session first.",
                               ORB_RESULT_UNKNOWN);
    if (error || status == ORB_SEND_FAILED)
        return result("Not sent", detail ? detail : "Your Mac didn’t take this message", ORB_RESULT_FAILED);
    if (status == ORB_SEND_QUEUED)
        return result("Sent", detail ? detail : "Waiting in the session inbox", ORB_RESULT_SENT);
    if (status == ORB_SEND_DELIVERED)
        return result("Delivered", detail ? detail : "The session received it", ORB_RESULT_DONE);
    return result("Unknown", "Your Mac didn’t confirm it. Check the session first.", ORB_RESULT_UNKNOWN);
}

orb_home_receipt_copy_t orb_home_event_receipt(const orb_notification_record_t *record)
{
    bool resolved = record && record->resolved;
    orb_home_receipt_copy_t copy = {
        .title = !resolved ? "Queued" : record->result.accepted ?
            (record->result.delivered ? "Sent" : "Recorded") : "Not sent",
        .explanation = !resolved ? "Waiting for your Mac." :
            !strcmp(record->result.error, "cancelled") ? "This closed before your answer was sent." :
            !record->result.accepted ? "Your Mac didn't take this answer. Check the session." :
            record->result.delivered ? "Answer delivered." :
            "Saved on your Mac, but the session didn't get it. Answer there.",
        .answer = resolved ? record->result.answer.text : NULL,
        .delivered = resolved && record->result.accepted && record->result.delivered,
    };
    snprintf(copy.decision, sizeof(copy.decision), "%s%s",
        record && record->decision[0] ? record->decision : "Reply", !resolved ? " · sending" : "");
    return copy;
}

