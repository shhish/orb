#include "voice_core.h"
#include "voice_wait.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(c) do { ++s_checks; if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct {
    bool audio_ok, request_ok, open_ok, answer_ok, auto_cancel;
    voice_core_t *core;
    size_t read_bytes, sample_offset, expected_payload_bytes;
    unsigned opens, closes, sends, answers, requests[4], publishes;
    uint32_t expected_seq;
    uint16_t expected_stream;
    orb_voice_state_t view;
} fake_t;
static bool open_audio(void *ctx) { fake_t *f = ctx; ++f->opens; return f->open_ok; }
static void close_audio(void *ctx) { fake_t *f = ctx; ++f->closes; CHECK(f->closes <= f->opens); }
static size_t read_audio(void *ctx, uint8_t *pcm, size_t capacity)
{
    fake_t *f = ctx;
    if (f->read_bytes <= capacity) {
        memset(pcm, 0, f->read_bytes);
        if (f->read_bytes >= f->sample_offset + 2) {
            pcm[f->sample_offset] = 0;
            pcm[f->sample_offset + 1] = 0x80;
        }
    }
    return f->read_bytes;
}
static bool send_audio(void *ctx, const uint8_t *frame, size_t bytes)
{
    fake_t *f = ctx;
    CHECK(bytes == (f->expected_payload_bytes ? f->expected_payload_bytes : f->read_bytes) + 7 && bytes <= 1031);
    CHECK(frame[0] == 1 && frame[1] == (f->expected_stream >> 8) && frame[2] == (f->expected_stream & 255));
    uint32_t seq = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[4] << 16) | ((uint32_t)frame[5] << 8) | frame[6];
    CHECK(seq == f->expected_seq++ && frame[7] == 0 && frame[8] == 0x80);
    ++f->sends;
    return f->audio_ok;
}
static bool request(void *ctx, voice_request_kind_t kind, const orb_voice_state_t *view)
{
    fake_t *f = ctx;
    ++f->requests[kind];
    CHECK(view->stream != 0);
    if (kind == VOICE_REQ_CANCEL && f->request_ok && f->auto_cancel)
        voice_core_reply(f->core, kind, view->stream, f->core->generation, true, NULL, 0);
    return f->request_ok;
}
static bool answer(void *ctx, const orb_voice_state_t *view)
{
    fake_t *f = ctx;
    CHECK(!strcmp(view->card, "card") && !strcmp(view->text, "hello"));
    ++f->answers;
    return f->answer_ok;
}
static void publish(void *ctx, const orb_voice_state_t *view)
{
    fake_t *f = ctx;
    f->view = *view;
    ++f->publishes;
}
static bool focus(void *ctx, char target[49], char card[65])
{
    (void)ctx;
    strcpy(target, "dot");
    card[0] = 0;
    return true;
}
static void setup(voice_core_t *s, fake_t *f)
{
    *f = (fake_t){.audio_ok = true, .request_ok = true, .open_ok = true,
                  .answer_ok = true, .auto_cancel = true, .core = s, .read_bytes = VOICE_PCM_BYTES};
    voice_ops_t ops = {open_audio, close_audio, read_audio, send_audio, request, answer, publish, focus};
    voice_core_init(s, &ops, f);
    voice_core_connection(s, true, 1, 0);
}
static void action(voice_core_t *s, orb_voice_action_t kind, uint64_t now)
{
    orb_voice_request_t r = {.action = kind};
    voice_core_action(s, &r, now);
}
static void begin(voice_core_t *s, fake_t *f, uint64_t now)
{
    action(s, ORB_VOICE_START, now);
    CHECK(s->view.phase == ORB_VOICE_STARTING && s->opened);
    f->expected_stream = s->view.stream;
    f->expected_seq = 0;
    voice_core_reply(s, VOICE_REQ_BEGIN, s->view.stream, s->generation, true, NULL, now + 1);
    CHECK(s->view.phase == ORB_VOICE_RECORDING);
    unsigned sends = f->sends;
    for (unsigned n = 0; n < VOICE_WARMUP_BYTES / VOICE_PCM_BYTES; ++n)
        voice_core_tick(s, now + 1);
    CHECK(!s->warmup_bytes && f->sends == sends && !s->pcm_bytes && !s->view.level);
}
static void test_frames(void)
{
    uint8_t out[1031], pcm[1026];
    for (size_t n = 0; n < sizeof(pcm); ++n) pcm[n] = n;
    for (size_t n = 2; n <= 1024; n += 2) {
        CHECK(voice_frame(out, sizeof(out), 0xabcd, 0x12345678, pcm, n) == n + 7);
        CHECK(!memcmp(out, (uint8_t[]){1,0xab,0xcd,0x12,0x34,0x56,0x78}, 7));
        CHECK(!memcmp(out + 7, pcm, n));
        CHECK(!voice_frame(out, n + 6, 1, 0, pcm, n));
    }
    CHECK(!voice_frame(out, sizeof(out), 0, 0, pcm, 2));
    CHECK(!voice_frame(out, sizeof(out), 1, 0, pcm, 0));
    CHECK(!voice_frame(out, sizeof(out), 1, 0, pcm, 1));
    CHECK(!voice_frame(out, sizeof(out), 1, 0, pcm, 1026));
}
static void test_lifecycle(void)
{
    voice_core_t s; fake_t f; setup(&s, &f);
    begin(&s, &f, 10);
    voice_core_tick(&s, 31);
    voice_core_tick(&s, 51);
    CHECK(f.sends == 2 && s.view.level == 100 && s.pcm_bytes == 1280);
    action(&s, ORB_VOICE_STOP, 55);
    CHECK(s.view.phase == ORB_VOICE_TRANSCRIBING && !s.opened && f.closes == 1);
    voice_core_result(&s, s.view.stream + 1, "stale", 56);
    CHECK(s.view.phase == ORB_VOICE_TRANSCRIBING);
    voice_core_result(&s, s.view.stream, "hello", 100);
    CHECK(s.view.phase == ORB_VOICE_CONFIRMING && !strcmp(s.view.text, "hello"));
    CHECK(f.requests[VOICE_REQ_SEND] == 0 && f.answers == 0);
    voice_core_tick(&s, 1000000);
    CHECK(s.view.phase == ORB_VOICE_CONFIRMING); /* Never auto-send/dismiss. */
    uint16_t stream = s.view.stream;
    action(&s, ORB_VOICE_CONFIRM, 1000001);
    CHECK(s.view.phase == ORB_VOICE_SENDING && f.requests[VOICE_REQ_SEND] == 1);
    action(&s, ORB_VOICE_CONFIRM, 1000002);
    CHECK(f.requests[VOICE_REQ_SEND] == 1);
    voice_core_reply(&s, VOICE_REQ_SEND, stream, 1, true, NULL, 1000003);
    CHECK(s.view.phase == ORB_VOICE_IDLE && f.closes == 1 && s.view.text[0] == 0);
    begin(&s, &f, 1000010);
    CHECK(s.view.stream == stream + 1);
    voice_core_tick(&s, 1000031);
    action(&s, ORB_VOICE_STOP, 1000032);
    voice_core_result(&s, s.view.stream, "hello", 1000033);
    strcpy(s.view.card, "card");
    action(&s, ORB_VOICE_CONFIRM, 1000034);
    CHECK(f.answers == 1 && s.view.phase == ORB_VOICE_IDLE && f.closes == 2);
}
static void test_failure_and_reconnect(void)
{
    voice_core_t s; fake_t f;
    setup(&s, &f); f.open_ok = false;
    action(&s, ORB_VOICE_START, 10);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 0);
    setup(&s, &f); f.request_ok = false;
    action(&s, ORB_VOICE_START, 10);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 1 && !s.opened);
    setup(&s, &f); begin(&s, &f, 10); f.audio_ok = false;
    voice_core_tick(&s, 31);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 1);
    setup(&s, &f); begin(&s, &f, 10); f.read_bytes = SIZE_MAX;
    voice_core_tick(&s, 31);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 1);
    setup(&s, &f); begin(&s, &f, 10); f.read_bytes = 0;
    voice_core_tick(&s, 31);
    CHECK(s.view.phase == ORB_VOICE_RECORDING && !s.pcm_bytes);
    action(&s, ORB_VOICE_STOP, 40);
    CHECK(s.view.phase == ORB_VOICE_IDLE && f.requests[VOICE_REQ_END] == 0);
    setup(&s, &f); begin(&s, &f, 10);
    uint16_t stream = s.view.stream;
    voice_core_connection(&s, false, 1, 31);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 1);
    voice_core_connection(&s, true, 2, 40);
    voice_core_reply(&s, VOICE_REQ_BEGIN, stream, 1, true, NULL, 41);
    CHECK(s.view.phase == ORB_VOICE_ERROR);
    begin(&s, &f, 50);
    CHECK(s.view.stream == 1 && s.generation == 2);
    voice_core_tick(&s, 71); action(&s, ORB_VOICE_STOP, 72);
    voice_core_result(&s, 1, "hello", 73);
    voice_core_connection(&s, false, 2, 74);
    action(&s, ORB_VOICE_CONFIRM, 75);
    CHECK(f.requests[VOICE_REQ_SEND] == 0 && s.view.phase == ORB_VOICE_ERROR);
}
static void test_deadlines_boot_and_cancel(void)
{
    voice_core_t s; fake_t f; setup(&s, &f);
    voice_core_boot(&s, true, 100);
    voice_core_tick(&s, 449);
    CHECK(f.opens == 0);
    voice_core_tick(&s, 450);
    CHECK(s.view.phase == ORB_VOICE_STARTING);
    voice_core_boot(&s, false, 451);
    CHECK(s.view.phase == ORB_VOICE_IDLE && f.closes == 1);
    begin(&s, &f, 500);
    voice_core_tick(&s, 525); action(&s, ORB_VOICE_STOP, 530);
    uint16_t stream = s.view.stream;
    voice_core_boot(&s, true, 600); voice_core_boot(&s, false, 650);
    CHECK(s.view.phase == ORB_VOICE_IDLE);
    voice_core_result(&s, stream, "late", 660);
    CHECK(s.view.phase == ORB_VOICE_IDLE);
    begin(&s, &f, 1000);
    voice_core_tick(&s, 61000);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 3);
    begin(&s, &f, 62000);
    voice_core_tick(&s, 62021); action(&s, ORB_VOICE_STOP, 62022);
    voice_core_tick(&s, 192022);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 4);
    setup(&s, &f); begin(&s, &f, 1);
    s.pcm_bytes = VOICE_PCM_MAX;
    voice_core_tick(&s, 22);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.closes == 1);
    setup(&s, &f); s.next_stream = 65536;
    action(&s, ORB_VOICE_START, 1);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.opens == 0);
}
static void test_cancel_retry_and_utf8(void)
{
    voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
    f.request_ok = false;
    action(&s, ORB_VOICE_CANCEL, 10);
    CHECK(!s.opened && s.cancel_stream && !s.cancel_waiting);
    f.request_ok = true; f.auto_cancel = false;
    voice_core_tick(&s, 1010);
    CHECK(s.cancel_stream && s.cancel_waiting && f.requests[VOICE_REQ_CANCEL] == 2);
    action(&s, ORB_VOICE_START, 1011);
    CHECK(s.view.phase == ORB_VOICE_ERROR && f.opens == 1);
    voice_core_reply(&s, VOICE_REQ_CANCEL, s.cancel_stream, 1, false, "timeout", 1012);
    voice_core_tick(&s, 2011);
    CHECK(f.requests[VOICE_REQ_CANCEL] == 2);
    voice_core_tick(&s, 2012);
    CHECK(f.requests[VOICE_REQ_CANCEL] == 3);
    voice_core_lost(&s, 2013);
    CHECK(s.cancel_waiting && f.requests[VOICE_REQ_CANCEL] == 4);
    voice_core_reply(&s, VOICE_REQ_CANCEL, s.cancel_stream, 1, true, NULL, 2014);
    CHECK(!s.cancel_stream);
    begin(&s, &f, 3000);
    voice_core_tick(&s, 3021); action(&s, ORB_VOICE_STOP, 3022);
    f.auto_cancel = true;
    voice_core_result(&s, s.view.stream, "\xed\xa0\x80", 3023);
    CHECK(s.view.phase == ORB_VOICE_ERROR && !s.view.text[0]);
    begin(&s, &f, 4000);
    voice_core_tick(&s, 4021); action(&s, ORB_VOICE_STOP, 4022);
    char text[281]; memset(text, 'a', 278); text[278] = (char)0xc3; text[279] = (char)0xa9; text[280] = 0;
    voice_core_result(&s, s.view.stream, text, 4023);
    CHECK(s.view.phase == ORB_VOICE_CONFIRMING && strlen(s.view.text) == 280);
    action(&s, ORB_VOICE_CONFIRM, 4024);
    voice_core_reply(&s, VOICE_REQ_SEND, s.view.stream, 1, false, "timeout", 4025);
    CHECK(s.view.phase == ORB_VOICE_ERROR && strlen(s.view.text) == 280);
}

