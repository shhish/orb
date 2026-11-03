#pragma once

/*
 * Orb's type and icons: Inter at Apple's watchOS text styles (docs/hig.md section 4)
 * and an SF-style icon set (Framework7, offset at the weight of the text), as
 * prerendered LVGL fonts (4 bpp, kerned).
 *
 *     orb_text_apply(title, ORB_TEXT_LARGE_TITLE, ORB_TEXT_REGULAR, 0);
 *     orb_text_apply(timer, ORB_TEXT_BODY, ORB_TEXT_TABULAR, 0);
 *     lv_label_set_text(row, ORB_ICON_TERMINAL " Sessions");
 *
 * Dynamic Type: the content size (Default, Larger, AX3) and Bold Text are global
 * settings. Labels styled with orb_text_apply() / orb_icon_apply() / orb_clock_apply()
 * (or the shared styles from orb_text_style() and friends) follow them by themselves.
 * All normal faces are in the signed assets partition. Firmware preloads Default
 * at boot and prepares other settings off the UI thread. Metadata stays in
 * PSRAM; selected Default icon/clock bitmaps remain leased in signed flash.
 * The signed app owns rescue Inter Body 28 px, icons and compact status markings.
 *
 * Every text font falls back to the icon font of its size and weight, so ORB_ICON_*
 * strings work inline in any label. The tabular variant falls back to the normal font,
 * so only the figures change.
 *
 * Call everything except the main service APIs with the LVGL lock held,
 * like any LVGL function.
 * See components/orb_fonts/README.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "orb_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apple's text styles, largest first. */
typedef enum {
    ORB_TEXT_LARGE_TITLE, /* Default size: 65 px Regular, emphasized Bold */
    ORB_TEXT_TITLE1,      /* 62 px */
    ORB_TEXT_TITLE2,      /* 49 px */
    ORB_TEXT_TITLE3,      /* 33 px */
    ORB_TEXT_HEADLINE,    /* 28 px Semibold */
    ORB_TEXT_BODY,        /* 28 px, the default */
    ORB_TEXT_CAPTION1,    /* 26 px */
    ORB_TEXT_CAPTION2,    /* 25 px */
    ORB_TEXT_FOOTNOTE1,   /* 23 px */
    ORB_TEXT_FOOTNOTE2,   /* 21 px, the smallest text allowed */
    ORB_TEXT_STYLE_COUNT,
} orb_text_style_t;

/* Variant flags, combinable. */
typedef uint8_t orb_text_variant_t;
#define ORB_TEXT_REGULAR    0u
#define ORB_TEXT_EMPHASIZED (1u << 0) /* Apple's bold variant: Large Title Bold, every other style Semibold */
#define ORB_TEXT_TABULAR    (1u << 1) /* fixed-width figures, for numbers that change */

/*
 * The time separator, U+2236: a colon raised to the figures' centre ("12" ORB_TIME_SEP "45").
 * Text, tabular and clock fonts all have it. A plain ':' sits on the baseline in text and
 * tabular fonts ("Elapsed: 01" ORB_TIME_SEP "23"); clock fonts raise both.
 */
#define ORB_TIME_SEP "\xE2\x88\xB6"

/* ---- Dynamic Type ------------------------------------------------------------------ */

/* The content sizes Orb offers (docs/hig.md section 4): Apple's watchOS categories. */
typedef enum {
    ORB_CONTENT_SIZE_DEFAULT, /* xLarge: Body 28 px */
    ORB_CONTENT_SIZE_LARGER,  /* xxxLarge: Body 31 px */
    ORB_CONTENT_SIZE_AX3,     /* accessibility size AX3: Body 38 px */
    ORB_CONTENT_SIZE_COUNT,
} orb_content_size_t;

typedef enum {
    ORB_FONTS_OK = 0,
    ORB_FONTS_ERR_ARG,           /* not a content size */
    ORB_FONTS_ERR_NO_PACK,       /* no verified signed pack in the assets partition */
    ORB_FONTS_ERR_PACK_MISMATCH, /* the pack there was generated with other fonts than this firmware */
    ORB_FONTS_ERR_NO_MEM,        /* background loader could not be allocated */
    ORB_FONTS_ERR_LOAD,          /* record read/inflate/parse/allocation failed */
} orb_fonts_err_t;

