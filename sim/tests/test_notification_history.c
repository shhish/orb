/* Value history and receipt lifetime, through the production module. */
#include "orb_notification_history.h"
#include "orb_receipt.h"
#include "orb_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

static orb_card_t approval(void)
{
    orb_card_t card = {.id = "history-approval", .session = "history-codex",
        .title = "Run local checks", .kind = ORB_CARD_APPROVAL, .options_count = 2};
    card.options[0] = (orb_option_t){.id = "allow", .label = "Allow", .role = ORB_OPTION_DEFAULT};
    card.options[1] = (orb_option_t){.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL};
    return card;
}

static void receipt_lifetime(orb_model_snapshot_t *model)
{
    *model = (orb_model_snapshot_t){.card_count = 1, .session_count = 1, .online = true, .synced = true};
    model->sessions[0] = (orb_session_t){.id = "history-codex", .agent = ORB_AGENT_CODEX};
    model->cards[0] = approval();
    orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
    orb_notification_arrivals_t arrivals = orb_notification_history_arrivals(history, model);
    CHECK(arrivals.new_card && arrivals.new_generic);
    orb_notification_effects_t effects = orb_notification_history_reconcile(history, model, false, true, true, 100);
    CHECK(!effects.all_clear && effects.approval_cue == ORB_NOTIFICATION_CUE_NONE);
    const orb_notification_record_t *record = orb_notification_history_receipt(history, model->cards[0].id);
    CHECK(record && !record->received_known && record->approval);
    CHECK(!strcmp(record->source, "Codex") && record->options_count == 2);
    orb_notification_history_mark_answer(history, record->id, "allow", "Allow");
    model->cards[0].answered = true;
    effects = orb_notification_history_reconcile(history, model, true, false, true, 200);
    record = orb_notification_history_receipt(history, model->cards[0].id);
    CHECK(record->queued && !record->resolved);
    CHECK(orb_answer_result(record->queued, record->resolved, &record->result).outcome == ORB_RESULT_PENDING);
    CHECK(!effects.all_clear && effects.approval_cue == ORB_NOTIFICATION_CUE_NONE);
    model->last_answer = (orb_answer_result_t){.answer = {.card = "history-approval", .option = "allow"},
        .accepted = true, .delivered = true};
    effects = orb_notification_history_reconcile(history, model, true, false, true, 300);
    record = orb_notification_history_receipt(history, "history-approval");
    CHECK(effects.all_clear && effects.approval_cue == ORB_NOTIFICATION_CUE_APPROVE);
    CHECK(record->resolved && !record->queued && !strcmp(record->decision, "Allow"));
    CHECK(orb_answer_result(record->queued, record->resolved, &record->result).outcome == ORB_RESULT_DONE);
    model->card_count = model->session_count = 0;
    for (unsigned i = 0; i < 4; i++) {
        effects = orb_notification_history_reconcile(history, model, true, false, false, 1000000000u + i);
        CHECK(!effects.all_clear && effects.approval_cue == ORB_NOTIFICATION_CUE_NONE);
        record = orb_notification_history_receipt(history, "history-approval");
        CHECK(record->resolved && record->result.delivered && !strcmp(record->source, "Codex"));
    }
    orb_notification_record_t retained = *record;
    orb_notification_history_clear(history);
    CHECK(orb_notification_history_cleared(history, retained.id));
    CHECK(orb_notification_history_can_undo(history));
    CHECK(!memcmp(&retained, orb_notification_history_receipt(history, retained.id), sizeof(retained)));
    orb_notification_history_undo(history);
    CHECK(!orb_notification_history_cleared(history, retained.id) && !orb_notification_history_can_undo(history));
    orb_notification_history_destroy(history);
}

