#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Starts one core-0 worker and registers link callbacks. Call after model init
 * and before link start. It never calls LVGL; the shell reads orb_model.voice. */
esp_err_t orb_voice_start(void);
/* Legacy raw-edge adapter for isolated callers. Production main uses the
 * orb_buttons recognizer and queues START/STOP directly through orb_model,
 * preserving BOOT short=Back without a second hold threshold. */
void orb_voice_boot(bool held);
/* Runtime sleep admission parks idle polling; physical/model/speech/link
 * producers still wake the worker. Link callbacks call wake on state changes. */
void orb_voice_set_sleep_eligible(bool eligible);
void orb_voice_wake(void);

/* Spoken-reply service, independent of capture/transcript confirmation. Calls
 * never execute an action or send captured speech. UI reads captions explicitly. */
typedef enum {
    ORB_SPEECH_STOP, ORB_SPEECH_DOUBLE_TAP, ORB_SPEECH_TALK,
} orb_speech_stop_t;
typedef struct {
    bool active, buffering;
    uint16_t stream;
    uint32_t rate, underruns;
    char text[1025];
} orb_speech_status_t;
void orb_voice_speech_stop(orb_speech_stop_t reason);
void orb_voice_speech_status(orb_speech_status_t *status);
