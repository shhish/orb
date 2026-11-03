#pragma once

/* Platform-free capture state machine. One owner calls every operation; audio,
 * transport and model publication are injected so failure paths run on a host. */
#include "orb_model.h"

#define VOICE_PCM_BYTES 640u
#define VOICE_WARMUP_BYTES 3200u /* 100 ms at 16 kHz mono PCM16. */
#define VOICE_FRAME_MAX 1031u
#define VOICE_CAPTURE_MS 60000u
#define VOICE_RESULT_MS 130000u
#define VOICE_PCM_MAX 1920000u

typedef enum { VOICE_REQ_BEGIN, VOICE_REQ_END, VOICE_REQ_CANCEL, VOICE_REQ_SEND } voice_request_kind_t;
typedef struct {
    bool (*open)(void *ctx);
    void (*close)(void *ctx);
    /* SIZE_MAX means failure; zero is a finite-timeout read with no samples. */
    size_t (*read)(void *ctx, uint8_t *pcm, size_t capacity);
    bool (*audio)(void *ctx, const uint8_t *frame, size_t bytes);
    bool (*request)(void *ctx, voice_request_kind_t kind, const orb_voice_state_t *state);
    bool (*answer)(void *ctx, const orb_voice_state_t *state);
    void (*publish)(void *ctx, const orb_voice_state_t *state);
    bool (*focus)(void *ctx, char target[49], char card[65]);
} voice_ops_t;

typedef struct {
    orb_voice_state_t view;
    voice_ops_t ops;
    void *ctx;
    uint32_t generation, next_stream, sequence;
    uint64_t started_ms, phase_ms, boot_ms, cancel_at;
    uint16_t cancel_stream;
    bool cancel_waiting;
    size_t pcm_bytes, warmup_bytes;
    bool available, opened, boot_held, boot_started;
    bool redo_pending, command_redo_pending;
    orb_voice_request_t redo_request;
    _Alignas(4) uint8_t pcm[VOICE_PCM_BYTES];
    uint8_t frame[VOICE_FRAME_MAX];
} voice_core_t;

void voice_core_init(voice_core_t *core, const voice_ops_t *ops, void *ctx);
void voice_core_connection(voice_core_t *core, bool available, uint32_t generation, uint64_t now);
void voice_core_action(voice_core_t *core, const orb_voice_request_t *request, uint64_t now);
void voice_core_boot(voice_core_t *core, bool held, uint64_t now);
void voice_core_tick(voice_core_t *core, uint64_t now);
void voice_core_reply(voice_core_t *core, voice_request_kind_t kind, uint16_t stream,
                      uint32_t generation, bool ok, const char *code, uint64_t now);
void voice_core_result(voice_core_t *core, uint16_t stream, const char *text, uint64_t now);
void voice_core_error(voice_core_t *core, uint16_t stream, const char *code, uint64_t now);
void voice_core_lost(voice_core_t *core, uint64_t now);
size_t voice_frame(uint8_t *out, size_t capacity, uint16_t stream, uint32_t sequence,
                   const uint8_t *pcm, size_t bytes);

/* An absent/expired stream needs no further cancellation retry. */
bool voice_cancel_absent(const char *code);
