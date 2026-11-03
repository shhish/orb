/**
 * ORB PATCH: exact backdrop reuse. A hit requires equal rendering parameters and
 * every input byte; hashes and object identities cannot establish a hit.
 * Metadata is locked, but allocation, comparison, copying and rendering run
 * outside the lock under a slot lease. Retained plus reserved bytes stay
 * within the configured budget even while allocations are in flight.
 */
#include "lv_draw_sw_blur_cache_private.h"
#include "../../osal/lv_os_private.h"
#include <stdatomic.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

#if LV_USE_DRAW_SW
#define CACHE_SLOTS 4
#define DEFAULT_ENTRY_LIMIT (96 * 1024)

typedef struct {
    lv_area_t coords, clipped;
    lv_color_format_t format;
    int32_t blur_radius, corner_radius;
    lv_blur_quality_t quality;
    uint32_t tint;
    lv_opa_t tint_opa, task_opa;
    uint8_t downsample_factor;
} cache_key_t;

struct _lv_blur_cache_entry_t {
    cache_key_t key;
    uint8_t * data;
    size_t capacity, image_bytes, row_bytes;
    uint32_t height;
    uint64_t used;
    bool valid, busy;
};

static struct {
    lv_mutex_t lock;
    lv_blur_cache_entry_t entries[CACHE_SLOTS];
    lv_draw_sw_blur_cache_config_t config;
    lv_draw_sw_blur_cache_stats_t stats;
    uint64_t clock;
    bool initialized, reconfiguring;
} cache;
static atomic_bool enabled;

static void * data_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    /* Draw workers cannot run UI eviction callbacks. Atomic admission shares
     * the optional cache ceiling; rejection uses the ordinary blur renderer. */
    if(!orb_memory_reserve(ORB_MEMORY_GLASS, bytes)) return NULL;
#endif
    void * data = cache.config.alloc_cb(bytes);
#ifdef ESP_PLATFORM
    if(!data) orb_memory_release(ORB_MEMORY_GLASS, bytes);
#endif
    return data;
}

static void data_free(void * data, size_t bytes, lv_draw_sw_blur_cache_free_cb_t release)
{
    if(!data) return;
    release(data);
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_GLASS, bytes);
#else
    LV_UNUSED(bytes);
#endif
}

static bool area_equal(const lv_area_t * a, const lv_area_t * b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

static bool key_equal(const cache_key_t * a, const cache_key_t * b)
{
    return area_equal(&a->coords, &b->coords) && area_equal(&a->clipped, &b->clipped) &&
           a->format == b->format && a->blur_radius == b->blur_radius &&
           a->corner_radius == b->corner_radius && a->quality == b->quality &&
           a->tint == b->tint && a->tint_opa == b->tint_opa && a->task_opa == b->task_opa &&
           a->downsample_factor == b->downsample_factor;
}

static lv_blur_cache_entry_t * oldest_idle(const lv_blur_cache_entry_t * except, size_t minimum)
{
    lv_blur_cache_entry_t * oldest = NULL;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) {
        lv_blur_cache_entry_t * e = &cache.entries[i];
        if(e == except || e->busy || e->capacity < minimum) continue;
        if(!oldest || e->used < oldest->used) oldest = e;
    }
    return oldest;
}

static bool any_busy(void)
{
    for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache.entries[i].busy) return true;
    return false;
}

#ifdef ESP_PLATFORM
static uint64_t cache_oldest(void * context)
{
    LV_UNUSED(context);
    if(!cache.initialized) return UINT64_MAX;
    lv_mutex_lock(&cache.lock);
    lv_blur_cache_entry_t * entry = cache.reconfiguring ? NULL : oldest_idle(NULL, 1);
    uint64_t used = entry ? entry->used : UINT64_MAX;
    lv_mutex_unlock(&cache.lock);
    return used;
}

