#include "voice_core.h"

#include <string.h>

static void publish(voice_core_t *s)
{
    s->ops.publish(s->ctx, &s->view);
}

static void phase(voice_core_t *s, orb_voice_phase_t value, uint64_t now)
{
    s->view.phase = value;
    s->phase_ms = now;
    if (value != ORB_VOICE_RECORDING) s->view.level = 0;
    publish(s);
}

static void close_audio(voice_core_t *s)
{
    if (s->opened) s->ops.close(s->ctx);
    s->opened = false;
    memset(s->pcm, 0, sizeof(s->pcm));
    memset(s->frame, 0, sizeof(s->frame));
}

static void flush_cancel(voice_core_t *s, uint64_t now)
{
    if (!s->available || !s->cancel_stream || s->cancel_waiting || now < s->cancel_at) return;
    orb_voice_state_t view = s->view;
    view.stream = s->cancel_stream;
    s->cancel_waiting = true;
    if (!s->ops.request(s->ctx, VOICE_REQ_CANCEL, &view)) {
        s->cancel_waiting = false;
        s->cancel_at = now + 1000;
    }
}

static void error_code(orb_voice_state_t *view, const char *code)
{
    view->error_code[0] = 0;
    if (!code) return;
    size_t n = strlen(code);
    if (!n || n >= sizeof(view->error_code)) return;
    for (size_t i = 0; i < n; ++i)
        if ((code[i] < 'a' || code[i] > 'z') && code[i] != '_') return;
    memcpy(view->error_code, code, n + 1);
}

static void cancel(voice_core_t *s, const char *code, const char *error, uint64_t now)
{
    close_audio(s);
    if (s->available && s->view.stream) {
        s->cancel_stream = s->view.stream;
        s->cancel_at = now;
        flush_cancel(s, now);
    }
    if (!error || s->view.phase != ORB_VOICE_SENDING)
        memset(s->view.text, 0, sizeof(s->view.text));
    s->view.error[0] = 0;
    error_code(&s->view, code);
    if (error) memcpy(s->view.error, error, strlen(error) + 1);
    s->view.stream = 0;
    phase(s, error ? ORB_VOICE_ERROR : ORB_VOICE_IDLE, now);
}

void voice_core_init(voice_core_t *s, const voice_ops_t *ops, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->ctx = ctx;
    s->next_stream = 1;
    publish(s);
}

void voice_core_connection(voice_core_t *s, bool available, uint32_t generation, uint64_t now)
{
    if (generation != s->generation || !available) {
        /* A transcript belongs to its source connection. It can never be sent
         * after reconnect, even if the new connection reuses a stream number. */
        s->available = false;
        s->redo_pending = false;
        s->command_redo_pending = false;
        s->cancel_stream = 0;
        s->cancel_waiting = false;
        if (s->view.stream) cancel(s, "disconnected", "Hub disconnected. Try again.", now);
        if (generation != s->generation) s->next_stream = 1;
        s->generation = generation;
    }
    s->available = available;
}

static void start(voice_core_t *s, const orb_voice_request_t *request, uint64_t now)
{
    if (s->view.phase != ORB_VOICE_IDLE && s->view.phase != ORB_VOICE_ERROR) return;
    if (s->cancel_stream) {
        strcpy(s->view.error, "Finishing previous recording. Try again.");
        error_code(&s->view, "busy");
        phase(s, ORB_VOICE_ERROR, now);
        return;
    }
    memset(&s->view, 0, sizeof(s->view));
    if (!s->available || s->next_stream > UINT16_MAX) {
        cancel(s, "unsupported", "Voice unavailable. Reconnect and try again.", now);
        return;
    }
    memcpy(s->view.target, request->target, sizeof(s->view.target));
    memcpy(s->view.card, request->card, sizeof(s->view.card));
    if (!s->view.target[0] && !s->ops.focus(s->ctx, s->view.target, s->view.card)) {
        cancel(s, "not_found", "Choose a conversation first.", now);
        return;
    }
    s->view.stream = (uint16_t)s->next_stream++;
    s->sequence = 0;
    s->pcm_bytes = 0;
    s->warmup_bytes = VOICE_WARMUP_BYTES;
    s->started_ms = now;
    phase(s, ORB_VOICE_STARTING, now);
    s->opened = s->ops.open(s->ctx);
    if (!s->opened) {
        cancel(s, "microphone", "Microphone unavailable. Try again.", now);
        return;
    }
    if (!s->ops.request(s->ctx, VOICE_REQ_BEGIN, &s->view))
        cancel(s, "busy", "Hub busy. Try again.", now);
}

