#include "orb_nav.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>

static orb_nav_t fixture(void)
{
    orb_nav_t nav;
    orb_nav_init(&nav, 1);
    assert(orb_nav_register(&nav, (orb_nav_place_t){1, ORB_NAV_ROLE_FACE, false}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){2, ORB_NAV_ROLE_AGENTS, true}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){3, ORB_NAV_ROLE_HOME, true}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){4, ORB_NAV_ROLE_STACK, true}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){5, ORB_NAV_ROLE_NOTIFICATIONS, true}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){6, ORB_NAV_ROLE_CONTROL, false}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){7, ORB_NAV_ROLE_SCREEN, true}));
    return nav;
}

static orb_nav_event_t feed(orb_nav_t *nav, orb_nav_touch_phase_t phase,
                            float x, float y, uint32_t ms)
{
    return orb_nav_touch(nav, phase, x, y, ms, true, true);
}

static void graph_and_stack(void)
{
    orb_nav_t nav = fixture();
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_LEFT) == 3);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_RIGHT) == 2);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_UP) == 4);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_DOWN) == 5);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_BACK) == ORB_NAV_INVALID_ID);
    assert(orb_nav_push(&nav, 2, NULL));
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_RIGHT) == ORB_NAV_INVALID_ID);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_LEFT) == ORB_NAV_INVALID_ID);
    orb_nav_token_t token = {100, 80, 72, 72};
    assert(orb_nav_push(&nav, 7, &token));
    orb_nav_back_entry_t entry;
    assert(orb_nav_back_peek(&nav, &entry));
    assert(entry.id == 2 && entry.iris && entry.token.x == 100);
    assert(nav.current == 7 && nav.back_count == 2);
    assert(orb_nav_back(&nav, &entry));
    assert(nav.current == 2 && nav.back_count == 1);
    assert(orb_nav_push(&nav, 4, NULL));
    assert(nav.layer_count == 1);
    assert(orb_nav_neighbor(&nav, ORB_NAV_GESTURE_DOWN) == 2);
    assert(orb_nav_back(&nav, NULL));
    assert(nav.layer_count == 0);
    for (unsigned i = 0; i < 40; i++) assert(orb_nav_push(&nav, i % 2 ? 7 : 2, NULL));
    assert(nav.back_count == ORB_NAV_BACK_CAPACITY);
    orb_nav_reset(&nav, 1);
    assert(!nav.back_count && !nav.layer_count && nav.current == 1);
    assert(!orb_nav_back(&nav, NULL));
}

static void repeated_layer_returns(void)
{
    orb_nav_t nav = fixture();
    for (unsigned run = 0; run < 24; run++) {
        /* bench_open's saved Controls -> Face -> Apps -> Play preparation
         * leaves a layer through a forward route before the next Back. */
        assert(orb_nav_push(&nav, 6, NULL));
        assert(nav.current == 6 && nav.layer_count == 1 && nav.layers[0] == 6);
        assert(orb_nav_push(&nav, 1, NULL));
        assert(nav.current == 1 && nav.layer_count == 0);
        assert(orb_nav_push(&nav, 2, NULL));
        orb_nav_token_t token = {197, 197, 72, 72};
        assert(orb_nav_push(&nav, 7, &token));
        orb_nav_back_entry_t previous;
        assert(orb_nav_back_peek(&nav, &previous) && previous.id == 2 && previous.iris);
        assert(orb_nav_back(&nav, NULL) && nav.current == 2 && nav.layer_count == 0);
        assert(orb_nav_back(&nav, NULL) && nav.current == 1 && nav.layer_count == 0);
        assert(orb_nav_back(&nav, NULL) && nav.current == 6 && nav.layer_count == 1);
        assert(orb_nav_back(&nav, NULL) && nav.current == 1 && nav.layer_count == 0);
    }
    assert(orb_nav_push(&nav, 4, NULL));
    assert(orb_nav_push(&nav, 6, NULL));
    assert(nav.layer_count == 2 && nav.layers[0] == 4 && nav.layers[1] == 6);
    assert(orb_nav_push(&nav, 7, NULL));
    assert(nav.layer_count == 0);
    assert(orb_nav_back(&nav, NULL));
    assert(nav.current == 6 && nav.layer_count == 2 && nav.layers[0] == 4 && nav.layers[1] == 6);
    assert(orb_nav_back(&nav, NULL) && nav.current == 4 && nav.layer_count == 1);
    assert(orb_nav_back(&nav, NULL) && nav.current == 1 && nav.layer_count == 0);
    assert(orb_nav_register(&nav, (orb_nav_place_t){8, ORB_NAV_ROLE_CONTROL, false}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){9, ORB_NAV_ROLE_CONTROL, false}));
    assert(orb_nav_push(&nav, 4, NULL));
    assert(orb_nav_push(&nav, 5, NULL));
    assert(orb_nav_push(&nav, 6, NULL));
    assert(orb_nav_push(&nav, 8, NULL));
    assert(nav.layer_count == ORB_NAV_LAYER_CAPACITY);
    assert(!orb_nav_push(&nav, 9, NULL));
    assert(nav.current == 8 && nav.layer_count == ORB_NAV_LAYER_CAPACITY);
    assert(orb_nav_push(&nav, 7, NULL) && nav.layer_count == 0);
    assert(orb_nav_back(&nav, NULL) && nav.current == 8 && nav.layer_count == ORB_NAV_LAYER_CAPACITY);
}

