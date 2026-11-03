#pragma once

#include "orb_app.h"

/* Keep this registry header out of the legacy orb_apps runtime translation
 * unit: that API currently uses its own orb_app_manifest_t type. */
const orb_app_manifest_t *orb_app_time_manifest(void);
