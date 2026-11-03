#pragma once

#include "orb_app_weather.h"
#include "orb_settings_host.h"

/* Weather is hosted by the same compatibility descriptor host as Settings, so
 * it inherits staging, chrome, context and outer transition behaviour. Its
 * stage/update adapters are declared with the host's others; this projection
 * only translates the model. It never routes or calls services. */
void orb_weather_host_project(const orb_model_snapshot_t *model, orb_weather_screen_model_t *out);
