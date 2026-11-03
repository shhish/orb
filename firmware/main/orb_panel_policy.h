#pragma once
#include "orb_shell.h"

/* UI-serialized panel adapter; commands remain on the drained sender fence. */
void orb_panel_policy_init(void);
int orb_panel_policy_apply(orb_shell_t *shell);
uint32_t orb_panel_policy_acknowledged(void);
