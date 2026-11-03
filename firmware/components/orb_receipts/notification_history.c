#include "orb_notification_history.h"
#include "orb_receipt.h"

#include "orb_fonts.h"
#include "orb_memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct orb_notification_history {
    orb_notification_record_t recent[ORB_NOTIFICATION_HISTORY_COUNT];
    unsigned recent_count;
    char cleared[ORB_NOTIFICATION_HISTORY_COUNT][ORB_MODEL_CARD_ID_MAX + 1];
    unsigned cleared_count, undo_cleared_count;
    char undo_cleared[ORB_NOTIFICATION_HISTORY_COUNT][ORB_MODEL_CARD_ID_MAX + 1];
    bool can_undo_clear;
    unsigned selected;
    char detail[ORB_MODEL_CARD_ID_MAX + 1];
};

size_t orb_notification_history_bytes(void)
{
    return sizeof(orb_notification_history_t);
}

orb_notification_history_t *orb_notification_history_create(void)
{
#ifdef ESP_PLATFORM
    return orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(orb_notification_history_t));
#else
    /* Host fixtures retain the former unbudgeted workspace payload behavior;
     * tracking reports its exact owner bytes without inventing board admission. */
    orb_notification_history_t *history = calloc(1, sizeof(*history));
    if (history) orb_memory_track(ORB_MEMORY_WORKSPACE, sizeof(*history));
    return history;
#endif
}

void orb_notification_history_destroy(orb_notification_history_t *history)
{
#ifdef ESP_PLATFORM
    orb_memory_free(history);
#else
    if (history) orb_memory_untrack(ORB_MEMORY_WORKSPACE, sizeof(*history));
    free(history);
#endif
}

unsigned orb_notification_history_count(const orb_notification_history_t *history)
{ return history ? history->recent_count : 0; }

const orb_notification_record_t *orb_notification_history_at(const orb_notification_history_t *history, unsigned index)
{ return history && index < history->recent_count ? &history->recent[index] : NULL; }

static orb_notification_record_t *find(orb_notification_history_t *history, const char *id)
{
    if (!history || !id) return NULL;
    for (unsigned i = 0; i < history->recent_count; i++)
        if (!strcmp(history->recent[i].id, id)) return &history->recent[i];
    return NULL;
}

const orb_notification_record_t *orb_notification_history_find(const orb_notification_history_t *history, const char *id)
{
    if (!history || !id) return NULL;
    for (unsigned i = 0; i < history->recent_count; i++)
        if (!strcmp(history->recent[i].id, id)) return &history->recent[i];
    return NULL;
}

const orb_notification_record_t *orb_notification_history_receipt(const orb_notification_history_t *history, const char *id)
{ return orb_notification_history_find(history, id); }

bool orb_notification_history_cleared(const orb_notification_history_t *history, const char *id)
{
    if (!history || !id) return false;
    for (unsigned i = 0; i < history->cleared_count; i++)
        if (!strcmp(history->cleared[i], id)) return true;
    return false;
}

void orb_notification_history_dismiss(orb_notification_history_t *history, const char *id)
{
    if (!history) return;
    if (history->cleared_count == ORB_NOTIFICATION_HISTORY_COUNT) {
        memmove(history->cleared, history->cleared + 1,
            (ORB_NOTIFICATION_HISTORY_COUNT - 1) * sizeof(*history->cleared));
        history->cleared_count--;
    }
    if (id && id[0]) snprintf(history->cleared[history->cleared_count++], sizeof(*history->cleared), "%s", id);
}

void orb_notification_history_clear(orb_notification_history_t *history)
{
    if (!history) return;
    memcpy(history->undo_cleared, history->cleared, sizeof(history->cleared));
    history->undo_cleared_count = history->cleared_count;
    history->can_undo_clear = true;
    for (unsigned i = 0; i < history->recent_count; i++)
        orb_notification_history_dismiss(history, history->recent[i].id);
}

void orb_notification_history_undo(orb_notification_history_t *history)
{
    if (!history) return;
    memcpy(history->cleared, history->undo_cleared, sizeof(history->cleared));
    history->cleared_count = history->undo_cleared_count;
    history->can_undo_clear = false;
}

bool orb_notification_history_can_undo(const orb_notification_history_t *history)
{ return history && history->can_undo_clear; }

unsigned orb_notification_history_selected(const orb_notification_history_t *history)
{ return history ? history->selected : 0; }

void orb_notification_history_select(orb_notification_history_t *history, unsigned index)
{ if (history) history->selected = index; }

void orb_notification_history_remember_detail(orb_notification_history_t *history, const char *id)
{
    if (history) snprintf(history->detail, sizeof(history->detail), "%s", id);
}

const char *orb_notification_history_detail(const orb_notification_history_t *history)
{
    return history ? history->detail : "";
}

