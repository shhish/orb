#include "orb_fonts.h"

#include "orb_fonts_private.h"

#ifdef ESP_PLATFORM
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_ORB_DEVTOOLS
#include <assert.h>
#endif
#endif

#if !LV_USE_FONT_COMPRESSED
#error "orb_fonts stores its large sizes RLE-compressed: enable CONFIG_LV_USE_FONT_COMPRESSED (orb_fonts' Kconfig selects it)"
#endif
#if !LV_USE_OBSERVER
#error "orb_fonts_subject() needs CONFIG_LV_USE_OBSERVER (orb_fonts' Kconfig selects it)"
#endif

static orb_content_size_t s_size;
static bool s_bold;
static bool s_ready;
static orb_fonts_err_t s_result;
#ifdef ESP_PLATFORM
static bool s_background_enabled;
static void (*s_cache_suspend)(void), (*s_cache_resume)(void);
#endif
static orb_text_style_info_t s_info[ORB_TEXT_STYLE_COUNT];
static orb_clock_info_t s_clock_info[ORB_CLOCK_COUNT];
static lv_subject_t *s_subject; /* an int: the number of setting changes */
static int32_t s_generation;
/* A glyph run shares one face. Avoid a full pack-table search per letter;
 * this stores no glyph/font ownership and is cleared before font retirement. */
static const lv_font_t *s_nominal_font;
static uint16_t s_nominal_px;

/* Shared styles, made on first use and kept current. */
typedef struct {
    lv_style_t style;
    bool made;
} shared_style_t;

static shared_style_t s_text_styles[ORB_TEXT_STYLE_COUNT][4]; /* [style][variant flags] */
static shared_style_t s_icon_styles[ORB_TEXT_STYLE_COUNT][2]; /* [style][emphasized] */
static shared_style_t s_clock_styles[ORB_CLOCK_COUNT];

static const orb_font_set_t *current(void)
{
    return &orb_fonts_table.sets[s_size][s_bold];
}

LV_FONT_DECLARE(orb_inter_28_regular)
LV_FONT_DECLARE(orb_icons_28_regular)

static void fill_info(void)
{
    const orb_font_set_t *set = current();
    for (int s = 0; s < ORB_TEXT_STYLE_COUNT; s++) {
        const orb_style_row_t *row = &set->styles[s];
        s_info[s] = (orb_text_style_info_t){
            .name = orb_fonts_table.style_names[s],
            .pt = row->pt,
            .size_px = row->size_px,
            .leading_px = row->leading_px,
            .letter_space = row->letter_space,
            .tracking = row->tracking,
            .weight = row->weight[0],
            .emphasized_weight = row->weight[1],
        };
    }
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        const orb_clock_face_t *face = &orb_fonts_table.clock_faces[c];
        s_clock_info[c] = (orb_clock_info_t){
            .size_px = face->size_px,
            .weight = set->clocks[c].weight,
            .tracking = face->tracking,
            .letter_space = face->letter_space,
        };
    }
}

static void ensure_ready(void)
{
    if (!s_ready) {
        s_ready = true;
        fill_info();
        orb_fonts_status_preload();
        s_subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    }
}

static orb_text_style_t clamp_style(orb_text_style_t style)
{
    return (unsigned)style < ORB_TEXT_STYLE_COUNT ? style : ORB_TEXT_BODY;
}

static orb_clock_t clamp_clock(orb_clock_t clock)
{
    return (unsigned)clock < ORB_CLOCK_COUNT ? clock : ORB_CLOCK_LARGE;
}

/* The requested face, or an app-owned rescue font if a pack record cannot load. */
typedef enum { SLOT_TEXT, SLOT_TABULAR, SLOT_ICONS } slot_t;

static const lv_font_t *slot_font(orb_text_style_t style, bool emphasized, slot_t slot)
{
    ensure_ready();
    style = clamp_style(style);
    return orb_fonts_handle(current(), s_size * 2 + s_bold, style * 6 + emphasized * 3 + slot);
}

