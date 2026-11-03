#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>
#include "lvgl.h"

typedef enum {
    EMU_INPUT_FACE_DOWN,
    EMU_INPUT_NAVIGATE,
    EMU_INPUT_DIAGNOSTICS,
    EMU_INPUT_SCREENSHOT,
    EMU_INPUT_QUIT,
} emu_input_action_t;

/* value is 1..6 for NAVIGATE, the new bool for FACE_DOWN, otherwise zero.
 * All entry points, including the callback, run on the LVGL/UI thread. */
typedef void (*emu_input_cb_t)(emu_input_action_t action, int value, void *ctx);
typedef struct emu_input emu_input_t;

emu_input_t *emu_input_create(lv_display_t *display, emu_input_cb_t cb, void *ctx);
void emu_input_destroy(emu_input_t *input);
/* The panel rectangle in SDL window coordinates (not Retina drawable pixels).
 * Supplying it explicitly keeps 1x, 2x and any letterboxing identical. */
void emu_input_viewport(emu_input_t *input, int x, int y, int width, int height);
void emu_input_event(emu_input_t *input, const SDL_Event *event, uint32_t now_ms);
void emu_input_step(emu_input_t *input, uint32_t now_ms);
/* Raw board-compatible mask: bit 0=PWR, bit 1=BOOT. Gesture recognition is
 * owned by orb_buttons, exactly as it is for GPIO samples on the board. */
unsigned emu_input_buttons(const emu_input_t *input);
bool emu_input_face_down(const emu_input_t *input);
void emu_input_gravity(const emu_input_t *input, float gravity[3]);
