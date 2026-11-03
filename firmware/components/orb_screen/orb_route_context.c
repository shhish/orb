#include "orb_route_context.h"
#include "orb_screen.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint16_t version, size;
    int32_t scroll, focus;
    uint32_t page;
} route_context_t;

_Static_assert(sizeof(route_context_t) == sizeof(orb_route_context_t), "route context footprint");
_Static_assert(offsetof(route_context_t, scroll) == sizeof(orb_screen_context_t), "context payload");

static route_context_t read(const orb_route_context_t *context)
{
    route_context_t value = {0};
    if (context) memcpy(&value, context, sizeof(value));
    /* Every payload byte is one I32/I32/U32 value. The only invalid encodings
     * are an unknown header; zero headers retain unused-slot observations. */
    if ((value.version || value.size) && (value.version != 1 || value.size != sizeof(value)))
        value = (route_context_t){0};
    return value;
}

orb_route_location_t orb_route_context_read(const orb_route_context_t *context)
{
    route_context_t value = read(context);
    return (orb_route_location_t){.scroll = value.scroll, .focus = value.focus,
        .page = value.page, .valid = value.version == 1};
}

void orb_route_context_save(orb_route_context_t *context, int32_t scroll, uint32_t page, int32_t focus)
{
    if (!context) return;
    route_context_t value = {.version = 1, .size = sizeof(value), .scroll = scroll, .focus = focus, .page = page};
    memcpy(context, &value, sizeof(value));
}

void orb_route_context_scroll(orb_route_context_t *context, int32_t scroll)
{
    if (!context) return;
    route_context_t value = read(context);
    value.scroll = scroll;
    memcpy(context, &value, sizeof(value));
}

int32_t orb_route_context_reading(orb_route_context_t *context, uint32_t identity)
{
    orb_route_location_t value = orb_route_context_read(context);
    int32_t scroll = value.valid && value.focus == (int32_t)identity ? value.scroll : 0;
    orb_route_context_save(context, scroll, value.page, (int32_t)identity);
    return scroll;
}