static void test_adc_warmup(void)
{
    voice_core_t s; fake_t f; setup(&s, &f);
    action(&s, ORB_VOICE_START, 1);
    f.expected_stream = s.view.stream;
    voice_core_reply(&s, VOICE_REQ_BEGIN, s.view.stream, s.generation, true, NULL, 2);
    f.read_bytes = 600;
    for (unsigned n = 0; n < 5; ++n) voice_core_tick(&s, 1000 + n * 20);
    CHECK(s.warmup_bytes == 200 && !s.sequence && !s.pcm_bytes && !f.sends && !s.view.level);
    f.read_bytes = 0;
    voice_core_tick(&s, 1100);
    CHECK(s.warmup_bytes == 200 && !f.sends);
    f.read_bytes = 640;
    f.sample_offset = 200;
    f.expected_payload_bytes = 440;
    voice_core_tick(&s, 1120);
    CHECK(!s.warmup_bytes && s.sequence == 1 && s.pcm_bytes == 440 && f.sends == 1);
    CHECK(s.view.level == 100); /* The first retained sample is metered. */
    action(&s, ORB_VOICE_CANCEL, 1121);
    f.sample_offset = 0;
    f.expected_payload_bytes = 0;
    action(&s, ORB_VOICE_START, 1130);
    CHECK(s.warmup_bytes == VOICE_WARMUP_BYTES && !s.pcm_bytes);
    voice_core_reply(&s, VOICE_REQ_BEGIN, s.view.stream, s.generation, true, NULL, 1131);
    voice_core_tick(&s, 1150);
    CHECK(s.warmup_bytes == VOICE_WARMUP_BYTES - VOICE_PCM_BYTES && f.sends == 1);
    action(&s, ORB_VOICE_STOP, 1151);
    CHECK(s.view.phase == ORB_VOICE_IDLE && !f.requests[VOICE_REQ_END]);
}

