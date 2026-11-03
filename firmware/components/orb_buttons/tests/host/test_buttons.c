#include "orb_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static void expected(orb_buttons_t *buttons, orb_button_t button,
                     orb_button_gesture_t gesture, uint64_t at_ms, bool synthetic)
{
    orb_button_event_t out;
    CHECK(orb_buttons_next(buttons, &out));
    CHECK(out.button == button);
    CHECK(out.gesture == gesture);
    CHECK(out.at_ms == at_ms);
    CHECK(out.synthetic == synthetic);
}

static void empty(orb_buttons_t *buttons)
{
    orb_button_event_t out;
    CHECK(!orb_buttons_next(buttons, &out));
}

static void debounce(void)
{
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 10, 1);
    orb_buttons_poll(&buttons, 39, 1);
    empty(&buttons);
    orb_buttons_poll(&buttons, 39, 0);
    orb_buttons_poll(&buttons, 50, 1);
    orb_buttons_poll(&buttons, 79, 1);
    empty(&buttons);
    orb_buttons_poll(&buttons, 80, 1);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 80, false);
    orb_buttons_poll(&buttons, 90, 0);
    orb_buttons_poll(&buttons, 119, 0);
    empty(&buttons);
    orb_buttons_poll(&buttons, 119, 1);
    orb_buttons_poll(&buttons, 130, 0);
    orb_buttons_poll(&buttons, 160, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 160, false);
    orb_buttons_poll(&buttons, 459, 0);
    empty(&buttons);
    orb_buttons_poll(&buttons, 460, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_SHORT, 460, false);
    empty(&buttons);
    orb_button_edge_t edges[ORB_BUTTON_HISTORY];
    CHECK(orb_buttons_history(&buttons, edges) == 8);
    CHECK(edges[0].held && !edges[0].debounced && edges[0].at_ms == 10);
    CHECK(edges[3].held && edges[3].debounced && edges[3].at_ms == 80);
    CHECK(!edges[7].held && edges[7].debounced && edges[7].at_ms == 160);
}

static void click(orb_buttons_t *buttons, uint64_t at_ms, unsigned mask)
{
    orb_buttons_poll(buttons, at_ms, mask);
    orb_buttons_poll(buttons, at_ms + 30, mask);
    orb_buttons_poll(buttons, at_ms + 60, 0);
    orb_buttons_poll(buttons, at_ms + 90, 0);
}

static void double_click(void)
{
    for (unsigned i = 0; i < ORB_BUTTON_COUNT; i++) {
        orb_buttons_t buttons;
        orb_buttons_init(&buttons);
        click(&buttons, 10, 1u << i);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOWN, 40, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_UP, 100, false);
        /* Second debounced down at399: strictly under300ms from release. */
        click(&buttons, 369, 1u << i);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOWN, 399, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_UP, 459, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOUBLE, 459, false);
        orb_buttons_poll(&buttons, 1000, 0);
        empty(&buttons);
        orb_buttons_init(&buttons);
        click(&buttons, 10, 1u << i);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOWN, 40, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_UP, 100, false);
        click(&buttons, 370, 1u << i);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOWN, 400, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_SHORT, 400, false);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_UP, 460, false);
        orb_buttons_poll(&buttons, 760, 0);
        expected(&buttons, (orb_button_t)i, ORB_BUTTON_SHORT, 760, false);
        empty(&buttons);
    }
}

static void hold(void)
{
    for (unsigned i = 0; i < ORB_BUTTON_COUNT; i++) {
        orb_button_t button = (orb_button_t)i;
        unsigned duration = i == ORB_BUTTON_PWR ? 2000 : 350;
        orb_buttons_t buttons;
        orb_buttons_init(&buttons);
        orb_buttons_poll(&buttons, 10, 1u << i);
        orb_buttons_poll(&buttons, 40, 1u << i);
        expected(&buttons, button, ORB_BUTTON_DOWN, 40, false);
        orb_buttons_poll(&buttons, 39 + duration, 1u << i);
        empty(&buttons);
        orb_buttons_poll(&buttons, 40 + duration, 1u << i);
        expected(&buttons, button, ORB_BUTTON_HOLD, 40 + duration, false);
        orb_buttons_poll(&buttons, 10040, 1u << i);
        empty(&buttons); /* Hardware's10s cut has no repeated software action. */
        orb_buttons_poll(&buttons, 10050, 0);
        orb_buttons_poll(&buttons, 10080, 0);
        expected(&buttons, button, ORB_BUTTON_UP, 10080, false);
        orb_buttons_poll(&buttons, 11000, 0);
        empty(&buttons);
        /* A delayed main task still observes HOLD before UP. */
        orb_buttons_init(&buttons);
        orb_buttons_poll(&buttons, 0, 1u << i);
        orb_buttons_poll(&buttons, 30, 1u << i);
        expected(&buttons, button, ORB_BUTTON_DOWN, 30, false);
        orb_buttons_poll(&buttons, 30 + duration, 0);
        orb_buttons_poll(&buttons, 60 + duration, 0);
        expected(&buttons, button, ORB_BUTTON_HOLD, 30 + duration, false);
        expected(&buttons, button, ORB_BUTTON_UP, 60 + duration, false);
        empty(&buttons);
    }
}

