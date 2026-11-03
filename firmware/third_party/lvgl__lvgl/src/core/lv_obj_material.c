/**
 * ORB Stage 1: exact retained material presentation. A retained rectangle is a
 * proof over an ordered dependency list, never an object-ID or frame-number hit.
 * The UI thread snapshots dependencies and drains tasks before touching pixels;
 * workers only consume the admitted map during that object's synchronous draw.
 */
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif
#include "lv_obj_material_private.h"
#include "lv_obj_private.h"
#include "lv_obj_raster_private.h"
#include "lv_obj_draw_private.h"
#include "../draw/lv_draw_private.h"
#include "../misc/lv_area_private.h"
#include "../display/lv_display_private.h"
#include <stdatomic.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

#if (defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) || \
    (defined(CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) && CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) || \
    (defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST)
static atomic_uint s_kernels, s_recaptures, s_preparation_us, s_solid_fills;
static uint32_t s_last_solid;
static uint64_t s_total_solid;
static uint32_t s_last_kernels, s_last_recaptures, s_last_preparation;
static uint64_t s_total_kernels, s_total_recaptures, s_total_preparation;

static void snapshot_work(lv_draw_sw_material_stats_t * out)
{
    /* UI-thread snapshots extend modulo-32 worker counters outside hot loops.
     * Phase snapshots are <=60 s apart, far below one preparation-time wrap. */
    uint32_t kernels = atomic_load(&s_kernels);
    uint32_t recaptures = atomic_load(&s_recaptures);
    uint32_t preparation = atomic_load(&s_preparation_us);
    s_total_kernels += (uint32_t)(kernels - s_last_kernels);
    s_total_recaptures += (uint32_t)(recaptures - s_last_recaptures);
    s_total_preparation += (uint32_t)(preparation - s_last_preparation);
    s_last_kernels = kernels;
    s_last_recaptures = recaptures;
    s_last_preparation = preparation;
    out->blur_kernels = s_total_kernels;
    out->source_recaptures = s_total_recaptures;
    out->preparation_us = s_total_preparation;
    uint32_t solid = atomic_load(&s_solid_fills);
    s_total_solid += (uint32_t)(solid - s_last_solid);
    s_last_solid = solid;
    out->solid_fills = s_total_solid;
}
#endif

void lv_obj_material_count_kernel(void)
{
#if (defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) || \
    (defined(CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) && CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING)
    atomic_fetch_add(&s_kernels, 1);
    atomic_fetch_add(&s_recaptures, 1);
#endif
}

void lv_obj_material_count_solid(void)
{
#if defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST
    atomic_fetch_add(&s_solid_fills, 1);
#endif
}

uint64_t lv_obj_material_preparation_begin(void)
{
#if defined(CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) && CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
#else
    return 0;
#endif
}

void lv_obj_material_preparation_end(uint64_t started)
{
#if defined(CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) && CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    atomic_fetch_add(&s_preparation_us, (uint32_t)(lv_obj_material_preparation_begin() - started));
#else
    LV_UNUSED(started);
#endif
}

#if defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL
#define MATERIAL_SLOTS 3
#define MATERIAL_DEPENDENCIES 32
#define MATERIAL_BUDGET (192u * 1024u)
#define MATERIAL_MAX_HEIGHT 160
#define REVISION_SLOTS 256
typedef struct { const lv_obj_t * object; uint32_t revision; } revision_t;
static revision_t * s_revisions;
static void (*s_revision_free)(void *);

typedef struct {
    const lv_obj_t * object;
    uint32_t revision;
    lv_area_t bounds, clip;
    int32_t transform_width, transform_height;
    lv_opa_t opacity;
} dependency_t;

typedef struct {
    int32_t radius, corner;
    lv_blur_quality_t quality;
    lv_color_format_t format;
    uint8_t factor;
    lv_color_t tint;
    lv_opa_t tint_opa, task_opa;
} material_key_t;