static void outcomes(orb_model_snapshot_t *model)
{
    for (unsigned outcome = 0; outcome < 5; outcome++) {
        *model = (orb_model_snapshot_t){.card_count = 1, .online = true, .synced = true};
        model->cards[0] = approval();
        orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
        orb_notification_history_reconcile(history, model, true, true, true, 100);
        orb_notification_history_mark_answer(history, model->cards[0].id, "allow", "Allow");
        model->cards[0].answered = true;
        model->last_answer = (orb_answer_result_t){.answer = {.card = "history-approval"},
            .accepted = outcome >= 2, .delivered = outcome >= 3};
        if (outcome == 0) strcpy(model->last_answer.error, "cancelled");
        if (outcome == 1) strcpy(model->last_answer.error, "timeout");
        if (outcome >= 2) strcpy(model->last_answer.answer.option, outcome == 4 ? "deny" : "allow");
        orb_notification_effects_t effects = orb_notification_history_reconcile(history, model, true, false, true, 200);
        const orb_notification_record_t *record = orb_notification_history_receipt(history, "history-approval");
        CHECK(record->resolved && !record->queued);
        CHECK(record->result.accepted == (outcome >= 2) && record->result.delivered == (outcome >= 3));
        CHECK(!strcmp(record->decision, outcome == 4 ? "Deny" : "Allow"));
        orb_result_outcome_t expected = outcome < 2 ? ORB_RESULT_FAILED : outcome == 2 ? ORB_RESULT_UNKNOWN : ORB_RESULT_DONE;
        CHECK(orb_answer_result(record->queued, record->resolved, &record->result).outcome == expected);
        CHECK(effects.all_clear == (outcome >= 3));
        CHECK(effects.approval_cue == (outcome == 3 ? ORB_NOTIFICATION_CUE_APPROVE : ORB_NOTIFICATION_CUE_NONE));
        orb_notification_history_retry(history, "history-approval");
        record = orb_notification_history_receipt(history, "history-approval");
        CHECK(!record->queued && !record->resolved && !record->decision[0] && !record->attempted[0]);
        orb_notification_history_destroy(history);
    }
    *model = (orb_model_snapshot_t){.card_count = 1}; model->cards[0] = approval();
    orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
    orb_notification_history_reconcile(history, model, false, true, true, 10);
    orb_notification_history_mark_answer(history, model->cards[0].id, "deny", "Deny");
    model->cards[0].answered = true;
    model->last_answer = (orb_answer_result_t){.answer = {.card = "history-approval", .option = "deny"},
        .accepted = true, .delivered = true};
    CHECK(orb_notification_history_reconcile(history, model, true, false, true, 20).approval_cue == ORB_NOTIFICATION_CUE_DENY);
    orb_notification_history_destroy(history);
}

static void bounded_history_and_preview(orb_model_snapshot_t *model)
{
    *model = (orb_model_snapshot_t){.card_count = 1};
    orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
    for (unsigned i = 0; i <= ORB_NOTIFICATION_HISTORY_COUNT; i++) {
        model->cards[0] = (orb_card_t){.kind = ORB_CARD_ATTENTION};
        snprintf(model->cards[0].id, sizeof(model->cards[0].id), "event-%u", i);
        snprintf(model->cards[0].title, sizeof(model->cards[0].title), "Event %u", i);
        orb_notification_history_reconcile(history, model, i > 0, true, true, 100 + i);
    }
    CHECK(orb_notification_history_count(history) == ORB_NOTIFICATION_HISTORY_COUNT);
    CHECK(!orb_notification_history_find(history, "event-0"));
    CHECK(!strcmp(orb_notification_history_at(history, 0)->id, "event-1"));
    CHECK(!orb_notification_history_at(history, ORB_NOTIFICATION_HISTORY_COUNT));
    const orb_notification_record_t *last = orb_notification_history_at(history, ORB_NOTIFICATION_HISTORY_COUNT - 1);
    CHECK(last && last->received_known && last->received_at == 116);
    orb_notification_history_select(history, 9);
    orb_notification_history_remember_detail(history, "event-16");
    orb_notification_history_dismiss(history, "already-hidden");
    orb_notification_history_clear(history);
    CHECK(orb_notification_history_cleared(history, last->id));
    orb_notification_history_t *preview = orb_notification_history_create(); CHECK(preview);
    orb_notification_history_clone_preview(preview, history);
    CHECK(orb_notification_history_count(preview) == ORB_NOTIFICATION_HISTORY_COUNT);
    CHECK(!orb_notification_history_cleared(preview, last->id));
    CHECK(!orb_notification_history_can_undo(preview) && !orb_notification_history_selected(preview));
    CHECK(!orb_notification_history_detail(preview)[0]);
    CHECK(!strcmp(orb_notification_history_detail(history), "event-16"));
    CHECK(!memcmp(last, orb_notification_history_at(preview, ORB_NOTIFICATION_HISTORY_COUNT - 1), sizeof(*last)));
    orb_notification_history_undo(history);
    CHECK(orb_notification_history_cleared(history, "already-hidden"));
    CHECK(!orb_notification_history_cleared(history, last->id));
    CHECK(orb_notification_history_selected(history) == 9);
    orb_notification_history_mark_doorbell_shown(history, last->id);
    CHECK(orb_notification_history_find(history, last->id)->doorbell_shown);
    CHECK(!orb_notification_history_find(preview, last->id)->doorbell_shown);
    orb_notification_history_fixture_age(history, last->id, false, 3000, 4000);
    CHECK(!orb_notification_history_find(history, last->id)->received_known);
    CHECK(orb_notification_history_find(preview, last->id)->received_known);
    orb_notification_history_destroy(history);
    CHECK(orb_notification_history_find(preview, "event-16"));
    orb_notification_history_destroy(preview);
}

