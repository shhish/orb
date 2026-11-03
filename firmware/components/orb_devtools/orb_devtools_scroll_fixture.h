#pragma once

#include "orb_scroll_fixture.h"

static inline orb_scroll_fixture_t *orb_devtools_scroll_fixture_create(lv_obj_t *parent,
                                                                     orb_scroll_surface_t surface)
{
    return orb_scroll_fixture_create(parent, surface);
}