typedef struct {
    lv_obj_t * object;
    void (*release)(void *);
    uint8_t * raw, * base, * map;
    size_t charge, raw_capacity, base_capacity, map_capacity, map_bytes;
    lv_area_t coords, patch;
    material_key_t key, filter_key, pending_key;
    dependency_t dependencies[MATERIAL_DEPENDENCIES];
    uint32_t count;
    uint8_t rows[MATERIAL_MAX_HEIGHT];
    int32_t padding;
    bool dependencies_valid, map_valid, base_valid, raw_valid, background_valid;
    bool restored, skip_blur, capturing, filter_pending, suspended;
} material_t;

static material_t * s_materials[MATERIAL_SLOTS];
static uint32_t s_revision;
static bool s_enabled = true;
static lv_draw_sw_material_stats_t s_stats;
static atomic_uint s_map_hits, s_map_builds;

static bool area_equal(const lv_area_t * a, const lv_area_t * b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

static material_t * find(const lv_obj_t * object)
{
    for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
        if(s_materials[i] && s_materials[i]->object == object) return s_materials[i];
    }
    return NULL;
}

static void drain(lv_layer_t * layer)
{
    while(layer->draw_task_head) {
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
}

static uint32_t revision(const lv_obj_t * object, bool changed)
{
    unsigned victim = 0;
    for(unsigned i = 0; i < REVISION_SLOTS; i++) {
        if(s_revisions[i].object == object) {
            if(changed) s_revisions[i].revision = s_revision;
            return s_revisions[i].revision;
        }
        if(s_revisions[i].revision < s_revisions[victim].revision) victim = i;
    }
    /* Eviction makes any old dependency compare fail; no pointer is ever
     * dereferenced from the registry. Creation invalidation gives a reused
     * address a new generation before it can enter a dependency list. */
    s_revisions[victim] = (revision_t){object, s_revision};
    return s_revision;
}

void lv_obj_material_note_invalidation(const lv_obj_t * object)
{
#if defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY
    /* note_motion invalidates every lease before moving children. Their next
     * stable proof starts fresh, so do not scan the revision registry for each
     * animated row's position/opacity/style invalidation. */
    if(lv_draw_sw_blur_motion_active(lv_obj_get_display(object))) return;
#endif
    if(++s_revision == 0) {
        ++s_revision;
        if(s_revisions) lv_memzero(s_revisions, sizeof(revision_t) * REVISION_SLOTS);
        for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
            if(s_materials[i]) s_materials[i]->dependencies_valid = false;
        }
    }
    if(!s_revisions) return;
    /* Child content invalidation also changes every retained raster source
     * above it, while pure presentation movement keeps the source bitmap. */
    for(const lv_obj_t * p = object; p; p = p->parent) {
        if(p == object || lv_obj_get_raster(p)) revision(p, true);
    }
}

void lv_obj_material_note_motion(lv_display_t * display)
{
#if defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY
    for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
        if(s_materials[i] && lv_obj_get_display(s_materials[i]->object) == display) {
            s_materials[i]->dependencies_valid = false;
        }
    }
#else
    LV_UNUSED(display);
#endif
}

static bool custom_drawing(lv_obj_t * object)
{
    for(uint32_t i = 0; i < lv_obj_get_event_count(object); i++) {
        lv_event_code_t code = (lv_obj_get_event_dsc(object, i)->filter & ~LV_EVENT_PREPROCESS);
        if(code == LV_EVENT_ALL || (code >= LV_EVENT_DRAW_MAIN_BEGIN && code <= LV_EVENT_DRAW_POST_END)) return true;
    }
    return false;
}

/* Raster roots are atomic sources. Their descendants' invalidations propagate
 * to the source generation above; a presentation move preserves the raster pixels
 * but still changes this generation. Unsupported callback painting never earns
 * a dependency proof. The callback owner can instead publish a managed raster. */
