#pragma once
#include "orb_game_logic.h"

/* Presentation only: preserve current rules/HUD and blend fixed-step positions.
 * No physics, input, storage or allocation occurs between simulation steps. */
void orb_game_interpolate(const orb_game_state_t *previous, orb_game_state_t *current,
                          float alpha);
