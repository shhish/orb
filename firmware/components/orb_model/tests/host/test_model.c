#include "orb_model.h"
#include "orb_weather_icons.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
static orb_model_snapshot_t s_view;
#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static orb_session_t session(const char *id)
{
    orb_session_t value = {.agent = ORB_AGENT_CODEX, .state = ORB_SESSION_BUSY};
    snprintf(value.id, sizeof(value.id), "%s", id);
    return value;
}

static void voice_notify(void *context)
{
    unsigned *count = context;
    ++*count;
    /* Re-entering the model proves the callback is outside its mutex. */
    orb_voice_state_t voice;
    CHECK(orb_model_get_voice(&voice));
}

static orb_card_t card(const char *id)
{
    orb_card_t value = {.kind = ORB_CARD_CHOICE, .priority = ORB_PRIORITY_NORMAL,
                        .options_count = 1,
                        .options = {{.id = "next", .label = "Continue"}}};
    snprintf(value.id, sizeof(value.id), "%s", id);
    return value;
}

static void test_init_and_revisions(void)
{
    CHECK(orb_model_revision() == 0);
    CHECK(!orb_model_snapshot(&s_view));
    CHECK(!orb_model_upsert_session(NULL));
    CHECK(orb_model_init());
    CHECK(orb_model_init());
    CHECK(orb_model_revision() == 1);
    CHECK(!orb_model_snapshot(NULL));
    CHECK(orb_model_snapshot_if_changed(0, &s_view));
    CHECK(s_view.revision == 1 && s_view.session_count == 0 && s_view.card_count == 0);
    s_view.session_count = 99;
    CHECK(!orb_model_snapshot_if_changed(1, &s_view));
    CHECK(s_view.session_count == 99); /* Unchanged means no copy. */
    orb_session_t value = session("one");
    value.updated = 10;
    CHECK(orb_model_upsert_session(&value));
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_upsert_session(&value));
    CHECK(orb_model_revision() == revision);
    value.updated = 9;
    strcpy(value.title, "stale");
    CHECK(!orb_model_upsert_session(&value));
    CHECK(orb_model_revision() == revision);
    CHECK(orb_model_apply_session(&value));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.sessions[0].updated == 9); /* Ordered wire data follows clock correction. */
    revision = orb_model_revision();
    value.updated = 11;
    CHECK(orb_model_upsert_session(&value));
    CHECK(orb_model_revision() == revision + 1);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!strcmp(s_view.sessions[0].title, "stale"));
    s_view.sessions[0].title[0] = 'X';
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.sessions[0].title[0] == 's'); /* Copy cannot mutate store. */
    CHECK(!orb_model_remove_session("missing"));
    CHECK(orb_model_remove_session("one"));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 0 && !s_view.sessions[0].id[0]);
    CHECK(!orb_model_remove_session("one"));
}

static void test_session_bounds(void)
{
    orb_model_reset();
    char id[49];
    for (unsigned i = 0; i < ORB_MODEL_MAX_SESSIONS; ++i) {
        snprintf(id, sizeof(id), "session-%u", i);
        orb_session_t value = session(id);
        CHECK(orb_model_upsert_session(&value));
    }
    orb_session_t extra = session("overflow");
    uint64_t revision = orb_model_revision();
    CHECK(!orb_model_upsert_session(&extra));
    CHECK(orb_model_revision() == revision);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == ORB_MODEL_MAX_SESSIONS);
    CHECK(orb_model_remove_session("session-10"));
    CHECK(orb_model_upsert_session(&extra));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!strcmp(s_view.sessions[10].id, "session-11"));
    CHECK(!strcmp(s_view.sessions[23].id, "overflow"));
    extra = session("session-0");
    strcpy(extra.reason, "Still working");
    CHECK(orb_model_upsert_session(&extra));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == ORB_MODEL_MAX_SESSIONS);
}

static void test_validation(void)
{
    orb_model_reset();
    orb_session_t value = session("");
    CHECK(!orb_model_upsert_session(&value));
    memset(value.id, 'a', sizeof(value.id));
    CHECK(!orb_model_upsert_session(&value));
    value = session("valid");
    memset(value.title, 'a', 64); value.title[64] = 0;
    memset(value.project, 'a', 32); value.project[32] = 0;
    memset(value.reason, 'a', 64); value.reason[64] = 0;
    memset(value.summary, 'a', 160); value.summary[160] = 0;
    CHECK(orb_model_upsert_session(&value));
    value.title[64] = 'x';
    CHECK(!orb_model_upsert_session(&value));
    strcpy(value.title, "Caf\xc3\xa9 \xf0\x9f\x8c\x99");
    CHECK(orb_model_upsert_session(&value));
    static const char *invalid[] = {"\xc0\x80", "\xc2", "\xed\xa0\x80", "\xf4\x90\x80\x80",
                                    "\x80", "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xe2\x28\xa1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(value.title, invalid[i]);
        CHECK(!orb_model_upsert_session(&value));
    }
    strcpy(value.title, "valid");
    value.agent = (orb_agent_t)-1;
    CHECK(!orb_model_upsert_session(&value));
    value.agent = ORB_AGENT_DOT;
    value.state = (orb_session_state_t)5;
    CHECK(!orb_model_upsert_session(&value));

    orb_card_t choice = card("choice");
    choice.options_count = 5;
    CHECK(!orb_model_upsert_card(&choice));
    choice.options_count = 2;
    choice.options[1] = choice.options[0];
    CHECK(!orb_model_upsert_card(&choice));
    strcpy(choice.options[1].id, "cancel");
    choice.options[1].role = (orb_option_role_t)99;
    CHECK(!orb_model_upsert_card(&choice));
    choice.options[1].role = ORB_OPTION_DEFAULT;
    choice.kind = ORB_CARD_APPROVAL;
    CHECK(!orb_model_upsert_card(&choice)); /* No safe decline. */
    choice.options[1].role = ORB_OPTION_CANCEL;
    CHECK(orb_model_upsert_card(&choice));
    choice.options_count = 1;
    CHECK(!orb_model_upsert_card(&choice));
    choice.kind = ORB_CARD_CHOICE;
    choice.options[0].label[0] = 0;
    CHECK(!orb_model_upsert_card(&choice));
    choice = card("choice");
    memset(choice.body, 'b', sizeof(choice.body));
    CHECK(!orb_model_upsert_card(&choice));
    CHECK(!orb_model_upsert_card(NULL));
    CHECK(!orb_model_cancel_card(NULL));
    CHECK(!orb_model_remove_session(NULL));
}