static bool walk(material_t * m, lv_obj_t * object, lv_area_t clip, uint32_t * count, bool compare, bool * found)
{
    if(object == m->object) { *found = true; return true; }
    if(lv_obj_is_hidden(object)) return true;
    lv_area_t bounds = object->coords;
    int32_t ext = lv_obj_get_ext_draw_size(object);
    lv_area_increase(&bounds, ext, ext);
    bool intersects = lv_area_is_on(&bounds, &m->patch);
    if(intersects) {
        if(lv_obj_get_layer_type(object) != LV_LAYER_TYPE_NONE || object->has_blur ||
           (!lv_obj_get_raster(object) && custom_drawing(object)) || *count == MATERIAL_DEPENDENCIES) return false;
        dependency_t d;
        lv_memzero(&d, sizeof(d));
        d.object = object;
        d.revision = revision(object, false);
        d.bounds = bounds;
        d.clip = clip;
        d.transform_width = lv_obj_get_style_transform_width(object, LV_PART_MAIN);
        d.transform_height = lv_obj_get_style_transform_height(object, LV_PART_MAIN);
        d.opacity = lv_obj_get_style_opa_recursive(object, LV_PART_MAIN);
        if(compare) {
            if(*count >= m->count || lv_memcmp(&d, &m->dependencies[*count], sizeof(d))) return false;
        }
        else m->dependencies[*count] = d;
        ++*count;
    }
    if(lv_obj_get_raster(object)) return true;
    if(!lv_obj_is_overflow_visible(object) && !lv_area_intersect(&clip, &clip, &object->coords)) return true;
    for(uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        if(!walk(m, lv_obj_get_child(object, i), clip, count, compare, found)) return false;
        if(*found) return true;
    }
    return true;
}

static bool supported_control(lv_obj_t * object)
{
    /* Orb owns these controls' paint contract. A caller adding a stock border,
     * image, shadow, gradient or clipped layer has changed that contract. */
    return lv_obj_get_style_blur_backdrop(object, LV_PART_MAIN) &&
           lv_obj_get_style_blur_quality(object, LV_PART_MAIN) == LV_BLUR_QUALITY_SPEED &&
           lv_obj_get_style_blur_radius(object, LV_PART_MAIN) > 0 &&
           lv_obj_get_style_bg_grad_dir(object, LV_PART_MAIN) == LV_GRAD_DIR_NONE &&
           !lv_obj_get_style_bg_image_src(object, LV_PART_MAIN) &&
           lv_obj_get_style_border_width(object, LV_PART_MAIN) == 0 &&
           lv_obj_get_style_outline_width(object, LV_PART_MAIN) == 0 &&
           lv_obj_get_style_shadow_width(object, LV_PART_MAIN) == 0 &&
           !lv_obj_get_style_clip_corner(object, LV_PART_MAIN);
}

static bool dependencies(material_t * m, bool compare)
{
    lv_display_t * display = lv_obj_get_display(m->object);
    lv_obj_t * root = lv_obj_get_screen(m->object);
    /* A native opaque screen makes lower display layers irrelevant. Transition
     * layers, rotation, alpha/layered opacity and unmanaged roots fall back. */
    if(!supported_control(m->object) || root != lv_display_get_screen_active(display) || display->prev_scr ||
       lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_0 ||
       lv_obj_get_style_bg_opa(root, LV_PART_MAIN) != LV_OPA_COVER ||
       lv_obj_get_style_opa_recursive(m->object, LV_PART_MAIN) != LV_OPA_COVER ||
       lv_obj_get_layer_type(m->object) != LV_LAYER_TYPE_NONE || lv_obj_is_hidden(m->object)) return false;
    lv_area_t clip = {0, 0, lv_display_get_horizontal_resolution(display) - 1,
                     lv_display_get_vertical_resolution(display) - 1};
    uint32_t count = 0;
    bool found = false;
    if(!walk(m, root, clip, &count, compare, &found) || !found || (compare && count != m->count)) return false;
    if(!compare) m->count = count;
    return true;
}

