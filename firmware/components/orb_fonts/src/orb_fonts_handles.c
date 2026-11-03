#include "orb_fonts_private.h"

#include <string.h>
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_refr_private.h"

typedef struct {
    lv_font_t font;
    const lv_font_t *source;
} font_handle_t;

/* Six bounded banks preserve font-pointer cache keys across setting changes.
 * Every published descriptor survives for the process lifetime, including those
 * kept by off-tree screens, local styles, rim caches and queued label tasks.
 * Only the backing records are retired. Allocate from the fonts PSRAM owner. */
static font_handle_t *s_handles;

#ifdef ESP_PLATFORM
_Static_assert(sizeof(font_handle_t) == 48, "update the font handle PSRAM budget for this ABI");
#endif

LV_FONT_DECLARE(orb_inter_28_regular)
LV_FONT_DECLARE(orb_icons_28_regular)

static const lv_font_t *source_font(const orb_font_set_t *set, unsigned role)
{
    orb_font_id_t id;
    bool icons = role < ORB_FONT_TEXT_ROLES && role % 3 == 2;
    if (!set) return icons ? orb_fonts_builtin_ellipsis(&orb_icons_28_regular) : &orb_inter_28_regular;
    if (role < ORB_FONT_TEXT_ROLES) {
        const orb_face_ids_t *face = &set->styles[role / 6].face[(role % 6) / 3];
        unsigned slot = role % 3;
        id = slot == 0 ? face->text : slot == 1 ? face->tabular : face->icons;
    } else {
        id = set->clocks[role - ORB_FONT_TEXT_ROLES].font;
    }
    const lv_font_t *font = orb_fonts_get(id);
    if (!font) font = icons ? &orb_icons_28_regular : &orb_inter_28_regular;
    return icons ? orb_fonts_builtin_ellipsis(font) : font;
}

static bool handle_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                          uint32_t letter, uint32_t next)
{
    const font_handle_t *handle = font->dsc;
    if (!handle->source->get_glyph_dsc(handle->source, glyph, letter, next)) return false;
    /* LVGL replaces resolved_font with this handle. Keep Unicode rather than a
     * record-local glyph index; a held descriptor must never retain pack memory. */
    glyph->gid.index = letter;
    glyph->entry = NULL;
    return true;
}

static const void *handle_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer)
{
    if (glyph->req_raw_bitmap) return NULL;
    const font_handle_t *handle = glyph->resolved_font->dsc;
    lv_font_glyph_dsc_t local = {0};
    if (!handle->source->get_glyph_dsc(handle->source, &local, glyph->gid.index, 0)) return NULL;
    local.resolved_font = handle->source;
    /* A caller may keep a resolved glyph across a switch. Reject old geometry
     * before touching its scratch buffer; the next layout gets current metrics.
     * Normal queued drawing is drained before any handle is retargeted. */
    if (local.box_w != glyph->box_w || local.box_h != glyph->box_h ||
        local.ofs_x != glyph->ofs_x || local.ofs_y != glyph->ofs_y ||
        local.stride != glyph->stride || local.format != glyph->format) return NULL;
    return local.resolved_font->get_glyph_bitmap(&local, buffer);
}

static const lv_font_t *handle_fallback(const orb_font_set_t *set, unsigned bank, const lv_font_t *source)
{
    if (!source->fallback) return NULL;
    for (unsigned i = 0; i < orb_fonts_table.builtin_count; i++) {
        if (source->fallback == orb_fonts_table.builtin[i]) return source->fallback;
    }
    /* Preserve the resolved fallback's own line metrics (row atlases use them).
     * All pack fallbacks occur as roles in the generated complete set. */
    for (unsigned role = 0; role < ORB_FONT_ROLES; role++) {
        if (orb_fonts_builtin_source(source_font(set, role)) == source->fallback)
            return orb_fonts_handle(set, bank, role);
    }
    return orb_fonts_ellipsis_fallback();
}

static void bind_handle(font_handle_t *handle, const orb_font_set_t *set, unsigned bank, const lv_font_t *source)
{
    handle->source = source;
    handle->font = *source;
    handle->font.dsc = handle;
    handle->font.fallback = handle_fallback(set, bank, source);
    handle->font.get_glyph_dsc = handle_glyph;
    handle->font.get_glyph_bitmap = handle_bitmap;
    handle->font.release_glyph = NULL;
    handle->font.static_bitmap = 0; /* never expose a borrowed payload pointer */
}

bool orb_fonts_handles_prepare(void)
{
    if (!s_handles) {
        size_t bytes = sizeof(*s_handles) * ORB_FONT_HANDLE_COUNT;
        s_handles = orb_fonts_port_alloc(bytes);
        if (s_handles) memset(s_handles, 0, bytes);
    }
    return s_handles != NULL;
}

const lv_font_t *orb_fonts_handle(const orb_font_set_t *set, unsigned bank, unsigned role)
{
    if (!orb_fonts_handles_prepare()) return role < ORB_FONT_TEXT_ROLES && role % 3 == 2 ?
        &orb_icons_28_regular : &orb_inter_28_regular;
    font_handle_t *handle = &s_handles[bank * ORB_FONT_ROLES + role];
    /* Existing handles are retargeted only by the publication transaction.
     * Cache-only lookup must never mutate a descriptor a worker can be using. */
    if (!handle->source) bind_handle(handle, set, bank, source_font(set, role));
    return &handle->font;
}

void orb_fonts_handles_refresh(const orb_font_set_t *set)
{
    /* NULL parks every retained descriptor on app-owned rescue faces while
     * records are retired. This also makes exhausted recovery safe to publish. */
    if (!s_handles) return;
    for (unsigned role = 0; role < ORB_FONT_ROLES; role++) {
        const lv_font_t *source = NULL;
        for (unsigned bank = 0; bank < ORB_CONTENT_SIZE_COUNT * 2; bank++) {
            font_handle_t *handle = &s_handles[bank * ORB_FONT_ROLES + role];
            if (!handle->source) continue;
            if (!source) source = source_font(set, role);
            bind_handle(handle, set, bank, source);
        }
    }
}

const lv_font_t *orb_fonts_handle_source(const lv_font_t *font)
{
    return font->get_glyph_dsc == handle_glyph ? ((const font_handle_t *)font->dsc)->source : font;
}

void orb_fonts_draw_barrier(void)
{
    /* Called with LVGL locked, outside refresh. wait_for_finish alone leaves
     * WAITING/BLOCKED tasks behind. Dispatch every display/layer until cleanup
     * has removed them too, then no worker can race descriptor publication. */
    lv_draw_wait_for_finish();
    lv_display_t *previous = lv_refr_get_disp_refreshing();
    bool pending;
    do {
        pending = false;
        for (lv_display_t *display = lv_display_get_next(NULL); display; display = lv_display_get_next(display)) {
            lv_refr_set_disp_refreshing(display);
            for (lv_layer_t *layer = display->layer_head; layer; layer = layer->next) {
                if (layer->draw_task_head) pending = true;
                lv_draw_dispatch_layer(display, layer);
            }
        }
        lv_draw_wait_for_finish();
    } while (pending);
    lv_refr_set_disp_refreshing(previous);
}