/* Capture the internal-stack main owner before UI startup. Service queued font
 * preparation and publication on that owner under LVGL as one render transaction.
 * Host tools keep synchronous preparation and these calls are no-ops. */
void orb_fonts_service_init(void);
void orb_fonts_service(void);
/* Register the UI's reversible cache lease transaction before starting services.
 * Both callbacks run on main under LVGL, with drawing drained. */
void orb_fonts_set_cache_callbacks(void (*suspend)(void), void (*resume)(void));

/* Boot/UI-thread entry, after LVGL initialization and before starting Wi-Fi. Loads
 * every Default style/variant and clock into PSRAM. Missing assets keep rescue fonts. */
orb_fonts_err_t orb_fonts_preload_default(void);
/* Boot only: prepare the persisted pair before any UI or glass prewarm exists. */
orb_fonts_err_t orb_fonts_preload_saved(void);
orb_fonts_err_t orb_fonts_preload(orb_content_size_t size, bool bold);
/* Last completed request; acceptance clears a prior failure so retry is possible. */
orb_fonts_err_t orb_fonts_last_result(void);

typedef struct {
    uint32_t loads;             /* successful cold pack loads, excluding cache hits */
    uint32_t failures;
    uint64_t elapsed_us;        /* top-level load wall time, including its fallbacks */
    size_t resident_bytes;     /* inflated records and parsed metadata, not staging */
    size_t mapped_bytes;       /* signed flash bitmap payload, outside the heap */
    size_t bitmap_copy_bytes;  /* mapped payload migrated to PSRAM before assets OTA */
} orb_fonts_load_stats_t;
void orb_fonts_load_stats(orb_fonts_load_stats_t *out);
bool orb_fonts_pending(void);  /* firmware setting load awaiting UI-thread commit */

/*
 * Change the content size or Bold Text. On firmware this queues PSRAM preparation
 * on the internal-stack main service; OK means accepted. The current setting
 * remains active until the complete set is ready, then the main service commits it
 * and notifies the subject.
 * Requests made while loading coalesce to the latest requested pair. Host tools
 * apply synchronously. Shared styles (so every label styled through this
 * API) switch to the new fonts at once and LVGL re-lays them out; then orb_fonts_subject()
 * notifies. Public font descriptors live forever and follow their current semantic
 * role, including pointers kept in local styles or retained screens. Only backing
 * payloads are freed. Observers still re-measure geometry and typography caches.
 *
 * Anything but the default needs the font pack; without a matching one the setting stays
 * as it was and the error says why. A pack font that can't be loaded later (out of memory,
 * damaged record) is replaced by the built-in 28 px rescue face, and logged.
 */
orb_fonts_err_t orb_fonts_set_content_size(orb_content_size_t size);
orb_content_size_t orb_fonts_content_size(void);
orb_fonts_err_t orb_fonts_set_bold_text(bool on);
bool orb_fonts_bold_text(void);
const char *orb_fonts_err_name(orb_fonts_err_t err);

/* An integer subject that changes after every content size or Bold Text change, for
 * screens that need to re-measure (lv_subject_add_observer_obj(orb_fonts_subject(), ...)). */
lv_subject_t *orb_fonts_subject(void);
/* Changes before style publication, for caches holding a retained font handle. */
uint32_t orb_fonts_revision(void);

/* ---- Text styles ------------------------------------------------------------------- */

/* A text style at the current content size and Bold Text setting. */
typedef struct {
    const char *name;           /* "Large Title" */
    uint8_t pt;                 /* Apple's point size */
    uint8_t size_px;
    uint8_t leading_px;         /* Apple's line height: baseline to baseline */
    int8_t letter_space;        /* whole pixels of the tracking, for text_letter_space; the
                                   fraction is already built into the font's advances */
    int16_t tracking;           /* Apple's tracking at this size, 1/1000 em */
    uint16_t weight;            /* 400 Regular, 500 Medium, 600 Semibold, 700 Bold, 800 Heavy */
    uint16_t emphasized_weight;
} orb_text_style_info_t;

/* Valid until the next content size or Bold Text change (the pointer stays the same). */
const orb_text_style_info_t *orb_text_style_info(orb_text_style_t style);

/* Nominal em size for an already published text/icon/clock face. Never loads
 * assets or follows a stale font pointer; 0 means the face is not Orb-owned. */