static bool geometry(material_t * m, lv_area_t * coords, lv_area_t * patch)
{
    *coords = m->object->coords;
    lv_area_increase(coords, lv_obj_get_style_transform_width(m->object, LV_PART_MAIN),
                     lv_obj_get_style_transform_height(m->object, LV_PART_MAIN));
    *patch = *coords;
    lv_area_increase(patch, m->padding, m->padding);
    /* The CO5300 rounds invalidations to even windows. Retain those exposed
     * outer pixels too or a one-pixel strip would still transform all rows. */
    patch->x1 &= ~1; patch->y1 &= ~1;
    patch->x2 |= 1; patch->y2 |= 1;
    lv_display_t * display = lv_obj_get_display(m->object);
    lv_area_t screen = {0, 0, lv_display_get_horizontal_resolution(display) - 1,
                       lv_display_get_vertical_resolution(display) - 1};
    if(!lv_area_intersect(coords, coords, &screen) || !lv_area_intersect(patch, patch, &screen)) return false;
    return (size_t)lv_area_get_size(coords) * 2 <= m->raw_capacity &&
           (size_t)lv_area_get_size(patch) * 2 <= m->base_capacity &&
           lv_area_get_height(patch) <= MATERIAL_MAX_HEIGHT;
}

static material_key_t current_key(material_t * m, lv_layer_t * layer)
{
    lv_draw_blur_dsc_t dsc;
    lv_draw_blur_dsc_init(&dsc);
    lv_obj_init_draw_blur_dsc(m->object, LV_PART_MAIN, &dsc);
    material_key_t key;
    lv_memzero(&key, sizeof(key));
    key.radius = dsc.blur_radius;
    key.corner = lv_obj_get_style_radius(m->object, LV_PART_MAIN);
    key.quality = dsc.quality;
    key.format = layer->color_format;
    key.factor = dsc.downsample_factor;
    key.tint = lv_obj_get_style_bg_color(m->object, LV_PART_MAIN);
    key.tint_opa = LV_OPA_MIX2(lv_obj_get_style_bg_opa(m->object, LV_PART_MAIN), layer->opa);
    key.task_opa = layer->opa;
    return key;
}

static void copy_region(lv_layer_t * layer, const lv_area_t * full, uint8_t * pixels, bool restore)
{
    lv_area_t a;
    if(!lv_area_intersect(&a, full, &layer->_clip_area) || !lv_area_intersect(&a, &a, &layer->buf_area)) return;
    size_t stride = (size_t)lv_area_get_width(full) * 2;
    for(int32_t y = a.y1; y <= a.y2; y++) {
        void * dest = lv_draw_layer_go_to_xy(layer, a.x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        uint8_t * retained = pixels + (size_t)(y - full->y1) * stride + (a.x1 - full->x1) * 2;
        if(restore) lv_memcpy(dest, retained, (size_t)lv_area_get_width(&a) * 2);
        else lv_memcpy(retained, dest, (size_t)lv_area_get_width(&a) * 2);
    }
}

bool lv_obj_material_register(lv_obj_t * object, int32_t width, int32_t height, int32_t padding,
                             void * (*allocate)(size_t), void (*release)(void *))
{
    if(!object || !allocate || !release || width <= 0 || width > 4096 || height <= 0 || height > MATERIAL_MAX_HEIGHT ||
       padding < 0 || padding > MATERIAL_MAX_HEIGHT ||
       height + 2 * padding > MATERIAL_MAX_HEIGHT || find(object)) return false;
    /* Only the presented screen needs material payloads. The benchmark keeps
     * the real shell alive while constructing a second screen; allowing its
     * invisible controls to pin all three slots disabled retention entirely.
     * Orb retries admission at DRAW_MAIN_BEGIN, so returning to the preserved
     * screen restores leases without retaining two screens' 192 KiB payloads. */
    lv_obj_t * screen = lv_obj_get_screen(object);
    for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
        if(s_materials[i] && lv_obj_get_screen(s_materials[i]->object) != screen) {
            lv_obj_material_unregister(s_materials[i]->object);
        }
    }
    unsigned slot;
    for(slot = 0; slot < MATERIAL_SLOTS && s_materials[slot]; slot++);
    if(slot == MATERIAL_SLOTS) return false;
    size_t raw = (size_t)width * height * 2;
    size_t base = (size_t)(width + 2 * padding + 2) * (height + 2 * padding + 2) * 2;
    size_t map = (size_t)((width + 1) / 2) * ((height + 1) / 2) * 3;
    size_t total = sizeof(material_t) + raw + base + map;
    if(total > MATERIAL_BUDGET - s_stats.bytes) return false;
    bool new_registry = s_revisions == NULL;
    size_t registry_bytes = new_registry ? sizeof(revision_t) * REVISION_SLOTS : 0;
    if(total + registry_bytes > MATERIAL_BUDGET - s_stats.bytes) return false;
#ifdef ESP_PLATFORM
    /* Registration runs on the UI owner before dispatch. Visible controls pin
     * their material; shared pressure evicts idle sources/masks before falling
     * back to the ordinary exact blur renderer. */
    if(!orb_memory_cache_admit(ORB_MEMORY_GLASS, total + registry_bytes)) return false;
#endif
    if(new_registry) {
        s_revisions = allocate(registry_bytes);
        if(!s_revisions) {
#ifdef ESP_PLATFORM
            orb_memory_release(ORB_MEMORY_GLASS, total + registry_bytes);
#endif
            return false;
        }
        lv_memzero(s_revisions, registry_bytes);
        s_revision_free = release;
    }
    material_t * m = allocate(total);
    if(!m) {
        if(new_registry) { release(s_revisions); s_revisions = NULL; }
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_GLASS, total + registry_bytes);
#endif
        return false;
    }
    s_stats.bytes += registry_bytes;
    lv_memzero(m, sizeof(*m));
    m->object = object;
    m->release = release;
    m->charge = total;
    m->raw_capacity = raw;
    m->base_capacity = base;
    m->map_capacity = map;
    m->raw = (uint8_t *)(m + 1);
    m->base = m->raw + raw;
    m->map = m->base + base;
    m->padding = padding;
    s_materials[slot] = m;
    s_stats.bytes += total;
    s_stats.entries++;
    s_stats.peak_bytes = LV_MAX(s_stats.peak_bytes, s_stats.bytes);
    return true;
}

