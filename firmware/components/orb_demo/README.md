# orb_demo

The Sessions demo, laid out round-first per `docs/hig.md`, and the renderer's
benchmark. Depends on LVGL and `orb_ui` only, so the desktop simulator builds
it as it is.

- **Content**, at the back: a heading and ten session rows on dark platters
  (not glass); state is a symbol and words, coloured only to reinforce them.
  Each row is a raster cache over the black canvas (`lv_obj_set_raster_matte`):
  scrolling copies opaque bitmaps.
- **Scroll edge**: rows dissolve to black as they scroll up under the toolbar
  and the time, and again above the bottom action, through cache compositing opacity.
  Rows shrink about their center to fit the text-safe circle without rebuilding their bitmaps.
- **Controls** on Liquid Glass: back and add on the 45-degree diagonals at
  72 px, the time at 12 o'clock, one tinted primary action at 6 o'clock.
- **Parallax**: content up to 8 px, controls a quarter of that.

With Reduce Motion off, it alternates two 8-second phases, which the log names:

- `scroll`: the list scrolls down and back up (full-screen redraws);
- `glass`: the list rests while the light sweeps the glass's rims every frame
  and the primary action is pressed and released every half second (only the
  glass redraws).

## API

```c
void orb_demo_build(lv_obj_t *screen);                         /* after orb_ui_init() */
void orb_demo_tick(float gx, float gy, float gz, float dt_s);  /* accelerometer, 30-60 Hz */
const char *orb_demo_phase(void);                              /* "scroll" or "glass" */
```

All with `lv_lock()` held or on the LVGL task. Build it on the LVGL task
(`lv_async_call`): drawing the first raster caches needs its stack.

## Required sdkconfig

As `orb_ui` (Montserrat sizes).

The clock and control labels retain their transparent caches; session rows retain opaque caches. Reduce Motion stops the benchmark animations and freezes staged lighting.