/* ---- Settings ------------------------------------------------------------------------ */

static void refresh_shared_styles(void);

static void publish_setting(orb_content_size_t size, bool bold)
{
    s_size = size;
    s_bold = bold;
    s_nominal_font = NULL;
    ++s_generation;
    orb_fonts_handles_refresh(current());
    fill_info();
    refresh_shared_styles(); /* loads what the live styles need; LVGL re-lays out their objects */
    if (s_subject) {
        lv_subject_set_int(s_subject, s_generation);
    }
    /* Raw caller-owned styles and retained trees keep lifetime-safe handles.
     * Observer-created drawing must also complete before payload retirement. */
    orb_fonts_draw_barrier();
    orb_fonts_pack_release_unused(current());
    s_nominal_font = NULL;
}

static orb_fonts_err_t switch_to(orb_content_size_t size, bool bold)
{
    ensure_ready();
    if (size == s_size && bold == s_bold) {
        orb_fonts_draw_barrier();
        orb_fonts_handles_refresh(current());
        return ORB_FONTS_OK;
    }
    if (!orb_fonts_handles_prepare()) return ORB_FONTS_ERR_NO_MEM;
    if (size != ORB_CONTENT_SIZE_DEFAULT || bold) {
        orb_fonts_err_t err = orb_fonts_pack_open();
        if (err != ORB_FONTS_OK) {
            LV_LOG_WARN("orb_fonts: content size %d%s unavailable: %s", (int)size, bold ? " with Bold Text" : "",
                        orb_fonts_err_name(err));
            return err;
        }
    }
    orb_fonts_draw_barrier();
    publish_setting(size, bold);
    return ORB_FONTS_OK;
}

orb_fonts_err_t orb_fonts_preload(orb_content_size_t size, bool bold)
{
    if ((unsigned)size >= ORB_CONTENT_SIZE_COUNT) return ORB_FONTS_ERR_ARG;
    ensure_ready();
    orb_fonts_err_t err = orb_fonts_handles_prepare() ?
        orb_fonts_pack_preload(&orb_fonts_table.sets[size][bold]) : ORB_FONTS_ERR_NO_MEM;
    if (err == ORB_FONTS_OK) {
        switch_to(size, bold);
    } else {
        orb_fonts_pack_release_unused(current());
    }
#ifdef ESP_PLATFORM
    orb_fonts_pack_cache_only();
    s_background_enabled = true;
#endif
    s_result = err;
    return err;
}

orb_fonts_err_t orb_fonts_preload_default(void)
{
    return orb_fonts_preload(ORB_CONTENT_SIZE_DEFAULT, false);
}

orb_fonts_err_t orb_fonts_preload_saved(void)
{
    orb_content_size_t size = ORB_CONTENT_SIZE_DEFAULT;
    bool bold = false;
#ifdef ESP_PLATFORM
    orb_fonts_port_preferences_load(&size, &bold);
#endif
    orb_fonts_err_t err = orb_fonts_preload(size, bold);
    if (err != ORB_FONTS_OK && (size != ORB_CONTENT_SIZE_DEFAULT || bold)) {
        /* Keep the saved choice for a later healthy boot, but publish only the
         * readable set that actually loaded. No shell exists during this retry. */
        orb_fonts_preload_default();
        s_result = err;
    }
    return err;
}

orb_fonts_err_t orb_fonts_last_result(void)
{
    return s_result;
}

#ifdef ESP_PLATFORM
static TaskHandle_t s_load_owner;
static atomic_bool s_load_requested;
static bool s_loading;
static orb_content_size_t s_wanted_size;
static bool s_wanted_bold;

