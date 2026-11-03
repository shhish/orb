/* Firmware staging against real LVGL, with an S3-sized PSRAM allocation limit.
 * Flash-backed pixels use host storage outside that limit, like mapped flash. */
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "../../../orb_ui/orb_ui_private.h"
#define ESP_PLATFORM 1
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "../../src/orb_fonts.c"
#undef ESP_PLATFORM
#define ORB_FONTS_TEST_MAPPING 1
#define ORB_FONTS_TEST_STREAMING 1
static void *test_allocate(size_t size, const char *caller);
#define orb_fonts_port_alloc(size) test_allocate(size, __func__)
#include "../../src/orb_fonts_pack.c"
#undef orb_fonts_port_alloc
#define ESP_PLATFORM 1

/* Supplied post-failure board receipt, conservatively excluding the old
 * pointer table/leases/allocator headers from the recoverable font charge.
 * Nav (1.30 MB), network (296 KB), and all other required allocations are
 * already paid by this receipt. They must not be subtracted a second time. */
#define BOARD_FONT_POOL (1061628u + 1056948u + 733000u)
#define PSRAM_FLOOR (384u * 1024u)
#define TLS_TRANSIENT (128u * 1024u)
#define CACHE_BYTES 733000u

static unsigned s_main_identity, s_ui_identity;
static TaskHandle_t s_running = &s_ui_identity;
static unsigned s_notifications, s_maps, s_runtime_maps, s_unmaps, s_trims;
static bool s_boot_loading = true;
#ifdef ORB_FONTS_SURFACE_TEST
static bool s_surface_mode;
int font_surfaces_main(const char *scenario);
#endif
static unsigned s_changes, s_refusals;
static bool s_fail_alloc;
static unsigned s_fail_allocations, s_fail_reads;
static uint32_t s_fail_read_offset = UINT32_MAX;
static uint32_t s_fail_read_second_offset = UINT32_MAX;
static unsigned s_clear_failure_trim;
static size_t s_charged, s_peak, s_cache, s_limit = BOARD_FONT_POOL - PSRAM_FLOOR;
static size_t s_font_peak;
static size_t s_network;
static size_t s_required_masks;
static lv_obj_t *s_deferred_raster;
static orb_content_size_t s_saved_size;
static bool s_saved_bold;
static unsigned s_saves;
static bool s_assets_updating, s_switch_locked;

bool orb_fonts_port_begin_switch(void)
{
    assert(!s_switch_locked);
    if (s_assets_updating) return false;
    s_switch_locked = true;
    return true;
}
void orb_fonts_port_end_switch(void) { assert(s_switch_locked); s_switch_locked = false; }

typedef union allocation {
    max_align_t alignment;
    struct { size_t charge, bytes, offset; union allocation *next; } value;
} allocation_t;
static allocation_t *s_allocations;
enum { FRAGMENT_SPAN = 256 * 1024, FRAGMENT_PIN = 4096 };

static size_t allocation_gap(size_t needed, size_t *largest)
{
    size_t result = SIZE_MAX;
    *largest = 0;
    /* Permanent foreign pins split the net-free receipt into 256 KiB holes.
     * Pins lie outside that receipt: enlarge address space by their charge.
     * Host payloads use malloc; this first-fit model charges Xtensa bytes. */
    for (size_t bytes = 0; bytes < BOARD_FONT_POOL; bytes += FRAGMENT_SPAN) {
        size_t base = bytes / FRAGMENT_SPAN * (FRAGMENT_SPAN + FRAGMENT_PIN);
        size_t end = base + LV_MIN(FRAGMENT_SPAN, BOARD_FONT_POOL - bytes);
        size_t start = base;
        for (;;) {
            allocation_t *next = NULL;
            size_t offset = end;
            for (allocation_t *item = s_allocations; item; item = item->value.next) {
                if (item->value.offset >= start && item->value.offset < offset) {
                    next = item;
                    offset = item->value.offset;
                }
            }
            *largest = LV_MAX(*largest, offset - start);
            if (result == SIZE_MAX && needed <= offset - start) result = start;
            if (!next) break;
            start = next->value.offset + next->value.charge;
        }
    }
    return result;
}