void orb_notification_history_mark_answer(orb_notification_history_t *history, const char *id,
    const char *option, const char *label)
{
    orb_notification_record_t *record = find(history, id);
    if (!record) return;
    record->queued = true;
    snprintf(record->attempted, sizeof(record->attempted), "%s", option);
    snprintf(record->decision, sizeof(record->decision), "%s", label);
}

void orb_notification_history_retry(orb_notification_history_t *history, const char *id)
{
    orb_notification_record_t *record = find(history, id);
    if (!record) return;
    record->queued = record->resolved = false;
    memset(&record->result, 0, sizeof(record->result));
    record->decision[0] = record->attempted[0] = '\0';
}

void orb_notification_history_mark_doorbell_shown(orb_notification_history_t *history, const char *id)
{
    orb_notification_record_t *record = find(history, id);
    if (record) record->doorbell_shown = true;
}

/* Source identities are content; never infer ownership from a human title. */
static const orb_notification_source_t s_notification_sources[] = {
    {"System", ORB_ICON_BELL, ORB_NOTIFICATION_BLUE},
    {"Home", ORB_ICON_HOUSE, ORB_NOTIFICATION_YELLOW},
    {"Claude", ORB_ICON_PROMPT, ORB_NOTIFICATION_ORANGE},
    {"Codex", ORB_ICON_CODE, ORB_NOTIFICATION_BLUE},
    {"Dot", ORB_ICON_CHAT, ORB_NOTIFICATION_INDIGO},
    {"Updates", ORB_ICON_DOWNLOAD, ORB_NOTIFICATION_BLUE},
};

const orb_notification_source_t *orb_notification_source(const char *name)
{
    for (unsigned i = 0; i < sizeof(s_notification_sources) / sizeof(s_notification_sources[0]); i++)
        if (name && !strcmp(name, s_notification_sources[i].name)) return &s_notification_sources[i];
    return &s_notification_sources[0];
}

const orb_notification_source_t *orb_notification_card_source(const orb_model_snapshot_t *model,
    const orb_card_t *card, const orb_notification_record_t *recent)
{
    if (card) for (unsigned i = 0; i < model->session_count; i++)
        if (!strcmp(card->session, model->sessions[i].id))
            return orb_notification_source(model->sessions[i].agent == ORB_AGENT_CLAUDE ? "Claude" :
                model->sessions[i].agent == ORB_AGENT_CODEX ? "Codex" : "Dot");
    if (card && !strncmp(card->id, "home-", 5)) return orb_notification_source("Home");
    if (card && (!strncmp(card->id, "update-", 7) || !strncmp(card->id, "system-update-", 14)))
        return orb_notification_source("Updates");
    /* Focus snapshots can evict a session before its card/history disappears. */
    return orb_notification_source(recent ? recent->source : NULL);
}

void orb_notification_history_seed(orb_notification_history_t *history, const orb_model_snapshot_t *model)
{
    if (!history || !model) return;
    for (unsigned i = 0; i < model->card_count && i < ORB_NOTIFICATION_HISTORY_COUNT; i++) {
        const orb_card_t *card = &model->cards[i];
        orb_notification_record_t *record = &history->recent[history->recent_count++];
        snprintf(record->id, sizeof(record->id), "%s", card->id);
        snprintf(record->title, sizeof(record->title), "%s", card->title);
        const orb_notification_source_t *source = orb_notification_card_source(model, card, NULL);
        snprintf(record->source, sizeof(record->source), "%s", source->name);
        snprintf(record->state, sizeof(record->state), "%s · Update", source->name);
    }
}

void orb_notification_history_fixture_age(orb_notification_history_t *history, const char *id,
    bool known, unsigned age_ms, uint32_t now)
{
    orb_notification_record_t *record = find(history, id);
    if (!record) return;
    record->received_at = now - age_ms;
    record->received_known = known;
}

void orb_notification_history_clone_preview(orb_notification_history_t *destination,
    const orb_notification_history_t *source)
{
    if (!destination || !source) return;
    memcpy(destination->recent, source->recent, sizeof(source->recent));
    destination->recent_count = source->recent_count;
}

orb_notification_arrivals_t orb_notification_history_arrivals(const orb_notification_history_t *history,
    const orb_model_snapshot_t *model)
{
    orb_notification_arrivals_t arrivals = {0};
    if (!history || !model) return arrivals;
    for (unsigned i = 0; i < model->card_count; i++)
        if (!orb_notification_history_find(history, model->cards[i].id)) {
            arrivals.new_card = true;
            const orb_card_t *card = &model->cards[i];
            arrivals.new_generic |= !(card->kind == ORB_CARD_ATTENTION && !strncmp(card->id, "home-doorbell-", 14));
        }
    return arrivals;
}

