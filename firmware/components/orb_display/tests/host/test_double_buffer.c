/* Exercise production admission and aliasing fences with delayed DMA. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks;
#define CHECK(cond) do { s_checks++; if (!(cond)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)
#define ORB_LCD_W 466
#define BAND_BYTES (ORB_LCD_W * BAND_ROWS * 2)
#define HALF_BAND_BYTES (BAND_BYTES / 2u)
#define HALF_BAND_STRIDE ((HALF_BAND_BYTES + 63u) & ~63u)
#define DRAW_ARENA_BYTES (2u * HALF_BAND_STRIDE)
#define DOUBLE_BUFFER_RESERVE 24576u
#define DOUBLE_BUFFER_DMA_BLOCK 8192u
static bool s_snapshot_transition;
static bool s_independent_bands;

#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_DMA 2U
#define MALLOC_CAP_8BIT 4U
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "test";
static unsigned s_pipeline_lock, s_st_lock, s_lock_depth;
#define portENTER_CRITICAL(lock) do { \
    CHECK((lock) == &s_pipeline_lock || (lock) == &s_st_lock); \
    CHECK(!s_lock_depth); s_lock_depth++; } while (0)
#define portEXIT_CRITICAL(lock) do { \
    CHECK((lock) == &s_pipeline_lock || (lock) == &s_st_lock); \
    CHECK(s_lock_depth == 1); s_lock_depth--; } while (0)

typedef struct { bool flushing; } lv_display_t;
static lv_display_t s_display, *s_disp = &s_display;
static _Alignas(64) uint8_t s_arena[DRAW_ARENA_BYTES];
static void *s_draw_buf = s_arena;
static bool s_double_buffer_requested, s_double_buffer_attempted, s_double_buffer_admitted, s_double_buffered;
static bool s_flush_pending, s_small_refresh;
#if CONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER
static bool s_live_independent;
static bool orb_display_live_bands_independent(lv_display_t *display)
{
    CHECK(display == s_disp);
    return s_live_independent;
}
#endif
static uint32_t s_half_frames, s_full_frames;
static struct { uint64_t wait_us; } s_st;
static int64_t s_now;
static unsigned s_sequence, s_wait_at, s_ready_at, s_install_at, s_installs, s_waits, s_ready;
static unsigned s_free_reads, s_largest_reads;
static size_t s_free_internal, s_free_dma, s_largest_dma;
#define LV_DISPLAY_RENDER_MODE_PARTIAL 1

static size_t heap_caps_get_free_size(unsigned caps)
{
    CHECK(!s_lock_depth);
    s_free_reads++;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return s_free_internal;
    CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    return s_free_dma;
}
static size_t heap_caps_get_largest_free_block(unsigned caps)
{
    CHECK(!s_lock_depth);
    CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    s_largest_reads++;
    return s_largest_dma;
}
static int64_t esp_timer_get_time(void) { return s_now; }
static bool orb_display_half_buffers_fit_scene(lv_display_t *disp, size_t bytes, bool snapshot_transition)
{
    CHECK(disp == s_disp && bytes == HALF_BAND_BYTES);
    CHECK(snapshot_transition == s_independent_bands);
    return s_small_refresh || snapshot_transition;
}
static void flush_wait_cb(lv_display_t *disp)
{
    CHECK(!s_lock_depth && disp == s_disp && s_flush_pending && disp->flushing);
    s_wait_at = ++s_sequence;
    s_waits++;
    s_now += 100; /* Transfer still owns its source until this returns. */
    s_flush_pending = false;
}
static void lv_display_flush_ready(lv_display_t *disp)
{
    CHECK(!s_lock_depth && disp == s_disp && !s_flush_pending);
    CHECK(s_wait_at && s_wait_at == s_sequence);
    s_ready_at = ++s_sequence;
    s_ready++;
    disp->flushing = false;
}
static void lv_display_set_buffers(lv_display_t *disp, void *first, void *second,
                                   uint32_t bytes, int mode)
{
    CHECK(!s_lock_depth && disp == s_disp && !disp->flushing && !s_flush_pending);
    CHECK(first == s_draw_buf && mode == LV_DISPLAY_RENDER_MODE_PARTIAL);
    CHECK(second == NULL || second == s_arena + HALF_BAND_STRIDE);
    CHECK(bytes == (second ? HALF_BAND_BYTES : BAND_BYTES));
    if (second) CHECK((uintptr_t)second % 64 == 0);
    CHECK(bytes <= DRAW_ARENA_BYTES);
    s_install_at = ++s_sequence;
    s_installs++;
}
#endif