void voice_core_action(voice_core_t *s, const orb_voice_request_t *request, uint64_t now)
{
    switch (request->action) {
    case ORB_VOICE_START:
        if (s->view.phase == ORB_VOICE_CONFIRMING || s->view.phase == ORB_VOICE_TRANSCRIBING) {
            /* Production BOOT already crossed its hold threshold before it
             * queues START. Retire the old result, then wait for cleanup without
             * requiring the legacy raw-edge adapter's second hold detector. */
            s->redo_request = *request;
            if (!s->redo_request.target[0]) {
                strcpy(s->redo_request.target, s->view.target);
                strcpy(s->redo_request.card, s->view.card);
            }
            s->redo_pending = false;
            s->command_redo_pending = true;
            cancel(s, NULL, NULL, now);
            break;
        }
        start(s, request, now);
        break;
    case ORB_VOICE_STOP:
        /* Release owns the pending restart too: cleanup may outlast the hold. */
        s->command_redo_pending = false;
        if (s->view.phase == ORB_VOICE_STARTING) cancel(s, NULL, NULL, now);
        else if (s->view.phase == ORB_VOICE_RECORDING) {
            close_audio(s);
            if (!s->pcm_bytes) cancel(s, NULL, NULL, now);
            else {
                phase(s, ORB_VOICE_TRANSCRIBING, now);
                if (!s->ops.request(s->ctx, VOICE_REQ_END, &s->view))
                    cancel(s, "busy", "Hub busy. Try again.", now);
            }
        }
        break;
    case ORB_VOICE_CANCEL:
        s->redo_pending = false;
        s->command_redo_pending = false;
        if (s->view.phase != ORB_VOICE_SENDING) cancel(s, NULL, NULL, now);
        break;
    case ORB_VOICE_CONFIRM:
        if (s->view.phase == ORB_VOICE_CONFIRMING) {
            phase(s, ORB_VOICE_SENDING, now);
            bool ok = s->view.card[0] ? s->ops.answer(s->ctx, &s->view) :
                                      s->ops.request(s->ctx, VOICE_REQ_SEND, &s->view);
            if (!ok) cancel(s, "busy", "Could not send. Try again.", now);
            else if (s->view.card[0]) {
                /* The durable card answer queue now owns delivery/retry. */
                cancel(s, NULL, NULL, now);
            }
        }
        break;
    }
}

void voice_core_boot(voice_core_t *s, bool held, uint64_t now)
{
    if (s->boot_held == held) return;
    s->boot_held = held;
    if (held) {
        s->boot_ms = now;
        s->boot_started = false;
        if (s->view.phase == ORB_VOICE_CONFIRMING) {
            /* BOOT immediately withdraws the old transcript, so a concurrent
             * countdown cannot send it during the hold threshold. Restart only
             * after cancellation is acknowledged and the physical hold remains. */
            memset(&s->redo_request, 0, sizeof(s->redo_request));
            s->redo_request.action = ORB_VOICE_START;
            strcpy(s->redo_request.target, s->view.target);
            strcpy(s->redo_request.card, s->view.card);
            cancel(s, NULL, NULL, now);
            s->redo_pending = true;
        }
    } else {
        s->redo_pending = false;
        orb_voice_request_t request = {0};
        if (s->boot_started) request.action = ORB_VOICE_STOP;
        else if (now - s->boot_ms < 350 && s->view.phase != ORB_VOICE_IDLE)
            request.action = ORB_VOICE_CANCEL;
        else return;
        voice_core_action(s, &request, now);
        s->boot_started = false;
    }
}