static void *allocate(size_t bytes, size_t charge)
{
    charge = (charge + 3) / 4 * 4 + 16; /* Xtensa alignment and owner header */
    size_t largest;
    size_t offset = allocation_gap(charge, &largest);
    bool fail = s_fail_alloc || s_fail_allocations;
    if (s_fail_allocations) s_fail_allocations--;
    if (fail || offset == SIZE_MAX || charge > s_limit || s_charged + s_cache + s_network > s_limit - charge) {
        s_refusals++;
        return NULL;
    }
    allocation_t *allocation = malloc(sizeof(*allocation) + bytes);
    if (!allocation) return NULL;
    allocation->value.charge = charge;
    allocation->value.bytes = bytes;
    allocation->value.offset = offset;
    allocation->value.next = s_allocations;
    s_allocations = allocation;
    s_charged += charge;
    s_font_peak = LV_MAX(s_font_peak, s_charged);
    s_peak = LV_MAX(s_peak, s_charged + s_cache + s_network);
    /* A concurrent PSRAM TLS allocation must still leave a real reserve. */
    assert(BOARD_FONT_POOL - s_charged - s_cache - s_network >= PSRAM_FLOOR);
    return allocation + 1;
}

static void *test_allocate(size_t size, const char *caller)
{
    size_t charge = size;
    if (!caller && size == ORB_FONT_HANDLE_COUNT * (sizeof(lv_font_t) + sizeof(void *))) {
        charge = ORB_FONT_HANDLE_COUNT * 48u; /* IDF-asserted font handle ABI */
    } else if (caller && !strcmp(caller, "orb_fonts_pack_open")) {
        charge = orb_fonts_table.pack_count * 4u;
    } else if (caller && !strcmp(caller, "record_font") && size >= sizeof(record_font_t) &&
               (size - sizeof(record_font_t)) % sizeof(lv_font_fmt_txt_cmap_t) == 0 &&
               (size - sizeof(record_font_t)) / sizeof(lv_font_fmt_txt_cmap_t) <= 16) {
        /* The owned parser keeps native glyphs in its record. Pointer-bearing
         * parser/cmap structures are 108/24 bytes on the actual Xtensa ABI. */
        charge = 108u + (size - sizeof(record_font_t)) / sizeof(lv_font_fmt_txt_cmap_t) * 24u;
    }
    return allocate(size, charge);
}
void *orb_fonts_port_alloc(size_t size) { return test_allocate(size, NULL); }

void orb_fonts_port_free(void *ptr)
{
    if (!ptr) return;
    allocation_t *allocation = (allocation_t *)ptr - 1;
    assert(allocation->value.charge <= s_charged);
    allocation_t **slot = &s_allocations;
    while (*slot != allocation) { assert(*slot); slot = &(*slot)->value.next; }
    *slot = allocation->value.next;
    s_charged -= allocation->value.charge;
    memset(ptr, 0xa5, allocation->value.bytes);
    free(allocation);
}

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    assert(s_boot_loading || s_running == &s_main_identity);
    assert(s_boot_loading || s_switch_locked);
    if (offset == s_fail_read_offset || offset == s_fail_read_second_offset || s_fail_reads) {
        if (s_fail_reads) s_fail_reads--;
        return false;
    }
    FILE *file = fopen(ORB_ASYNC_TEST_PACK_PATH, "rb");
    if (!file) return false;
    bool ok = fseek(file, (long)offset, SEEK_SET) == 0 && fread(dst, 1, size, file) == size;
    fclose(file);
    return ok;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    uLongf actual = dst_size;
    return uncompress(dst, &actual, src, src_size) == Z_OK && actual == dst_size;
}

bool orb_fonts_port_read_inflate(void *dst, size_t dst_size, uint32_t offset, size_t stored)
{
    /* Charge the ROM inflater and bounded input window even though host zlib
     * owns its own unrelated workspace. Never charge a whole stored stream. */
    void *workspace = allocate(11000, 11000);
    uint8_t *input = allocate(1024, 1024);
    uint8_t *compressed = malloc(stored);
    bool ok = workspace && input && compressed && orb_fonts_port_read(offset, compressed, stored) &&
        orb_fonts_port_inflate(dst, dst_size, compressed, stored);
    free(compressed);
    orb_fonts_port_free(input);
    orb_fonts_port_free(workspace);
    return ok;
}