#include "orb_double_buffer_impl.inc"

#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
static void reset(void)
{
    s_double_buffer_requested = s_double_buffer_attempted = s_double_buffer_admitted = s_double_buffered = false;
    s_snapshot_transition = false;
    s_independent_bands = false;
#if CONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER
    s_live_independent = false;
#endif
    s_flush_pending = s_small_refresh = s_display.flushing = false;
    s_half_frames = s_full_frames = 0;
    s_sequence = s_wait_at = s_ready_at = s_install_at = s_installs = s_waits = s_ready = 0;
    s_free_reads = s_largest_reads = s_lock_depth = 0;
    s_st.wait_us = s_now = 0;
    s_free_internal = s_free_dma = DOUBLE_BUFFER_RESERVE;
    s_largest_dma = DOUBLE_BUFFER_DMA_BLOCK;
}
static void admission(void)
{
    reset();
    try_double_buffer();
    CHECK(!s_double_buffer_attempted && !s_free_reads && !s_largest_reads);
    orb_display_request_double_buffer();
    CHECK(s_double_buffer_requested && !s_double_buffer_attempted);
    CHECK(!s_free_reads && !s_largest_reads && !s_installs);
    try_double_buffer();
    CHECK(s_double_buffer_attempted && s_double_buffer_admitted && !s_double_buffered);
    CHECK(s_free_reads == 2 && s_largest_reads == 1 && !s_installs);
    orb_display_request_double_buffer();
    try_double_buffer();
    CHECK(s_free_reads == 2 && s_largest_reads == 1);
    /* Runtime figures from round1 A: old full-buffer allocation failed here. */
    reset();
    s_free_internal = 41731;
    s_free_dma = 33943;
    s_largest_dma = 31744;
    orb_display_request_double_buffer();
    try_double_buffer();
    CHECK(s_double_buffer_admitted);
    for (unsigned which = 0; which < 3; which++) {
        reset();
        if (which == 0) s_free_internal--;
        if (which == 1) s_free_dma--;
        if (which == 2) s_largest_dma--;
        orb_display_request_double_buffer();
        try_double_buffer();
        CHECK(s_double_buffer_attempted && !s_double_buffer_admitted);
        s_free_internal = s_free_dma = s_largest_dma = 100000;
        orb_display_request_double_buffer();
        try_double_buffer();
        CHECK(!s_double_buffer_admitted && s_free_reads == 2 && s_largest_reads == 1);
    }
}
static void switching(bool pending)
{
    reset();
    s_small_refresh = true;
    select_draw_buffers();
    CHECK(!s_double_buffered && s_full_frames == 1 && !s_installs);
    orb_display_request_double_buffer();
    try_double_buffer();
    s_display.flushing = s_flush_pending = pending;
    select_draw_buffers();
    CHECK(s_double_buffered && s_half_frames == 1 && s_installs == 1);
    CHECK(s_waits == (unsigned)pending && s_ready == (unsigned)pending);
    if (pending) CHECK(s_wait_at < s_ready_at && s_ready_at < s_install_at);
    CHECK(s_st.wait_us == (pending ? 100u : 0u));
    /* Unchanged mode must not drain the sender: that would lose overlap. */
    s_display.flushing = s_flush_pending = true;
    select_draw_buffers();
    CHECK(s_double_buffered && s_half_frames == 2 && s_installs == 1);
    CHECK(s_flush_pending && s_display.flushing);
    CHECK(s_waits == (unsigned)pending);
    s_small_refresh = false;
    select_draw_buffers();
    CHECK(!s_double_buffered && s_full_frames == 2 && s_installs == 2);
    CHECK(!s_flush_pending && !s_display.flushing);
    CHECK(s_waits == (unsigned)pending + 1 && s_ready == s_waits);
    CHECK(s_wait_at < s_ready_at && s_ready_at < s_install_at);
    CHECK(s_st.wait_us == (pending ? 200u : 100u));
    select_draw_buffers();
    CHECK(!s_double_buffered && s_full_frames == 3 && s_installs == 2);
}