size_t voice_frame(uint8_t *out, size_t capacity, uint16_t stream, uint32_t sequence,
                   const uint8_t *pcm, size_t bytes)
{
    if (!out || !pcm || !stream || !bytes || bytes > 1024 || (bytes & 1) || capacity < bytes + 7)
        return 0;
    out[0] = 1;
    out[1] = stream >> 8;
    out[2] = stream & 255;
    out[3] = sequence >> 24;
    out[4] = sequence >> 16;
    out[5] = sequence >> 8;
    out[6] = sequence & 255;
    memcpy(out + 7, pcm, bytes);
    return bytes + 7;
}

void voice_core_tick(voice_core_t *s, uint64_t now)
{
    flush_cancel(s, now);
    if (s->command_redo_pending && !s->cancel_stream) {
        s->command_redo_pending = false;
        start(s, &s->redo_request, now);
    }
    if (s->boot_held && !s->boot_started && now - s->boot_ms >= 350 &&
        (s->view.phase == ORB_VOICE_IDLE || s->view.phase == ORB_VOICE_ERROR)) {
        if (!s->cancel_stream) {
            orb_voice_request_t request = s->redo_pending ? s->redo_request :
                                                   (orb_voice_request_t){.action = ORB_VOICE_START};
            s->redo_pending = false;
            s->boot_started = true;
            start(s, &request, now);
        }
    }
    if ((s->view.phase == ORB_VOICE_STARTING || s->view.phase == ORB_VOICE_RECORDING) &&
        now - s->started_ms >= VOICE_CAPTURE_MS) {
        cancel(s, "too_large", "Recording reached 60 seconds. Try a shorter reply.", now);
        return;
    }
    if (s->view.phase == ORB_VOICE_TRANSCRIBING && now - s->phase_ms >= VOICE_RESULT_MS) {
        cancel(s, "timeout", "Transcription timed out. Try again.", now);
        return;
    }
    if (s->view.phase != ORB_VOICE_RECORDING) return;
    size_t bytes = s->ops.read(s->ctx, s->pcm, sizeof(s->pcm));
    if (bytes == SIZE_MAX || bytes > sizeof(s->pcm) || (bytes & 1)) {
        cancel(s, "microphone", "Microphone stopped. Try again.", now);
        return;
    }
    if (!bytes) return;
    /* The ES7210 power-up click must never reach transcription or the meter.
     * Count samples rather than elapsed time: a delayed begin/read still owns
     * the same buffered ADC transient, and reads may straddle the boundary. */
    size_t skip = bytes < s->warmup_bytes ? bytes : s->warmup_bytes;
    s->warmup_bytes -= skip;
    bytes -= skip;
    if (!bytes) return;
    const uint8_t *pcm = s->pcm + skip;
    if (s->pcm_bytes + bytes > VOICE_PCM_MAX) {
        cancel(s, "too_large", "Recording reached 60 seconds. Try a shorter reply.", now);
        return;
    }
    size_t n = voice_frame(s->frame, sizeof(s->frame), s->view.stream, s->sequence, pcm, bytes);
    if (!s->ops.audio(s->ctx, s->frame, n)) {
        cancel(s, "network", "Connection too slow. Try again.", now);
        return;
    }
    ++s->sequence;
    s->pcm_bytes += bytes;
    uint32_t peak = 0;
    for (size_t i = 0; i < bytes; i += 2) {
        int32_t sample = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
        uint32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
    }
    uint8_t level = (uint8_t)((peak * 100u) / 32768u);
    if (level != s->view.level) { s->view.level = level; publish(s); }
}

