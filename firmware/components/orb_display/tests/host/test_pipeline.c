/* Host regression: the UI may reuse its one buffer while both DMA snapshots
 * remain owned, and a failure must not free an in-flight DMA source. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_display_pipeline.h"

static unsigned s_checks;
#define CHECK(cond) do { s_checks++; if (!(cond)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

static void two_outstanding_bands(void)
{
    orb_pipeline_t p = { .enabled = true };
    unsigned char draw[37], snapshot[2][37];
    memset(draw, 0x12, sizeof(draw));
    CHECK(orb_pipeline_claim(&p, 0));
    memcpy(snapshot[0], draw, sizeof(draw));
    CHECK(orb_pipeline_queue(&p, 0));
    CHECK(orb_pipeline_begin(&p, 0));
    memset(draw, 0x34, sizeof(draw));
    CHECK(orb_pipeline_claim(&p, 1));
    memcpy(snapshot[1], draw, sizeof(draw));
    CHECK(orb_pipeline_queue(&p, 1));
    /* LVGL has already reused its source twice; neither DMA payload changes.
     * A third flush must wait rather than overwrite either snapshot. */
    memset(draw, 0x56, sizeof(draw));
    CHECK(!orb_pipeline_claim(&p, 0));
    CHECK(!orb_pipeline_claim(&p, 1));
    for (unsigned i = 0; i < sizeof(draw); i++) {
        CHECK(snapshot[0][i] == 0x12);
        CHECK(snapshot[1][i] == 0x34);
    }
    CHECK(!orb_pipeline_cancel_copy(&p, 0));
    CHECK(!orb_pipeline_cancel_copy(&p, 1));
    CHECK(!orb_pipeline_retire(&p, 1));
    /* The sender has now drained the first transfer; only its slot is free. */
    CHECK(orb_pipeline_retire(&p, 0));
    CHECK(orb_pipeline_claim(&p, 0));
    CHECK(!orb_pipeline_claim(&p, 1));
    CHECK(orb_pipeline_begin(&p, 1));
    CHECK(orb_pipeline_retire(&p, 1));
    CHECK(orb_pipeline_cancel_copy(&p, 0));
}

static void failure_during_next_copy(void)
{
    orb_pipeline_t p = { .enabled = true };
    CHECK(orb_pipeline_claim(&p, 0));
    CHECK(orb_pipeline_queue(&p, 0));
    CHECK(orb_pipeline_begin(&p, 0));
    CHECK(orb_pipeline_claim(&p, 1));
    /* Underflow is reported while the UI copies the next band. Disabling
     * requests recovery but does not erase either task's ownership. */
    orb_pipeline_disable(&p);
    CHECK(p.redraw && !p.enabled);
    CHECK(p.slot[0] == ORB_SLOT_IN_DMA);
    CHECK(p.slot[1] == ORB_SLOT_COPYING);
    CHECK(!orb_pipeline_queue(&p, 1));
    CHECK(orb_pipeline_cancel_copy(&p, 1));
    CHECK(!orb_pipeline_claim(&p, 1));
    CHECK(!orb_pipeline_cancel_copy(&p, 0));
    CHECK(orb_pipeline_retire(&p, 0));
    CHECK(!orb_pipeline_claim(&p, 0));
}

static void failure_with_queued_band(void)
{
    orb_pipeline_t p = { .enabled = true };
    for (unsigned i = 0; i < ORB_PIPELINE_SLOTS; i++) {
        CHECK(orb_pipeline_claim(&p, i));
        CHECK(orb_pipeline_queue(&p, i));
    }
    CHECK(orb_pipeline_begin(&p, 0));
    orb_pipeline_disable(&p);
    CHECK(p.slot[1] == ORB_SLOT_QUEUED);
    CHECK(!orb_pipeline_retire(&p, 1));
    CHECK(orb_pipeline_retire(&p, 0));
    /* Queued jobs still reach the FIFO sender. It discards the disabled
     * pipeline's job, then retires its slot without submitting it to DMA. */
    CHECK(orb_pipeline_begin(&p, 1));
    CHECK(!p.enabled);
    CHECK(orb_pipeline_retire(&p, 1));
    CHECK(p.slot[0] == ORB_SLOT_FREE && p.slot[1] == ORB_SLOT_FREE);
    CHECK(!orb_pipeline_claim(&p, 0));
}

static void complete_frame_accounting(void)
{
    orb_frame_result_t f = { 0 };
    CHECK(!orb_frame_complete(&f, 100, true, false));
    CHECK(!orb_frame_complete(&f, 100, false, false));
    /* A successful final band must not publish a frame or latency sample if
     * an earlier band failed (including a discarded queued PSRAM band). */
    CHECK(!orb_frame_complete(&f, 100, true, true));
    CHECK(!orb_frame_complete(&f, 200, true, false));
    CHECK(orb_frame_complete(&f, 200, true, true));
    CHECK(!orb_frame_complete(&f, 300, false, true));
    CHECK(orb_frame_complete(&f, 400, true, true));
    CHECK(!orb_frame_complete(&f, 500, false, false));
    CHECK(!orb_frame_complete(&f, 500, false, true));
    CHECK(orb_frame_complete(&f, 600, true, true));
}

int main(void)
{
    two_outstanding_bands();
    failure_during_next_copy();
    failure_with_queued_band();
    complete_frame_accounting();
    printf("pipeline: %u checks passed\n", s_checks);
    return 0;
}
