#pragma once

/* Face data rules shared by every face, editor and the locked surface:
 * dates, weather freshness and complication values. No LVGL objects. */
#include "orb_faces.h"
#include "orb_icons.h"

#define ORB_FACES_STALE_GLYPH ORB_ICON_CLOCK

/* Polish #14-16: one rule on every face. A value the model still holds is
 * shown dimmed beside the shared stale mark (the clock glyph); "—" beside
 * the provider's own glyph appears only when there is no value at all. */
typedef enum { ORB_FACES_WEATHER_NONE, ORB_FACES_WEATHER_STALE, ORB_FACES_WEATHER_CURRENT } orb_faces_weather_t;

bool orb_faces_clock_valid(time_t now);
orb_faces_weather_t orb_faces_weather_state(const orb_faces_snapshot_t *s);
const char *orb_faces_weather_glyph(const orb_faces_snapshot_t *s);
void orb_faces_weather_short(const orb_faces_snapshot_t *s, char *value, size_t capacity);
void orb_faces_weekday_short(char *weekday);
void orb_faces_day_parts(const orb_faces_snapshot_t *s, char *weekday, size_t weekday_size,
                         char *day, size_t day_size, char *clock, size_t clock_size);
void orb_faces_activity_summary(const orb_faces_snapshot_t *s, char *out, size_t capacity);
/* `locked`: Lock/Always On withhold the household count (polish #42). */
bool orb_faces_complication_data(const orb_faces_snapshot_t *s, bool locked, orb_face_provider_t provider,
                                 orb_face_complication_t *out);