static void repeated_layer_identity(void)
{
    orb_nav_t nav = fixture();
    const orb_nav_token_t token = {100, 80, 72, 72};
    assert(orb_nav_push(&nav, 7, &token));
    /* Reopening an existing sheet reveals that sheet. Accumulating repeated
     * Notifications/Control/Stack identities used every bounded layer slot,
     * then refused the next route while the UI still changed surfaces. */
    for (unsigned cycle = 0; cycle < 40; cycle++) {
        assert(orb_nav_push(&nav, 5, NULL));
        assert(orb_nav_push(&nav, 6, NULL));
        assert(orb_nav_push(&nav, 4, NULL));
        assert(nav.back_count == 4 && nav.layer_count == 3);
        assert(orb_nav_push(&nav, 6, NULL));
        assert(nav.current == 6 && nav.back_count == 3 && nav.layer_count == 2);
        assert(nav.layers[0] == 5 && nav.layers[1] == 6);
        assert(orb_nav_push(&nav, 5, NULL));
        assert(nav.current == 5 && nav.back_count == 2 && nav.layer_count == 1);
        assert(nav.layers[0] == 5);
        assert(orb_nav_back(&nav, NULL) && nav.current == 7);
        assert(nav.back_count == 1 && nav.layer_count == 0);
    }
    orb_nav_back_entry_t previous;
    assert(orb_nav_back(&nav, &previous));
    assert(nav.current == 1 && previous.iris && previous.token.x == token.x);
    /* A plain place between sheets is a separate context, not a duplicate
     * in the active layer stack. Its Back path must survive re-entry. */
    assert(orb_nav_push(&nav, 5, NULL));
    assert(orb_nav_push(&nav, 7, NULL));
    assert(orb_nav_push(&nav, 5, NULL));
    assert(orb_nav_back(&nav, NULL) && nav.current == 7);
    assert(orb_nav_back(&nav, NULL) && nav.current == 5);
}

static void drag_ownership_and_velocity(void)
{
    orb_nav_t nav = fixture();
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 10);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 232, 233, 11);
    assert(event.type == ORB_NAV_EVENT_NONE && !orb_nav_touch_owned(&nav));
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 213, 233, 25);
    assert(event.type == ORB_NAV_EVENT_BEGIN && event.gesture == ORB_NAV_GESTURE_LEFT);
    assert(event.dx == -20 && event.progress > 0 && orb_nav_touch_owned(&nav));
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 160, 280, 40);
    assert(event.gesture == ORB_NAV_GESTURE_LEFT); /* No axis handoff. */
    event = feed(&nav, ORB_NAV_TOUCH_UP, 160, 280, 45);
    assert(event.type == ORB_NAV_EVENT_END && event.commit && !orb_nav_touch_owned(&nav));
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 60);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 50, 233, 200);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 110, 233, 220);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 110, 233, 222);
    assert(!event.commit && event.vx > 0); /* Reverse flick cancels. */
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 300);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 150, 233, 320);
    event = feed(&nav, ORB_NAV_TOUCH_CANCEL, 150, 233, 330);
    assert(event.type == ORB_NAV_EVENT_CANCEL && !orb_nav_touch_owned(&nav));
    event = feed(&nav, ORB_NAV_TOUCH_UP, 150, 233, 340);
    assert(event.type == ORB_NAV_EVENT_NONE);
    orb_nav_set_current(&nav, 7);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 400);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 280, 233, 420);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT); /* App paging is not Back. */
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 280, 233, 430);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, UINT32_MAX - 10);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 210, 5);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT && isfinite(event.vy));
}

