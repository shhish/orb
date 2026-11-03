#pragma once
#include "cJSON.h"
#include "orb_home.h"
#include "link_v11.h"

/* Home's bounded compact model, exactly as specified in docs/protocol.md.
 * Invalid messages disable Home until a fresh snapshot; never end the link. */
link_v11_result_t link_home_event(const char *name, const cJSON *params);
/* Static diagnostic category only, never any provider/payload content. */
const char *link_home_error(void);
void link_home_reset(void);
cJSON *link_home_request_params(const orb_home_request_t *request);
const char *link_home_request_method(const orb_home_request_t *request);
bool link_home_read_reply(const cJSON *result, const orb_home_request_t *request,
                          orb_home_state_t *accepted);
