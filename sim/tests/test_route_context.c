#include "orb_route_context.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

int main(void)
{
    /* Route slots stay independent, inline and usable without LVGL, a service
     * or an allocator. A preview can copy one value without inheriting a bank. */
    orb_route_context_t routes[31] = {0};
    assert(sizeof(routes) == 31 * 16);
    orb_route_location_t value = orb_route_context_read(&routes[25]);
    assert(!value.valid && !value.scroll && !value.page && !value.focus);
    orb_route_context_scroll(&routes[25], 123);
    value = orb_route_context_read(&routes[25]);
    assert(!value.valid && value.scroll == 123);
    assert(orb_route_context_reading(&routes[25], 0xfedcba98u) == 0);
    orb_route_context_scroll(&routes[25], 321);
    assert(orb_route_context_reading(&routes[25], 0xfedcba98u) == 321);
    assert(orb_route_context_reading(&routes[25], 42) == 0);
    orb_route_context_save(&routes[25], 456, 7, 42);
    assert(orb_route_context_reading(&routes[25], 42) == 456);
    value = orb_route_context_read(&routes[25]);
    assert(value.valid && value.page == 7 && value.focus == 42);
    /* Ordinary departure overwrites the reading identity with Apps focus.
     * Retain this existing shell behavior instead of fixing it in extraction. */
    orb_route_context_save(&routes[25], 456, 7, 2);
    assert(orb_route_context_reading(&routes[25], 42) == 0);
    assert(orb_route_context_read(&routes[25]).page == 7);
    assert(!orb_route_context_read(&routes[2]).valid);
    orb_route_context_save(&routes[2], INT32_MIN, UINT32_MAX, INT32_MAX);
    value = orb_route_context_read(&routes[2]);
    assert(value.valid && value.scroll == INT32_MIN && value.page == UINT32_MAX && value.focus == INT32_MAX);
    orb_route_context_t copy = routes[2];
    memset(&routes[2], 0, sizeof(routes[2]));
    value = orb_route_context_read(&copy);
    assert(value.valid && value.scroll == INT32_MIN && value.page == UINT32_MAX && value.focus == INT32_MAX);
    /* Unknown version and wrong size cannot restore stale or malformed data. */
    unsigned char *bytes = (unsigned char *)&copy;
    bytes[0] = 2;
    assert(!orb_route_context_read(&copy).valid);
    orb_route_context_save(&copy, 8, 9, 10);
    bytes[2] = 1;
    assert(!orb_route_context_read(&copy).valid);
    orb_route_context_save(&copy, -10, 0, -20);
    assert(orb_route_context_reading(&copy, (uint32_t)-20) == -10);
    assert(!orb_route_context_read(NULL).valid);
    orb_route_context_save(NULL, 1, 2, 3);
    orb_route_context_scroll(NULL, 1);
    assert(orb_route_context_reading(NULL, 1) == 0);
    return 0;
}