static void test_card_queue(void)
{
    orb_model_reset();
    orb_card_t choice = card("choice");
    CHECK(orb_model_upsert_card(&choice));
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_upsert_card(&choice));
    CHECK(orb_model_revision() == revision);
    orb_answer_t answer = {.card = "choice", .option = "next"};
    CHECK(!orb_model_answer(NULL));
    CHECK(!orb_model_take_answer(NULL));
    CHECK(!orb_model_take_answer(&answer));
    answer.option[0] = 0;
    CHECK(!orb_model_answer(&answer));
    strcpy(answer.option, "unknown");
    CHECK(!orb_model_answer(&answer));
    strcpy(answer.option, "next");
    strcpy(answer.text, "both");
    CHECK(!orb_model_answer(&answer));
    answer.option[0] = 0;
    CHECK(!orb_model_answer(&answer)); /* reply is not offered. */
    answer.text[0] = 0;
    strcpy(answer.option, "next");
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_revision() == revision + 1);
    CHECK(orb_model_pending_answers() == 1);
    CHECK(!orb_model_answer(&answer));
    CHECK(orb_model_upsert_card(&choice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.cards[0].answered); /* Retransmit cannot reactivate action. */
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.card, "choice") && !strcmp(answer.option, "next") && !answer.text[0]);
    CHECK(!orb_model_take_answer(&answer));
    CHECK(orb_model_pending_answers() == 0);
    CHECK(!orb_model_answer(&answer)); /* Already handed to link. */
    CHECK(orb_model_cancel_card("choice"));
    CHECK(orb_model_upsert_card(&choice));
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_cancel_card("choice"));
    CHECK(orb_model_pending_answers() == 0); /* Withdraw pending answer on cancel. */
    CHECK(!orb_model_take_answer(&answer));
    CHECK(!orb_model_cancel_card("choice"));

    orb_card_t message = {.id = "message", .kind = ORB_CARD_MESSAGE, .reply = true};
    CHECK(orb_model_upsert_card(&message));
    answer = (orb_answer_t){.card = "message", .text = "Yes, please."};
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.text, "Yes, please.") && !answer.option[0]);
}

static void test_full_queue_and_expiry(void)
{
    orb_model_reset();
    char id[49];
    for (unsigned i = 0; i < ORB_MODEL_MAX_CARDS; ++i) {
        snprintf(id, sizeof(id), "card-%u", i);
        orb_card_t choice = card(id);
        choice.expires = i % 2 ? 100 : 0;
        CHECK(orb_model_upsert_card(&choice));
        orb_answer_t answer = {.option = "next"};
        strcpy(answer.card, id);
        CHECK(orb_model_answer(&answer));
    }
    orb_card_t extra = card("overflow");
    CHECK(!orb_model_upsert_card(&extra));
    CHECK(orb_model_pending_answers() == ORB_MODEL_MAX_ANSWERS);
    CHECK(orb_model_expire(0) == 0);
    CHECK(orb_model_expire(99) == 0);
    CHECK(orb_model_expire(100) == 8);
    CHECK(orb_model_pending_answers() == 8);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.card_count == 8);
    orb_answer_t answer;
    for (unsigned i = 0; i < 16; i += 2) {
        CHECK(orb_model_take_answer(&answer));
        snprintf(id, sizeof(id), "card-%u", i);
        CHECK(!strcmp(answer.card, id));
    }
    CHECK(!orb_model_take_answer(&answer));
    CHECK(orb_model_upsert_card(&extra));
    CHECK(orb_model_cancel_card("card-0"));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.card_count == 8 && !strcmp(s_view.cards[0].id, "card-2"));
    orb_model_reset();
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.card_count == 0 && s_view.session_count == 0 && !s_view.cards[0].id[0]);
}

