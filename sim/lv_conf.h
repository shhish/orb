/*
 * LVGL for the simulator: the firmware's settings (firmware/sdkconfig.defaults
 * over the fork's Kconfig defaults), so the UI renders here as on the device.
 * Anything not set here is LVGL's default, which matches the Kconfig default.
 * sim/conf-diff.sh compares the two after a firmware build.
 *
 * Differences, all for the host:
 *  - pthreads instead of FreeRTOS, still with two draw threads;
 *  - draw threads get 64 KB of stack (64-bit frames, and macOS has a 16 KB minimum);
 *  - plain C fills and copies instead of the S3's SIMD (orb_lv_simd);
 *  - lodepng, for the screenshots;
 *  - a failed assertion traps (a crash report, a debugger stop) instead of spinning.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_PTHREAD

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_DEF_REFR_PERIOD 16

#define LV_USE_SNAPSHOT 1
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 2
#define LV_DRAW_THREAD_STACK_SIZE (64 * 1024)
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE

#define LV_USE_FLOAT 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_LOG_USE_TIMESTAMP 1
#define LV_LOG_USE_FILE_LINE 1
#define LV_LOG_TRACE_MEM 1
#define LV_LOG_TRACE_TIMER 1
#define LV_LOG_TRACE_INDEV 1
#define LV_LOG_TRACE_DISP_REFR 1
#define LV_LOG_TRACE_EVENT 1
#define LV_LOG_TRACE_OBJ_CREATE 1
#define LV_LOG_TRACE_LAYOUT 1
#define LV_LOG_TRACE_ANIM 1
#define LV_LOG_TRACE_CACHE 1
#define LV_CHECK_ARG_LOG_MODE LV_CHECK_ARG_LOG_MODE_VERBOSE

#define LV_ASSERT_HANDLER __builtin_trap();

/* Inter and Framework7 are supplied by orb_fonts, including packed AX sizes.
 * LVGL's default widget font remains available until an Orb style is applied. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_USE_OBSERVER 1
#define LV_USE_ARCLABEL 1

#define LV_USE_LODEPNG 1

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif /*LV_CONF_H*/
