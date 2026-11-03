#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "orb_shell.h"
#include "emu_input.h"

/* LVGL and the display/tick/pointer are owned by the SDL frontend. Everything
 * here is synthetic and process-local; no hub, microphone or user file opens. */
bool emu_fixture_init(lv_display_t *display);
orb_shell_t *emu_fixture_shell(void);
/* Numbered debug jumps wait for app exit. Physical button gestures are routed
 * by the shared recognizer and shell in step(), not keyboard callbacks. */
void emu_fixture_input(emu_input_action_t action, int value);
void emu_fixture_step(uint32_t now_ms, unsigned pressed_mask, bool screen_on,
                      float gx, float gy, float gz);
void emu_fixture_face_down(bool face_down);
void emu_fixture_shutdown(void);

/* Synthetic, deterministic Design 1.0 examples; false for unknown names. */
bool emu_fixture_design_scene(const char *name);
