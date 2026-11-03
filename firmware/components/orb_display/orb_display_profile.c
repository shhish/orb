#include "orb_display.h"
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LV_USE_PROFILER_BUILTIN
#include "debugging/profiler/lv_profiler_builtin_private.h"

static uint64_t prof_tick(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void prof_flush(const char *buf)
{
    fputs(buf, stdout);
}

static int prof_tid(void)
{
    return (int)(((uintptr_t)xTaskGetCurrentTaskHandle() >> 4) & 0xFFFF);
}

static int prof_cpu(void)
{
    return (int)xPortGetCoreID();
}

/* Traces LVGL for ms milliseconds, then prints the trace (ftrace text) to the console. */
void orb_display_profile(int ms)
{
    lv_lock();
    lv_profiler_builtin_uninit();
    lv_profiler_builtin_config_t cfg;
    lv_profiler_builtin_config_init(&cfg);
    cfg.buf_size = 192 * 1024;
    cfg.tick_per_sec = 1000000;
    cfg.tick_get_cb = prof_tick;
    cfg.flush_cb = prof_flush;
    cfg.tid_get_cb = prof_tid;
    cfg.cpu_get_cb = prof_cpu;
    lv_profiler_builtin_init(&cfg);
    /* A full profiler buffer can flush while capture is still running. */
    printf("\n=== PROFILE BEGIN ===\n");
    lv_profiler_builtin_set_enable(true);
    lv_unlock();
    vTaskDelay(pdMS_TO_TICKS(ms));
    lv_lock();
    lv_profiler_builtin_set_enable(false);
    lv_profiler_builtin_flush();
    printf("\n=== PROFILE END ===\n");
    lv_unlock();
}
#endif