static void test_error_codes_and_retry(void)
{
    voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
    voice_core_tick(&s, 22);
    action(&s, ORB_VOICE_STOP, 23);
    uint16_t stream = s.view.stream;
    voice_core_error(&s, stream + 1, "no_speech", 24);
    CHECK(s.view.phase == ORB_VOICE_TRANSCRIBING && !s.view.error_code[0]);
    voice_core_error(&s, stream, "no_speech", 25);
    CHECK(s.view.phase == ORB_VOICE_ERROR && !strcmp(s.view.error_code, "no_speech"));
    CHECK(!strcmp(s.view.error, "Hold and speak") && !s.opened && !s.view.text[0]);
    begin(&s, &f, 30);
    CHECK(!s.view.error_code[0] && !s.view.error[0] && s.view.stream == stream + 1);
    voice_core_connection(&s, false, 1, 31);
    CHECK(!strcmp(s.view.error_code, "disconnected") && strstr(s.view.error, "disconnected"));
    voice_core_connection(&s, true, 1, 32);
    action(&s, ORB_VOICE_START, 40);
    voice_core_reply(&s, VOICE_REQ_BEGIN, s.view.stream, 1, false, "unsupported", 41);
    CHECK(s.view.phase == ORB_VOICE_ERROR && !strcmp(s.view.error_code, "unsupported"));
    CHECK(!strcmp(s.view.error, "Voice unavailable on the hub."));
    begin(&s, &f, 50);
    voice_core_error(&s, s.view.stream, "provider/private data", 51);
    CHECK(s.view.phase == ORB_VOICE_ERROR && !s.view.error_code[0]);
    CHECK(!strcmp(s.view.error, "Voice failed. Try again."));
    begin(&s, &f, 60);
    voice_core_error(&s, s.view.stream, "abcdefghijklmnopqrstuvwxyzabcdefg", 61);
    CHECK(!s.view.error_code[0]);
    begin(&s, &f, 70);
    voice_core_error(&s, s.view.stream, "not_listening", 71);
    CHECK(!strcmp(s.view.error_code, "not_listening") && strstr(s.view.error, "Dot isn't listening"));
    action(&s, ORB_VOICE_CANCEL, 72);
    CHECK(s.view.phase == ORB_VOICE_IDLE && !s.view.error_code[0] && !s.view.error[0]);
}