static bool cache_evict(void * context)
{
    LV_UNUSED(context);
    if(!cache.initialized) return false;
    lv_mutex_lock(&cache.lock);
    lv_blur_cache_entry_t * entry = cache.reconfiguring ? NULL : oldest_idle(NULL, 1);
    if(!entry) { lv_mutex_unlock(&cache.lock); return false; }
    /* Workers never acquire the UI lock while holding this short metadata
     * lock. Lease the victim across the unlocked free: allocation hooks may
     * reenter diagnostics, and a concurrent worker must not reuse this slot. */
    entry->busy = true;
    entry->valid = false;
    void * retired = entry->data;
    size_t bytes = entry->capacity;
    lv_draw_sw_blur_cache_free_cb_t release = cache.config.free_cb;
    entry->data = NULL;
    entry->capacity = 0;
    cache.stats.bytes -= bytes;
    cache.stats.evictions++;
    lv_mutex_unlock(&cache.lock);
    data_free(retired, bytes, release);
    lv_mutex_lock(&cache.lock);
    entry->busy = false;
    lv_mutex_unlock(&cache.lock);
    return true;
}
#endif

void lv_draw_sw_blur_cache_init(void)
{
    lv_memzero(&cache, sizeof(cache));
    atomic_store(&enabled, false);
    cache.initialized = lv_mutex_init(&cache.lock) == LV_RESULT_OK;
#ifdef ESP_PLATFORM
    orb_memory_cache_register(ORB_MEMORY_GLASS, cache_oldest, cache_evict, NULL);
#endif
}

/* Callers have quiesced draws and stopped draw workers before deinit. */
void lv_draw_sw_blur_cache_deinit(void)
{
    if(!cache.initialized) return;
    lv_draw_sw_blur_cache_config_t off = {0};
    lv_result_t result = lv_draw_sw_blur_cache_configure(&off);
    LV_ASSERT(result == LV_RESULT_OK);
    if(result != LV_RESULT_OK) return;
    lv_mutex_delete(&cache.lock);
    cache.initialized = false;
}

static lv_result_t reset(const lv_draw_sw_blur_cache_config_t * config)
{
    if(!cache.initialized) return LV_RESULT_INVALID;
    lv_mutex_lock(&cache.lock);
    if(cache.reconfiguring || any_busy()) {
        lv_mutex_unlock(&cache.lock);
        return LV_RESULT_INVALID;
    }
    cache.reconfiguring = true;
    atomic_store(&enabled, false);
    void * retired[CACHE_SLOTS];
    size_t capacities[CACHE_SLOTS];
    for(unsigned i = 0; i < CACHE_SLOTS; i++) {
        retired[i] = cache.entries[i].data;
        capacities[i] = cache.entries[i].capacity;
    }
    lv_draw_sw_blur_cache_free_cb_t release = cache.config.free_cb;
    lv_mutex_unlock(&cache.lock);
    for(unsigned i = 0; i < CACHE_SLOTS; i++) data_free(retired[i], capacities[i], release);
    lv_mutex_lock(&cache.lock);
    lv_memzero(cache.entries, sizeof(cache.entries));
    cache.stats.bytes = 0;
    cache.clock = 0;
    if(config) {
        cache.config = *config;
        if(!cache.config.alloc_cb) cache.config.alloc_cb = lv_malloc;
        if(!cache.config.free_cb) cache.config.free_cb = lv_free;
        if(!cache.config.max_entry_bytes) cache.config.max_entry_bytes = DEFAULT_ENTRY_LIMIT;
        if(cache.config.max_entry_bytes > cache.config.budget_bytes) {
            cache.config.max_entry_bytes = cache.config.budget_bytes;
        }
        lv_memzero(&cache.stats, sizeof(cache.stats));
    }
    cache.stats.budget_bytes = cache.config.budget_bytes;
    cache.reconfiguring = false;
    atomic_store(&enabled, cache.config.budget_bytes > 0);
    lv_mutex_unlock(&cache.lock);
    return LV_RESULT_OK;
}

