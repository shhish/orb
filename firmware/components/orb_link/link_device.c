#include "link_device.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "orb_net.h"

#define DEVICE_NAME "Orb"
#define DEVICE_MODEL "waveshare-s3-amoled-1.75c"

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

/* fw is the app version; build is the first 7 hex digits of the app's ELF
 * SHA-256, which the hub can read from the same image's descriptor. */
void link_device_info(link_device_t *d, char build[8])
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_get_elf_sha256(build, 8);
    *d = (link_device_t){
        .device_id = orb_net_device_id(),
        .name = DEVICE_NAME,
        .model = DEVICE_MODEL,
        .fw = app->version,
        .build = build,
        .idf = app->idf_ver,
        .boot_slot = running ? running->label : "",
        .reset_reason = reset_reason(),
        .uptime_s = uptime_s(),
    };
}

const char *link_device_display_state_name(orb_link_display_state_t state)
{
    switch (state) {
    case ORB_LINK_DISPLAY_DIMMED: return "dimmed";
    case ORB_LINK_DISPLAY_ALWAYS_ON: return "always_on";
    case ORB_LINK_DISPLAY_SLEEP: return "sleep";
    default: return "active";
    }
}