static void arrivals_sources_and_seed(orb_model_snapshot_t *model)
{
    *model = (orb_model_snapshot_t){.card_count = 1};
    model->cards[0] = (orb_card_t){.id = "home-doorbell-fixture", .title = "Front Door", .kind = ORB_CARD_ATTENTION};
    orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
    orb_notification_arrivals_t arrivals = orb_notification_history_arrivals(history, model);
    CHECK(arrivals.new_card && !arrivals.new_generic);
    orb_notification_history_seed(history, model);
    const orb_notification_record_t *record = orb_notification_history_at(history, 0);
    CHECK(record && !record->received_known && !record->approval && !record->options_count);
    CHECK(!strcmp(record->source, "Home") && !strcmp(record->state, "Home · Update"));
    CHECK(!orb_notification_history_arrivals(history, model).new_card);
    CHECK(!strcmp(orb_notification_source("bad source")->name, "System"));
    model->cards[0] = (orb_card_t){.id = "generic", .session = "gone-provider"};
    orb_notification_record_t retained = {.source = "Claude"};
    CHECK(!strcmp(orb_notification_card_source(model, &model->cards[0], &retained)->name, "Claude"));
    model->session_count = 1;
    model->sessions[0] = (orb_session_t){.id = "gone-provider", .agent = ORB_AGENT_DOT};
    CHECK(!strcmp(orb_notification_card_source(model, &model->cards[0], &retained)->name, "Dot"));
    model->session_count = 0;
    strcpy(model->cards[0].id, "system-update-received");
    CHECK(!strcmp(orb_notification_card_source(model, &model->cards[0], NULL)->name, "Updates"));
    orb_notification_history_destroy(history);
}

static void presentation_never_sends(orb_model_snapshot_t *model)
{
    CHECK(orb_model_init()); orb_model_reset(); orb_model_set_connection(true, true);
    orb_card_t card = approval(); CHECK(orb_model_upsert_card(&card));
    CHECK(orb_model_snapshot(model));
    orb_notification_history_t *history = orb_notification_history_create(); CHECK(history);
    orb_notification_history_reconcile(history, model, true, true, true, 100);
    orb_answer_t answer = {.card = "history-approval", .option = "allow"};
    CHECK(orb_model_answer(&answer));
    size_t queued = orb_model_pending_answers(); CHECK(queued == 1);
    uint64_t revision = orb_model_revision();
    orb_notification_history_mark_answer(history, card.id, answer.option, "Allow");
    orb_notification_history_clear(history); orb_notification_history_undo(history);
    orb_notification_history_dismiss(history, card.id);
    orb_notification_history_retry(history, card.id);
    for (unsigned i = 0; i < 3; i++) orb_notification_history_reconcile(history, model, true, false, true, UINT32_MAX - i);
    CHECK(orb_model_pending_answers() == queued && orb_model_revision() == revision);
    orb_answer_t delivered; CHECK(orb_model_take_answer(&delivered));
    CHECK(!strcmp(delivered.card, answer.card) && !strcmp(delivered.option, answer.option));
    CHECK(!orb_model_take_answer(&delivered));
    orb_notification_history_destroy(history); orb_model_reset();
}

static void home_event_projection(void)
{
    orb_home_receipt_copy_t copy = orb_home_event_receipt(NULL);
    CHECK(!strcmp(copy.title, "Queued") && !strcmp(copy.decision, "Reply · sending"));
    CHECK(!copy.answer && !copy.delivered && !strcmp(copy.explanation, "Waiting for your Mac."));
    orb_notification_record_t record = {.decision = "Dismiss",
        .result = {.accepted = true, .delivered = true, .answer = {.text = "Retained answer"}}};
    /* Stale result fields cannot turn an unresolved history entry into a
     * delivered receipt; the existing projection keys only on resolved. */
    copy = orb_home_event_receipt(&record);
    CHECK(!strcmp(copy.title, "Queued") && !strcmp(copy.decision, "Dismiss · sending"));
    CHECK(!copy.answer && !copy.delivered);
    record.resolved = true;
    copy = orb_home_event_receipt(&record);
    CHECK(copy.answer == record.result.answer.text && copy.delivered);
    CHECK(!strcmp(copy.title, "Sent") && !strcmp(copy.answer, "Retained answer"));
    strcpy(record.decision, "Later label");
    CHECK(!strcmp(copy.decision, "Dismiss")); /* The bounded decision is a value. */
    strcpy(record.result.error, "cancelled");
    copy = orb_home_event_receipt(&record);
    /* Keep the legacy precedence even for contradictory input facts. */
    CHECK(!strcmp(copy.title, "Sent") && copy.delivered);
    CHECK(!strcmp(copy.explanation, "This closed before your answer was sent."));
}

int main(void)
{
    CHECK(orb_model_init());
    orb_memory_stats_t before, after;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &before);
    orb_model_snapshot_t *model = calloc(1, sizeof(*model)); CHECK(model);
    receipt_lifetime(model); outcomes(model); bounded_history_and_preview(model);
    arrivals_sources_and_seed(model); presentation_never_sends(model); home_event_projection();
    free(model);
    orb_notification_history_destroy(NULL);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &after);
    CHECK(after.used == before.used && after.rejections == before.rejections);
    printf("Notification history: %u checks passed\n", s_checks);
    return 0;
}