static void test_atomic_snapshot_and_reconnect(void)
{
    orb_model_reset();
    orb_session_t original = session("old-session");
    orb_session_t first = session("first"), second = session("second");
    orb_card_t old_card = card("waiting");
    orb_answer_t answer = {.card = "waiting", .option = "next"};
    CHECK(orb_model_upsert_session(&original));
    CHECK(orb_model_upsert_card(&old_card));
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_peek_answer(&answer));
    orb_model_set_connection(true, false);
    CHECK(orb_model_sessions_chunk(&first, 1, true, true));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 1 && !strcmp(s_view.sessions[0].id, "old-session"));
    CHECK(s_view.card_count == 1 && s_view.online && !s_view.synced);
    orb_model_sessions_discard(); /* Disconnect midway must never publish partial state. */
    orb_model_set_connection(false, false);
    CHECK(!orb_model_sessions_chunk(&second, 1, false, false));
    CHECK(orb_model_pending_answers() == 1);
    CHECK(orb_model_sessions_chunk(&first, 1, true, true));
    CHECK(!orb_model_sessions_chunk(&first, 1, false, false)); /* Duplicate ID. */
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!strcmp(s_view.sessions[0].id, "old-session"));
    CHECK(orb_model_sessions_chunk(&first, 1, true, true));
    orb_model_set_connection(true, false);
    CHECK(orb_model_sessions_chunk(&second, 1, false, false));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.synced && s_view.session_count == 2 && s_view.card_count == 0);
    CHECK(!strcmp(s_view.sessions[0].id, "first") && !strcmp(s_view.sessions[1].id, "second"));
    CHECK(orb_model_pending_answers() == 1);
    CHECK(orb_model_upsert_card(&old_card));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.cards[0].answered); /* Replay cannot enable a retried approval. */
    CHECK(orb_model_cancel_card("waiting"));
    CHECK(orb_model_pending_answers() == 1); /* Claimed answer survives late cancellation. */
    CHECK(orb_model_peek_answer(&answer));
    CHECK(!strcmp(answer.card, "waiting"));
    orb_answer_t winning = {.card = "waiting", .text = "Answered on another device"};
    CHECK(orb_model_complete_answer("waiting", &winning, false, NULL));
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.last_answer.accepted && !s_view.last_answer.delivered);
    CHECK(!strcmp(s_view.last_answer.answer.text, winning.text));
    CHECK(!orb_model_complete_answer("waiting", &winning, true, NULL));
    CHECK(orb_model_sessions_chunk(NULL, 0, true, false));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 0 && s_view.card_count == 0);

    orb_session_t full[ORB_MODEL_MAX_SESSIONS];
    for (size_t i = 0; i < ORB_MODEL_MAX_SESSIONS; ++i) {
        char id[49];
        snprintf(id, sizeof(id), "session-%zu", i);
        full[i] = session(id);
    }
    CHECK(orb_model_sessions_chunk(full, ORB_MODEL_MAX_SESSIONS, true, true));
    CHECK(!orb_model_sessions_chunk(&second, 1, false, false));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 0); /* Overflow preserves the last complete snapshot. */
    full[23].title[0] = (char)0xff;
    CHECK(!orb_model_sessions_chunk(full, ORB_MODEL_MAX_SESSIONS, true, false));
    CHECK(!orb_model_sessions_chunk(NULL, 0, false, false));
}

static void test_reliable_answer_and_id_limits(void)
{
    orb_model_reset();
    orb_card_t value = card("unused");
    memset(value.id, 'c', ORB_MODEL_CARD_ID_MAX); value.id[ORB_MODEL_CARD_ID_MAX] = 0;
    memset(value.options[0].id, 'o', ORB_MODEL_OPTION_ID_MAX);
    value.options[0].id[ORB_MODEL_OPTION_ID_MAX] = 0;
    CHECK(orb_model_upsert_card(&value));
    orb_answer_t answer = {0};
    strcpy(answer.card, value.id);
    strcpy(answer.option, value.options[0].id);
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_peek_answer(&answer));
    CHECK(orb_model_expire(100) == 0);
    CHECK(orb_model_cancel_card(value.id));
    CHECK(orb_model_peek_answer(&answer));
    CHECK(strlen(answer.card) == 64 && strlen(answer.option) == 32);
    CHECK(!orb_model_ack_answer("unknown"));
    CHECK(orb_model_ack_answer(answer.card));
    CHECK(!orb_model_peek_answer(&answer));
    value.id[64] = 'c';
    CHECK(!orb_model_upsert_card(&value));
    value.id[64] = 0;
    value.options[0].id[32] = 'o';
    CHECK(!orb_model_upsert_card(&value));
    value.options[0].id[32] = 0;
    value.scope = ORB_APPROVAL_TURN;
    CHECK(!orb_model_upsert_card(&value)); /* Scope only applies to approvals. */
    value.scope = ORB_APPROVAL_UNSPECIFIED;
    value.expires = 100;
    CHECK(orb_model_upsert_card(&value));
    strcpy(answer.card, value.id);
    strcpy(answer.option, value.options[0].id);
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_peek_answer(&answer));
    CHECK(orb_model_expire(100) == 1);
    CHECK(orb_model_pending_answers() == 1);
    CHECK(orb_model_complete_answer(answer.card, NULL, false, "expired"));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_answer.accepted && !strcmp(s_view.last_answer.error, "expired"));
    value.expires = 0;
    value.has_expiry = true;
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_expire(0) == 1); /* Explicit epoch zero is not an immortal card. */
}

static void test_unclaimed_answer_cancellation_receipts(void)
{
    orb_model_reset();
    orb_card_t value = card("cancel-before-send");
    orb_answer_t answer = {.card = "cancel-before-send", .option = "next"};
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_cancel_card(value.id));
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_answer.accepted && !s_view.last_answer.delivered);
    CHECK(!strcmp(s_view.last_answer.error, "cancelled"));
    CHECK(!strcmp(s_view.last_answer.answer.card, answer.card));
    CHECK(!strcmp(s_view.last_answer.answer.option, answer.option));
    CHECK(!orb_model_peek_answer(&answer));
    CHECK(!orb_model_complete_answer(value.id, NULL, false, "expired"));
    orb_answer_result_t previous = s_view.last_answer;
    CHECK(!orb_model_cancel_card(value.id));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_answer, &previous, sizeof(previous)));

    value = card("claimed-before-cancel");
    strcpy(answer.card, value.id);
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_peek_answer(&answer));
    CHECK(orb_model_cancel_card(value.id));
    CHECK(orb_model_pending_answers() == 1);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_answer, &previous, sizeof(previous)));
    CHECK(orb_model_complete_answer(value.id, &answer, true, NULL));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.last_answer.accepted && s_view.last_answer.delivered);

    value = card("expiry-before-send");
    value.expires = 100;
    strcpy(answer.card, value.id);
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_answer(&answer));
    CHECK(orb_model_expire(100) == 1);
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_answer.accepted && !s_view.last_answer.delivered);
    CHECK(!strcmp(s_view.last_answer.error, "cancelled"));
    CHECK(!strcmp(s_view.last_answer.answer.card, value.id));

    /* Snapshot replacement is a replay boundary, not a hub cancellation. */
    value = card("pending-during-snapshot");
    strcpy(answer.card, value.id);
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_answer(&answer));
    previous = s_view.last_answer;
    CHECK(orb_model_sessions_chunk(NULL, 0, true, false));
    CHECK(orb_model_pending_answers() == 1);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_answer, &previous, sizeof(previous)));
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.cards[0].answered);
}

