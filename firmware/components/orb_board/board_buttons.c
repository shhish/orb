/* Capture real GPIO edges independently of the service's IMU poll cadence. */
#include "orb_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define PWR_GPIO GPIO_NUM_3
#define BOOT_GPIO GPIO_NUM_0
#define EDGE_CAPACITY 32u

static portMUX_TYPE s_edges_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_board_button_edge_t s_edges[EDGE_CAPACITY];
static unsigned s_read, s_write;
static void (*s_notify)(void *arg);
static void *s_notify_arg;

static void IRAM_ATTR capture_edge(void)
{
    portENTER_CRITICAL_ISR(&s_edges_lock);
    /* The restored ISR can run on the other core while sleep exit resamples.
     * Serialize the physical sample with its queue position so a stale held
     * snapshot cannot follow a newer release and become a phantom hold. */
    orb_board_button_edge_t edge = {
        .at_us = (uint64_t)esp_timer_get_time(), .buttons = orb_board_buttons(),
    };
    /* Keep the most recent physical state even if extreme contact bounce
     * fills the queue. Ordinary presses retain both edges and their times. */
    if (s_write - s_read == EDGE_CAPACITY) s_read++;
    s_edges[s_write++ % EDGE_CAPACITY] = edge;
    portEXIT_CRITICAL_ISR(&s_edges_lock);
}

static void IRAM_ATTR button_edge(void *arg)
{
    (void)arg;
    capture_edge();
    if (s_notify) s_notify(s_notify_arg);
}

void orb_board_buttons_sleep_wake(void) { capture_edge(); }

esp_err_t orb_board_buttons_notify(void (*notify)(void *arg), void *arg)
{
    s_notify = notify;
    s_notify_arg = arg;
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, "board", "GPIO ISR service");
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(PWR_GPIO, GPIO_INTR_ANYEDGE), "board", "PWR interrupt");
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(BOOT_GPIO, GPIO_INTR_ANYEDGE), "board", "BOOT interrupt");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PWR_GPIO, button_edge, NULL), "board", "PWR ISR");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOOT_GPIO, button_edge, NULL), "board", "BOOT ISR");
    ESP_RETURN_ON_ERROR(gpio_intr_enable(PWR_GPIO), "board", "PWR interrupt enable");
    return gpio_intr_enable(BOOT_GPIO);
}

bool orb_board_button_edge(orb_board_button_edge_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_edges_lock);
    bool ready = s_read != s_write;
    if (ready) *out = s_edges[s_read++ % EDGE_CAPACITY];
    portEXIT_CRITICAL(&s_edges_lock);
    return ready;
}
