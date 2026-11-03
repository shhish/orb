#include "orb_nav_compositor.h"
#include "esp_heap_caps.h"
#include "orb_memory.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>

static void *s_allocations[ORB_NAV_SNAPSHOT_SLOTS];
static unsigned s_allocated, s_released;

void *orb_memory_alloc(orb_memory_owner_t owner, size_t bytes)
{
    assert(owner == ORB_MEMORY_NAV);
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void orb_memory_free(void *pixels) { heap_caps_free(pixels); }

void *heap_caps_malloc(size_t bytes, uint32_t capabilities)
{
    assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(s_allocated < ORB_NAV_SNAPSHOT_SLOTS);
    void *pixels = malloc(bytes);
    assert(pixels);
    s_allocations[s_allocated++] = pixels;
    return pixels;
}

void heap_caps_free(void *pixels)
{
    unsigned i = 0;
    while (i < s_allocated && s_allocations[i] != pixels) i++;
    assert(i < s_allocated);
    s_allocations[i] = NULL;
    s_released++;
    free(pixels);
}

int main(void)
{
    /* Compile the real ESP default path to check explicit heap-capability
     * release pairing. The compositor itself uses libc. */
    assert(orb_nav_compositor_reserve_boot(16, 16));
    assert(s_allocated == ORB_NAV_SNAPSHOT_SLOTS);
    orb_nav_compositor_t *c = orb_nav_compositor_create(16, 16);
    assert(c);
    for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    assert(s_allocated == ORB_NAV_SNAPSHOT_SLOTS);
    assert(orb_nav_compositor_swap(c, 0, 1));
    assert(orb_nav_compositor_valid(c, 0, 1));
    orb_nav_compositor_destroy(c);
    assert(s_allocated == ORB_NAV_SNAPSHOT_SLOTS);
    assert(s_released == s_allocated);
    return 0;
}