static orb_fonts_err_t request_switch(orb_content_size_t size, bool bold)
{
    ensure_ready();
    if (!s_loading && size == s_size && bold == s_bold) return ORB_FONTS_OK;
    orb_fonts_err_t err = orb_fonts_pack_open();
    if (err != ORB_FONTS_OK) return err;
    if (!s_load_owner) return ORB_FONTS_ERR_LOAD;
    s_result = ORB_FONTS_OK;
    s_wanted_size = size;
    s_wanted_bold = bold;
    s_loading = true;
    atomic_store_explicit(&s_load_requested, true, memory_order_release);
    xTaskNotifyGive(s_load_owner);
    return ORB_FONTS_OK;
}
#endif

void orb_fonts_service_init(void)
{
#ifdef ESP_PLATFORM
    s_load_owner = xTaskGetCurrentTaskHandle();
#endif
}

void orb_fonts_set_cache_callbacks(void (*suspend)(void), void (*resume)(void))
{
#ifdef ESP_PLATFORM
    s_cache_suspend = suspend;
    s_cache_resume = resume;
#else
    LV_UNUSED(suspend);
    LV_UNUSED(resume);
#endif
}

#ifdef ESP_PLATFORM
static orb_fonts_err_t prepare_setting(orb_content_size_t *size, bool *bold)
{
    if (!orb_fonts_handles_prepare()) {
        *size = s_size;
        *bold = s_bold;
        return ORB_FONTS_ERR_NO_MEM;
    }
    const orb_font_set_t *previous = current();
    const orb_font_set_t *requested = &orb_fonts_table.sets[*size][*bold];
    /* Nothing can draw under the locked barrier. Park every retained handle
     * before retiring old-only records, including those held off-tree. */
    orb_fonts_handles_refresh(NULL);
    s_nominal_font = NULL;
    orb_fonts_pack_release_unused(requested);
    orb_fonts_err_t err = orb_fonts_pack_preload(requested);
    if (err != ORB_FONTS_OK) {
        orb_fonts_port_prepare(); /* retry once after trimming every optional owner */
        err = orb_fonts_pack_preload(requested);
    }
    if (err == ORB_FONTS_OK) return err;
    /* Dropping partial new records restores old capacity with fixed external
     * allocations. Network growth can consume those holes and reads can fail;
     * the assets gate keeps OTA from replacing this transaction's image. */
    orb_fonts_pack_release_unused(previous);
    *size = s_size;
    *bold = s_bold;
    if (orb_fonts_pack_preload(previous) == ORB_FONTS_OK) return err;
    *size = ORB_CONTENT_SIZE_DEFAULT;
    *bold = false;
    orb_fonts_pack_release_unused(&orb_fonts_table.sets[*size][*bold]);
    if (orb_fonts_pack_preload(&orb_fonts_table.sets[*size][*bold]) != ORB_FONTS_OK)
        orb_fonts_pack_release_unused(NULL); /* immutable app fonts need no heap */
    return err;
}
#endif

