#pragma once
#include <stdbool.h>
bool orb_display_refresh_pending(void);

#include <stdint.h>
bool orb_display_te_pacing_active(void);
uint32_t orb_display_panel_period_us(void);