static void simultaneous_and_history(void)
{
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 0, 3);
    orb_buttons_poll(&buttons, 30, 3);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 30, false);
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_DOWN, 30, false);
    orb_buttons_poll(&buttons, 380, 3);
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, 380, false);
    orb_buttons_poll(&buttons, 2030, 3);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_HOLD, 2030, false);
    orb_buttons_poll(&buttons, 2040, 0);
    orb_buttons_poll(&buttons, 2070, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 2070, false);
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_UP, 2070, false);
    for (uint64_t at_ms = 3000; at_ms < 5000; at_ms += 200) {
        click(&buttons, at_ms, 1);
        orb_button_event_t out;
        while (orb_buttons_next(&buttons, &out)) { }
    }
    orb_button_edge_t edges[ORB_BUTTON_HISTORY];
    CHECK(orb_buttons_history(&buttons, edges) == 16);
    CHECK(edges[0].at_ms == 4200);
    CHECK(edges[15].at_ms == 4890);
    for (unsigned i = 1; i < ORB_BUTTON_HISTORY; i++) CHECK(edges[i].at_ms >= edges[i - 1].at_ms);
}

static void synthetic(void)
{
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    CHECK(orb_buttons_inject(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, 10));
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_DOWN, 10, true);
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, 10, true);
    CHECK(!orb_buttons_inject(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_SHORT, 20));
    orb_buttons_poll(&buttons, 259, 0);
    empty(&buttons);
    orb_buttons_poll(&buttons, 260, 0);
    expected(&buttons, ORB_BUTTON_BOOT, ORB_BUTTON_UP, 260, true);
    for (unsigned i = 0; i < ORB_BUTTON_COUNT; i++) {
        for (unsigned kind = ORB_BUTTON_SHORT; kind <= ORB_BUTTON_DOUBLE; kind++) {
            CHECK(orb_buttons_inject(&buttons, (orb_button_t)i, (orb_button_gesture_t)kind, 300));
            expected(&buttons, (orb_button_t)i, ORB_BUTTON_DOWN, 300, true);
            expected(&buttons, (orb_button_t)i, (orb_button_gesture_t)kind, 300, true);
            expected(&buttons, (orb_button_t)i, ORB_BUTTON_UP, 300, true);
        }
    }
    CHECK(!orb_buttons_inject(&buttons, ORB_BUTTON_COUNT, ORB_BUTTON_SHORT, 0));
    CHECK(!orb_buttons_inject(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 0));
    orb_buttons_poll(&buttons, 500, 1);
    CHECK(!orb_buttons_inject(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_HOLD, 500));
    CHECK(!strcmp(orb_button_name(ORB_BUTTON_BOOT), "boot"));
    CHECK(!strcmp(orb_button_gesture_name(ORB_BUTTON_HOLD), "hold"));
}

static void wake_consumes_press(void)
{
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 0, 1);
    orb_buttons_poll(&buttons, 30, 1);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 30, false);
    orb_buttons_consume_press(&buttons, ORB_BUTTON_PWR);
    orb_buttons_poll(&buttons, 10030, 1);
    empty(&buttons);
    orb_buttons_poll(&buttons, 10050, 0);
    orb_buttons_poll(&buttons, 10080, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 10080, false);
    orb_buttons_poll(&buttons, 11000, 0);
    empty(&buttons);
    click(&buttons, 12000, 1);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 12030, false);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 12090, false);
    orb_buttons_poll(&buttons, 12390, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_SHORT, 12390, false);
    empty(&buttons);
    /* Consume the second click of a double: neither the first pending click
     * nor the second press may navigate after the wake gesture. */
    orb_buttons_init(&buttons);
    click(&buttons, 0, 1);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 30, false);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 90, false);
    orb_buttons_poll(&buttons, 100, 1);
    orb_buttons_poll(&buttons, 130, 1);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_DOWN, 130, false);
    orb_buttons_consume_press(&buttons, ORB_BUTTON_PWR);
    orb_buttons_poll(&buttons, 160, 0);
    orb_buttons_poll(&buttons, 190, 0);
    expected(&buttons, ORB_BUTTON_PWR, ORB_BUTTON_UP, 190, false);
    orb_buttons_poll(&buttons, 490, 0);
    empty(&buttons);
}

int main(void)
{
    debounce();
    double_click();
    hold();
    simultaneous_and_history();
    synthetic();
    wake_consumes_press();
    printf("buttons: %u checks passed\n", s_checks);
    return 0;
}