lv_result_t lv_draw_sw_blur_cache_configure(const lv_draw_sw_blur_cache_config_t * config)
{
    if(!config || (!!config->alloc_cb != !!config->free_cb)) return LV_RESULT_INVALID;
    return reset(config);
}

lv_result_t lv_draw_sw_blur_cache_clear(void)
{
    return reset(NULL);
}

void lv_draw_sw_blur_cache_get_stats(lv_draw_sw_blur_cache_stats_t * out)
{
    if(!out) return;
    lv_memzero(out, sizeof(*out));
    if(!cache.initialized) return;
    lv_mutex_lock(&cache.lock);
    *out = cache.stats;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) out->leased_entries += cache.entries[i].busy;
    lv_mutex_unlock(&cache.lock);
}

/* Enter and leave with the metadata lock held. The target is already leased.
 * Capacity doubles as a reservation while data == NULL. Free retired blocks
 * before transferring their reservation, so actual memory never exceeds it. */
static bool reserve(lv_blur_cache_entry_t * target, size_t needed)
{
    if(target->capacity >= needed) return true;
    uint8_t * retired = target->data;
    target->data = NULL;
    lv_mutex_unlock(&cache.lock);
    data_free(retired, target->capacity, cache.config.free_cb);
    lv_mutex_lock(&cache.lock);

    while(target->capacity < needed) {
        size_t available = cache.config.budget_bytes - cache.stats.bytes;
        size_t growth = LV_MIN(needed - target->capacity, available);
        target->capacity += growth;
        cache.stats.bytes += growth;
        if(cache.stats.bytes > cache.stats.peak_bytes) cache.stats.peak_bytes = cache.stats.bytes;
        if(target->capacity == needed) break;
        lv_blur_cache_entry_t * victim = oldest_idle(target, 1);
        if(!victim) {
            cache.stats.bytes -= target->capacity;
            target->capacity = 0;
            cache.stats.busy_bypasses++;
            return false;
        }
        victim->busy = true;
        if(victim->valid) cache.stats.evictions++;
        victim->valid = false;
        retired = victim->data;
        victim->data = NULL;
        lv_mutex_unlock(&cache.lock);
        data_free(retired, victim->capacity, cache.config.free_cb);
        lv_mutex_lock(&cache.lock);
        cache.stats.bytes -= victim->capacity;
        victim->capacity = 0;
        victim->busy = false;
    }
    if(cache.stats.bytes > cache.stats.peak_bytes) cache.stats.peak_bytes = cache.stats.bytes;
    size_t size = target->capacity;
    lv_mutex_unlock(&cache.lock);
    uint8_t * data = data_alloc(size);
    lv_mutex_lock(&cache.lock);
    if(!data) {
        cache.stats.bytes -= target->capacity;
        target->capacity = 0;
        cache.stats.allocation_failures++;
        return false;
    }
    target->data = data;
    return true;
}

