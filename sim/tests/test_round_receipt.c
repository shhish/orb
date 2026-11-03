#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_devtools_round.h"

static char s_lines[8192];
static size_t s_length;
static unsigned s_count, s_checks;
static bool s_reject;
#define CHECK(value) do { ++s_checks; if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static bool reply(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(s_lines + s_length, sizeof(s_lines) - s_length, format, args);
    va_end(args);
    CHECK(written >= 0 && (size_t)written < sizeof(s_lines) - s_length);
    s_length += written;
    ++s_count;
    return !s_reject;
}

int main(void)
{
    orb_ui_scroll_diagnostics_t ui = {.refresh_phase_available = true, .round_us = 37,
        .round_inside_refresh_us = 11, .round_outside_refresh_us = 26,
        .hash = {.calls = 2, .max_us = 7, .us = 9, .outside_us = 3}};
    orb_display_cadence_t cadence = {0};
    cadence.phase_us[0] = 123;
    cadence.phase_max_us[0] = 19;
    orb_devtools_round_receipt("notifications", &ui, &cadence, reply);
    CHECK(s_count == 1 + 10 + ORB_DISPLAY_PHASES);
    CHECK(strstr(s_lines, "phase_available=1 inclusive=1 round_us=37 inside_refresh_us=11 outside_refresh_us=26\n"));
    CHECK(strstr(s_lines, "stage=hash inclusive=1 calls=2 us=9 max_us=7 outside_us=3\n"));
    CHECK(strstr(s_lines, "stage=source_prepare inclusive=1 calls=0 us=0 max_us=0 outside_us=0\n"));
    CHECK(strstr(s_lines, "stage=source_lookahead inclusive=1 calls=0 us=0 max_us=0 outside_us=0\n"));
    CHECK(strstr(s_lines, "phase=other total_us=123 max_handler_us=19\n"));
    s_length = s_count = 0;
    s_lines[0] = '\0';
    ui = (orb_ui_scroll_diagnostics_t){0};
    orb_devtools_round_receipt("settings", &ui, &cadence, reply);
    CHECK(strstr(s_lines, "phase_available=0 inclusive=1 round_us=0 inside_refresh_us=0 outside_refresh_us=0\n"));
    s_length = s_count = 0;
    s_lines[0] = '\0';
    orb_display_counters_t before = {.frames = 10, .dma_timed_frames = 10};
    orb_display_counters_t after = {.frames = 30, .dma_timed_frames = 30};
    after.completion_interval[4] = 10;
    after.completion_interval[9] = 10;
    after.frame_latency[1] = 20;
    CHECK(orb_devtools_distribution_report("nav", "iris", &before, &after, reply));
    CHECK(s_count == 2);
    CHECK(strstr(s_lines, "basis=spi_isr_verified_endpoints bucket_us=1000 p50_upper_ms=5.000 p95_upper_ms=10.000 p99_upper_ms=10.000"));
    CHECK(strstr(s_lines, "basis=render_to_final_dma bucket_us=4000 p50_upper_ms=8.000 p95_upper_ms=8.000 p99_upper_ms=8.000"));
    s_length = s_count = 0;
    s_lines[0] = '\0';
    s_reject = true;
    CHECK(!orb_devtools_distribution_report("nav", "iris", &before, &after, reply));
    CHECK(s_count == 2);
    CHECK(orb_devtools_percentile_upper_ms(before.frame_latency, before.frame_latency,
        ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US, 95) == -1);
    after.frame_latency[ORB_DISPLAY_LATENCY_BUCKETS - 1] = 1000;
    CHECK(orb_devtools_percentile_upper_ms(before.frame_latency, after.frame_latency,
        ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US, 95) == -1);
    printf("round receipt: %u checks passed\n", s_checks);
    return 0;
}
