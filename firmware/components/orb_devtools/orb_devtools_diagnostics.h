#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_game_view.h"

typedef int (*orb_devtools_report_fn)(const char *format, ...);
typedef bool (*orb_devtools_reply_fn)(const char *format, ...);

void orb_devtools_heap_report(orb_devtools_report_fn report, orb_devtools_reply_fn reply);
void orb_devtools_top(orb_devtools_report_fn report);

/* On-demand scratch only; an allocation refusal preserves the basic receipt. */
void orb_devtools_heap_detail(orb_devtools_report_fn report);

typedef struct {
    uint64_t clock_start, worker_us;
    bool worker_available;
} orb_devtools_game_cpu_t;

/* Counts the whole worker lifetime, including loop/pose/publication and IRQ
 * time charged to the task. This bounds solver CPU; it is not solver-only. */
void orb_devtools_game_cpu_begin(orb_devtools_game_cpu_t *cpu);
void orb_devtools_game_cpu_stop(orb_devtools_game_cpu_t *cpu, TaskHandle_t worker, bool joined);
void orb_devtools_game_cpu_report(const orb_devtools_game_cpu_t *cpu, uint64_t steps,
                                  orb_devtools_report_fn report);

void orb_devtools_game_beads_report(const orb_game_view_stats_t *before,
                                    const orb_game_view_stats_t *after, orb_devtools_report_fn report);
