#include "orb_display.h"
#include "orb_display_touch.h"
#include "orb_board.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define TOUCH_POLL_MS 16    /* while a finger is down */
#define TOUCH_IDLE_MS 250   /* IRQ remains the prompt wake path. */
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
#define SCROLL_TRACE(kind, at, a, b) orb_display_scroll_trace_record(kind, at, a, b)
#else
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
#endif
static const char *TAG = "display";
static lv_indev_t *s_touch_indev;
static esp_lcd_touch_handle_t s_tp;
static SemaphoreHandle_t s_touch_irq;
static atomic_bool s_touch_ready;
static bool s_touch_pending;
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int32_t x, y;
    bool pressed;
} s_touch;
static void IRAM_ATTR on_touch_irq(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_touch_irq, &woken);
    portYIELD_FROM_ISR(woken);
}

static void touch_poll_ready(void *arg)
{
    xSemaphoreGive(s_touch_irq);
}

/*
 * Reads the touch controller off the UI task: an I2C read takes milliseconds,
 * which the UI task would otherwise spend waiting instead of drawing. Idle, it
 * waits for the controller's interrupt (and reads now and then anyway, should
 * one be missed); while a finger is down it reads at the panel's pace until it
 * lifts.
 */
static void touch_task(void *arg)
{
    esp_timer_handle_t poll = NULL;
    const esp_timer_create_args_t poll_args = { .callback = touch_poll_ready, .name = "touch_poll" };
    if (esp_timer_create(&poll_args, &poll) != ESP_OK) {
        ESP_LOGW(TAG, "precise touch poll unavailable; using rounded-up RTOS ticks");
    }
    for (;;) {
        uint32_t poll_ms = s_touch.pressed ? TOUCH_POLL_MS : TOUCH_IDLE_MS;
        if (!s_touch.pressed && orb_display_sleep_poll()) {
            xSemaphoreTake(s_touch_irq, portMAX_DELAY);
        } else if (poll && esp_timer_start_once(poll, poll_ms * 1000u) == ESP_OK) {
            xSemaphoreTake(s_touch_irq, portMAX_DELAY);
            /* An interrupt may beat the fallback timer. A pending callback is
             * harmless: it requests another controller read, not synthetic input. */
            esp_timer_stop(poll);
        } else {
            xSemaphoreTake(s_touch_irq, (poll_ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        }
        esp_lcd_touch_point_data_t pt;
        uint8_t n = 0;
        esp_lcd_touch_read_data(s_tp);
        esp_lcd_touch_get_data(s_tp, &pt, &n, 1);
        portENTER_CRITICAL(&s_touch_lock);
        bool publish = n > 0 || s_touch.pressed;
        s_touch.pressed = n > 0;
        if (n > 0) {
            s_touch.x = pt.x;
            s_touch.y = pt.y;
        }
        s_touch_pending |= publish;
        portEXIT_CRITICAL(&s_touch_lock);
        if (!publish) continue;
        lv_lock();
        if (s_touch_indev) lv_timer_ready(lv_indev_get_read_timer(s_touch_indev));
        lv_unlock();
        orb_display_wake();
        SCROLL_TRACE(n ? ORB_SCROLL_TRACE_TOUCH_SAMPLE : ORB_SCROLL_TRACE_TOUCH_RELEASE,
                     esp_timer_get_time(), s_touch.x, s_touch.y);
    }
}

bool orb_display_touch_ready(void)
{
    return atomic_load(&s_touch_ready);
}

void IRAM_ATTR orb_display_touch_sleep_wake(void)
{
    if (s_touch_irq) xSemaphoreGiveFromISR(s_touch_irq, NULL);
}

void orb_display_touch_sleep_changed(void)
{
    if (s_touch_irq) xSemaphoreGive(s_touch_irq);
}

void orb_display_touch_service(void)
{
    portENTER_CRITICAL(&s_touch_lock);
    bool pending = s_touch_pending;
    s_touch_pending = false;
    portEXIT_CRITICAL(&s_touch_lock);
    if (pending && s_touch_indev) lv_indev_read(s_touch_indev);
}

bool orb_display_touch_pressed(void)
{
    portENTER_CRITICAL(&s_touch_lock);
    bool pressed = s_touch.pressed;
    portEXIT_CRITICAL(&s_touch_lock);
    return pressed;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    portENTER_CRITICAL(&s_touch_lock);
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_touch_pending = false;
    portEXIT_CRITICAL(&s_touch_lock);
    SCROLL_TRACE(data->state == LV_INDEV_STATE_PRESSED ? ORB_SCROLL_TRACE_INPUT_READ : ORB_SCROLL_TRACE_INPUT_RELEASE,
                 esp_timer_get_time(), data->point.x, data->point.y);
}

void orb_display_touch_start(lv_display_t *display, unsigned priority)
{
    s_touch_irq = xSemaphoreCreateBinary();
    if (s_touch_irq && orb_board_touch_new(&s_tp, on_touch_irq, NULL) == ESP_OK &&
        xTaskCreatePinnedToCore(touch_task, "touch", 2560, NULL, priority, NULL, 0) == pdPASS) {
        lv_indev_t *touch = lv_indev_create();
        s_touch_indev = touch;
        lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch, touch_read);
        lv_indev_set_display(touch, display);
        atomic_store(&s_touch_ready, true);
    } else {
        ESP_LOGW(TAG, "no touch");
    }

}