static void snapshots(bool admitted)
{
    reset();
    if (admitted) {
        orb_display_request_double_buffer();
        try_double_buffer();
    }
    /* Full damage is ineligible until the shell certifies the opaque cached
     * scene. Enabling changes no aliased buffer until RENDER_START. */
    select_draw_buffers();
    CHECK(!s_double_buffered && !s_installs);
    orb_display_set_snapshot_transition(true);
    CHECK(s_snapshot_transition && !s_double_buffered && !s_installs);
    s_display.flushing = s_flush_pending = true;
    select_draw_buffers();
    CHECK(s_double_buffered == admitted);
    CHECK(s_installs == (unsigned)admitted);
    CHECK(s_waits == (unsigned)admitted);
    if (admitted) CHECK(s_wait_at < s_ready_at && s_ready_at < s_install_at);

    /* Repeated cached frames retain overlap. Clearing the certificate fences
     * the last half before live glass can reappear, even without a redraw. */
    s_display.flushing = s_flush_pending = true;
    select_draw_buffers();
    CHECK(s_flush_pending && s_display.flushing);
    orb_display_set_snapshot_transition(false);
    CHECK(!s_snapshot_transition && !s_double_buffered);
    CHECK(s_installs == (admitted ? 2u : 0u));
    CHECK(s_waits == (admitted ? 2u : 0u));
    if (admitted) {
        CHECK(!s_flush_pending && !s_display.flushing);
        CHECK(s_wait_at < s_ready_at && s_ready_at < s_install_at);
    }
    unsigned installs = s_installs, waits = s_waits;
    orb_display_set_snapshot_transition(false);
    CHECK(s_installs == installs && s_waits == waits);
    CHECK(s_free_reads == (admitted ? 2u : 0u));
    CHECK(s_largest_reads == (unsigned)admitted);
}
#if CONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER
static void live_scene_switching(void)
{
    reset();
    orb_display_request_double_buffer(); try_double_buffer();
    s_live_independent = true;
    select_draw_buffers();
    CHECK(s_independent_bands && s_double_buffered);
    s_flush_pending = s_display.flushing = true;
    s_live_independent = false;
    select_draw_buffers();
    CHECK(!s_independent_bands && !s_double_buffered && !s_flush_pending);
    CHECK(s_waits == 1 && s_wait_at < s_ready_at && s_ready_at < s_install_at);
    s_live_independent = true;
    select_draw_buffers();
    CHECK(s_independent_bands && s_double_buffered);
    CHECK(DRAW_ARENA_BYTES - BAND_BYTES < 128); /* same early arena */
}
#endif
#endif
int main(void)
{
    CHECK(HALF_BAND_BYTES == ORB_LCD_W * (BAND_ROWS / 2) * 2u);
    CHECK(HALF_BAND_STRIDE % 64 == 0);
    CHECK(DRAW_ARENA_BYTES >= BAND_BYTES && DRAW_ARENA_BYTES - BAND_BYTES < 128);
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    admission();
    switching(false);
    switching(true);
    snapshots(false);
    snapshots(true);
#if CONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER
    live_scene_switching();
#endif
#else
    orb_display_request_double_buffer();
    try_double_buffer();
    orb_display_set_snapshot_transition(true);
    CHECK(s_snapshot_transition);
    orb_display_set_snapshot_transition(false);
    CHECK(!s_snapshot_transition);
#endif
    printf("adaptive buffers (%d rows, option %d): %u checks passed\n", BAND_ROWS,
           CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER, s_checks);
    return 0;
}
