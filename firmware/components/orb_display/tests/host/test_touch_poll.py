"""Verify real-controller polling deadlines and IRQ early wake without hardware."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "orb_display_touch.c").read_text()

    def function(signature):
        start = source.index(signature)
        end = source.index("\n}", start) + len("\n}")
        return source[start:end]

    fixture = r"""
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { int x, y; } esp_lcd_touch_point_data_t;
#define TOUCH_POLL_MS 16
#define TOUCH_IDLE_MS 250
#define IRAM_ATTR
#define ESP_OK 0
#define ESP_LOGW(tag, message) ((void)0)
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS s_tick_ms
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(!s_locked); s_locked = true; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(s_locked); s_locked = false; } while (0)
static unsigned s_tick_ms, s_reads, s_starts, s_stops, s_waits, s_signals, s_deadline;
static unsigned s_wakes, s_indev_reads, s_count = 3;
static bool s_fail_alloc, s_locked, s_early, s_sleep_eligible, s_timer_active;
static bool s_samples[4] = {true, true, false};
static int s_touch_lock, s_touch_irq = 1, s_tp = 2;
static atomic_bool s_touch_ready;
static bool s_touch_pending;
static bool orb_display_sleep_poll(void) { return s_sleep_eligible; }
static void orb_display_wake(void) { s_wakes++; }
static void *s_touch_indev = (void *)3;
static void lv_lock(void) {}
static void lv_unlock(void) {}
static void *lv_indev_get_read_timer(void *indev) { return indev; }
static void lv_timer_ready(void *timer) { (void)timer; }
static void lv_indev_read(void *indev) { assert(indev == s_touch_indev); s_indev_reads++; }
static struct { int32_t x, y; bool pressed; } s_touch;
static jmp_buf s_done;
static int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(args->callback && !strcmp(args->name, "touch_poll"));
    if (s_fail_alloc) return -1;
    *out = (void *)1; return ESP_OK;
}
static int esp_timer_start_once(esp_timer_handle_t timer, unsigned us) {
    assert(timer == (void *)1 && !s_fail_alloc && !s_locked);
    assert(us == (s_touch.pressed ? 16000 : 250000));
    s_deadline = us; s_starts++; s_timer_active = true; return ESP_OK;
}
static void esp_timer_stop(esp_timer_handle_t timer) {
    assert(timer == (void *)1); s_stops++; s_timer_active = false;
}
static void xSemaphoreGive(int semaphore) { assert(semaphore == s_touch_irq); s_signals++; }
static void xSemaphoreGiveFromISR(int semaphore, void *woken) {
    assert(semaphore == s_touch_irq && !woken); s_signals++;
}
static int xSemaphoreTake(int semaphore, unsigned ticks) {
    assert(semaphore == s_touch_irq && !s_locked);
    if (s_reads == s_count) longjmp(s_done, 1);
    s_waits++;
    if (s_sleep_eligible && !s_touch.pressed) {
        assert(ticks == portMAX_DELAY && !s_timer_active);
    } else if (s_fail_alloc) {
        unsigned ms = s_touch.pressed ? TOUCH_POLL_MS : TOUCH_IDLE_MS;
        assert(ticks * s_tick_ms >= ms && ticks * s_tick_ms < ms + s_tick_ms);
    } else {
        assert(ticks == portMAX_DELAY && s_deadline);
        /* IRQ can wake before its fallback deadline. The task cancels the
         * timer and reads fresh state rather than waiting another poll. */
        if (s_early) assert(s_deadline > 1000);
    }
    return 1;
}
static void esp_lcd_touch_read_data(int touch) { assert(touch == s_tp && !s_locked); }
static void esp_lcd_touch_get_data(int touch, esp_lcd_touch_point_data_t *point, uint8_t *count, unsigned max) {
    assert(touch == s_tp && max == 1 && !s_locked);
    point->x = 200 + s_reads; point->y = 300 - s_reads;
    *count = s_samples[s_reads];
    s_reads++;
}
"""
    fixture += function("static void touch_poll_ready(void *arg)") + "\n"
    fixture += function("static void touch_task(void *arg)")
    fixture += "\n" + function("bool orb_display_touch_ready(void)")
    fixture += "\n" + function("void IRAM_ATTR orb_display_touch_sleep_wake(void)")
    fixture += "\n" + function("void orb_display_touch_sleep_changed(void)")
    fixture += "\n" + function("void orb_display_touch_service(void)")
    fixture += r"""
int main(void) {
    for (s_tick_ms = 1; s_tick_ms <= 10; s_tick_ms += 9) {
        for (unsigned failed = 0; failed < 2; failed++) {
            for (unsigned early = 0; early < 2; early++) {
                memset(&s_touch, 0, sizeof(s_touch));
                s_fail_alloc = failed; s_early = early;
                s_reads = s_starts = s_stops = s_waits = 0;
                s_wakes = 0; s_touch_pending = s_timer_active = false;
                if (!setjmp(s_done)) touch_task(NULL);
                assert(s_reads == 3 && s_waits == 3 && !s_locked && !s_touch.pressed);
                assert(s_touch.x == 201 && s_touch.y == 299);
                if (failed) assert(!s_starts && !s_stops);
                else assert(s_starts == 4 && s_stops == 3);
                assert(s_wakes == 3);
            }
        }
    }
    /* One configuration kick reads an unchanged idle controller. Then a real
     * IRQ publishes first contact, held samples and release at the old16ms. */
    s_count = 4; s_samples[0] = false; s_samples[1] = s_samples[2] = true; s_samples[3] = false;
    for (unsigned failed = 0; failed < 2; failed++) {
        memset(&s_touch, 0, sizeof(s_touch));
        s_reads = s_starts = s_stops = s_waits = s_wakes = 0;
        s_fail_alloc = failed; s_sleep_eligible = true;
        s_touch_pending = s_timer_active = false;
        if (!setjmp(s_done)) touch_task(NULL);
        assert(s_reads == 4 && s_waits == 4 && s_wakes == 3 && !s_timer_active);
        assert(s_starts == (failed ? 0u : 2u) && s_stops == s_starts);
        assert(s_touch_pending);
        orb_display_touch_service(); assert(!s_touch_pending && s_indev_reads == failed + 1);
        orb_display_touch_service(); assert(s_indev_reads == failed + 1);
    }
    assert(!orb_display_touch_ready());
    atomic_store(&s_touch_ready, true); assert(orb_display_touch_ready());
    orb_display_touch_sleep_changed(); assert(s_signals == 1);
    touch_poll_ready(NULL); assert(s_signals == 2);
    unsigned reads = s_reads;
    orb_display_touch_sleep_wake(); assert(s_signals == 3 && s_reads == reads);
    puts("touch poll:16ms held cadence, IRQ-only sleep idle, unchanged reads silent, pending physical indev and fallback passed");
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-touch-poll-") as temporary:
        directory = Path(temporary)
        path = directory / "touch.c"
        path.write_text(fixture)
        binary = directory / "touch"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