void voice_core_reply(voice_core_t *s, voice_request_kind_t kind, uint16_t stream,
                      uint32_t generation, bool ok, const char *code, uint64_t now)
{
    if (generation != s->generation || !stream) return;
    if (kind == VOICE_REQ_CANCEL) {
        if (stream == s->cancel_stream) {
            s->cancel_waiting = false;
            if (ok) s->cancel_stream = 0;
            else s->cancel_at = now + 1000;
        }
        return;
    }
    if (stream != s->view.stream) return;
    if ((kind == VOICE_REQ_BEGIN && s->view.phase != ORB_VOICE_STARTING) ||
        (kind == VOICE_REQ_END && s->view.phase != ORB_VOICE_TRANSCRIBING) ||
        (kind == VOICE_REQ_SEND && s->view.phase != ORB_VOICE_SENDING)) return;
    if (!ok) {
        /* message.send is not replayed after an ambiguous timeout: transport
         * acceptance may already have delivered this untrusted user text. */
        if (kind == VOICE_REQ_SEND) {
            /* A rejected request is different from a lost receipt. Only known
             * protocol rejections establish that delivery did not happen; a
             * timeout/internal transport failure must retain the uncertainty. */
            const char *message = "Delivery unconfirmed. Check the conversation.";
            if (!strcmp(s->view.target, "time")) message = "Reminder not saved. Check the details below.";
            else if (code && !strcmp(code, "not_found")) message = "Conversation ended. Nothing sent.";
            else if (code && !strcmp(code, "expired")) message = "Recording expired. Nothing sent.";
            else if (code && !strcmp(code, "not_listening")) message = "Dot isn't connected. Nothing sent.";
            else if (code && !strcmp(code, "unsupported")) message = "Conversation can't receive messages.";
            else if (code && !strcmp(code, "busy")) message = "Conversation busy. Nothing sent.";
            cancel(s, code, message, now);
        } else voice_core_error(s, stream, code, now);
        return;
    }
    if (kind == VOICE_REQ_BEGIN && s->view.phase == ORB_VOICE_STARTING)
        phase(s, ORB_VOICE_RECORDING, now);
    else if (kind == VOICE_REQ_SEND && s->view.phase == ORB_VOICE_SENDING)
        cancel(s, NULL, NULL, now);
}

static bool utf8(const char *text, size_t n)
{
    const uint8_t *bytes = (const uint8_t *)text;
    for (size_t i = 0; i < n;) {
        uint8_t c = bytes[i++];
        if (c < 0x80) continue;
        unsigned need;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (n - i < need) return false;
        while (need--) {
            c = bytes[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

void voice_core_result(voice_core_t *s, uint16_t stream, const char *text, uint64_t now)
{
    if (!stream || stream != s->view.stream || s->view.phase != ORB_VOICE_TRANSCRIBING) return;
    size_t n = text ? strlen(text) : 0;
    if (!n || n >= sizeof(s->view.text) || !utf8(text, n)) { voice_core_error(s, stream, "bad_request", now); return; }
    memcpy(s->view.text, text, n + 1);
    phase(s, ORB_VOICE_CONFIRMING, now);
}

void voice_core_error(voice_core_t *s, uint16_t stream, const char *code, uint64_t now)
{
    if (!stream || stream != s->view.stream) return;
    const char *message = "Voice failed. Try again.";
    if (code && !strcmp(code, "no_speech")) message = "Hold and speak";
    else if (code && !strcmp(code, "unsupported")) message = "Voice unavailable on the hub.";
    else if (code && !strcmp(code, "not_listening")) message = "Dot isn't listening. Try another conversation.";
    else if (code && !strcmp(code, "timeout")) message = "Transcription timed out. Try again.";
    else if (code && !strcmp(code, "busy")) message = "Hub busy. Try again.";
    else if (code && !strcmp(code, "internal") && s->view.phase == ORB_VOICE_TRANSCRIBING)
        message = "Transcription failed. Try again.";
    else if (code && !strcmp(code, "not_found")) message = "Conversation ended. Choose another.";
    else if (code && !strcmp(code, "expired")) message = "Recording expired. Try again.";
    else if (code && !strcmp(code, "too_large")) message = "Recording too long. Try a shorter reply.";
    cancel(s, code, message, now);
}

bool voice_cancel_absent(const char *code)
{
    return code && (!strcmp(code, "not_found") || !strcmp(code, "expired"));
}

void voice_core_lost(voice_core_t *s, uint64_t now)
{
    /* A callback queue overflow can lose the only cancel acknowledgment. Clear
     * that local wait as well as active capture, then retry idempotent cleanup. */
    s->cancel_waiting = false;
    s->cancel_at = now;
    if (s->view.stream) voice_core_error(s, s->view.stream, "internal", now);
    else flush_cancel(s, now);
}