uint16_t orb_fonts_nominal_px(const lv_font_t *font);

/* The style's font: text, figures and inline icons. */
const lv_font_t *orb_font(orb_text_style_t style);
const lv_font_t *orb_font_variant(orb_text_style_t style, orb_text_variant_t variant);

/* Icons only, at the style's size and weight, centred in the line box (toolbar glyphs,
 * list leading icons, big state glyphs). ORB_TEXT_TABULAR is ignored. */
const lv_font_t *orb_icon_font(orb_text_style_t style);
const lv_font_t *orb_icon_font_variant(orb_text_style_t style, orb_text_variant_t variant);

/* Compact tabular digits (0-9, space) and ORB_ICON_LIGHTNING_FILL for orb_status.
 * The size tracks the text style at about 44%; regular uses Semibold and emphasized
 * or Headline uses Bold for legibility. Always built-in, including Dynamic Type sizes.
 * Not a general text font: only U+2026 has the app-owned Inter rescue fallback.
 * Status widgets keep numeric content and do not use DOT truncation. */
const lv_font_t *orb_status_font(orb_text_style_t style, orb_text_variant_t variant);
/* Use a widget's already measured cap height. Keeps content inside its geometry
 * if a pack load fails during layout then recovers before drawing. No pack access. */
const lv_font_t *orb_status_font_for_cap(orb_text_style_t style,
                                        orb_text_variant_t variant, int32_t cap_height);

/* text_line_space that makes LVGL's line pitch equal to Apple's leading for this font
 * (negative: Inter's line box, with room for accented capitals, is taller). */
int32_t orb_text_line_space(orb_text_style_t style, orb_text_variant_t variant);

/* Shared styles owned by orb_fonts (font, letter space, line space), kept current through
 * content size and Bold Text changes. Add them with lv_obj_add_style(), or use the
 * apply functions below. */
const lv_style_t *orb_text_style(orb_text_style_t style, orb_text_variant_t variant);
const lv_style_t *orb_icon_style(orb_text_style_t style, orb_text_variant_t variant);

/* Gives `obj` the shared text style (replacing any orb_fonts style it had for `selector`).
 * The label follows content size and Bold Text changes. */
void orb_text_apply(lv_obj_t *obj, orb_text_style_t text_style, orb_text_variant_t variant,
                    lv_style_selector_t selector);

/* Icons-only labels: the icon font, no letter or line spacing. */
void orb_icon_apply(lv_obj_t *obj, orb_text_style_t text_style, orb_text_variant_t variant,
                    lv_style_selector_t selector);

/* Fills your own lv_style_t (lv_style_init first) with the style's current values. It is a
 * snapshot: it doesn't follow content size changes (see orb_fonts_set_content_size()). */
void orb_text_style_init(lv_style_t *style, orb_text_style_t text_style, orb_text_variant_t variant);

/* ---- Clock faces ------------------------------------------------------------------- */

/* Tabular figures, a colon raised to the figures' centre (':' and ORB_TIME_SEP), space,
 * A, M, P. Clocks keep their size at every content size; Bold Text makes them heavier. */
typedef enum {
    ORB_CLOCK_LARGE,  /* 140 px, the face */
    ORB_CLOCK_MEDIUM, /* 96 px, timers and the stopwatch */
    ORB_CLOCK_ALWAYS_ON, /* 128 px, low-ink Light (Regular with Bold Text) */
    ORB_CLOCK_COUNT,
} orb_clock_t;

typedef struct {
    uint8_t size_px;
    uint16_t weight;
    int16_t tracking;    /* 1/1000 em */
    int8_t letter_space; /* whole pixels, as for text styles */
} orb_clock_info_t;

const orb_clock_info_t *orb_clock_info(orb_clock_t clock);
const lv_font_t *orb_clock_font(orb_clock_t clock);
const lv_style_t *orb_clock_style(orb_clock_t clock);
void orb_clock_apply(lv_obj_t *obj, orb_clock_t clock, lv_style_selector_t selector);

/* The licences of Inter (SIL OFL 1.1), Framework7 and Phosphor (MIT), for Settings > About >
 * Legal. Linked into every image that uses these fonts, as the licences require. */
const char *orb_fonts_notices(void);

#ifdef __cplusplus
}
#endif