static void test_explicit_cancellation_marker(void)
{
    orb_model_reset();
    orb_card_t value = card("replay-gap");
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_sessions_chunk(NULL, 0, true, false));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.card_count == 0 && !s_view.last_cancelled[0]);
    CHECK(!orb_model_cancel_card(value.id));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_cancelled[0]);

    /* An expiry is closed locally, not an invented card.cancel from the hub. */
    value.expires = 100;
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_expire(100) == 1);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_cancelled[0]);
    value.expires = 0;
    value.has_expiry = false;
    memset(value.id, 'c', ORB_MODEL_CARD_ID_MAX);
    value.id[ORB_MODEL_CARD_ID_MAX] = 0;
    CHECK(orb_model_upsert_card(&value));
    CHECK(orb_model_cancel_card(value.id));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(strlen(s_view.last_cancelled) == ORB_MODEL_CARD_ID_MAX);
    CHECK(!strcmp(s_view.last_cancelled, value.id));
    CHECK(orb_model_sessions_chunk(NULL, 0, true, false));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!strcmp(s_view.last_cancelled, value.id));
}

static void test_messages_and_voice_handoff(void)
{
    orb_model_reset();
    orb_message_t message = {.target = "observed", .text = "Please continue."};
    orb_session_t value = session("observed");
    CHECK(orb_model_upsert_session(&value));
    CHECK(!orb_model_message(&message)); /* Read-only sessions never become sendable. */
    value.can_message = true;
    CHECK(orb_model_upsert_session(&value));
    message.stream = 1;
    CHECK(!orb_model_message(&message)); /* Stream confirmation requires its source link. */
    message.generation = 7;
    for (unsigned i = 0; i < ORB_MODEL_MAX_MESSAGES; ++i) {
        message.stream = i + 1;
        CHECK(orb_model_message(&message));
    }
    CHECK(!orb_model_message(&message));
    for (unsigned i = 0; i < ORB_MODEL_MAX_MESSAGES; ++i) {
        CHECK(orb_model_take_message(&message));
        CHECK(message.stream == i + 1 && message.generation == 7 && !strcmp(message.target, "observed"));
    }
    CHECK(!orb_model_take_message(&message));
    orb_model_complete_message(&message, false, "timeout");
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_message.accepted && !strcmp(s_view.last_message.error, "timeout"));
    CHECK(!strcmp(s_view.last_message.message.text, "Please continue."));
    CHECK(!orb_model_take_message(&message)); /* An ambiguous timeout never resubmits. */
    strcpy(message.target, "dot");
    CHECK(orb_model_message(&message));
    CHECK(orb_model_take_message(&message));
    message.text[0] = 0;
    CHECK(!orb_model_message(&message));
    strcpy(message.text, "\xc0\x80");
    CHECK(!orb_model_message(&message));

    char target[49], card_id[65];
    CHECK(orb_model_get_voice_focus(target, card_id));
    CHECK(!strcmp(target, "dot") && !card_id[0]);
    CHECK(orb_model_voice_focus("observed", "reply-card"));
    CHECK(orb_model_voice_request(ORB_VOICE_START, NULL, NULL));
    unsigned notifications = 0;
    orb_model_set_voice_notify(voice_notify, &notifications);
    CHECK(notifications == 1); /* Registration observes startup work. */
    orb_voice_request_t request;
    CHECK(orb_model_take_voice_request(&request));
    CHECK(request.action == ORB_VOICE_START && !strcmp(request.target, "observed"));
    CHECK(!strcmp(request.card, "reply-card"));
    for (unsigned i = 0; i < ORB_MODEL_MAX_VOICE_REQUESTS; ++i)
        CHECK(orb_model_voice_request(ORB_VOICE_STOP, NULL, NULL));
    CHECK(!orb_model_voice_request(ORB_VOICE_START, "dot", NULL));
    CHECK(orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL));
    CHECK(orb_model_take_voice_request(&request));
    CHECK(request.action == ORB_VOICE_CANCEL);
    CHECK(!orb_model_take_voice_request(&request));
    CHECK(!orb_model_voice_request((orb_voice_action_t)99, NULL, NULL));
    CHECK(notifications == ORB_MODEL_MAX_VOICE_REQUESTS + 2);
    orb_model_set_voice_notify(NULL, NULL);
    orb_voice_state_t voice = {.phase = ORB_VOICE_RECORDING, .stream = 12, .target = "observed", .level = 50};
    CHECK(orb_model_set_voice(&voice));
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_revision() == revision);
    voice.phase = ORB_VOICE_CONFIRMING;
    voice.level = 0;
    strcpy(voice.text, "Please check the build.");
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.voice.phase == ORB_VOICE_CONFIRMING && !strcmp(s_view.voice.text, voice.text));
    voice.level = 101;
    CHECK(!orb_model_set_voice(&voice));
    CHECK(orb_model_get_voice(&voice));
    CHECK(voice.level == 0 && voice.stream == 12);
}