void lv_obj_material_unregister(lv_obj_t * object)
{
    for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
        material_t * m = s_materials[i];
        if(!m || m->object != object) continue;
        /* Orb calls after that object's draw has drained; no queued task owns
         * a retained pointer after capture_base's synchronous fence. */
        lv_draw_wait_for_finish();
        s_materials[i] = NULL;
        s_stats.bytes -= m->charge;
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_GLASS, m->charge);
#endif
        s_stats.entries--;
        m->release(m);
        if(s_stats.entries == 0 && s_revisions) {
            s_revision_free(s_revisions);
            s_revisions = NULL;
            s_stats.bytes -= sizeof(revision_t) * REVISION_SLOTS;
#ifdef ESP_PLATFORM
            orb_memory_release(ORB_MEMORY_GLASS, sizeof(revision_t) * REVISION_SLOTS);
#endif
        }
    }
}

void lv_obj_material_set_enabled(bool enabled)
{
    s_enabled = enabled;
    for(unsigned i = 0; i < MATERIAL_SLOTS; i++) {
        if(s_materials[i]) s_materials[i]->dependencies_valid = false;
    }
}

void lv_obj_material_set_active(lv_obj_t * object, bool active)
{
    material_t * m = find(object);
    if(!m || m->suspended == !active) return;
    m->suspended = !active;
    m->dependencies_valid = false;
}

static bool backdrop_moving(const lv_obj_t * object)
{
#if defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY
    return lv_draw_sw_blur_motion_active(lv_obj_get_display(object));
#else
    LV_UNUSED(object);
    return false;
#endif
}

bool lv_obj_material_cover(lv_obj_t * object, const lv_area_t * area)
{
    material_t * m = find(object);
    if(backdrop_moving(object) || !s_enabled || !m || !m->dependencies_valid || !m->background_valid || !m->raw_valid || m->suspended) return false;
    lv_area_t coords, patch;
    return geometry(m, &coords, &patch) && area_equal(&coords, &m->coords) && area_equal(&patch, &m->patch) &&
           lv_area_is_in(area, &m->patch, 0) && dependencies(m, true);
}

