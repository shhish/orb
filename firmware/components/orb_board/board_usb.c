/* Light sleep stops the USB Serial/JTAG clocks, and a host gives up on a device
 * that stops answering. It only enumerates again on a connect event, which a
 * software reset does not produce. Drop the D+ pull-up to make one. */
#include "orb_board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/usb_serial_jtag_struct.h"

void orb_board_usb_reattach(void)
{
    USB_SERIAL_JTAG.conf0.dp_pullup = 0;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
    vTaskDelay(pdMS_TO_TICKS(30));
    USB_SERIAL_JTAG.conf0.dp_pullup = 1;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
}