static void test_voice_message_receipts(void)
{
    orb_model_reset();
    orb_voice_state_t voice = {.phase = ORB_VOICE_CONFIRMING, .stream = 17,
        .target = "sendable", .text = "Please check the build."};
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_message.message.text[0]); /* Transcription is not a send. */
    voice.phase = ORB_VOICE_SENDING;
    CHECK(orb_model_set_voice(&voice));
    /* No UI snapshot between sending and acceptance: the receipt must survive
     * coalesced polling after the worker clears its transient transcript. */
    voice.phase = ORB_VOICE_IDLE;
    voice.stream = 0;
    voice.text[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.last_message.accepted && !s_view.last_message.error[0]);
    CHECK(!strcmp(s_view.last_message.message.target, "sendable"));
    CHECK(!strcmp(s_view.last_message.message.text, "Please check the build."));
    CHECK(s_view.last_message.message.stream == 17 && s_view.last_message.message.generation == 0);
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_revision() == revision);
    orb_message_t message;
    CHECK(!orb_model_take_message(&message)); /* A receipt never resubmits. */

    voice.phase = ORB_VOICE_SENDING;
    voice.stream = 18;
    strcpy(voice.text, "Check the conversation before trying again.");
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_ERROR;
    voice.stream = 0;
    strcpy(voice.error, "Delivery unconfirmed. Check the conversation.");
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.last_message.accepted);
    CHECK(!strcmp(s_view.last_message.error, "delivery_unconfirmed"));
    CHECK(!strcmp(s_view.last_message.message.text, voice.text));
    CHECK(!strcmp(s_view.voice.text, voice.text));
    CHECK(s_view.last_message.message.stream == 18);
    CHECK(!orb_model_take_message(&message));

    /* Stream IDs restart on reconnect. A local failure before a new callback
     * must replace an older delivered receipt even for identical text/target. */
    orb_message_t reused = {.target = "sendable", .text = "Repeated request", .stream = 1, .generation = 7};
    orb_model_complete_message_receipt(&reused, ORB_SEND_DELIVERED, "Transport accepted", NULL);
    voice.phase = ORB_VOICE_CONFIRMING; voice.stream = 1;
    strcpy(voice.text, reused.text);
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_SENDING;
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_ERROR; voice.stream = 0;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view) && !s_view.last_message.accepted &&
          s_view.last_message.status == ORB_SEND_FAILED);
    voice.phase = ORB_VOICE_SENDING; voice.stream = 1;
    CHECK(orb_model_set_voice(&voice));
    reused.generation = 8;
    orb_model_complete_message_receipt(&reused, ORB_SEND_QUEUED, "Queued for Codex", NULL);
    voice.phase = ORB_VOICE_IDLE; voice.stream = 0; voice.text[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view) && s_view.last_message.accepted &&
          s_view.last_message.status == ORB_SEND_QUEUED && s_view.last_message.message.generation == 8);
    CHECK(!strcmp(s_view.last_message.reason, "Queued for Codex"));

    /* Cancelling an unconfirmed transcript is not a failed or accepted send. */
    orb_message_result_t previous = s_view.last_message;
    voice.phase = ORB_VOICE_CONFIRMING;
    voice.stream = 19;
    strcpy(voice.text, "Discard this transcript.");
    voice.error[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_IDLE;
    voice.stream = 0;
    voice.text[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_message, &previous, sizeof(previous)));

    /* Card confirmation hands off to last_answer, never a session receipt. */
    voice.phase = ORB_VOICE_SENDING;
    voice.stream = 20;
    strcpy(voice.card, "reply-card");
    strcpy(voice.text, "My card reply.");
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_IDLE;
    voice.stream = 0;
    voice.text[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_message, &previous, sizeof(previous)));
    voice.phase = ORB_VOICE_SENDING;
    voice.stream = 21;
    strcpy(voice.text, "Another card reply.");
    CHECK(orb_model_set_voice(&voice));
    voice.phase = ORB_VOICE_ERROR;
    CHECK(orb_model_set_voice(&voice));
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!memcmp(&s_view.last_message, &previous, sizeof(previous)));
}

typedef struct { unsigned writer; unsigned failures; unsigned iterations; } worker_t;
static void *write_sessions(void *argument)
{
    worker_t *worker = argument;
    char id[49];
    snprintf(id, sizeof(id), "writer-%u", worker->writer);
    orb_session_t value = session(id);
    for (unsigned n = 1; n <= 4000; ++n) {
        value.updated = n;
        snprintf(value.title, sizeof(value.title), "writer-%u:%u", worker->writer, n);
        snprintf(value.summary, sizeof(value.summary), "%u", n);
        value.can_message = n % 2 != 0;
        if (!orb_model_upsert_session(&value)) ++worker->failures;
        ++worker->iterations;
    }
    return NULL;
}

static void *read_sessions(void *argument)
{
    worker_t *worker = argument;
    orb_model_snapshot_t *view = malloc(sizeof(*view));
    if (!view) { worker->failures = 1; return NULL; }
    for (unsigned n = 0; n < 8000; ++n) {
        if (!orb_model_snapshot(view) || view->session_count > 4) {
            ++worker->failures;
            break;
        }
        for (size_t i = 0; i < view->session_count; ++i) {
            const orb_session_t *value = &view->sessions[i];
            unsigned writer = 0, version = 0;
            if (sscanf(value->title, "writer-%u:%u", &writer, &version) != 2 ||
                value->updated != version || strtoul(value->summary, NULL, 10) != version ||
                value->can_message != (version % 2 != 0) || writer > 3) ++worker->failures;
        }
        ++worker->iterations;
    }
    free(view);
    return NULL;
}