void orb_fonts_port_prepare(void)
{
    assert(s_running == &s_main_identity);
    orb_glass_cache_clear();
#ifdef ORB_FONTS_SURFACE_TEST
    if (s_surface_mode) return;
#endif
    orb_glass_build_stats_t masks;
    orb_glass_build_stats(&masks);
    lv_draw_sw_material_stats_t material;
    lv_draw_sw_material_get_stats(&material);
    assert(material.bytes == 0); /* active controls' retained blur is leased too */
    /* Real mask leases must have been suspended first, including hidden ones.
     * The small metadata table survives while its controls still exist. */
    assert(masks.bytes < 8192);
    s_cache = masks.bytes;
    s_trims++;
    if (s_clear_failure_trim && s_trims >= s_clear_failure_trim) s_fail_read_offset = UINT32_MAX;
}
void orb_fonts_port_preferences_load(orb_content_size_t *size, bool *bold)
{
    *size = s_saved_size; *bold = s_saved_bold;
}
bool orb_fonts_port_preferences_save(orb_content_size_t size, bool bold)
{
    assert(s_running == &s_main_identity);
    s_saved_size = size; s_saved_bold = bold; s_saves++;
    return true;
}

struct orb_fonts_bitmap { struct orb_fonts_bitmap *next; uint8_t *data; size_t bytes; };
static orb_fonts_bitmap_t *s_bitmaps;

orb_fonts_bitmap_t *orb_fonts_port_map(uint32_t offset, size_t size)
{
    assert(s_boot_loading || s_running == &s_main_identity);
    orb_fonts_bitmap_t *bitmap = allocate(sizeof(*bitmap), 20);
    if (!bitmap) return NULL;
    bitmap->data = malloc(size); /* flash mapping is not a PSRAM bitmap copy */
    bitmap->bytes = size;
    if (!bitmap->data || !orb_fonts_port_read(offset, bitmap->data, size)) {
        free(bitmap->data); orb_fonts_port_free(bitmap);
        return NULL;
    }
    bitmap->next = s_bitmaps; s_bitmaps = bitmap;
    s_maps++;
    if (!s_boot_loading) s_runtime_maps++;
    return bitmap;
}
const uint8_t *orb_fonts_port_bitmap_acquire(orb_fonts_bitmap_t *bitmap) { return bitmap->data; }
void orb_fonts_port_bitmap_release(void) {}
void orb_fonts_port_bitmap_stats(size_t *mapped, size_t *copied)
{
    *mapped = *copied = 0;
    for (orb_fonts_bitmap_t *bitmap = s_bitmaps; bitmap; bitmap = bitmap->next) *mapped += bitmap->bytes;
}
void orb_fonts_port_unmap(orb_fonts_bitmap_t *bitmap)
{
    orb_fonts_bitmap_t **slot = &s_bitmaps;
    while (*slot != bitmap) { assert(*slot); slot = &(*slot)->next; }
    *slot = bitmap->next;
    s_unmaps++;
    memset(bitmap->data, 0xa5, bitmap->bytes);
    free(bitmap->data); orb_fonts_port_free(bitmap);
}

int xTaskCreateWithCaps(void (*entry)(void *), const char *name, unsigned stack,
                        void *context, unsigned priority, TaskHandle_t *handle, unsigned capabilities)
{
    assert(!"font preparation must not create a PSRAM task");
    return 0;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_running; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task == &s_main_identity); s_notifications++; }

static void work(void)
{
    assert(s_notifications);
    s_notifications = 0;
    s_running = &s_main_identity; orb_fonts_service(); s_running = &s_ui_identity;
    if (s_deferred_raster) {
        lv_obj_raster_prepare(s_deferred_raster);
        assert(lv_obj_raster_ready(s_deferred_raster)); /* after old-font retirement */
    }
    orb_glass_build_stats_t masks;
    orb_glass_build_stats(&masks);
    if (s_required_masks) assert(masks.bytes == s_required_masks);
    lv_draw_sw_material_stats_t material;
    lv_draw_sw_material_get_stats(&material);
    lv_obj_raster_stats_t rasters;
    lv_obj_raster_get_stats(&rasters);
    s_cache = masks.bytes + material.bytes + rasters.bytes;
    s_peak = LV_MAX(s_peak, s_charged + s_cache + s_network);
    assert(BOARD_FONT_POOL - s_charged - s_cache - s_network >= PSRAM_FLOOR);
}

