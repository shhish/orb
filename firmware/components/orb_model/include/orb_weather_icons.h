#pragma once

#include "orb_model.h"
#include "orb_icons.h"

/* One glyph per condition, outline like the Face's other symbols. Unknown
 * returns "" so callers show the label alone rather than a guessed symbol. */
static inline const char *orb_weather_icon(orb_weather_condition_t condition, bool is_day)
{
    switch (condition) {
    case ORB_WEATHER_CLEAR: return is_day ? ORB_ICON_SUN : ORB_ICON_MOON;
    case ORB_WEATHER_PARTLY_CLOUDY: return is_day ? ORB_ICON_CLOUD_SUN : ORB_ICON_CLOUD_MOON;
    case ORB_WEATHER_CLOUDY: return ORB_ICON_CLOUD;
    case ORB_WEATHER_FOG: return ORB_ICON_CLOUD_FOG;
    case ORB_WEATHER_DRIZZLE: return ORB_ICON_CLOUD_DRIZZLE;
    case ORB_WEATHER_RAIN: return ORB_ICON_CLOUD_RAIN;
    case ORB_WEATHER_SNOW: return ORB_ICON_CLOUD_SNOW;
    case ORB_WEATHER_THUNDER: return ORB_ICON_CLOUD_BOLT;
    default: return "";
    }
}