void orb_fonts_service(void)
{
#ifdef ESP_PLATFORM
    if (!atomic_load_explicit(&s_load_requested, memory_order_acquire)) return;
#if CONFIG_ORB_DEVTOOLS
    assert(xTaskGetCurrentTaskHandle() == s_load_owner);
#endif
    /* Main owns an internal stack for flash MMU changes. Hold LVGL across this
     * rare transaction: leased glass can be reclaimed without ever drawing its
     * temporary fallback, and no pointer can escape between restyle/retirement.
     * The panel keeps its last complete frame while network tasks keep running. */
    lv_lock();
    /* Include requests that arrived while main waited for the display lock. */
    atomic_store_explicit(&s_load_requested, false, memory_order_relaxed);
    orb_fonts_draw_barrier();
    /* Observer reflow may request transparent rasters. Delay their optional
     * build scratch until the previous font set has actually been retired. */
    lv_obj_raster_cache_set_paused(true);
    if (s_cache_suspend) s_cache_suspend();
    orb_fonts_port_prepare();
    orb_content_size_t size = s_wanted_size;
    bool bold = s_wanted_bold;
    bool assets_ready = orb_fonts_port_begin_switch();
    if (assets_ready) {
        s_result = prepare_setting(&size, &bold);
        orb_fonts_port_end_switch();
    } else {
        size = s_size;
        bold = s_bold;
        s_result = ORB_FONTS_ERR_LOAD;
    }
    s_loading = false;
    if (s_result == ORB_FONTS_OK) {
        publish_setting(size, bold); /* completion is observable even after coalescing back */
    } else {
        /* Publish the recovered truth once, even if it is the previous pair.
         * Observers restore native switches and show one transient failure. */
        publish_setting(size, bold);
        LV_LOG_WARN("orb_fonts: deferred setting unavailable: %s", orb_fonts_err_name(s_result));
    }
    lv_obj_raster_cache_set_paused(false);
    if (s_cache_resume) s_cache_resume();
    bool save = s_result == ORB_FONTS_OK;
    lv_unlock();
    /* Never save an obsolete or failed staging set, and never perform storage
     * writes under LVGL. A reboot loads this committed pair before the shell. */
    if (save && !orb_fonts_port_preferences_save(size, bold))
        LV_LOG_WARN("orb_fonts: couldn't save text preferences");
#endif
}

bool orb_fonts_pending(void)
{
#ifdef ESP_PLATFORM
    return s_loading;
#else
    return false;
#endif
}

orb_fonts_err_t orb_fonts_set_content_size(orb_content_size_t size)
{
    if ((unsigned)size >= ORB_CONTENT_SIZE_COUNT) {
        return ORB_FONTS_ERR_ARG;
    }
    s_result = ORB_FONTS_OK;
#ifdef ESP_PLATFORM
    /* The latency A/B control deliberately keeps lazy synchronous reads. Never
     * mix those writers with asynchronous main-owned preparation. */
    if (!s_background_enabled) return s_result = switch_to(size, s_bold);
    return s_result = request_switch(size, s_loading ? s_wanted_bold : s_bold);
#else
    return s_result = switch_to(size, s_bold);
#endif
}

orb_content_size_t orb_fonts_content_size(void)
{
    return s_size;
}

orb_fonts_err_t orb_fonts_set_bold_text(bool on)
{
    s_result = ORB_FONTS_OK;
#ifdef ESP_PLATFORM
    if (!s_background_enabled) return s_result = switch_to(s_size, on);
    return s_result = request_switch(s_loading ? s_wanted_size : s_size, on);
#else
    return s_result = switch_to(s_size, on);
#endif
}

bool orb_fonts_bold_text(void)
{
    return s_bold;
}

const char *orb_fonts_err_name(orb_fonts_err_t err)
{
    switch (err) {
    case ORB_FONTS_OK:
        return "ok";
    case ORB_FONTS_ERR_ARG:
        return "invalid argument";
    case ORB_FONTS_ERR_NO_PACK:
        return "no font pack in the assets partition";
    case ORB_FONTS_ERR_PACK_MISMATCH:
        return "the font pack doesn't match this firmware";
    case ORB_FONTS_ERR_NO_MEM:
        return "not enough PSRAM for the background font loader";
    case ORB_FONTS_ERR_LOAD:
        return "could not prepare the requested font set";
    }
    return "?";
}

lv_subject_t *orb_fonts_subject(void)
{
    ensure_ready();
    return s_subject;
}

uint32_t orb_fonts_revision(void)
{
    return (uint32_t)s_generation;
}

const char *orb_fonts_notices(void)
{
    return orb_fonts_table.notices;
}

/* ---- Text styles ------------------------------------------------------------------- */

const orb_text_style_info_t *orb_text_style_info(orb_text_style_t style)
{
    ensure_ready();
    return &s_info[clamp_style(style)];
}

const lv_font_t *orb_font(orb_text_style_t style)
{
    return slot_font(style, false, SLOT_TEXT);
}

