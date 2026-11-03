#pragma once

/* Main calls this before local services and UI creation so startup reservations
 * and fail-closed storage checks keep their original order. */
void orb_boot_prepare(void);