static void centre_content_ownership(void)
{
    orb_nav_t nav = fixture();
    assert(orb_nav_register(&nav, (orb_nav_place_t){8, ORB_NAV_ROLE_SCREEN, false}));
    const int deltas[][2] = {{-130, 0}, {130, 0}, {0, -130}, {0, 130}, {80, 90}};
    const int starts[][2] = {{233, 233}, {140, 160}, {320, 170}, {140, 305}, {310, 310}};
    for (unsigned source = 2; source <= 8; source++) {
        orb_nav_set_current(&nav, source);
        for (unsigned point = 0; point < sizeof(starts) / sizeof(starts[0]); point++) {
            for (unsigned d = 0; d < sizeof(deltas) / sizeof(deltas[0]); d++) {
                for (unsigned at_top = 0; at_top < 2; at_top++) {
                    float x = starts[point][0], y = starts[point][1];
                    orb_nav_touch(&nav, ORB_NAV_TOUCH_DOWN, x, y, 0, false, at_top);
                    orb_nav_event_t event = orb_nav_touch(&nav, ORB_NAV_TOUCH_MOVE,
                        x + deltas[d][0], y + deltas[d][1], 100, false, at_top);
                    assert(event.type == ORB_NAV_EVENT_BEGIN);
                    assert(event.gesture == ORB_NAV_GESTURE_CONTENT);
                    assert(!orb_nav_touch_owned(&nav));
                    event = orb_nav_touch(&nav, ORB_NAV_TOUCH_UP,
                        x + deltas[d][0], y + deltas[d][1], 120, false, at_top);
                    assert(event.gesture == ORB_NAV_GESTURE_CONTENT && !event.commit);
                }
            }
        }
    }
}

static void velocity_polling(void)
{
    orb_nav_t nav = fixture();
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 233, 16);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 200, 233, 33);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 200, 233, 48);
    assert(event.vx < -680 && event.vx > -690);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 167, 233, 66);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 167, 233, 80);
    assert(event.vx == -825);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 167, 233, 90);
    assert(event.vx < -730 && event.vx > -740 && event.commit);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 100);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 200, 233, 133);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 200, 233, 214);
    assert(event.vx < 0); /* Held reads decay across the window, without a cliff. */
    event = feed(&nav, ORB_NAV_TOUCH_UP, 200, 233, 220);
    assert(!event.commit);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 200, 233, 240);
    assert(event.type == ORB_NAV_EVENT_NONE);
}

static orb_nav_event_t finger_trace(orb_nav_t *nav, float x, float y, float dx, float dy,
                                    unsigned duration, unsigned pause, bool decelerate,
                                    bool at_top)
{
    /* Production LVGL reads every16ms, while devtools produces10ms glides.
     * Round coordinates like the controller, curve8px perpendicular to travel,
     * and finish at the real endpoint before observing a delayed release. */
    orb_nav_touch(nav, ORB_NAV_TOUCH_DOWN, x, y, 0, false, at_top);
    float length = hypotf(dx, dy);
    float px = -dy / length, py = dx / length;
    unsigned now;
    for (now = 16; now < duration; now += 16) {
        float t = (float)now / duration;
        float progress = decelerate ? 1 - (1 - t) * (1 - t) : t;
        float curve = 8 * sinf(3.14159265f * t);
        orb_nav_touch(nav, ORB_NAV_TOUCH_MOVE, roundf(x + dx * progress + px * curve),
            roundf(y + dy * progress + py * curve), now, false, at_top);
    }
    orb_nav_touch(nav, ORB_NAV_TOUCH_MOVE, x + dx, y + dy, duration, false, at_top);
    for (now = 16; now <= pause; now += 16)
        orb_nav_touch(nav, ORB_NAV_TOUCH_MOVE, x + dx, y + dy, duration + now, false, at_top);
    return orb_nav_touch(nav, ORB_NAV_TOUCH_UP, x + dx, y + dy,
        duration + pause + 16, false, at_top);
}