uint16_t orb_fonts_nominal_px(const lv_font_t *font)
{
    if (!font) return 0;
    if (font == s_nominal_font) return s_nominal_px;
    const lv_font_t *source = orb_fonts_builtin_source(orb_fonts_handle_source(font));
    uint16_t nominal = 0;
    for (int size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        for (int bold = 0; bold < 2; bold++) {
            const orb_font_set_t *set = &orb_fonts_table.sets[size][bold];
            for (int style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
                const orb_style_row_t *row = &set->styles[style];
                for (int variant = 0; variant < 2; variant++) {
                    const orb_face_ids_t *ids = &row->face[variant];
                    if (orb_fonts_peek(ids->text) == source || orb_fonts_peek(ids->tabular) == source ||
                        orb_fonts_peek(ids->icons) == source) {
                        nominal = row->size_px;
                        goto found;
                    }
                }
            }
            for (int clock = 0; clock < ORB_CLOCK_COUNT; clock++) {
                if (orb_fonts_peek(set->clocks[clock].font) == source) {
                    nominal = orb_fonts_table.clock_faces[clock].size_px;
                    goto found;
                }
            }
        }
    }
found:
    /* Unknown callers may mutate their font in place; only stable pack faces
     * are memoized. Settings changes clear this before publication/unload. */
    s_nominal_font = nominal ? font : NULL;
    s_nominal_px = nominal;
    return nominal;
}

const lv_font_t *orb_font_variant(orb_text_style_t style, orb_text_variant_t variant)
{
    return slot_font(style, variant & ORB_TEXT_EMPHASIZED, (variant & ORB_TEXT_TABULAR) ? SLOT_TABULAR : SLOT_TEXT);
}

const lv_font_t *orb_icon_font(orb_text_style_t style)
{
    return slot_font(style, false, SLOT_ICONS);
}

const lv_font_t *orb_icon_font_variant(orb_text_style_t style, orb_text_variant_t variant)
{
    return slot_font(style, variant & ORB_TEXT_EMPHASIZED, SLOT_ICONS);
}

int32_t orb_text_line_space(orb_text_style_t style, orb_text_variant_t variant)
{
    return (int32_t)orb_text_style_info(style)->leading_px - lv_font_get_line_height(orb_font_variant(style, variant));
}

static void fill_text_style(lv_style_t *s, orb_text_style_t style, orb_text_variant_t variant)
{
    lv_style_set_text_font(s, orb_font_variant(style, variant));
    lv_style_set_text_letter_space(s, orb_text_style_info(style)->letter_space);
    lv_style_set_text_line_space(s, orb_text_line_space(style, variant));
}

static void fill_icon_style(lv_style_t *s, orb_text_style_t style, bool emphasized)
{
    lv_style_set_text_font(s, slot_font(style, emphasized, SLOT_ICONS));
    lv_style_set_text_letter_space(s, 0);
    lv_style_set_text_line_space(s, 0);
}

static void fill_clock_style(lv_style_t *s, orb_clock_t clock)
{
    lv_style_set_text_font(s, orb_clock_font(clock));
    lv_style_set_text_letter_space(s, orb_clock_info(clock)->letter_space);
    lv_style_set_text_line_space(s, 0);
}

const lv_style_t *orb_text_style(orb_text_style_t style, orb_text_variant_t variant)
{
    style = clamp_style(style);
    shared_style_t *s = &s_text_styles[style][variant & 3u];
    if (!s->made) {
        lv_style_init(&s->style);
        fill_text_style(&s->style, style, variant & 3u);
        s->made = true;
    }
    return &s->style;
}

const lv_style_t *orb_icon_style(orb_text_style_t style, orb_text_variant_t variant)
{
    style = clamp_style(style);
    bool emphasized = variant & ORB_TEXT_EMPHASIZED;
    shared_style_t *s = &s_icon_styles[style][emphasized];
    if (!s->made) {
        lv_style_init(&s->style);
        fill_icon_style(&s->style, style, emphasized);
        s->made = true;
    }
    return &s->style;
}