static void test_concurrent_snapshots(void)
{
    orb_model_reset();
    pthread_t threads[6];
    worker_t workers[6] = {{.writer = 0}, {.writer = 1}, {.writer = 2}, {.writer = 3}, {0}, {0}};
    for (unsigned i = 0; i < 6; ++i)
        CHECK(pthread_create(&threads[i], NULL, i < 4 ? write_sessions : read_sessions, &workers[i]) == 0);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(workers[i].failures == 0);
        CHECK(workers[i].iterations == (i < 4 ? 4000u : 8000u));
    }
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 4);
    for (size_t i = 0; i < s_view.session_count; ++i) CHECK(s_view.sessions[i].updated == 4000);
}

static void test_demo(void)
{
    orb_model_load_demo();
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.session_count == 5 && s_view.card_count == 4);
    CHECK(!strcmp(s_view.sessions[0].id, "claude-firmware"));
    CHECK(!strcmp(s_view.sessions[0].reason, "Approve Bash: idf.py build"));
    for (size_t i = 0; i < s_view.card_count; ++i) CHECK(!s_view.cards[i].answered);
    orb_answer_t answer = {.card = "approval-build", .option = "deny"};
    CHECK(orb_model_answer(&answer));
    orb_model_load_demo();
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_model_snapshot(&s_view));
    CHECK(!s_view.cards[0].answered);
}

static void test_focus_preferences_and_idle(void)
{
    orb_model_reset();
    CHECK(orb_model_snapshot(&s_view));
    CHECK(s_view.prefs.sources[0] && s_view.prefs.sources[1] && s_view.prefs.sources[2]);
    CHECK(s_view.prefs.idle_minutes == 10 && !s_view.prefs_pending);
    orb_prefs_patch_t patch = {.sources_mask = 1u << ORB_AGENT_CLAUDE};
    CHECK(!orb_model_prefs_request(&patch));
    orb_model_set_connection(true, true);
    CHECK(orb_model_prefs_request(&patch));
    CHECK(!orb_model_prefs_request(&patch));
    CHECK(orb_model_snapshot(&s_view) && s_view.prefs_pending && s_view.prefs.sources[0]);
    orb_prefs_patch_t taken;
    CHECK(orb_model_take_prefs_request(&taken) && taken.sources_mask == 1 && !taken.sources[0]);
    CHECK(!orb_model_take_prefs_request(&taken));
    orb_prefs_t confirmed = {.sources = {false, true, true}, .idle_minutes = 10};
    orb_model_complete_prefs(&confirmed, NULL);
    CHECK(orb_model_snapshot(&s_view) && !s_view.prefs_pending && !s_view.prefs.sources[0]);
    patch = (orb_prefs_patch_t){.has_idle_minutes = true, .idle_minutes = 30};
    CHECK(orb_model_prefs_request(&patch));
    orb_model_complete_prefs(NULL, "timeout");
    CHECK(orb_model_snapshot(&s_view) && !s_view.prefs_pending && s_view.prefs.idle_minutes == 10);
    CHECK(!strcmp(s_view.prefs_error, "timeout"));
    patch.idle_minutes = 1441; CHECK(!orb_model_prefs_request(&patch));
    patch = (orb_prefs_patch_t){0}; CHECK(!orb_model_prefs_request(&patch));
    const uint32_t hidden[] = {2, 0, 0};
    CHECK(orb_model_set_summary(9, hidden));
    CHECK(orb_model_snapshot(&s_view) && s_view.idle_count == 9 && s_view.hidden_by_source[0] == 2);

    static orb_idle_page_t page;
    CHECK(!orb_model_idle_snapshot(&page));
    CHECK(orb_model_idle_open() && orb_model_idle_open());
    CHECK(orb_model_idle_snapshot(&page) && page.pending && page.count == 0);
    orb_idle_request_t request, old;
    CHECK(orb_model_take_idle_request(&request) && !request.cursor[0]);
    CHECK(!orb_model_take_idle_request(&old));
    orb_session_t sessions[ORB_MODEL_MAX_IDLE] = {0};
    for (size_t i = 0; i < ORB_MODEL_MAX_IDLE; ++i) {
        sessions[i].agent = ORB_AGENT_CODEX;
        sessions[i].state = ORB_SESSION_IDLE;
        snprintf(sessions[i].id, sizeof(sessions[i].id), "idle-%zu", i);
    }
    CHECK(!orb_model_complete_idle(&request, sessions, 9, "next", NULL));
    CHECK(orb_model_complete_idle(&request, sessions, 8, "next", NULL));
    CHECK(orb_model_idle_snapshot(&page) && page.count == 8 && !page.pending);
    CHECK(!strcmp(page.sessions[7].id, "idle-7") && !strcmp(page.cursor, "next"));
    CHECK(orb_model_snapshot(&s_view) && s_view.session_count == 0); /* Separate from focus. */
    CHECK(orb_model_idle_more() && !orb_model_idle_more());
    CHECK(orb_model_take_idle_request(&request) && !strcmp(request.cursor, "next"));
    old = request;
    CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, "conflict"));
    CHECK(orb_model_idle_snapshot(&page) && page.pending && page.count == 0 && !page.cursor[0]);
    CHECK(orb_model_take_idle_request(&request) && !request.cursor[0]);
    CHECK(!orb_model_complete_idle(&old, sessions, 8, NULL, NULL));
    CHECK(orb_model_complete_idle(&request, sessions, 1, NULL, NULL));
    CHECK(!orb_model_idle_more()); /* cursor:null is exhausted. */
    orb_model_idle_close();
    CHECK(!orb_model_idle_snapshot(&page));
    CHECK(orb_model_idle_open() && orb_model_take_idle_request(&old));
    orb_model_idle_close();
    CHECK(orb_model_idle_open() && orb_model_take_idle_request(&request));
    CHECK(!orb_model_complete_idle(&old, sessions, 1, NULL, NULL));
    CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, "timeout"));
    CHECK(orb_model_idle_snapshot(&page) && !page.pending && !strcmp(page.error, "timeout"));
    CHECK(orb_model_idle_more() && orb_model_take_idle_request(&request));
    CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, NULL));
    CHECK(orb_model_idle_snapshot(&page) && page.count == 0 && !page.pending && !page.error[0]);
    orb_model_idle_close();
    CHECK(!orb_model_complete_idle(&request, sessions, 1, NULL, NULL));

    patch = (orb_prefs_patch_t){.has_idle_minutes = true, .idle_minutes = 5};
    CHECK(orb_model_prefs_request(&patch));
    CHECK(orb_model_idle_open() && orb_model_take_idle_request(&request));
    orb_model_set_connection(false, false);
    CHECK(orb_model_snapshot(&s_view) && !s_view.prefs_pending && !strcmp(s_view.prefs_error, "disconnected"));
    CHECK(orb_model_idle_snapshot(&page) && !page.pending && !strcmp(page.error, "disconnected"));
    CHECK(!orb_model_complete_idle(&request, sessions, 1, NULL, NULL));
    orb_model_reset();
    CHECK(!orb_model_idle_snapshot(&page));
}