static void natural_swipes(void)
{
    const struct {
        unsigned source;
        float x, y, dx, dy;
        orb_nav_gesture_t gesture;
    } paths[] = {
        {1, 300, 240, -130, 10, ORB_NAV_GESTURE_LEFT},
        {1, 160, 240, 130, 10, ORB_NAV_GESTURE_RIGHT},
        {1, 233, 180, 6, 130, ORB_NAV_GESTURE_DOWN},
        {1, 233, 300, 6, -130, ORB_NAV_GESTURE_UP},
        {4, 233, 12, 6, 130, ORB_NAV_GESTURE_DOWN},
        {5, 233, 454, 6, -130, ORB_NAV_GESTURE_UP},
        {6, 12, 240, 130, 10, ORB_NAV_GESTURE_BACK},
        {7, 12, 233, 130, 6, ORB_NAV_GESTURE_BACK},
        {1, 454, 233, -130, 6, ORB_NAV_GESTURE_CONTROL},
    };
    const unsigned durations[] = {200, 350, 600};
    const unsigned pauses[] = {0, 48, 128};
    for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
            for (unsigned h = 0; h < sizeof(pauses) / sizeof(pauses[0]); h++) {
                orb_nav_t nav = fixture();
                orb_nav_set_current(&nav, paths[p].source);
                orb_nav_event_t event = finger_trace(&nav, paths[p].x, paths[p].y,
                    paths[p].dx, paths[p].dy, durations[d], pauses[h], true, true);
                assert(event.type == ORB_NAV_EVENT_END && event.gesture == paths[p].gesture);
                assert(event.commit && isfinite(event.vx) && isfinite(event.vy));
                assert(fabsf(event.dx - paths[p].dx) < .01f && fabsf(event.dy - paths[p].dy) < .01f);
            }
        }
    }
    for (unsigned distance = 100; distance <= 200; distance += 50) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
            orb_nav_t nav = fixture();
            orb_nav_event_t event = finger_trace(&nav, 320, 240, -(float)distance, 8,
                durations[d], 0, false, true);
            assert(event.commit);
        }
    }
    for (unsigned distance = 125; distance <= 200; distance += 25) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
            for (unsigned h = 0; h < sizeof(pauses) / sizeof(pauses[0]); h++) {
                orb_nav_t nav = fixture();
                orb_nav_event_t event = finger_trace(&nav, 320, 240, -(float)distance, 8,
                    durations[d], pauses[h], true, true);
                assert(event.commit);
            }
        }
    }
    /* A fully held 100px stroke has neither quarter-screen travel nor lift
     * momentum. Keep that boundary explicit rather than retaining old speed. */
    for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
        orb_nav_t nav = fixture();
        orb_nav_event_t event = finger_trace(&nav, 320, 240, -100, 8,
            durations[d], 128, true, true);
        assert(!event.commit && fabsf(event.vx) < 1);
    }
    for (unsigned distance = 16; distance < 60; distance += 7) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
            orb_nav_t nav = fixture();
            orb_nav_event_t event = finger_trace(&nav, 300, 240, -(float)distance, 3,
                durations[d], 48, true, true);
            assert(!event.commit);
        }
    }

    orb_nav_t nav = fixture();
    /* A deliberate reverse before lift cancels the same held contact. */
    feed(&nav, ORB_NAV_TOUCH_DOWN, 300, 240, 1000);
    for (unsigned ms = 16; ms <= 320; ms += 16)
        feed(&nav, ORB_NAV_TOUCH_MOVE, 300 - 180.f * ms / 320, 240, 1000 + ms);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 130, 240, 1336);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 144, 240, 1352);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_UP, 158, 240, 1368);
    assert(!event.commit && event.progress > .25f);

    orb_nav_set_current(&nav, 2);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 1500);
    assert(feed(&nav, ORB_NAV_TOUCH_MOVE, 234, 234, 1516).type == ORB_NAV_EVENT_NONE);
    assert(feed(&nav, ORB_NAV_TOUCH_MOVE, 243, 243, 1532).gesture == ORB_NAV_GESTURE_CONTENT);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 244, 265, 1548);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT); /* List top remains content. */
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 244, 265, 1564);
    orb_nav_touch(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 1600, false, false);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 234, 232, 1616);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 244, 201, 1632);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT && !orb_nav_touch_owned(&nav));
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 100, 190, 1648);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT); /* Native list keeps its lock. */
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 100, 190, 1664);
}

static void resumed_navigation(void)
{
    orb_nav_t nav = fixture();
    assert(!orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_LEFT, .8f, false));
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
    assert(!orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_ROTARY, .8f, false));
    assert(!orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_LEFT, NAN, false));
    assert(orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_LEFT, .8f, false));
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 233, 20);
    assert(event.type == ORB_NAV_EVENT_UPDATE && event.progress == .8f && !event.commit);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 233, 233, 120);
    assert(event.type == ORB_NAV_EVENT_END && !event.commit && event.vx == 0);
    assert(!orb_nav_touch_owned(&nav));

    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 200);
    assert(orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_LEFT, .1f, true));
    event = feed(&nav, ORB_NAV_TOUCH_UP, 233, 233, 220);
    assert(event.commit && event.progress == .1f);

    const orb_nav_gesture_t gestures[] = {
        ORB_NAV_GESTURE_LEFT, ORB_NAV_GESTURE_RIGHT, ORB_NAV_GESTURE_UP,
        ORB_NAV_GESTURE_DOWN, ORB_NAV_GESTURE_BACK, ORB_NAV_GESTURE_CONTROL,
    };
    const int dx[] = {-100, 100, 0, 0, 100, -100};
    const int dy[] = {0, 0, -100, 100, 0, 0};
    for (unsigned i = 0; i < sizeof(gestures) / sizeof(gestures[0]); i++) {
        uint32_t ms = 300 + i * 500;
        float x = i == 4 ? 12 : i == 5 ? 454 : 233;
        orb_nav_set_current(&nav, i < 4 ? 1 : 7);
        feed(&nav, ORB_NAV_TOUCH_DOWN, x, 233, ms);
        assert(orb_nav_touch_resume(&nav, gestures[i], .2f, false));
        event = feed(&nav, ORB_NAV_TOUCH_MOVE, x + dx[i], 233 + dy[i], ms + 160);
        assert(event.commit && fabsf(event.progress - (.2f + 100.f / 466)) < .00001f);
        event = feed(&nav, ORB_NAV_TOUCH_UP, x + dx[i], 233 + dy[i], ms + 170);
        assert(event.commit);

        feed(&nav, ORB_NAV_TOUCH_DOWN, x, 233, ms + 200);
        assert(orb_nav_touch_resume(&nav, gestures[i], .8f, true));
        event = feed(&nav, ORB_NAV_TOUCH_UP, x - dx[i], 233 - dy[i], ms + 220);
        assert(!event.commit && event.progress > .32f); /* Reverse velocity wins. */
    }
    orb_nav_set_current(&nav, 1);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 3400);
    assert(orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_LEFT, .8f, true));
    event = feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 233, 3420);
    assert(event.type == ORB_NAV_EVENT_CANCEL && !event.commit && !orb_nav_touch_owned(&nav));
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 3500);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 223, 233, 3660);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 223, 233, 3670);
    assert(!event.commit && event.type == ORB_NAV_EVENT_NONE); /* Below direction-lock slop. */
}

