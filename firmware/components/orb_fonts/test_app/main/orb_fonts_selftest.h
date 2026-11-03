#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Checks every Orb font through LVGL's own lookups, for each content size with and
 * without Bold Text: each style and variant covers the text character set and every icon
 * (through its fallbacks), tabular and clock figures are all the same width, the colons
 * sit where they should, metrics follow Apple's leading, icon-only fonts are centred and
 * filled icons cover their outlines. Labels styled through the API must follow setting
 * changes. Then, if `disp` is set, it renders a screen of each style and reports how long
 * LVGL took. `now_us` is a microsecond clock. Sets that need the font pack are skipped
 * when it is missing, unless `require_pack`. Plain C and LVGL only: the ESP-IDF test app
 * and the host check (tools/fonts/lvgl_check.py) both run it. Returns the failures.
 */
int orb_fonts_selftest(lv_display_t *disp, uint64_t (*now_us)(void), bool require_pack);

#ifdef __cplusplus
}
#endif
