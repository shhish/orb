#pragma once

/* Main-owned storage services; neither entry point may run under the UI lock. */
void orb_local_services_init(void);
void orb_local_services_poll(void);