static void changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer; (void)subject;
    s_changes++;
    assert(orb_font(ORB_TEXT_TITLE1)->line_height > 0);
    if (s_deferred_raster && s_running == &s_main_identity) {
        lv_obj_invalidate_raster(s_deferred_raster);
        lv_obj_raster_prepare(s_deferred_raster);
        assert(!lv_obj_raster_ready(s_deferred_raster)); /* no cold workspace beside two font sets */
    }
}

#include "test_font_retained.h"

static void settle(unsigned size, bool bold)
{
    assert(orb_fonts_set_content_size(size) == ORB_FONTS_OK);
    assert(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
    while (orb_fonts_pending()) work();
    if (s_notifications) work(); /* persistence is main-owned too */
    assert(orb_fonts_content_size() == size && orb_fonts_bold_text() == bold);
    assert(orb_fonts_last_result() == ORB_FONTS_OK);
}

static void boot(unsigned selected)
{
    s_saved_size = selected / 2; s_saved_bold = selected % 2;
    assert(orb_fonts_preload_saved() == ORB_FONTS_OK);
    assert(orb_fonts_content_size() == s_saved_size && orb_fonts_bold_text() == s_saved_bold);
    const orb_font_set_t *set = current();
    for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++)
        assert((loaded(i) != NULL) == set_uses(set, orb_fonts_table.builtin_count + i));
    /* Release-before-load uses the generated complete set's declared IDs.
     * Every wire fallback is a declared role, even before its root is loaded. */
    for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) {
        record_font_t *root = loaded(i);
        if (!root) continue;
        for (const lv_font_t *fallback = root->font.fallback; fallback; fallback = fallback->fallback) {
            for (uint16_t j = 0; j < orb_fonts_table.pack_count; j++)
                if (loaded(j) && fallback == &loaded(j)->font)
                    assert(set_uses(set, orb_fonts_table.builtin_count + j));
        }
    }
    assert(!orb_fonts_pending() && !s_saves); /* never request Default then switch later */
    printf("saved boot %u: resident=%zu peak=%zu\n", selected, s_charged, s_peak);
}

