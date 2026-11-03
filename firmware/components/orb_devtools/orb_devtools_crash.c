#include "orb_devtools_power.h"

#include "orb_link_crash.h"

/* The last panic's summary, reachable over Wi-Fi when USB is not. */
void orb_devtools_crash_report(orb_devtools_power_printf_fn output)
{
    orb_link_crash_t summary;
    esp_err_t err = orb_link_crash_read(&summary);
    if (err == ESP_ERR_NOT_FOUND) {
        output("@@ORBDEV CRASH none\n");
    } else if (err != ESP_OK) {
        output("@@ORBDEV CRASH error=%s\n", esp_err_to_name(err));
    } else {
        output("@@ORBDEV CRASH task=%.16s pc=0x%08lx cause=%lu vaddr=0x%08lx bt=", summary.task,
               (unsigned long)summary.pc, (unsigned long)summary.cause, (unsigned long)summary.vaddr);
        for (uint32_t i = 0; i < summary.depth; i++)
            output("0x%08lx ", (unsigned long)summary.backtrace[i]);
        output("corrupted=%u\n", summary.corrupted);
    }
}