static void test_boot_redo(void)
{
    voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
    voice_core_tick(&s, 22);
    action(&s, ORB_VOICE_STOP, 23);
    uint16_t old = s.view.stream;
    voice_core_result(&s, old, "old transcript", 24);
    strcpy(s.view.target, "original-target");
    f.auto_cancel = false;
    voice_core_boot(&s, true, 25);
    CHECK(s.view.phase == ORB_VOICE_IDLE && !s.view.text[0] && s.redo_pending);
    CHECK(s.cancel_stream == old && f.requests[VOICE_REQ_CANCEL] == 1);
    action(&s, ORB_VOICE_CONFIRM, 26);
    CHECK(!f.requests[VOICE_REQ_SEND]);
    voice_core_tick(&s, 375);
    CHECK(!s.boot_started && s.redo_pending && f.opens == 1);
    voice_core_reply(&s, VOICE_REQ_CANCEL, old, s.generation, true, NULL, 376);
    voice_core_tick(&s, 377);
    CHECK(s.view.phase == ORB_VOICE_STARTING && s.view.stream == old + 1);
    CHECK(!strcmp(s.view.target, "original-target") && s.boot_started && !s.redo_pending);
    voice_core_boot(&s, false, 378);
    CHECK(s.view.phase == ORB_VOICE_IDLE);
    setup(&s, &f); begin(&s, &f, 1);
    voice_core_tick(&s, 22);
    action(&s, ORB_VOICE_STOP, 23);
    voice_core_result(&s, s.view.stream, "old transcript", 24);
    voice_core_boot(&s, true, 25);
    voice_core_boot(&s, false, 30);
    voice_core_tick(&s, 400);
    CHECK(!s.redo_pending && f.opens == 1 && s.view.phase == ORB_VOICE_IDLE);
}

