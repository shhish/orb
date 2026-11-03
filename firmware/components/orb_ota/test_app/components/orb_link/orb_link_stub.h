#pragma once

/* Test hooks of the orb_link stub: test_main.c plays the hub through these. */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* Delivers a hub → device event (params as JSON text) to its handler. */
void orb_link_stub_event(const char *name, const char *params_json);

/* A hello as orb_link would send it, after the hello hooks ran. Caller frees. */
cJSON *orb_link_stub_hello(void);

/* Events the device sent so far; the last one's params go in last as JSON text. */
int orb_link_stub_sent(char *last, size_t len);
