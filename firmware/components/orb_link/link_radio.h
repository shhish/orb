#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* The adapter calls these on the link task except rx/tx and the hint observer.
 * A queue wake is advisory: state remains latched outside the payload queue. */
void link_radio_init(void (*wake)(void));
uint32_t link_radio_wait_begin(unsigned attempt, uint32_t random);
bool link_radio_wait_prompt(void);
int64_t link_radio_wait_until(int64_t until, int64_t now);
void link_radio_wait_end(void);
void link_radio_reset(void);
bool link_radio_hello(cJSON *hello);
void link_radio_accept(const cJSON *result);
bool link_radio_changed(void);
bool link_radio_report(cJSON *report);
void link_radio_report_sent(void);
unsigned link_radio_ping_s(void);
bool link_radio_negotiated(void);
uint32_t link_radio_model_poll_ms(void);
bool link_radio_model_prompt(void);
void link_radio_rx(size_t bytes);
void link_radio_tx(size_t bytes);
void link_radio_connect(void);