static void test_temperature_units(void)
{
    orb_model_reset();
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_C);
    CHECK(orb_temperature_display(0, ORB_TEMPERATURE_F) == 32);
    CHECK(orb_temperature_display(-40, ORB_TEMPERATURE_F) == -40);
    CHECK(orb_temperature_display(21.5f, ORB_TEMPERATURE_C) == 22);
    CHECK(orb_temperature_display(-1.5f, ORB_TEMPERATURE_C) == -2);
    CHECK(orb_temperature_target(-1, ORB_TEMPERATURE_F) == 50);
    CHECK(orb_temperature_target(2, ORB_TEMPERATURE_F) == 90);
    /* Every Fahrenheit detent survives the required tenth-Celsius wire
     * rounding, including repeated writes; the chosen 72 never becomes 71. */
    for (int f = 50; f <= 90; ++f) {
        float c = orb_temperature_celsius(f, ORB_TEMPERATURE_F);
        CHECK(fabsf(c * 10 - roundf(c * 10)) < .0001f);
        CHECK(orb_temperature_display(c, ORB_TEMPERATURE_F) == f);
        CHECK(orb_temperature_target(orb_temperature_position(c, ORB_TEMPERATURE_F), ORB_TEMPERATURE_F) == f);
    }
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs);
    prefs.temperature = ORB_TEMPERATURE_F; prefs.temperature_owner = true;
    CHECK(orb_model_set_prefs(&prefs));
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
    orb_model_prefs_defaults(&prefs); /* Older hub omits units. */
    CHECK(orb_model_set_prefs(&prefs));
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
    CHECK(orb_model_snapshot(&s_view) && s_view.prefs.temperature_owner);
    orb_model_set_connection(true, true);
    orb_prefs_patch_t patch = {.has_temperature = true, .temperature = ORB_TEMPERATURE_C};
    CHECK(orb_model_prefs_request(&patch));
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F); /* Confirmed until ack. */
    orb_model_complete_prefs(NULL, "timeout");
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
    CHECK(orb_model_prefs_request(&patch));
    prefs.temperature = ORB_TEMPERATURE_C; prefs.temperature_owner = true;
    orb_model_complete_prefs(&prefs, NULL);
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_C);
    prefs.temperature = ORB_TEMPERATURE_F; prefs.temperature_owner = false;
    CHECK(orb_model_set_prefs(&prefs));
    orb_model_set_connection(false, false);
    CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
    patch.temperature = (orb_temperature_t)99;
    CHECK(!orb_model_prefs_request(&patch));
    CHECK(!orb_model_set_prefs(&(orb_prefs_t){.temperature = (orb_temperature_t)-1}));
}

static void test_speech_voice_metadata(void)
{
    orb_model_reset(); orb_model_set_connection(true, true);
    CHECK(orb_model_set_speech_voice("Ava"));
    CHECK(orb_model_snapshot(&s_view)); CHECK(!strcmp(s_view.speech_voice, "Ava"));
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_set_speech_voice("Ava")); CHECK(orb_model_revision() == revision);
    CHECK(!orb_model_set_speech_voice(NULL));
    CHECK(!orb_model_set_speech_voice("Ava\n"));
    CHECK(!orb_model_set_speech_voice("\xc0\x80"));
    char oversized[66]; memset(oversized, 'a', 65); oversized[65] = 0;
    CHECK(!orb_model_set_speech_voice(oversized));
    CHECK(orb_model_set_speech_voice(""));
    CHECK(orb_model_snapshot(&s_view)); CHECK(!s_view.speech_voice[0]);
    CHECK(orb_model_set_speech_voice("Ava"));
    orb_model_set_connection(false, false);
    CHECK(orb_model_snapshot(&s_view)); CHECK(!s_view.speech_voice[0]);
}

