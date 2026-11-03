#include "orb_nav_compositor_private.h"

#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "orb_memory.h"
#endif

static orb_nav_compositor_t *s_boot_pool;

static void *snapshot_allocate(size_t bytes, void *context)
{
    (void)context;
#ifdef ESP_PLATFORM
    return orb_memory_alloc(ORB_MEMORY_NAV, bytes);
#else
    return malloc(bytes);
#endif
}

static void snapshot_release(void *pixels, void *context)
{
    (void)context;
#ifdef ESP_PLATFORM
    orb_memory_free(pixels);
#else
    free(pixels);
#endif
}

bool orb_nav_compositor_admit_slot(orb_nav_compositor_t *c, unsigned slot)
{
    if (!slot_exists(c, slot)) return false;
    snapshot_t *s = &c->slots[slot];
    if (s->pixels) return true;
    size_t bytes = (size_t)c->width * c->height * sizeof(uint16_t);
    s->pixels = c->allocate(bytes, c->allocator_context);
    if (!s->pixels) return false;
    c->stats.snapshot_bytes += bytes;
    return true;
}

orb_nav_compositor_t *orb_nav_compositor_create(uint16_t width, uint16_t height)
{
    if (s_boot_pool && s_boot_pool->width == width && s_boot_pool->height == height) {
        orb_nav_compositor_t *c = s_boot_pool;
        s_boot_pool = NULL;
        return c;
    }
    orb_nav_compositor_t *c = orb_nav_compositor_create_with_allocator(width, height,
        snapshot_allocate, snapshot_release, NULL);
    if (!c) return NULL;
    for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
        if (!orb_nav_compositor_admit_slot(c, slot)) {
            orb_nav_compositor_destroy(c);
            return NULL;
        }
    }
    return c;
}

bool orb_nav_compositor_reserve_boot(uint16_t width, uint16_t height)
{
    if (s_boot_pool) return s_boot_pool->width == width && s_boot_pool->height == height;
    s_boot_pool = orb_nav_compositor_create(width, height);
    return s_boot_pool != NULL;
}

orb_nav_compositor_t *orb_nav_compositor_create_with_allocator(uint16_t width, uint16_t height,
    orb_nav_snapshot_alloc_fn allocate, orb_nav_snapshot_free_fn release, void *context)
{
    if (!width || !height || width > 1024 || height > 1024 || !allocate || !release) return NULL;
    orb_nav_compositor_t *c = calloc(1, sizeof(*c));
    if (c) {
        c->width = width; c->height = height;
        c->allocate = allocate; c->release = release; c->allocator_context = context;
    }
    return c;
}

void orb_nav_compositor_destroy(orb_nav_compositor_t *c)
{
    if (!c) return;
    for (unsigned i = 0; i < ORB_NAV_SNAPSHOT_SLOTS; i++)
        if (c->slots[i].pixels) c->release(c->slots[i].pixels, c->allocator_context);
    if (c->glyph) c->release(c->glyph, c->allocator_context);
    free(c);
}

bool orb_nav_compositor_swap(orb_nav_compositor_t *c, unsigned a, unsigned b)
{
    if (!slot_exists(c, a) || !slot_exists(c, b) || c->active ||
        c->slots[a].capturing || c->slots[b].capturing) return false;
    snapshot_t temporary = c->slots[a];
    c->slots[a] = c->slots[b];
    c->slots[b] = temporary;
    if (c->prepared == a) c->prepared = b;
    else if (c->prepared == b) c->prepared = a;
    return true;
}

bool orb_nav_compositor_capture_begin(orb_nav_compositor_t *c, unsigned slot, uint64_t revision)
{
    if (!orb_nav_compositor_admit_slot(c, slot) || (c->active && (slot == c->from || slot == c->to ||
        ((c->kind == ORB_NAV_TRANSITION_SHEET || c->kind == ORB_NAV_TRANSITION_BACK ||
          c->kind == ORB_NAV_TRANSITION_IRIS) &&
         slot == c->prepared)))) return false;
    snapshot_t *s = &c->slots[slot];
    if (slot == c->prepared || s->pixels == c->prepared_source) c->prepared_valid = false;
    s->valid = false; s->capturing = true; s->next_y = 0; s->revision = revision;
    memset(s->pixels, 0, (size_t)c->width * c->height * 2);
    return true;
}

uint16_t *orb_nav_compositor_pixels(orb_nav_compositor_t *c, unsigned slot)
{
    return slot_exists(c, slot) ? c->slots[slot].pixels : NULL;
}

bool orb_nav_compositor_capture_band(orb_nav_compositor_t *c, unsigned slot,
                                    int x, int y, int width, int height,
                                    const void *pixels, size_t stride, bool swapped)
{
    if (!slot_exists(c, slot) || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > c->width || y + height > c->height || stride < (size_t)width * 2) return false;
    snapshot_t *s = &c->slots[slot];
    if (!s->capturing || y != (int)s->next_y) { s->capturing = false; return false; }
    for (int row = 0; row < height; row++) {
        uint16_t *dst = s->pixels + (size_t)(y + row) * c->width + x;
        const uint8_t *src = (const uint8_t *)pixels + (size_t)row * stride;
        if (!swapped) memcpy(dst, src, (size_t)width * 2);
        else for (int col = 0; col < width; col++) dst[col] = (uint16_t)((src[col * 2] << 8) | src[col * 2 + 1]);
    }
    s->next_y += height;
    return true;
}

bool orb_nav_compositor_capture_complete(orb_nav_compositor_t *c, unsigned slot)
{
    if (!slot_exists(c, slot) || !c->slots[slot].capturing) return false;
    c->slots[slot].next_y = c->height;
    return orb_nav_compositor_capture_end(c, slot);
}

bool orb_nav_compositor_capture_end(orb_nav_compositor_t *c, unsigned slot)
{
    if (!slot_exists(c, slot)) return false;
    snapshot_t *s = &c->slots[slot];
    s->valid = s->capturing && s->next_y == c->height;
    s->capturing = false;
    if (s->valid) c->stats.captures++;
    return s->valid;
}

bool orb_nav_compositor_valid(const orb_nav_compositor_t *c, unsigned slot, uint64_t revision)
{
    return slot_exists(c, slot) && c->slots[slot].valid && c->slots[slot].revision == revision;
}

void orb_nav_compositor_invalidate(orb_nav_compositor_t *c, unsigned slot)
{
    if (!slot_exists(c, slot)) return;
    if (slot == c->prepared || c->slots[slot].pixels == c->prepared_source) c->prepared_valid = false;
    /* The prepared backdrop is a consumed plane too. Invalidation can admit
     * a new capture into it; retire its reader before those pixels change. */
    if (c->active && (slot == c->from || slot == c->to ||
        (!c->reduced_motion && slot == c->prepared &&
         (c->kind == ORB_NAV_TRANSITION_IRIS || c->kind == ORB_NAV_TRANSITION_SHEET ||
          c->kind == ORB_NAV_TRANSITION_BACK)))) orb_nav_compositor_cancel(c);
    c->slots[slot].valid = c->slots[slot].capturing = false;
}

void orb_nav_compositor_invalidate_all(orb_nav_compositor_t *c)
{
    if (!c) return;
    orb_nav_compositor_cancel(c);
    c->prepared_valid = false;
    for (unsigned i = 0; i < ORB_NAV_SNAPSHOT_SLOTS; i++) c->slots[i].valid = c->slots[i].capturing = false;
}