static void saved_boots(const char *executable)
{
    for (unsigned selected = 0; selected < 6; selected++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            char value[] = {(char)('0' + selected), '\0'};
            execl(executable, executable, "--saved-boot", value, (char *)NULL);
            _exit(127);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void partial_boot(void)
{
    orb_font_id_t shared = orb_fonts_table.sets[ORB_CONTENT_SIZE_DEFAULT][0].clocks[ORB_CLOCK_LARGE].font;
    s_fail_read_offset = orb_fonts_table.pack[shared - orb_fonts_table.builtin_count].offset;
    assert(orb_fonts_preload_default() == ORB_FONTS_ERR_LOAD);
    const lv_font_t *rescue = orb_clock_font(ORB_CLOCK_LARGE);
    assert(rescue->line_height < 100);
    s_fail_read_offset = UINT32_MAX; s_boot_loading = false;
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    work();
    const lv_font_t *published = orb_clock_font(ORB_CLOCK_LARGE);
    assert(published != rescue && published->line_height > 100);
    assert(!orb_fonts_pending() && orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
    assert(orb_clock_font(ORB_CLOCK_LARGE) == published);
    puts("async fonts: partial-boot shared fallback recovered");
}

#ifdef ORB_FONTS_SURFACE_TEST
void font_surfaces_complete(bool failed)
{
    assert(orb_fonts_pending());
    if (failed) {
        const orb_font_set_t *requested = &orb_fonts_table.sets[s_wanted_size][s_wanted_bold];
        for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) {
            orb_font_id_t id = orb_fonts_table.builtin_count + i;
            if (set_uses(requested, id) && !set_uses(current(), id)) {
                s_fail_read_offset = orb_fonts_table.pack[i].offset;
                break;
            }
        }
        assert(s_fail_read_offset != UINT32_MAX);
    }
    while (orb_fonts_pending() || s_notifications) {
        s_notifications = 0;
        s_running = &s_main_identity; orb_fonts_service(); s_running = &s_ui_identity;
    }
    s_fail_read_offset = UINT32_MAX;
    assert((orb_fonts_last_result() != ORB_FONTS_OK) == failed);
}

void font_surfaces_prepare(void)
{
    s_surface_mode = true;
    s_limit = SIZE_MAX / 4; /* UI caches are outside the fonts-only board fixture. */
    lv_init();
    s_running = &s_main_identity; orb_fonts_service_init(); s_running = &s_ui_identity;
    assert(orb_fonts_preload_default() == ORB_FONTS_OK);
    s_boot_loading = false;
    orb_fonts_set_cache_callbacks(orb_glass_cache_suspend, orb_glass_cache_resume);
}

void font_surfaces_switch(unsigned pair, bool retained)
{
    bool switched = pair != (unsigned)orb_fonts_content_size() * 2u + orb_fonts_bold_text();
    if (retained) {
        retain_before_switch();
        queue_before_switch(lv_display_get_default());
    }
    assert(orb_fonts_set_content_size(pair / 2) == ORB_FONTS_OK);
    assert(orb_fonts_set_bold_text(pair % 2) == ORB_FONTS_OK);
    while (orb_fonts_pending() || s_notifications) {
        s_notifications = 0;
        s_running = &s_main_identity; orb_fonts_service(); s_running = &s_ui_identity;
    }
    assert(orb_fonts_content_size() == pair / 2 && orb_fonts_bold_text() == pair % 2);
    assert(orb_fonts_last_result() == ORB_FONTS_OK);
    if (retained) {
        finish_queued_after_switch(switched);
        visit_retained_after_switch();
    }
}
#endif

static void fallback_closure(void)
{
    assert(orb_fonts_preload_default() == ORB_FONTS_OK);
    orb_font_id_t root_id = current()->styles[ORB_TEXT_TITLE1].face[0].tabular;
    const lv_font_t *font = orb_fonts_get(root_id);
    assert(font && font->fallback);
    const lv_font_t *fallback = font->fallback;
    unsigned fallback_index = orb_fonts_table.pack_count;
    for (unsigned i = 0; i < orb_fonts_table.pack_count; i++)
        if (loaded(i) && &loaded(i)->font == fallback) fallback_index = i;
    assert(fallback_index < orb_fonts_table.pack_count);
    orb_font_set_t minimal = {0};
    for (unsigned style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
        for (unsigned variant = 0; variant < 2; variant++)
            minimal.styles[style].face[variant] = (orb_face_ids_t){root_id, root_id, root_id};
    }
    for (unsigned clock = 0; clock < ORB_CLOCK_COUNT; clock++) minimal.clocks[clock].font = root_id;
    assert(!set_uses(&minimal, orb_fonts_table.builtin_count + fallback_index));
    orb_fonts_pack_release_unused(&minimal);
    assert(loaded(fallback_index)); /* closure does not require a top-level ID */
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(font, &glyph, 'H', 0));
    assert(glyph.resolved_font == fallback);
    decode_retained(&glyph, true);
    puts("async fonts: published fallback closure survives retirement");
}

static uint32_t exclusive_record(unsigned pair, unsigned other, bool exclude_default)
{
    const orb_font_set_t *set = &orb_fonts_table.sets[pair / 2][pair % 2];
    const orb_font_set_t *excluded = &orb_fonts_table.sets[other / 2][other % 2];
    for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) {
        orb_font_id_t id = orb_fonts_table.builtin_count + i;
        if (set_uses(set, id) && !set_uses(excluded, id) &&
            (!exclude_default || !set_uses(&orb_fonts_table.sets[0][0], id)))
            return orb_fonts_table.pack[i].offset;
    }
    assert(!"fixture needs an unshared record");
    return UINT32_MAX;
}

static void rollback_histories(void)
{
    unsigned checked = 0;
    for (unsigned history = 0; history < 6; history++) {
        for (unsigned from = 0; from < 6; from++) {
            for (unsigned to = 0; to < 6; to++) {
                if (from == to) continue;
                settle(history / 2, history % 2);
                settle(from / 2, from % 2);
                size_t resident = s_charged;
                unsigned refusals = s_refusals, changes = s_changes, saves = s_saves;
                s_fail_read_offset = exclusive_record(to, from, false);
                assert(orb_fonts_set_content_size(to / 2) == ORB_FONTS_OK);
                assert(orb_fonts_set_bold_text(to % 2) == ORB_FONTS_OK);
                work();
                assert(orb_fonts_last_result() != ORB_FONTS_OK && !orb_fonts_pending());
                assert(orb_fonts_content_size() == from / 2 && orb_fonts_bold_text() == (bool)(from % 2));
                assert(s_charged == resident && s_refusals == refusals && s_saves == saves);
                assert(s_changes == changes + 1);
                s_fail_read_offset = UINT32_MAX;
                checked++;
            }
        }
    }
    printf("async fonts: %u fragmented rollback histories restored without allocation refusal\n", checked);
}

