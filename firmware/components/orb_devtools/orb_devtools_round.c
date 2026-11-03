#include <stdio.h>
#include "esp_heap_caps.h"
#include "orb_devtools_round.h"

void orb_devtools_round_receipt(const char *surface, const orb_ui_scroll_diagnostics_t *ui,
                                const orb_display_cadence_t *cadence, orb_devtools_round_reply_t reply)
{
    /* Stages overlap deliberately: layout/hash/preparation can nest in round,
     * and geometry also runs during draw. Outside is a subset of each us total. */
    reply("@@ORBDEV SCROLL_ROUND surface=%s phase_available=%u inclusive=1 round_us=%llu "
          "inside_refresh_us=%llu outside_refresh_us=%llu\n", surface, ui->refresh_phase_available,
          (unsigned long long)ui->round_us, (unsigned long long)ui->round_inside_refresh_us,
          (unsigned long long)ui->round_outside_refresh_us);
    const char *names[] = {"layout", "geometry", "hash", "source_prepare", "child_prepare", "palette_prepare",
                           "source_cuts", "source_snapshot", "source_pack", "source_lookahead"};
    const orb_ui_scroll_stage_t *stages[] = {&ui->layout, &ui->geometry, &ui->hash,
                                            &ui->source_prepare, &ui->child_prepare, &ui->palette_prepare,
                                            &ui->source_cuts, &ui->source_snapshot, &ui->source_pack,
                                            &ui->source_lookahead};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        const orb_ui_scroll_stage_t *stage = stages[i];
        reply("@@ORBDEV SCROLL_ROUND_STAGE surface=%s stage=%s inclusive=1 calls=%lu "
              "us=%llu max_us=%lu outside_us=%llu\n", surface, names[i], (unsigned long)stage->calls,
              (unsigned long long)stage->us, (unsigned long)stage->max_us, (unsigned long long)stage->outside_us);
    }
    for (unsigned i = 0; i < ORB_DISPLAY_PHASES; i++) {
        reply("@@ORBDEV SCROLL_PHASE surface=%s phase=%s total_us=%llu max_handler_us=%llu\n",
              surface, orb_display_phase_name(i), (unsigned long long)cadence->phase_us[i],
              (unsigned long long)cadence->phase_max_us[i]);
    }
}

double orb_devtools_percentile_upper_ms(const uint32_t *before, const uint32_t *after,
                                            unsigned buckets, unsigned bucket_us, unsigned percentile)
{
    uint64_t total = 0, seen = 0;
    for (unsigned i = 0; i < buckets; i++) total += (uint32_t)(after[i] - before[i]);
    if (!total) return -1;
    uint64_t rank = (total * percentile + 99) / 100;
    for (unsigned i = 0; i < buckets; i++) {
        seen += (uint32_t)(after[i] - before[i]);
        if (seen >= rank) return i + 1 == buckets ? -1 : (i + 1) * bucket_us / 1000.0;
    }
    return -1;
}

bool orb_devtools_distribution_report(const char *sample, const char *name,
                                          const orb_display_counters_t *before,
                                          const orb_display_counters_t *after, orb_devtools_round_reply_t reply)
{
    /* Emit after capture, without consuming the small console/UI stack. The
     * completion bins are independent of optional tracing/profiling switches. */
    size_t capacity = ORB_DISPLAY_INTERVAL_BUCKETS * 12u;
    char *histogram = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!histogram) return false;
    size_t used = 0;
    for (unsigned i = 0; i < ORB_DISPLAY_INTERVAL_BUCKETS; i++) {
        int wrote = snprintf(histogram + used, capacity - used, "%s%lu", i ? "," : "",
            (unsigned long)(uint32_t)(after->completion_interval[i] - before->completion_interval[i]));
        if (wrote < 0 || (size_t)wrote >= capacity - used) { heap_caps_free(histogram); return false; }
        used += (size_t)wrote;
    }
    uint64_t frames = after->frames - before->frames;
    uint64_t timed_frames = after->dma_timed_frames - before->dma_timed_frames;
    double count = frames ? (double)frames : 1;
    bool ok = reply("@@ORBDEV CADENCE sample=%s name=%s basis=%s bucket_us=1000 "
        "p50_upper_ms=%.3f p95_upper_ms=%.3f p99_upper_ms=%.3f te_available=%d torn=%llu "
        "half_frames=%llu full_frames=%llu "
        "dma_timed_frames=%llu dma_timed_bands=%llu dma_wire_ms=%.3f dma_queue_ms=%.3f dma_cleanup_ms=%.3f "
        "paced_scheduled=%llu paced_on_slot=%llu paced_late=%llu paced_skipped=%llu "
        "paced_period_us=%lu paced_divisor=%lu paced_lead_us=%lu paced_capacity_us=%lu counts=%s\n", sample, name,
        frames && timed_frames == frames ? "spi_isr_verified_endpoints" : "spi_result_fence_upper_bound",
        orb_devtools_percentile_upper_ms(before->completion_interval, after->completion_interval,
            ORB_DISPLAY_INTERVAL_BUCKETS, 1000, 50),
        orb_devtools_percentile_upper_ms(before->completion_interval, after->completion_interval,
            ORB_DISPLAY_INTERVAL_BUCKETS, 1000, 95),
        orb_devtools_percentile_upper_ms(before->completion_interval, after->completion_interval,
            ORB_DISPLAY_INTERVAL_BUCKETS, 1000, 99), orb_display_te_pacing_active(),
        (unsigned long long)(after->torn - before->torn),
        (unsigned long long)(after->half_frames - before->half_frames),
        (unsigned long long)(after->full_frames - before->full_frames),
        (unsigned long long)timed_frames,
        (unsigned long long)(after->dma_timed_bands - before->dma_timed_bands),
        (after->dma_wire_us - before->dma_wire_us) / count / 1000,
        (after->dma_queue_us - before->dma_queue_us) / count / 1000,
        (after->dma_cleanup_us - before->dma_cleanup_us) / count / 1000,
        (unsigned long long)(after->paced_scheduled - before->paced_scheduled),
        (unsigned long long)(after->paced_on_slot - before->paced_on_slot),
        (unsigned long long)(after->paced_late - before->paced_late),
        (unsigned long long)(after->paced_skipped - before->paced_skipped),
        (unsigned long)after->paced_period_us, (unsigned long)after->paced_divisor,
        (unsigned long)after->paced_lead_us, (unsigned long)after->paced_capacity_us, histogram);
    used = 0;
    for (unsigned i = 0; i < ORB_DISPLAY_LATENCY_BUCKETS; i++) {
        int wrote = snprintf(histogram + used, capacity - used, "%s%lu", i ? "," : "",
            (unsigned long)(uint32_t)(after->frame_latency[i] - before->frame_latency[i]));
        if (wrote < 0 || (size_t)wrote >= capacity - used) { heap_caps_free(histogram); return false; }
        used += (size_t)wrote;
    }
    ok = reply("@@ORBDEV LATENCY sample=%s name=%s basis=render_to_final_dma bucket_us=%u "
        "p50_upper_ms=%.3f p95_upper_ms=%.3f p99_upper_ms=%.3f counts=%s\n", sample, name,
        ORB_DISPLAY_LATENCY_BUCKET_US,
        orb_devtools_percentile_upper_ms(before->frame_latency, after->frame_latency,
            ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US, 50),
        orb_devtools_percentile_upper_ms(before->frame_latency, after->frame_latency,
            ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US, 95),
        orb_devtools_percentile_upper_ms(before->frame_latency, after->frame_latency,
            ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US, 99), histogram) && ok;
    heap_caps_free(histogram);
    return ok;
}