static void reconcile_cards(orb_notification_history_t *history, const orb_model_snapshot_t *model,
    bool had_history, bool new_card, bool was_synced, uint32_t now)
{
    for (unsigned i = 0; i < history->recent_count; i++) {
        bool found = false;
        for (size_t j = 0; j < model->card_count; j++)
            if (!strcmp(history->recent[i].id, model->cards[j].id)) found = true;
        if (!found && !history->recent[i].queued && !history->recent[i].resolved)
            snprintf(history->recent[i].state, sizeof(history->recent[i].state), "%s",
                orb_card_gone(model, history->recent[i].id, true).detail);
    }
    for (size_t i = 0; i < model->card_count; i++) {
        const orb_card_t *card = &model->cards[i];
        bool cleared = orb_notification_history_cleared(history, card->id);
        /* Clearing presentation never stops updating an existing receipt. */
        if (cleared && !find(history, card->id)) continue;
        unsigned j;
        for (j = 0; j < history->recent_count; j++) if (!strcmp(history->recent[j].id, card->id)) break;
        if (j == ORB_NOTIFICATION_HISTORY_COUNT) {
            memmove(history->recent, history->recent + 1,
                (ORB_NOTIFICATION_HISTORY_COUNT - 1) * sizeof(*history->recent));
            j--; history->recent_count--;
        }
        if (j == history->recent_count) {
            memset(&history->recent[j], 0, sizeof(history->recent[j]));
            history->recent_count++;
        }
        orb_notification_record_t *record = &history->recent[j];
        if (!record->id[0]) {
            record->received_at = now;
            record->received_known = had_history && new_card && was_synced;
        }
        snprintf(record->id, sizeof(record->id), "%s", card->id);
        snprintf(record->title, sizeof(record->title), "%s", card->title);
        record->approval = card->kind == ORB_CARD_APPROVAL;
        record->options_count = card->options_count;
        memcpy(record->options, card->options, sizeof(card->options));
        record->queued |= card->answered;
        const char *source = orb_notification_card_source(model, card, record)->name;
        snprintf(record->source, sizeof(record->source), "%s", source);
        snprintf(record->state, sizeof(record->state), "%s · %s", source,
            card->answered ? "Queued" : card->kind == ORB_CARD_APPROVAL ? "Decision needed" :
            card->kind == ORB_CARD_CHOICE ? "Choose next step" : card->kind == ORB_CARD_MESSAGE ? "New message" : "Update");
    }
}

static unsigned waiting_cards(const orb_model_snapshot_t *model)
{
    unsigned count = 0;
    for (unsigned i = 0; i < model->card_count; i++) count += !model->cards[i].answered;
    return count;
}

static orb_notification_effects_t reconcile_result(orb_notification_history_t *history,
    const orb_model_snapshot_t *model)
{
    orb_notification_effects_t effects = {0};
    const orb_answer_result_t *result = &model->last_answer;
    orb_notification_record_t *record = find(history, result->answer.card);
    if (!record || !result->answer.card[0]) return effects;
    bool first_result = !record->resolved;
    effects.all_clear = first_result && record->queued && result->accepted && result->delivered && !waiting_cards(model);
    record->resolved = true; record->queued = false; record->result = *result;
    if (first_result && record->approval && result->accepted && result->delivered &&
        record->attempted[0] && !strcmp(record->attempted, result->answer.option)) {
        bool denied = !strcmp(result->answer.option, "deny");
        for (unsigned i = 0; i < record->options_count; i++)
            if (!strcmp(record->options[i].id, result->answer.option) && record->options[i].role == ORB_OPTION_CANCEL)
                denied = true;
        effects.approval_cue = denied ? ORB_NOTIFICATION_CUE_DENY : ORB_NOTIFICATION_CUE_APPROVE;
    }
    /* Failed answers have no recorded option: keep the attempted label until
     * an actual winning answer supplies its own option or reply text. */
    if (result->answer.text[0] || result->answer.option[0]) {
        snprintf(record->decision, sizeof(record->decision), "%s", result->answer.text[0] ? "Reply" : result->answer.option);
        for (unsigned i = 0; i < record->options_count; i++)
            if (!strcmp(record->options[i].id, result->answer.option))
                snprintf(record->decision, sizeof(record->decision), "%s", record->options[i].label);
    }
    snprintf(record->state, sizeof(record->state), "%s · %s",
        result->accepted ? (result->delivered ? "Sent" : "Recorded, not delivered") : "Not sent", record->decision);
    return effects;
}

orb_notification_effects_t orb_notification_history_reconcile(orb_notification_history_t *history,
    const orb_model_snapshot_t *model, bool had_history, bool new_card, bool was_synced, uint32_t now)
{
    if (!history || !model) return (orb_notification_effects_t){0};
    reconcile_cards(history, model, had_history, new_card, was_synced, now);
    return reconcile_result(history, model);
}
