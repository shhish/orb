#include <assert.h>
#include <stdio.h>
#include "orb_display_slow.h"

int main(void)
{
    orb_display_phase_clock_t clock = {0};
    orb_display_slow_handler_t sample = {0}, worst[ORB_DISPLAY_SLOW_HANDLERS] = {0};
    orb_display_phase_mark(&clock, 10, 2);
    assert(!clock.active && !clock.phase_at);
    orb_display_phase_begin(&clock, 100);
    orb_display_phase_timer(&clock, 100, 0x42, true);
    orb_display_phase_mark(&clock, 200, 1);
    orb_display_phase_mark(&clock, 300, 2);
    orb_display_phase_mark(&clock, 400, ORB_DISPLAY_PHASES); /* invalid mark has no effect */
    orb_display_phase_mark(&clock, 500, 4);
    orb_display_phase_mark(&clock, 1000, 6);
    orb_display_phase_mark(&clock, 33100, 0);
    orb_display_phase_timer(&clock, 33101, 0x42, false);
    orb_display_phase_timer(&clock, 33101, 0x43, true);
    orb_display_phase_timer(&clock, 33105, 0x43, false);
    orb_display_phase_finish(&clock, 33200, &sample);
    assert(!clock.active);
    assert(sample.phase_us[0] == 200 && sample.phase_us[1] == 100);
    assert(sample.phase_us[2] == 200 && sample.phase_us[4] == 500 && sample.phase_us[6] == 32100);
    assert(sample.timer_us == 33001 && sample.timer_callback == 0x42);
    uint64_t sum = 0;
    for (unsigned i = 0; i < ORB_DISPLAY_PHASES; i++) sum += sample.phase_us[i];
    assert(sum == 33100);
    sample.handler_us = 33000;
    orb_display_slow_insert(worst, &sample);
    assert(!worst[0].handler_us);
    for (unsigned i = 0; i < 10; i++) {
        sample.handler_us = 33001 + i * 10;
        sample.sequence = i;
        sample.moving = i & 1;
        orb_display_slow_insert(worst, &sample);
    }
    for (unsigned i = 0; i < ORB_DISPLAY_SLOW_HANDLERS; i++) {
        assert(worst[i].handler_us == 33091 - i * 10 && worst[i].sequence == 9 - i);
        assert(worst[i].phase_us[6] == 32100 && worst[i].timer_callback == 0x42);
    }
    sample.handler_us = 33091;
    sample.sequence = 42;
    orb_display_slow_insert(worst, &sample); /* stable tie retains the first worst receipt */
    assert(worst[0].sequence == 9);
    orb_display_phase_timer(&clock, 999999, 0x99, true);
    assert(clock.timer_max_us == 33001);
    assert(!strcmp(orb_display_phase_name(4), "raster"));
    assert(!strcmp(orb_display_phase_name(99), "unknown"));
    puts("slow handlers: exclusive phase sums, callback identity, threshold, bounded worst ordering passed");
}