static void test_rejected_delivery_and_transcription(void)
{
    static const struct { const char *code, *message; } failures[] = {
        {"not_found", "Conversation ended. Nothing sent."},
        {"expired", "Recording expired. Nothing sent."},
        {"not_listening", "Dot isn't connected. Nothing sent."},
        {"unsupported", "Conversation can't receive messages."},
        {"busy", "Conversation busy. Nothing sent."},
        {"timeout", "Delivery unconfirmed. Check the conversation."},
        {"internal", "Delivery unconfirmed. Check the conversation."},
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
        voice_core_tick(&s, 22); action(&s, ORB_VOICE_STOP, 23);
        voice_core_result(&s, s.view.stream, "Keep my transcript for review", 24);
        action(&s, ORB_VOICE_CONFIRM, 25);
        uint16_t stream = s.view.stream;
        voice_core_reply(&s, VOICE_REQ_SEND, stream, 1, false, failures[i].code, 26);
        CHECK(s.view.phase == ORB_VOICE_ERROR && !s.view.stream && !s.opened);
        CHECK(!strcmp(s.view.error, failures[i].message));
        CHECK(!strcmp(s.view.text, "Keep my transcript for review"));
        voice_core_reply(&s, VOICE_REQ_SEND, stream, 1, true, NULL, 27);
        action(&s, ORB_VOICE_CONFIRM, 28); voice_core_tick(&s, 10000);
        CHECK(s.view.phase == ORB_VOICE_ERROR && f.requests[VOICE_REQ_SEND] == 1);
    }
    voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
    voice_core_tick(&s, 22); action(&s, ORB_VOICE_STOP, 23);
    voice_core_error(&s, s.view.stream, "internal", 24);
    CHECK(!strcmp(s.view.error, "Transcription failed. Try again."));
    CHECK(!s.view.text[0] && !f.requests[VOICE_REQ_SEND]);
}