const lv_style_t *orb_clock_style(orb_clock_t clock)
{
    clock = clamp_clock(clock);
    shared_style_t *s = &s_clock_styles[clock];
    if (!s->made) {
        lv_style_init(&s->style);
        fill_clock_style(&s->style, clock);
        s->made = true;
    }
    return &s->style;
}

static void refresh_shared_styles(void)
{
    for (int st = 0; st < ORB_TEXT_STYLE_COUNT; st++) {
        for (unsigned v = 0; v < 4; v++) {
            shared_style_t *s = &s_text_styles[st][v];
            if (s->made) {
                fill_text_style(&s->style, (orb_text_style_t)st, (orb_text_variant_t)v);
                lv_obj_report_style_change(&s->style);
            }
        }
        for (int e = 0; e < 2; e++) {
            shared_style_t *s = &s_icon_styles[st][e];
            if (s->made) {
                fill_icon_style(&s->style, (orb_text_style_t)st, e);
                lv_obj_report_style_change(&s->style);
            }
        }
    }
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        shared_style_t *s = &s_clock_styles[c];
        if (s->made) {
            fill_clock_style(&s->style, (orb_clock_t)c);
            lv_obj_report_style_change(&s->style);
        }
    }
}

/* Removes the orb_fonts styles `obj` has for `selector`, so applying another one replaces
 * them instead of stacking up. */
static void detach_shared_styles(lv_obj_t *obj, lv_style_selector_t selector)
{
    shared_style_t *all[] = {&s_text_styles[0][0], &s_icon_styles[0][0], &s_clock_styles[0]};
    size_t counts[] = {ORB_TEXT_STYLE_COUNT * 4, ORB_TEXT_STYLE_COUNT * 2, ORB_CLOCK_COUNT};
    for (size_t g = 0; g < 3; g++) {
        for (size_t i = 0; i < counts[g]; i++) {
            if (all[g][i].made) {
                lv_obj_remove_style(obj, &all[g][i].style, selector);
            }
        }
    }
}

void orb_text_apply(lv_obj_t *obj, orb_text_style_t text_style, orb_text_variant_t variant,
                    lv_style_selector_t selector)
{
    const lv_style_t *style = orb_text_style(text_style, variant);
    /* A previous fitted/raw face must not outrank the shared role when Bold
     * Text or Dynamic Type changes. Applying a role owns all three metrics. */
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_FONT, selector);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_LETTER_SPACE, selector);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_LINE_SPACE, selector);
    detach_shared_styles(obj, selector);
    lv_obj_add_style(obj, style, selector);
}

void orb_icon_apply(lv_obj_t *obj, orb_text_style_t text_style, orb_text_variant_t variant,
                    lv_style_selector_t selector)
{
    const lv_style_t *style = orb_icon_style(text_style, variant);
    detach_shared_styles(obj, selector);
    lv_obj_add_style(obj, style, selector);
}

void orb_text_style_init(lv_style_t *style, orb_text_style_t text_style, orb_text_variant_t variant)
{
    fill_text_style(style, clamp_style(text_style), variant);
}

/* ---- Clock faces ------------------------------------------------------------------- */

const orb_clock_info_t *orb_clock_info(orb_clock_t clock)
{
    ensure_ready();
    return &s_clock_info[clamp_clock(clock)];
}

const lv_font_t *orb_clock_font(orb_clock_t clock)
{
    ensure_ready();
    clock = clamp_clock(clock);
    return orb_fonts_handle(current(), s_size * 2 + s_bold, ORB_FONT_TEXT_ROLES + clock);
}

void orb_clock_apply(lv_obj_t *obj, orb_clock_t clock, lv_style_selector_t selector)
{
    const lv_style_t *style = orb_clock_style(clock);
    detach_shared_styles(obj, selector);
    lv_obj_add_style(obj, style, selector);
}