static void edges_and_compass(void)
{
    orb_nav_t nav = fixture();
    feed(&nav, ORB_NAV_TOUCH_DOWN, 455, 233, 0);
    assert(feed(&nav, ORB_NAV_TOUCH_MOVE, 448, 233, 10).type == ORB_NAV_EVENT_NONE);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 440, 233, 20);
    assert(event.gesture == ORB_NAV_GESTURE_CONTROL && event.type == ORB_NAV_EVENT_BEGIN);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 435, 320, 40);
    assert(event.gesture == ORB_NAV_GESTURE_CONTROL);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 435, 320, 50);
    orb_nav_push(&nav, 2, NULL);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 12, 233, 60);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 32, 233, 80);
    assert(event.gesture == ORB_NAV_GESTURE_BACK);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 32, 233, 90);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 100);
    assert(feed(&nav, ORB_NAV_TOUCH_TICK, 233, 233, 499).type == ORB_NAV_EVENT_NONE);
    event = feed(&nav, ORB_NAV_TOUCH_TICK, 233, 233, 500);
    assert(event.type == ORB_NAV_EVENT_COMPASS);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 100, 520);
    assert(event.selection == ORB_NAV_COMPASS_SETTINGS);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 370, 233, 540);
    assert(event.selection == ORB_NAV_COMPASS_AGENTS);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 370, 560);
    assert(event.selection == ORB_NAV_COMPASS_PLAY);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 100, 233, 580);
    assert(event.selection == ORB_NAV_COMPASS_HOME);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 233, 233, 600);
    assert(event.selection == ORB_NAV_COMPASS_CANCEL && !event.commit);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 155, 190, 650);
    feed(&nav, ORB_NAV_TOUCH_TICK, 155, 190, 1050);
    event = feed(&nav, ORB_NAV_TOUCH_UP, 155, 190, 1060);
    assert(!event.commit); /* Lifting where held never commits the Compass. */
    orb_nav_touch(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 1100, false, true);
    assert(feed(&nav, ORB_NAV_TOUCH_TICK, 233, 233, 1500).type == ORB_NAV_EVENT_NONE);
}

static void layer_dismissal(void)
{
    orb_nav_t nav = fixture();
    orb_nav_push(&nav, 4, NULL);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 12, 0);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 49, 20);
    assert(event.gesture == ORB_NAV_GESTURE_DOWN);
    assert(orb_nav_neighbor(&nav, event.gesture) == 1);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 49, 30);
    orb_nav_back(&nav, NULL);
    orb_nav_push(&nav, 5, NULL);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 454, 40);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 411, 60);
    assert(event.gesture == ORB_NAV_GESTURE_UP);
    assert(orb_nav_neighbor(&nav, event.gesture) == 1);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 411, 70);
    orb_nav_touch(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 80, true, false);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 190, 100);
    assert(event.gesture == ORB_NAV_GESTURE_CONTENT);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 190, 110);
    orb_nav_back(&nav, NULL);
    orb_nav_push(&nav, 6, NULL);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 12, 233, 120);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 49, 233, 140);
    assert(event.gesture == ORB_NAV_GESTURE_BACK);
    assert(orb_nav_neighbor(&nav, event.gesture) == 1);
}