static void recovery(void)
{
    settle(0, false);
    size_t old_charge = s_charged;
    unsigned before = s_changes, saves = s_saves, trims = s_trims;
    retain_before_switch();
    queue_before_switch(lv_display_get_default());
    s_fail_read_offset = exclusive_record(1, 0, false);
    assert(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    work();
    assert(!orb_fonts_pending() && !orb_fonts_bold_text());
    assert(orb_fonts_last_result() != ORB_FONTS_OK && s_changes == before + 1);
    assert(s_charged == old_charge && s_saves == saves && s_trims == trims + 2);
    finish_queued_after_switch(true);
    visit_retained_after_switch();
    s_fail_read_offset = UINT32_MAX;
    /* A successful completion coalesced back to the applied pair still clears
     * the previous failure through exactly one observer notification. */
    before = s_changes;
    assert(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    assert(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    work();
    assert(orb_fonts_last_result() == ORB_FONTS_OK && s_changes == before + 1);
    trims = s_trims;
    s_fail_read_offset = exclusive_record(1, 0, false);
    s_clear_failure_trim = trims + 2;
    settle(0, true); /* first attempt fails; all-optional trim enables one retry */
    assert(s_trims == trims + 2);
    s_clear_failure_trim = 0;
    assert(s_saved_bold && s_saved_size == ORB_CONTENT_SIZE_DEFAULT);
    settle(ORB_CONTENT_SIZE_LARGER, true);
    s_fail_read_offset = exclusive_record(5, 3, true);
    s_fail_read_second_offset = exclusive_record(3, 5, true);
    retain_before_switch();
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    work(); /* requested and previous fail, but Default remains available */
    assert(orb_fonts_last_result() != ORB_FONTS_OK && !orb_fonts_bold_text());
    assert(orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT);
    assert(orb_clock_font(ORB_CLOCK_LARGE)->line_height > 100);
    visit_retained_after_switch();
    s_fail_read_offset = s_fail_read_second_offset = UINT32_MAX;
    settle(ORB_CONTENT_SIZE_AX3, true);
    retain_before_switch();
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER) == ORB_FONTS_OK);
    s_fail_alloc = true; work(); s_fail_alloc = false;
    assert(orb_fonts_last_result() != ORB_FONTS_OK && !orb_fonts_bold_text());
    assert(orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT);
    assert(orb_clock_font(ORB_CLOCK_LARGE)->line_height < 100);
    orb_fonts_load_stats_t stats;
    orb_fonts_load_stats(&stats);
    assert(!stats.resident_bytes && !s_bitmaps); /* complete app-font rescue */
    visit_retained_after_switch();
    settle(0, true); /* an exhausted recovery can still retry successfully */
    old_charge = s_charged;
    saves = s_saves;
    s_assets_updating = true;
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    work();
    assert(orb_fonts_last_result() != ORB_FONTS_OK && s_charged == old_charge);
    assert(orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT && orb_fonts_bold_text());
    assert(s_saves == saves && !s_switch_locked); /* OTA rejects before retirement */
    s_assets_updating = false;
}

int main(int argc, char **argv)
{
#ifdef ORB_FONTS_SURFACE_TEST
    if (argc == 3 && !strcmp(argv[1], "--surface")) return font_surfaces_main(argv[2]);
#endif
    if (argc == 1) saved_boots(argv[0]);
    lv_init();
    s_running = &s_main_identity; orb_fonts_service_init(); s_running = &s_ui_identity;
    if (argc == 3 && !strcmp(argv[1], "--saved-boot")) { boot((unsigned)atoi(argv[2])); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--fallback-closure")) { fallback_closure(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--partial-boot")) { partial_boot(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--lazy-control")) {
        assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        assert(!orb_fonts_pending() && orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
        assert(orb_font(ORB_TEXT_TITLE1)->line_height > 0);
        return 0;
    }
    ensure_ready(); /* inject handle/table OOM after status wrappers exist */
    if (argc == 1) {
        s_fail_alloc = true;
        assert(orb_fonts_preload_default() == ORB_FONTS_ERR_NO_MEM);
        s_fail_alloc = false;
        assert(orb_fonts_handles_prepare());
        s_fail_alloc = true;
        assert(orb_fonts_preload_default() == ORB_FONTS_ERR_NO_MEM); /* pointer table too */
        s_fail_alloc = false;
    }
    assert(orb_fonts_preload_default() == ORB_FONTS_OK); /* pointer-table OOM can retry */
    s_boot_loading = false;
    assert(sim_display_create()); orb_ui_init(NULL);
    orb_fonts_set_cache_callbacks(orb_glass_cache_suspend, orb_glass_cache_resume);
    lv_obj_t *root = lv_screen_active();
    orb_glass_cache_prewarm();
    const int shapes[][2] = {{80,80}, {88,64}, {250,72}};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *control = lv_obj_create(root);
        lv_obj_set_size(control, shapes[i][0], shapes[i][1]);
        orb_glass_apply(control, ORB_GLASS_REGULAR, lv_color_white());
        if (i < 2) orb_glass_kit_press(control, 0);
        if (i == 1) lv_obj_set_hidden(control, true); /* hidden controls keep leases too */
    }
    orb_glass_cache_suspend(); orb_glass_cache_clear(); orb_glass_cache_resume();
    orb_glass_build_stats_t masks;
    orb_glass_build_stats(&masks);
    assert(masks.bytes > 200000); /* every original visible/hidden lease restored */
    s_required_masks = masks.bytes;
    s_deferred_raster = lv_obj_create(root);
    lv_obj_set_size(s_deferred_raster, 100, 50);
    lv_obj_set_raster(s_deferred_raster, true);
    lv_obj_set_style_bg_opa(s_deferred_raster, LV_OPA_COVER, 0);
    orb_kit_rim_title_create(root, "Accessibility", lv_color_white());
    lv_obj_t *list = lv_obj_create(root); lv_obj_set_size(list, 418, 300);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){.title="Elapsed", .value="01:23"});
    orb_kit_row_set_live_value(row, "01:23", "88:88");
    assert(lv_subject_add_observer(orb_fonts_subject(), changed, NULL));
    /* Every directed combination, including switches between the largest
     * non-default sets; a successful Default->Bold check alone is inadequate. */
    for (unsigned from = 0; from < 6; from++) {
        for (unsigned to = 0; to < 6; to++) {
            settle(from / 2, from % 2);
            s_cache = CACHE_BYTES;
            s_network = TLS_TRANSIENT; /* held handshake or application-OTA growth */
            assert(s_charged + s_cache + s_network <= s_limit);
            s_peak = s_charged + s_cache + s_network;
            s_font_peak = s_charged;
            size_t largest;
            (void)allocation_gap(0, &largest);
            assert(largest <= FRAGMENT_SPAN && largest * 3 < BOARD_FONT_POOL - s_charged);
            retain_before_switch();
            queue_before_switch(lv_display_get_default());
            unsigned refusals = s_refusals;
            settle(to / 2, to % 2);
            assert(s_refusals == refusals);
            finish_queued_after_switch(from != to);
            visit_retained_after_switch();
            lv_obj_update_layout(root);
            printf("switch %u->%u: fonts=%zu resident=%zu peak=%zu free-with-tls=%zu largest=%zu\n", from, to,
                s_font_peak, s_charged, s_peak, BOARD_FONT_POOL - s_peak, largest);
        }
    }
    s_network = 0;
    rollback_histories();
    recovery();
    /* Rapid requests coalesce before the worker takes the UI lock. */
    unsigned before = s_changes;
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER) == ORB_FONTS_OK);
    while (orb_fonts_pending()) work();
    if (s_notifications) work();
    assert(orb_fonts_content_size() == ORB_CONTENT_SIZE_LARGER && orb_fonts_bold_text());
    assert(s_changes == before + 1 && s_saved_size == ORB_CONTENT_SIZE_LARGER);
    assert(s_maps && s_unmaps && s_trims);
    lv_obj_delete(list); lv_obj_clean(root);
    puts("async fonts: six saved boots, 36 bounded switches, TLS reserve, live raw styles, failure/retry and coalescing passed");
    return 0;
}
