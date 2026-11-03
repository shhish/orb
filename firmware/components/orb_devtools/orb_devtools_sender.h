#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_devtools_diagnostics.h"

typedef struct {
    TaskHandle_t task;
    uint64_t clock_start, before_us, elapsed_us;
    bool begun, available;
} orb_devtools_sender_cpu_t;

/* UI-only, after orb_display_drain. The sender lives for the whole app; no
 * producer can wake its blocked queue while this caller owns LVGL. */
void orb_devtools_sender_begin(orb_devtools_sender_cpu_t *sample);
void orb_devtools_sender_end(orb_devtools_sender_cpu_t *sample);
void orb_devtools_sender_report(const orb_devtools_sender_cpu_t *sample, uint64_t frames,
                                  orb_devtools_report_fn report);
