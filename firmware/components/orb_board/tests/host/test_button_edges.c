#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "orb_buttons.h"
typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef struct { uint64_t at_us; unsigned buttons; } orb_board_button_edge_t;
#define GPIO_NUM_3 3
#define GPIO_NUM_0 0
#define GPIO_INTR_ANYEDGE 3
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define IRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static void enter_isr_lock(portMUX_TYPE *lock);
#define portENTER_CRITICAL_ISR(lock) enter_isr_lock(lock)
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#define ESP_RETURN_ON_ERROR(x, ...) do { int e = (x); if (e) return e; } while (0)
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
static unsigned s_mask, s_notifications, s_any_edges, s_enabled;
static uint64_t s_time;
static void (*s_isr[4])(void *);
static void (*s_before_lock)(void);
static void enter_isr_lock(portMUX_TYPE *lock)
{
    (void)lock;
    if (s_before_lock) {
        void (*before)(void) = s_before_lock;
        s_before_lock = NULL;
        before();
    }
}
static unsigned orb_board_buttons(void) { return s_mask; }
static int64_t esp_timer_get_time(void) { return (int64_t)s_time; }
static int gpio_install_isr_service(int flags) { assert(!flags); return ESP_ERR_INVALID_STATE; }
static int gpio_set_intr_type(int pin, int type)
{
    assert((pin == 0 || pin == 3) && type == GPIO_INTR_ANYEDGE); s_any_edges++; return ESP_OK;
}
static int gpio_isr_handler_add(int pin, void (*isr)(void *), void *arg)
{
    assert((pin == 0 || pin == 3) && isr && !arg); s_isr[pin] = isr; return ESP_OK;
}
static int gpio_intr_enable(int pin) { assert(pin == 0 || pin == 3); s_enabled++; return ESP_OK; }
#include "button_edges.inc"
static void notify(void *arg) { assert(arg == (void *)1); s_notifications++; }
static void edge(unsigned mask, uint64_t at)
{
    s_mask = mask; s_time = at; s_isr[3](NULL);
}

static void release_on_other_core(void) { edge(0, 225000); }

static void wake_release(void)
{
    /* The wake snapshot records PWR down. Its release lands while normal
     * edges are masked, so only the post-restore resample can record it. */
    s_mask = 1; s_time = 300000;
    orb_board_buttons_sleep_wake();
    s_mask = 0; s_time = 340000;
    orb_board_buttons_sleep_wake();
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    unsigned mask = 0, downs = 0, ups = 0;
    orb_board_button_edge_t item;
    while (orb_board_button_edge(&item)) {
        uint64_t at = item.at_us / 1000;
        orb_buttons_poll(&buttons, at, mask);
        orb_button_event_t event;
        while (orb_buttons_next(&buttons, &event)) {
            assert(event.button == ORB_BUTTON_PWR && !event.synthetic);
            assert(event.gesture == ORB_BUTTON_DOWN);
            downs++;
            orb_buttons_consume_press(&buttons, ORB_BUTTON_PWR);
        }
        orb_buttons_poll(&buttons, at, item.buttons);
        mask = item.buttons;
    }
    orb_buttons_poll(&buttons, 4000, mask);
    orb_button_event_t event;
    while (orb_buttons_next(&buttons, &event)) {
        assert(event.button == ORB_BUTTON_PWR && !event.synthetic);
        assert(event.gesture == ORB_BUTTON_UP);
        ups++;
    }
    assert(downs == 1 && ups == 1 && !buttons.buttons[ORB_BUTTON_PWR].held);
}
int main(void)
{
    orb_board_button_edge_t item;
    assert(!orb_board_button_edge(NULL) && !orb_board_button_edge(&item));
    assert(orb_board_buttons_notify(notify, (void *)1) == ESP_OK && s_any_edges == 2 && s_enabled == 2);
    edge(1, 100000); edge(0, 135000); /* Whole 35 ms press between 50 ms polls. */
    assert(s_notifications == 2);
    assert(orb_board_button_edge(&item) && item.at_us == 100000 && item.buttons == 1);
    assert(orb_board_button_edge(&item) && item.at_us == 135000 && item.buttons == 0);
    assert(!orb_board_button_edge(&item));
    /* Sleep exit samples the real held/released pins without invoking the
     * normal ISR notifier, which may request a yield inside the PM callback. */
    s_mask = 3; s_time = 150000;
    orb_board_buttons_sleep_wake();
    assert(orb_board_button_edge(&item) && item.at_us == 150000 && item.buttons == 3);
    s_mask = 0; s_time = 155000;
    orb_board_buttons_sleep_wake();
    assert(orb_board_button_edge(&item) && item.at_us == 155000 && item.buttons == 0);
    assert(s_notifications == 2 && !orb_board_button_edge(&item));
    /* Model an ISR reaching the queue before a cross-core wake resample
     * acquires its lock. The old pre-lock sample would append a stale down
     * after this release, leaving the recognizer held indefinitely. */
    s_mask = 1; s_time = 200000; s_before_lock = release_on_other_core;
    orb_board_buttons_sleep_wake();
    assert(orb_board_button_edge(&item) && item.at_us == 225000 && item.buttons == 0);
    assert(orb_board_button_edge(&item) && item.at_us == 225000 && item.buttons == 0);
    assert(!orb_board_button_edge(&item));
    wake_release();
    for (unsigned i = 0; i < 40; i++) edge(i % 4, 200000 + i);
    for (unsigned i = 8; i < 40; i++) {
        assert(orb_board_button_edge(&item) && item.at_us == 200000 + i && item.buttons == i % 4);
    }
    assert(!orb_board_button_edge(&item));
    puts("button edges: any-edge GPIO setup, inter-poll short press timestamps, notifications, ring wrap and bounded overflow passed");
}
