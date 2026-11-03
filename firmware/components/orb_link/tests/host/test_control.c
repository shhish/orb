#include <stdint.h>
#include "test.h"
#include "link_control.h"

void test_control(void)
{
    for (uint32_t epoch = 1; epoch <= 200; epoch++) {
        link_control_t state = { .conn = epoch };
        CHECK(link_control_observe(&state, epoch, LINK_CONTROL_UP));
        uint32_t sequence = 0;
        /* A full payload queue cannot discard DOWN, but accepted pair.done
         * and other final events still have to dispatch before teardown. */
        for (unsigned n = 1; n <= 32; n++) {
            CHECK(link_control_accept_frame(&state, epoch, &sequence));
            CHECK(sequence == n);
        }
        CHECK(link_control_observe(&state, epoch, LINK_CONTROL_DOWN));
        CHECK(!link_control_accept_frame(&state, epoch, &sequence));
        for (unsigned consumed = 0; consumed < 32; consumed++) {
            CHECK(!link_control_should_end(&state, consumed));
        }
        CHECK(link_control_should_end(&state, 32));
        CHECK(link_control_observe(&state, epoch, LINK_CONTROL_UP));
        CHECK(link_control_should_end(&state, 32));

        state = (link_control_t){ .conn = epoch + 1 };
        CHECK(!link_control_observe(&state, epoch, LINK_CONTROL_DOWN));
        CHECK(!link_control_observe(&state, epoch, LINK_CONTROL_LOST_FRAME));
        CHECK(!link_control_accept_frame(&state, epoch, &sequence));
        CHECK(!state.ended && !state.lost_frame && state.accepted_frames == 0);
        CHECK(link_control_observe(&state, epoch + 1, LINK_CONTROL_UP));
        for (unsigned n = 1; n <= 33; n++) CHECK(link_control_accept_frame(&state, epoch + 1, &sequence));
        /* The 33rd accepted frame could not enter the 32-slot queue: do not
         * wait for it forever or keep rendering an incomplete state stream. */
        CHECK(link_control_observe(&state, epoch + 1, LINK_CONTROL_LOST_FRAME));
        CHECK(link_control_should_end(&state, 0));
        CHECK(link_control_should_end(&state, 32));
        CHECK(!link_control_accept_frame(&state, epoch + 1, &sequence));
    }
    link_control_t wrap = { .conn = 9, .opened = true, .accepted_frames = UINT32_MAX };
    uint32_t sequence;
    CHECK(link_control_accept_frame(&wrap, 9, &sequence));
    CHECK(sequence == 0);
    CHECK(link_control_observe(&wrap, 9, LINK_CONTROL_DOWN));
    CHECK(!link_control_should_end(&wrap, UINT32_MAX));
    CHECK(link_control_should_end(&wrap, 0));
}
