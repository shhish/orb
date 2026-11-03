# orb_status

Live iOS-style battery and Wi-Fi symbols for Orb. The C99 component depends only
on `lvgl` and `orb_fonts`; it does not read hardware or start timers. The shell
passes power and network state to it on the LVGL thread. Draws use LVGL's software
anti-aliased rounded rectangles, arcs and lines, with tiny Inter tabular figures
and the Framework7 `bolt_fill` glyph from `orb_fonts`.

```c
#include "orb_status.h"

lv_obj_t *battery = orb_battery_create(parent, ORB_TEXT_BODY, ORB_TEXT_REGULAR);
orb_battery_set_state(battery, 80, false, false);
orb_battery_set_percentage(battery, true);

lv_obj_t *wifi = orb_wifi_create(parent, ORB_TEXT_BODY, ORB_TEXT_REGULAR);
orb_wifi_set_rssi(wifi, -62, true);

/* Adjacent text and symbols share the same font baseline. */
lv_obj_set_pos(battery, x, baseline_y - orb_status_baseline(battery));
lv_obj_set_pos(wifi, x + 60, baseline_y - orb_status_baseline(wifi));
```

Use `REQUIRES orb_status` in the consuming component's CMake registration. Keep
state updates behind the usual LVGL lock. The existing protocol's
`power.battery_pct`, `power.charging` and `rssi` correspond directly to the setter
arguments. Low Power Mode is a local setting; there is no protocol change and no
hub dependency. Connection state comes from the network service, independently
of RSSI. Do not infer disconnection from a weak signal.

## Battery

The rounded body is 0.9 Inter cap heights tall and 2.2 times as wide, with a
separate terminal nub. It is centred on the selected text style's cap height.
Fill width is proportional to the clamped 0–100 level; 0 is empty. Shape dimensions
remain unchanged when the level, charging state or percentage visibility changes.

Colour precedence is Low Power Mode (`#FFD600`), charging (`#30D158`), low battery
at **20% or below** (`#FF4245`), then the inherited label colour. Charging still
shows a bolt in Low Power Mode, so state is not conveyed by colour alone. Optional
percentage digits omit the `%` sign, like iOS, and remain tabular. A charging
battery with percentages shows both the number and bolt, separated by a single
pixel gap. Text/bolt pixels over
the fill are knocked out in the backdrop colour; over empty space they retain
the status colour. The level remains visible even with a number inside.

## Wi-Fi

The fan consists of three concentric 90-degree arcs and a bottom dot. Active arcs
use the inherited label colour and opacity; inactive arcs use 30% of that opacity.
A connected zero-bar signal keeps the dot active. Disconnection dims the fan and
adds an anti-aliased slash with a backdrop-colour separation; it cannot be confused
with a connected, weak signal.

| RSSI | Bars |
| --- | --- |
| −55 dBm or stronger | 3 |
| −67 through −56 dBm | 2 |
| −80 through −68 dBm | 1 |
| Below −80 dBm | 0 |

`orb_wifi_set_bars()` also accepts an already classified signal. Values clamp to
0–3. The component deliberately does not smooth readings or use hysteresis; the
network service should supply stable state if desired.

## Layout and drawing

Both objects have the selected text font's line height, inherit `text_color` and
`text_opa`, and expose their baseline through `orb_status_baseline()`. Their
Dynamic Type observer measures again on content-size or Bold Text changes and is
automatically removed on deletion. Compact built-in fonts are selected from the
measured cap height when drawing, so even a failed pack-font load cannot make
numbers outgrow the battery. No stale pack font pointer survives a font change. Use ordinary layout or reposition against
the reported baseline in your layout's font-change observer when needed.

`orb_status_set_text_style()` changes the style and emphasis. The `TABULAR` flag
is ignored because battery figures are always tabular. Status objects do not
accept clicks or scroll. Their exact sizes are owned by the component; use style
selection rather than manually resizing them.

The widgets are transparent. Knock-outs assume black by default, matching Orb's
AMOLED canvas. On another flat surface call `orb_status_set_background()` with
that surface's colour. A varying backdrop such as glass requires compositing by
the owning surface; this component does not sample pixels behind it.

A changed state invalidates only the small widget bounds; a repeated state does
nothing. There is no bitmap cache, child-object tree, animation or periodic
redraw. The charging bolt and digits use ordinary generated font glyphs; battery
and Wi-Fi geometry is drawn live. LVGL copies clip areas into draw tasks, so the
fill knock-out remains safe with the firmware's two draw units.

## Validation

`scripts/fonts.sh check` builds this component as C99 with the real LVGL fork on
macOS, runs state/layout/render assertions, and writes
`docs/screens/fonts/status.png` and `screen-face.png`. The status sheet covers
Body and Headline, levels 100/80/50/20/5, charging and Low Power Mode, percentage
on/off, and Wi-Fi 0–3/disconnected. Firmware integration still needs an ESP-IDF
build and an on-device rendering/performance check; this work does not flash the
board.