static void movement_cancels_blank_hold(void)
{
    const float strokes[][2] = {{10, 0}, {11, 0}, {9, 9}};
    for (unsigned i = 0; i < sizeof(strokes) / sizeof(strokes[0]); i++) {
        orb_nav_t nav = fixture();
        feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
        feed(&nav, ORB_NAV_TOUCH_MOVE, 233 + strokes[i][0], 233 + strokes[i][1], 100);
        /* Returning to DOWN cannot turn a cancelled hold back into a hold. */
        feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 233, 200);
        orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_TICK, 233, 233, 400);
        assert(event.type != ORB_NAV_EVENT_COMPASS);
        assert(nav.touch.owner != ORB_NAV_GESTURE_COMPASS);
    }
    orb_nav_t nav = fixture();
    feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 239, 239, 100);
    assert(feed(&nav, ORB_NAV_TOUCH_TICK, 239, 239, 400).type == ORB_NAV_EVENT_COMPASS);
}

static orb_nav_gesture_t expected_edge(float rx, float ry)
{
    if (fabsf(ry) >= fabsf(rx)) return ry < 0 ? ORB_NAV_GESTURE_DOWN : ORB_NAV_GESTURE_UP;
    return rx < 0 ? ORB_NAV_GESTURE_BACK : ORB_NAV_GESTURE_CONTROL;
}

static void round_edge_band(void)
{
    orb_nav_t nav = fixture();
    orb_nav_set_current(&nav, 7);
    const float radii[] = {192, 194, 210, 232};
    for (unsigned r = 0; r < sizeof(radii) / sizeof(radii[0]); r++) {
        for (unsigned degrees = 0; degrees < 360; degrees += 5) {
            float angle = degrees * 3.14159265f / 180;
            float rx = cosf(angle), ry = sinf(angle);
            float x = 233 + radii[r] * rx, y = 233 + radii[r] * ry;
            feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 0);
            assert(nav.touch.rim == (radii[r] >= 193));
            orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE,
                x - 40 * rx, y - 40 * ry, 40);
            assert(event.type == ORB_NAV_EVENT_BEGIN);
            assert(event.gesture == (radii[r] >= 193 ? expected_edge(rx, ry) :
                                    ORB_NAV_GESTURE_CONTENT));
            feed(&nav, ORB_NAV_TOUCH_CANCEL, x - 40 * rx, y - 40 * ry, 50);

            /* Moving outward never falls through to centre navigation. */
            feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 60);
            event = feed(&nav, ORB_NAV_TOUCH_MOVE, x + 40 * rx, y + 40 * ry, 100);
            assert(event.gesture == ORB_NAV_GESTURE_CONTENT);
            feed(&nav, ORB_NAV_TOUCH_CANCEL, x + 40 * rx, y + 40 * ry, 110);

            /* Centre DOWN stays content even after entering the edge band. */
            if (radii[r] < 193) {
                feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 120);
                feed(&nav, ORB_NAV_TOUCH_MOVE, x + 40 * rx, y + 40 * ry, 160);
                event = feed(&nav, ORB_NAV_TOUCH_MOVE, x - 100 * rx, y - 100 * ry, 200);
                assert(event.gesture == ORB_NAV_GESTURE_CONTENT);
                feed(&nav, ORB_NAV_TOUCH_CANCEL, x - 100 * rx, y - 100 * ry, 210);
            }
        }
    }
    /* Exact 40 px boundary on all four axes, plus either side. */
    const float axes[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (unsigned a = 0; a < 4; a++) {
        for (unsigned inside = 0; inside < 3; inside++) {
            float radius = 192.5f + inside * .5f;
            float x = 233 + radius * axes[a][0], y = 233 + radius * axes[a][1];
            feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 0);
            assert(nav.touch.rim == (radius >= 193));
            orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE,
                x - 24 * axes[a][0], y - 24 * axes[a][1], 30);
            assert(event.gesture == (radius >= 193 ? expected_edge(axes[a][0], axes[a][1]) :
                                    ORB_NAV_GESTURE_CONTENT));
            feed(&nav, ORB_NAV_TOUCH_CANCEL, x, y, 40);
        }
    }
    /* Diagonal boundaries consistently belong to top/bottom. */
    for (int xsign = -1; xsign <= 1; xsign += 2) {
        for (int ysign = -1; ysign <= 1; ysign += 2) {
            float x = 233 + xsign * 150, y = 233 + ysign * 150;
            feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 0);
            orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE,
                x - xsign * 30, y - ysign * 30, 30);
            assert(event.gesture == (ysign < 0 ? ORB_NAV_GESTURE_DOWN : ORB_NAV_GESTURE_UP));
            feed(&nav, ORB_NAV_TOUCH_CANCEL, x, y, 40);
        }
    }
}

