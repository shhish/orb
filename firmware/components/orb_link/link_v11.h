#pragma once

/* Bounded, LVGL-free v1.1 mapping. Run event/reset on the link owner task.
 * INVALID requires a resync: silently losing state could leave an approval
 * looking current after the hub has withdrawn it. */
#include "cJSON.h"
#include "orb_model.h"

#define LINK_V11_MAX_MSG 4096

typedef enum { LINK_V11_IGNORED, LINK_V11_APPLIED, LINK_V11_INVALID, LINK_V11_RESYNC } link_v11_result_t;

link_v11_result_t link_v11_event(const char *name, const cJSON *params);
void link_v11_reset(void);

/* Builders return an owned object or NULL for invalid bounded content/OOM. */
cJSON *link_v11_answer_params(const orb_answer_t *answer);
cJSON *link_v11_message_params(const char *target, const char *text, uint16_t stream);

/* A durable first-answer-wins reply may carry another controller's answer.
 * Validate its card identity and copy the recorded decision, never insist that
 * it match what this device submitted. delivered:false is still terminal. */
bool link_v11_read_answer(const cJSON *result, const char *expected_card,
                          orb_answer_t *recorded, bool *delivered);

/* Missing hello prefs use protocol defaults; present prefs must be complete. */
bool link_v11_read_prefs(const cJSON *value, orb_prefs_t *out);
cJSON *link_v11_prefs_params(const orb_prefs_patch_t *patch);
cJSON *link_v11_idle_params(const orb_idle_request_t *request);
/* Validates the whole bounded page, then installs only for its open view. */
bool link_v11_apply_idle(const cJSON *result, const orb_idle_request_t *request);

/* Shared with the voice callback; acceptance alone is never delivery. */
bool orb_link_read_message_receipt(const cJSON *result, const cJSON *error,
                                    orb_message_result_t *out);

/* Additive Sound and Media proposal; capability gating is owned by link hello. */
bool link_v11_read_nowplaying(const cJSON *value, orb_nowplaying_t *out);
bool link_v11_read_weather(const cJSON *value, orb_weather_snapshot_t *out);
cJSON *link_v11_nowplaying_params(const orb_media_request_t *request);
bool link_v11_apply_nowplaying_reply(const cJSON *result, const orb_media_request_t *request);