/* Production BOOT uses model START/STOP, not the legacy raw-edge adapter. */
static void test_command_redo(void)
{
    for (unsigned confirming = 0; confirming < 2; ++confirming) {
        voice_core_t s; fake_t f; setup(&s, &f); begin(&s, &f, 1);
        voice_core_tick(&s, 22); action(&s, ORB_VOICE_STOP, 23);
        uint16_t old = s.view.stream;
        if (confirming) voice_core_result(&s, old, "Old transcript", 24);
        f.auto_cancel = false;
        orb_voice_request_t redo = {.action = ORB_VOICE_START, .target = "original-target"};
        voice_core_action(&s, &redo, 25);
        CHECK(s.view.phase == ORB_VOICE_IDLE && !s.view.text[0] && s.cancel_stream == old);
        CHECK(f.requests[VOICE_REQ_CANCEL] == 1 && !f.requests[VOICE_REQ_SEND]);
        voice_core_tick(&s, 400);
        CHECK(f.opens == 1);
        voice_core_result(&s, old, "Late old result", 401);
        voice_core_reply(&s, VOICE_REQ_CANCEL, old, s.generation, true, NULL, 402);
        voice_core_tick(&s, 403);
        CHECK(s.view.phase == ORB_VOICE_STARTING && s.view.stream == old + 1);
        CHECK(!strcmp(s.view.target, "original-target") && f.opens == 2);
        action(&s, ORB_VOICE_STOP, 404);
        CHECK(s.view.phase == ORB_VOICE_IDLE && f.closes == 2 && !f.requests[VOICE_REQ_SEND]);

        /* Releasing BOOT before cleanup completes must prevent delayed capture. */
        setup(&s, &f); begin(&s, &f, 1);
        voice_core_tick(&s, 22); action(&s, ORB_VOICE_STOP, 23);
        old = s.view.stream;
        if (confirming) voice_core_result(&s, old, "Old transcript", 24);
        f.auto_cancel = false;
        voice_core_action(&s, &redo, 25);
        action(&s, ORB_VOICE_STOP, 26);
        voice_core_reply(&s, VOICE_REQ_CANCEL, old, s.generation, true, NULL, 27);
        voice_core_tick(&s, 400);
        CHECK(s.view.phase == ORB_VOICE_IDLE && f.opens == 1 && !f.requests[VOICE_REQ_SEND]);
    }
}

static void test_sleep_wait(void)
{
    voice_core_t core = {0};
    CHECK(voice_wait_ms(&core, false, false) == 20);
    CHECK(voice_wait_ms(&core, false, true) == 0);
    core.view.phase = ORB_VOICE_CONFIRMING;
    CHECK(voice_wait_ms(&core, false, true) == 0);
    CHECK(voice_wait_ms(&core, true, true) == 20);
    core.boot_held = true;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.boot_held = false; core.cancel_stream = 1;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.cancel_stream = 0; core.redo_pending = true;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.redo_pending = false; core.command_redo_pending = true;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.view.phase = ORB_VOICE_RECORDING;
    CHECK(voice_wait_ms(&core, false, true) == 1);
    core.view.phase = ORB_VOICE_STARTING;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.view.phase = ORB_VOICE_TRANSCRIBING;
    CHECK(voice_wait_ms(&core, false, true) == 20);
    core.view.phase = ORB_VOICE_SENDING;
    CHECK(voice_wait_ms(&core, false, true) == 20);
}

int main(void)
{
    test_sleep_wait();
    CHECK(voice_cancel_absent("not_found"));
    CHECK(voice_cancel_absent("expired"));
    CHECK(!voice_cancel_absent(NULL));
    CHECK(!voice_cancel_absent("busy"));
    CHECK(!voice_cancel_absent("timeout"));
    CHECK(!voice_cancel_absent("internal"));
    test_adc_warmup(); test_error_codes_and_retry(); test_boot_redo(); test_command_redo(); test_rejected_delivery_and_transcription();
    test_frames(); test_lifecycle(); test_failure_and_reconnect(); test_deadlines_boot_and_cancel(); test_cancel_retry_and_utf8();
    printf("voice: %u checks passed (framing, fake capture, confirmation, cancellation, reconnect, deadlines)\n", s_checks);
}