static void round_crown_arbitration(void)
{
    orb_nav_t nav = fixture();
    orb_nav_set_current(&nav, 7);
    orb_nav_rotary_owner_t rotary = {.owner = 7, .minimum = 0, .maximum = 1000,
        .value = 500, .detent = 10, .units_per_turn = 1000};
    orb_nav_rotary_bind(&nav, &rotary);
    for (unsigned degrees = 0; degrees < 360; degrees += 10) {
        float angle = degrees * 3.14159265f / 180;
        float rx = cosf(angle), ry = sinf(angle);
        float x = 233 + 200 * rx, y = 233 + 200 * ry;
        feed(&nav, ORB_NAV_TOUCH_DOWN, x, y, 0);
        orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE,
            x - 30 * ry, y + 30 * rx, 30);
        assert(event.gesture == ORB_NAV_GESTURE_ROTARY);
        event = feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 233, 60);
        assert(event.gesture == ORB_NAV_GESTURE_ROTARY);
        feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 233, 70);
    }
}

static void face_exception_and_resume_admission(void)
{
    orb_nav_t nav = fixture();
    assert(orb_nav_register(&nav, (orb_nav_place_t){8, ORB_NAV_ROLE_SCREEN, false}));
    assert(orb_nav_register(&nav, (orb_nav_place_t){9, ORB_NAV_ROLE_FACE, false}));
    const orb_nav_gesture_t gestures[] = {ORB_NAV_GESTURE_LEFT, ORB_NAV_GESTURE_RIGHT,
        ORB_NAV_GESTURE_UP, ORB_NAV_GESTURE_DOWN};
    const int deltas[][2] = {{-80, 0}, {80, 0}, {0, -80}, {0, 80}};
    const unsigned destinations[] = {3, 2, 4, 5};
    for (unsigned face = 0; face < 2; face++) {
        orb_nav_set_current(&nav, face ? 9 : 1);
        for (unsigned d = 0; d < 4; d++) {
            feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
            orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE,
                233 + deltas[d][0], 233 + deltas[d][1], 40);
            assert(event.gesture == gestures[d]);
            assert(orb_nav_neighbor(&nav, event.gesture) == destinations[d]);
            feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 233, 50);
        }
        feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
        assert(!orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_BACK, .5f, true));
        assert(!orb_nav_touch_resume(&nav, ORB_NAV_GESTURE_CONTROL, .5f, true));
        feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 233, 50);
        /* Face-local gallery/colour/slot pages share the Face place ID.
         * Expose edge Back for the adapter to unwind those local pages; the
         * Face's centre-right swipe remains Agents above. */
        feed(&nav, ORB_NAV_TOUCH_DOWN, 12, 233, 100);
        orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 142, 233, 260);
        assert(event.gesture == ORB_NAV_GESTURE_BACK);
        event = feed(&nav, ORB_NAV_TOUCH_UP, 142, 233, 280);
        assert(event.type == ORB_NAV_EVENT_END && event.commit);
    }
    for (unsigned source = 2; source <= 8; source++) {
        orb_nav_set_current(&nav, source);
        feed(&nav, ORB_NAV_TOUCH_DOWN, 233, 233, 0);
        for (unsigned gesture = ORB_NAV_GESTURE_LEFT; gesture <= ORB_NAV_GESTURE_CONTROL; gesture++)
            assert(!orb_nav_touch_resume(&nav, gesture, .5f, true));
        feed(&nav, ORB_NAV_TOUCH_CANCEL, 233, 233, 50);
        const int starts[][2] = {{12, 233}, {454, 233}, {233, 12}, {233, 454}};
        const orb_nav_gesture_t edges[] = {ORB_NAV_GESTURE_BACK, ORB_NAV_GESTURE_CONTROL,
            ORB_NAV_GESTURE_DOWN, ORB_NAV_GESTURE_UP};
        for (unsigned e = 0; e < 4; e++) {
            feed(&nav, ORB_NAV_TOUCH_DOWN, starts[e][0], starts[e][1], 0);
            for (unsigned gesture = ORB_NAV_GESTURE_LEFT; gesture <= ORB_NAV_GESTURE_CONTROL; gesture++)
                if (gesture != (unsigned)edges[e])
                    assert(!orb_nav_touch_resume(&nav, gesture, .5f, true));
            assert(orb_nav_touch_resume(&nav, edges[e], .5f, true));
            feed(&nav, ORB_NAV_TOUCH_CANCEL, starts[e][0], starts[e][1], 50);
        }
    }
}