static void test_weather(void)
{
    orb_model_reset();
    orb_weather_snapshot_t weather = {.configured = true, .available = true, .place = "New York",
        .label = "Sunny", .unit = ORB_TEMPERATURE_F, .temperature = 72, .high = 75, .low = 58,
        .condition = ORB_WEATHER_CLEAR, .is_day = true, .updated_at = 1793221200,
        .hourly_count = ORB_MODEL_MAX_WEATHER_HOURS};
    for (size_t i = 0; i < weather.hourly_count; ++i)
        weather.hourly[i] = (orb_weather_hour_t){.hour = weather.updated_at + 3600 * (i + 1),
            .temperature = 71 - i, .condition = ORB_WEATHER_RAIN, .is_day = i == 0};
    CHECK(!orb_model_set_weather(&weather));
    orb_model_set_connection(true, true);
    CHECK(!orb_model_set_weather(&weather));
    orb_model_weather_capability(true);
    CHECK(orb_model_set_weather(&weather));
    orb_weather_snapshot_t copy;
    CHECK(orb_model_weather_snapshot(&copy));
    CHECK(copy.revision == 1 && copy.temperature == 72 && copy.hourly_count == 6 && !copy.stale);
    CHECK(copy.unit == ORB_TEMPERATURE_F && !strcmp(copy.place, "New York"));
    uint64_t revision = orb_model_revision();
    CHECK(orb_model_set_weather(&weather) && orb_model_revision() == revision);
    weather.revision = UINT64_MAX; /* Caller cannot inject the local revision. */
    CHECK(orb_model_set_weather(&weather) && orb_model_revision() == revision);
    copy.temperature = -999;
    CHECK(orb_model_weather_snapshot(&copy) && copy.temperature == 72);
    orb_weather_snapshot_t bad = weather;
    bad.hourly_count = 7;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.temperature = NAN;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.high = INFINITY;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.low = -251;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.high = 50;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; memset(bad.place, 'p', sizeof(bad.place));
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; strcpy(bad.place, "\xc0\x80");
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.condition = (orb_weather_condition_t)99;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.unit = ORB_TEMPERATURE_UNKNOWN;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.hourly[1].hour = bad.hourly[0].hour;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.hourly[0].hour = bad.updated_at;
    CHECK(!orb_model_set_weather(&bad));
    bad = weather; bad.hourly[5].condition = ORB_WEATHER_UNKNOWN;
    CHECK(!orb_model_set_weather(&bad));
    CHECK(orb_model_revision() == revision);
    orb_model_set_connection(false, false);
    CHECK(orb_model_weather_snapshot(&copy) && copy.stale && copy.revision == 2 && copy.temperature == 72);
    CHECK(orb_model_snapshot(&s_view) && !s_view.weather_supported && s_view.weather_valid);
    revision = orb_model_revision();
    orb_model_set_connection(false, false);
    CHECK(orb_model_revision() == revision);
    orb_model_set_connection(true, true); orb_model_weather_capability(true);
    CHECK(orb_model_set_weather(&weather));
    CHECK(orb_model_weather_snapshot(&copy) && !copy.stale && copy.revision == 3);
    orb_model_weather_capability(false);
    CHECK(orb_model_weather_snapshot(&copy) && copy.stale && copy.revision == 4);
    orb_model_weather_capability(true);
    weather = (orb_weather_snapshot_t){.unit = ORB_TEMPERATURE_C, .label = "Set a location in orb-hub"};
    CHECK(orb_model_set_weather(&weather));
    CHECK(orb_model_weather_snapshot(&copy) && !copy.configured && !copy.available && !copy.updated_at && !copy.stale);
    CHECK(!strcmp(copy.label, "Set a location in orb-hub") && copy.revision == 5);
    weather.configured = true; strcpy(weather.place, "Berlin"); strcpy(weather.label, "Loading weather");
    CHECK(orb_model_set_weather(&weather));
    weather.stale = true; strcpy(weather.label, "Weather unavailable");
    CHECK(orb_model_set_weather(&weather));
    weather.temperature = 20;
    CHECK(!orb_model_set_weather(&weather)); /* No invented unavailable readings. */
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_CLEAR, true), ORB_ICON_SUN));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_CLEAR, false), ORB_ICON_MOON));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_PARTLY_CLOUDY, true), ORB_ICON_CLOUD_SUN));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_PARTLY_CLOUDY, false), ORB_ICON_CLOUD_MOON));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_RAIN, false), ORB_ICON_CLOUD_RAIN));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_THUNDER, true), ORB_ICON_CLOUD_BOLT));
    CHECK(!strcmp(orb_weather_icon(ORB_WEATHER_UNKNOWN, true), ""));
    for (int condition = ORB_WEATHER_CLEAR; condition <= ORB_WEATHER_THUNDER; ++condition)
        CHECK(orb_weather_icon((orb_weather_condition_t)condition, true)[0]); /* every known condition has a glyph */
    CHECK(!strcmp(orb_weather_condition_label(ORB_WEATHER_CLEAR, false), "Clear"));
    CHECK(!strcmp(orb_weather_condition_name(ORB_WEATHER_PARTLY_CLOUDY), "partly_cloudy"));
    CHECK(!orb_model_weather_snapshot(NULL));
}

int main(void)
{
    test_init_and_revisions();
    test_speech_voice_metadata();
    test_session_bounds();
    test_validation();
    test_card_queue();
    test_full_queue_and_expiry();
    test_atomic_snapshot_and_reconnect();
    test_reliable_answer_and_id_limits();
    test_unclaimed_answer_cancellation_receipts();
    test_explicit_cancellation_marker();
    test_messages_and_voice_handoff();
    test_voice_message_receipts();
    test_concurrent_snapshots();
    test_demo();
    test_focus_preferences_and_idle();
    test_temperature_units();
    test_weather();
    printf("orb_model: %u checks passed; 16,000 concurrent updates and 16,000 coherent snapshots\n", s_checks);
    return 0;
}
