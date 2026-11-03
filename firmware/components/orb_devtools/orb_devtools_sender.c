#include "orb_devtools_sender.h"

#include <string.h>
#include "esp_timer.h"

#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
static bool sender_snapshot(TaskHandle_t sender, uint64_t *runtime)
{
    if (!sender) return false;
    /* The frame fence can win just before the sender blocks. Only a blocked
     * task has committed its final runtime slice; never sample a running TCB. */
    int64_t until = esp_timer_get_time() + 2000;
    while (eTaskGetState(sender) != eBlocked) {
        if (esp_timer_get_time() >= until) return false;
        vTaskDelay(1);
    }
    TaskStatus_t status;
    vTaskGetInfo(sender, &status, pdFALSE, eBlocked);
    *runtime = status.ulRunTimeCounter;
    return true;
}
#endif

void orb_devtools_sender_begin(orb_devtools_sender_cpu_t *sample)
{
    memset(sample, 0, sizeof(*sample));
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    sample->task = xTaskGetHandle("lcd_send");
    sample->begun = sender_snapshot(sample->task, &sample->before_us);
    sample->clock_start = (configRUN_TIME_COUNTER_TYPE)portGET_RUN_TIME_COUNTER_VALUE();
#endif
}

void orb_devtools_sender_end(orb_devtools_sender_cpu_t *sample)
{
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    uint64_t after;
    if (!sample->begun || sample->task != xTaskGetHandle("lcd_send") ||
        !sender_snapshot(sample->task, &after)) return;
    uint64_t clock = (configRUN_TIME_COUNTER_TYPE)portGET_RUN_TIME_COUNTER_VALUE();
    /* IDF omits the slice that crosses a runtime-clock wrap. Refuse any
     * ambiguous window or task-counter wrap instead of understating CPU. */
    if (clock < sample->clock_start || after < sample->before_us ||
        after - sample->before_us > clock - sample->clock_start) return;
    sample->elapsed_us = after - sample->before_us;
    sample->available = true;
#else
    (void)sample;
#endif
}

void orb_devtools_sender_report(const orb_devtools_sender_cpu_t *sample, uint64_t frames,
                                  orb_devtools_report_fn report)
{
    report("@@ORBDEV GAME_SENDER_CPU sender_available=%d sender_task_us=%llu sender_mean_us=%.3f basis=blocked_task_runtime_us scope=sender_service_wait_spin_irq_charged cache_stalls_separated=0\n",
        sample->available, (unsigned long long)sample->elapsed_us,
        sample->available && frames ? sample->elapsed_us / (double)frames : 0);
}