static unsigned s_changes, s_ticks;
static float s_value, s_delta;
static void changed(void *ctx, float value, float delta)
{
    assert(ctx == &s_value && delta != 0);
    s_value = value;
    s_delta = delta;
    s_changes++;
}
static void ticked(void *ctx)
{
    assert(ctx == &s_value);
    s_ticks++;
}
static void rotary(void)
{
    orb_nav_t nav = fixture();
    orb_nav_set_current(&nav, 2);
    orb_nav_rotary_owner_t owner = {
        .owner = 2, .ctx = &s_value, .change = changed, .tick = ticked,
        .minimum = 0, .maximum = 1000, .value = 100, .detent = 10, .units_per_turn = 1000,
    };
    orb_nav_rotary_bind(&nav, &owner);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 453, 233, 0);
    orb_nav_event_t event = feed(&nav, ORB_NAV_TOUCH_MOVE, 451, 265, 10);
    assert(event.gesture == ORB_NAV_GESTURE_ROTARY && s_changes && s_ticks && s_value > 100);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 440, 300, 20);
    assert(event.gesture == ORB_NAV_GESTURE_ROTARY);
    event = feed(&nav, ORB_NAV_TOUCH_MOVE, 300, 233, 30);
    assert(event.gesture == ORB_NAV_GESTURE_ROTARY); /* Inward cannot become Back/CC. */
    feed(&nav, ORB_NAV_TOUCH_MOVE, 233, 453, 40);
    feed(&nav, ORB_NAV_TOUCH_UP, 233, 453, 45);
    assert(nav.rotary.flinging && fabsf(nav.rotary.velocity) <= 2000);
    unsigned before = s_changes;
    assert(orb_nav_tick(&nav, 61));
    assert(s_changes > before);
    for (uint32_t ms = 77; ms < 4000; ms += 16) orb_nav_tick(&nav, ms);
    assert(!nav.rotary.flinging && s_value >= 0 && s_value <= 1000);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 453, 233, 4100);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 451, 265, 4110);
    feed(&nav, ORB_NAV_TOUCH_UP, 451, 265, 4115);
    orb_nav_set_current(&nav, 7);
    assert(!orb_nav_tick(&nav, 4130) && !nav.rotary.flinging);
    orb_nav_rotary_unbind(&nav, 7);
    assert(nav.rotary.bound); /* A stale screen cannot revoke another owner. */
    orb_nav_rotary_unbind(&nav, 2);
    assert(!nav.rotary.bound);
    assert(orb_nav_rubber_band(0, 466) == 0);
    assert(orb_nav_rubber_band(10000, 466) < 466);
    assert(orb_nav_rubber_band(-10000, 466) > -466);
}

static void rotary_detent_delta(void)
{
    orb_nav_t nav = fixture();
    orb_nav_set_current(&nav, 2);
    orb_nav_rotary_owner_t owner = {
        .owner = 2, .ctx = &s_value, .change = changed,
        .minimum = 0, .maximum = 3, .detent = 1, .units_per_turn = 8,
    };
    orb_nav_rotary_bind(&nav, &owner);
    feed(&nav, ORB_NAV_TOUCH_DOWN, 453, 233, 0);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 451, 265, 20);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 13, 233, 40);
    assert(s_value == 3 && s_delta > 2 && nav.rotary.owner.value == s_value);
    feed(&nav, ORB_NAV_TOUCH_MOVE, 453, 233, 60);
    assert(s_value == 0 && s_delta == -3 && nav.rotary.owner.value == s_value);
    feed(&nav, ORB_NAV_TOUCH_CANCEL, 453, 233, 80);
}

static void twist_owner_path(void)
{
    orb_nav_t nav = fixture();
    orb_nav_set_current(&nav, 2);
    orb_nav_rotary_owner_t owner = {.owner = 2, .ctx = &s_value,
        .change = changed, .tick = ticked, .minimum = 0, .maximum = 30,
        .value = 10, .detent = 10, .units_per_turn = 100};
    s_changes = s_ticks = 0;
    orb_nav_rotary_bind(&nav, &owner);
    assert(orb_nav_rotary_step(&nav, 1) && s_value == 20 && s_changes == 1 && s_ticks == 1);
    assert(orb_nav_rotary_step(&nav, 8) && s_value == 30 && s_changes == 2 && s_ticks == 2);
    assert(!orb_nav_rotary_step(&nav, 1) && s_value == 30);
    assert(orb_nav_rotary_step(&nav, -2) && s_value == 10 && s_ticks == 4);
    assert(!nav.rotary.flinging);
    nav.touch.down = true;
    assert(!orb_nav_rotary_step(&nav, 1) && s_value == 10);
    nav.touch.down = false;
    orb_nav_set_current(&nav, 3);
    assert(!orb_nav_rotary_step(&nav, 1));
    orb_nav_rotary_unbind(&nav, 2);
    assert(!orb_nav_rotary_step(&nav, -1));
}

int main(void)
{
    graph_and_stack();
    repeated_layer_returns();
    repeated_layer_identity();
    drag_ownership_and_velocity();
    edges_and_compass();
    movement_cancels_blank_hold();
    velocity_polling();
    natural_swipes();
    resumed_navigation();
    centre_content_ownership();
    rotary();
    rotary_detent_delta();
    twist_owner_path();
    layer_dismissal();
    round_edge_band();
    round_crown_arbitration();
    face_exception_and_resume_admission();
    puts("orb_nav: graph, bounded stacks, drag ownership/velocity/cancel, rim arbitration, Compass, bounded rotary passed");
    return 0;
}