lv_blur_cache_result_t lv_draw_sw_blur_cache_begin(const lv_draw_task_t * task,
    const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords, const lv_area_t * clipped,
    uint8_t * source, uint32_t stride, uint32_t pixel_size, lv_blur_cache_entry_t ** lease)
{
    *lease = NULL;
    if(!atomic_load(&enabled)) return LV_BLUR_CACHE_BYPASS;
    int64_t w = (int64_t)clipped->x2 - clipped->x1 + 1;
    int64_t h = (int64_t)clipped->y2 - clipped->y1 + 1;
    if(w <= 0 || h <= 0 || w > SIZE_MAX || h > UINT32_MAX || !pixel_size || pixel_size > 4) {
        return LV_BLUR_CACHE_BYPASS;
    }
    size_t width = (size_t)w, height = (size_t)h;
    if(width > SIZE_MAX / pixel_size) return LV_BLUR_CACHE_BYPASS;
    size_t row_bytes = width * pixel_size;
    if(height > SIZE_MAX / row_bytes / 2) return LV_BLUR_CACHE_BYPASS;
    size_t image_bytes = row_bytes * height, needed = image_bytes * 2;
    cache_key_t key = {
        .coords = *coords, .clipped = *clipped, .format = task->target_layer->draw_buf->header.cf,
        .blur_radius = dsc->blur_radius, .corner_radius = dsc->corner_radius, .quality = dsc->quality,
        .tint = ((uint32_t)dsc->tint.red << 16) | ((uint32_t)dsc->tint.green << 8) | dsc->tint.blue,
        .tint_opa = dsc->tint_opa, .task_opa = task->opa, .downsample_factor = dsc->downsample_factor
    };
    lv_mutex_lock(&cache.lock);
    if(cache.reconfiguring || !cache.config.budget_bytes) {
        lv_mutex_unlock(&cache.lock);
        return LV_BLUR_CACHE_BYPASS;
    }
    if(needed > cache.config.max_entry_bytes) {
        cache.stats.oversize_bypasses++;
        lv_mutex_unlock(&cache.lock);
        return LV_BLUR_CACHE_BYPASS;
    }
    lv_blur_cache_entry_t * entry = NULL;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) {
        lv_blur_cache_entry_t * e = &cache.entries[i];
        if(!e->busy && e->valid && key_equal(&e->key, &key)) { entry = e; break; }
    }
    bool candidate = entry != NULL;
    if(!entry) {
        for(unsigned i = 0; i < CACHE_SLOTS; i++) {
            if(!cache.entries[i].busy && !cache.entries[i].valid) { entry = &cache.entries[i]; break; }
        }
    }
    if(!entry) entry = oldest_idle(NULL, needed);
    if(!entry) entry = oldest_idle(NULL, 0);
    if(!entry) {
        cache.stats.busy_bypasses++;
        lv_mutex_unlock(&cache.lock);
        return LV_BLUR_CACHE_BYPASS;
    }
    entry->busy = true;
#ifdef ESP_PLATFORM
    entry->used = orb_memory_cache_touch();
#else
    entry->used = ++cache.clock;
#endif
    if(!candidate && entry->valid) cache.stats.evictions++;
    bool compare = candidate;
    if(!candidate) entry->valid = false;
    if(!reserve(entry, needed)) {
        entry->busy = false;
        lv_mutex_unlock(&cache.lock);
        return LV_BLUR_CACHE_BYPASS;
    }
    lv_mutex_unlock(&cache.lock);
    for(size_t y = 0; compare && y < height; y++) {
        compare = lv_memcmp(source + y * stride, entry->data + y * row_bytes, row_bytes) == 0;
    }
    if(compare) {
        for(size_t y = 0; y < height; y++) {
            lv_memcpy(source + y * stride, entry->data + image_bytes + y * row_bytes, row_bytes);
        }
        lv_mutex_lock(&cache.lock);
        cache.stats.hits++;
        entry->busy = false;
        lv_mutex_unlock(&cache.lock);
        return LV_BLUR_CACHE_HIT;
    }
    lv_mutex_lock(&cache.lock);
    cache.stats.misses++;
    entry->valid = false;
    entry->key = key;
    entry->row_bytes = row_bytes;
    entry->image_bytes = image_bytes;
    entry->height = (uint32_t)height;
    lv_mutex_unlock(&cache.lock);
    for(size_t y = 0; y < height; y++) lv_memcpy(entry->data + y * row_bytes, source + y * stride, row_bytes);
    *lease = entry;
    return LV_BLUR_CACHE_MISS;
}

void lv_draw_sw_blur_cache_finish(lv_blur_cache_entry_t * lease, const uint8_t * source,
                                  uint32_t stride, bool rendered)
{
    if(!lease) return;
    if(rendered) {
        for(uint32_t y = 0; y < lease->height; y++) {
            lv_memcpy(lease->data + lease->image_bytes + (size_t)y * lease->row_bytes,
                      source + (size_t)y * stride, lease->row_bytes);
        }
    }
    lv_mutex_lock(&cache.lock);
    lease->valid = rendered;
    lease->busy = false;
    lv_mutex_unlock(&cache.lock);
}
#endif