void lv_obj_material_expand_draw(lv_obj_t * object, lv_area_t * area)
{
    material_t * m = find(object);
    lv_area_t coords, patch;
    if(m && s_enabled && !backdrop_moving(object) && geometry(m, &coords, &patch)) *area = patch;
}

void lv_obj_material_restore(lv_layer_t * layer, lv_obj_t * object)
{
    material_t * m = find(object);
    if(!m) return;
    m->restored = m->skip_blur = m->capturing = m->filter_pending = false;
    if(backdrop_moving(object)) {
        /* HIG §2/§6: the live backdrop stays exact while scrolling. Retention
         * otherwise re-captures and re-builds every moving frame. Keep payload
         * leases, but do no tree proof, drain, copy or filter-map publication;
         * the settle invalidation builds the first new stable base. */
        m->dependencies_valid = m->raw_valid = m->base_valid = m->map_valid = m->background_valid = false;
        s_stats.moving_bypasses++;
        return;
    }
    if(!s_enabled || m->suspended || (layer->color_format != LV_COLOR_FORMAT_RGB565 &&
                      layer->color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) return;
    lv_area_t coords, patch;
    if(!geometry(m, &coords, &patch)) return;
    bool same = m->dependencies_valid && area_equal(&coords, &m->coords) && area_equal(&patch, &m->patch) &&
                dependencies(m, true);
    if(!same) {
        m->coords = coords;
        m->patch = patch;
        m->dependencies_valid = dependencies(m, false);
        m->raw_valid = m->base_valid = m->map_valid = m->background_valid = false;
        lv_memzero(m->rows, sizeof(m->rows));
    }
    if(!m->dependencies_valid) { s_stats.fallbacks++; return; }
    material_key_t key = current_key(m, layer);
    if(lv_memcmp(&key, &m->pending_key, sizeof(key))) {
        /* A shadow-only first band cannot publish a new tint/filter key while
         * inner rows still contain the preceding material. Keep the proven
         * background, but accumulate a complete new base for this exact key. */
        m->base_valid = false;
        lv_memzero(m->rows, sizeof(m->rows));
    }
    m->pending_key = key;
    bool base_same = m->base_valid && !lv_memcmp(&key, &m->key, sizeof(key));
    drain(layer);
    if(same && m->background_valid && m->raw_valid) {
        copy_region(layer, &m->patch, m->base, true);
        if(base_same) { m->skip_blur = true; s_stats.base_hits++; }
        else copy_region(layer, &m->coords, m->raw, true);
        m->restored = true;
    }
    m->capturing = true;
}

bool lv_obj_material_managed(const lv_draw_blur_dsc_t * dsc)
{
    material_t * m = find(dsc->base.obj);
    return m && m->capturing;
}

bool lv_obj_material_skip_blur(const lv_draw_blur_dsc_t * dsc)
{
    material_t * m = find(dsc->base.obj);
    return m && m->restored && m->skip_blur;
}

bool lv_obj_material_filter_get(const lv_draw_task_t * task, const lv_draw_blur_dsc_t * dsc,
                               const lv_area_t * coords, const lv_area_t * clipped,
                               const uint8_t * source, uint32_t stride, uint32_t px_size,
                               uint8_t * small, size_t bytes)
{
    material_t * m = find(dsc->base.obj);
    if(!m || !m->capturing || px_size != 2 || !area_equal(coords, clipped) ||
       !area_equal(coords, &m->coords) || bytes > m->map_capacity || dsc->quality != LV_BLUR_QUALITY_SPEED) return false;
    LV_UNUSED(task);
    material_key_t key = m->pending_key;
    bool same_filter = m->map_valid && m->map_bytes == bytes && key.radius == m->filter_key.radius &&
                       key.quality == m->filter_key.quality && key.factor == m->filter_key.factor &&
                       key.format == m->filter_key.format;
    m->filter_pending = !same_filter;
    if(same_filter) {
        lv_memcpy(small, m->map, bytes);
        atomic_fetch_add(&s_map_hits, 1);
        return true;
    }
    size_t row_bytes = (size_t)lv_area_get_width(coords) * 2;
    for(int32_t y = 0; y < lv_area_get_height(coords); y++) lv_memcpy(m->raw + y * row_bytes, source + y * stride, row_bytes);
    m->raw_valid = true;
    m->map_valid = false;
    return false;
}

void lv_obj_material_filter_store(const lv_draw_blur_dsc_t * dsc, const uint8_t * small, size_t bytes)
{
    material_t * m = find(dsc->base.obj);
    if(!m || !m->filter_pending || bytes > m->map_capacity) return;
    lv_memcpy(m->map, small, bytes);
    m->map_bytes = bytes;
    m->map_valid = true;
    m->filter_key = m->pending_key;
    m->filter_pending = false;
    atomic_fetch_add(&s_map_builds, 1);
}

void lv_obj_material_store_solid(const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords,
                               const lv_area_t * clipped, const uint8_t * source, uint32_t stride,
                               const uint8_t rgb[3], size_t bytes)
{
    material_t * m = find(dsc->base.obj);
    if(!m || !m->capturing || !area_equal(coords, clipped) || !area_equal(coords, &m->coords) ||
       bytes > m->map_capacity || dsc->quality != LV_BLUR_QUALITY_SPEED) return;
    size_t row_bytes = (size_t)lv_area_get_width(coords) * 2;
    for(int32_t y = 0; y < lv_area_get_height(coords); y++) {
        lv_memcpy(m->raw + y * row_bytes, source + y * stride, row_bytes);
    }
    for(size_t i = 0; i < bytes; i += 3) lv_memcpy(m->map + i, rgb, 3);
    m->raw_valid = m->map_valid = true;
    m->map_bytes = bytes;
    m->filter_key = m->pending_key;
    atomic_fetch_add(&s_map_builds, 1);
}

void lv_obj_material_capture_base(lv_layer_t * layer, lv_obj_t * object)
{
    material_t * m = find(object);
    if(!m || !m->capturing || m->skip_blur) return;
    drain(layer);
    copy_region(layer, &m->patch, m->base, false);
    lv_area_t a;
    if(lv_area_intersect(&a, &m->patch, &layer->_clip_area) && lv_area_intersect(&a, &a, &layer->buf_area) &&
       a.x1 == m->patch.x1 && a.x2 == m->patch.x2) {
        for(int32_t y = a.y1; y <= a.y2; y++) m->rows[y - m->patch.y1] = 1;
    }
    m->base_valid = m->raw_valid && m->map_valid;
    for(int32_t y = 0; y < lv_area_get_height(&m->patch); y++) m->base_valid &= m->rows[y] != 0;
    if(m->base_valid) {
        m->background_valid = true;
        m->key = m->pending_key;
        s_stats.base_builds++;
    }
}

void lv_draw_sw_material_get_stats(lv_draw_sw_material_stats_t * out)
{
    if(!out) return;
    *out = s_stats;
    out->map_hits = atomic_load(&s_map_hits);
    out->map_builds = atomic_load(&s_map_builds);
    snapshot_work(out);
    out->budget_bytes = MATERIAL_BUDGET;
}
#else
bool lv_obj_material_register(lv_obj_t * object, int32_t width, int32_t height, int32_t padding,
                             void * (*allocate)(size_t), void (*release)(void *))
{
    LV_UNUSED(object); LV_UNUSED(width); LV_UNUSED(height); LV_UNUSED(padding);
    LV_UNUSED(allocate); LV_UNUSED(release); return false;
}
void lv_obj_material_unregister(lv_obj_t * object) { LV_UNUSED(object); }
void lv_obj_material_capture_base(lv_layer_t * layer, lv_obj_t * object) { LV_UNUSED(layer); LV_UNUSED(object); }
void lv_obj_material_set_enabled(bool enabled) { LV_UNUSED(enabled); }
void lv_obj_material_set_active(lv_obj_t * object, bool active) { LV_UNUSED(object); LV_UNUSED(active); }
void lv_draw_sw_material_get_stats(lv_draw_sw_material_stats_t * out)
{
    if(!out) return;
    lv_memzero(out, sizeof(*out));
#if (defined(CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) && CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING) || \
    (defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST)
    snapshot_work(out);
#endif
}
#endif
