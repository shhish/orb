# Home merge and continuous lists — 2026-10-28

Resolved the working files on `orb/w2-home-merge`. No Git write command was run:
the three paths remain unmerged in the index until the lead stages them and
finishes the existing merge. No board, live HomeRelay, owner configuration or
running service was changed.

## Conflict decisions

| File / original conflict | Resolution |
| --- | --- |
| `orb_home_view.c` 1: view state | Retained trunk's separate Rooms/device scroll offsets and polish's scene/failure feedback timestamps. Removed the obsolete room-page rotary accumulator. |
| `orb_home_view.c` 2: More Devices callback | Retained the invisible semantic action, implemented as shared-list scrolling and settle instead of page replacement. It has no physical hit target. |
| `orb_home_view.c` 3: room view preamble | Kept polish's honest state formatter, physical All off action and receipts; kept trunk's actual device scroll owner. |
| `orb_home_view.c` 4: header and row construction | Kept the room-name action, pending/offline/count header and stable category ordering. All devices are direct children of one native list; the virtual viewport and fixed page slots are gone. |
| `orb_home_view.c` 5: discs and trailing values | Kept kind glyphs, including the floor-lamp glyph, plus polish's pending/failed/unknown readings. Glyphs are passive content in the full-row target. Default state remains right aligned; AX3 stacks name/state. |
| `orb_home_view.c` 6: pointer gestures | Kept trunk's latched 10px tap cancellation and native vertical tracking. Removed the page-swipe branch and restored the room early return so horizontal room drags cannot start an empty page transition. |
| `orb_home_view.c` 7: selecting a room | Saves Rooms scroll, resets the newly selected room to its first row, and prevents the outgoing room's offset overwriting that reset. Detail/action Back retains its room offset. |
| `home_main.c` 1: scrolling helper | Kept real viewport finger scrolling and all incoming polish scenes/fixtures; added explicit Rooms and busy-room scrolled captures. |
| `home_main.c` 2: continuation/Back check | Kept native scroll-position restoration, replacing obsolete fixed-page offset expectations. |
| `test_home_view.c`: navigation contract | Kept trunk's native-owner/extent/Back checks alongside every polish behavior group. Retained identity is distinguished from currently readable presentation. |

The unconflicted model/detail changes remain: rollback, All off batching,
scene feedback, thermostat modes, pending locks, sensor ages, empty/offline
states and 24-device coverage. The HomeRelay delayed-reply/SSE/replacement
snapshot race fix remains intact.

## Layout and input

Both Rooms and room devices now use `orb_round_list` with Settings' native row
metrics: **340×72, 84px cadence** at Default; **340×100, 116px cadence** at AX3.
Every row has the same dimensions and spacing. The shared stepped presentation
owns the native center and 82%/50%-opacity edge variant, and settles to logical
row positions after finger, rim or twist input.

Removed the permanently compact fifth accessory, its transform override, the
irregular row positions and the page-derived indicator. Native press focus
cannot move a partially visible row ahead of a drag. Scaled/clipped hit bounds
and the 10px cancellation guard prevent scrolling from becoming actuation.

Home's fixed header/scene controls leave a shorter viewport than Settings.
Rows whose transformed name/state would be clipped by that viewport or the
safe circle are presentation-hidden, with native geometry and snap points
preserved. A content-edge mask dissolves departing content. The usual 78px
tail is limited by available viewport space so AX3 Rooms can fully reveal its
last row. AX3 Rooms intentionally shows one complete stacked row at a time.

## Validation

- Twice, sequentially, under `nice -n 10`:
  `cmake --build sim/build -j4 && ctest --test-dir sim/build -j4 --output-on-failure`.
  **872/872 passed both times**, in 27.29s and 27.19s.
- Final `nice -n 10 scripts/idf.sh build`: passed with Ninja `-j 4`.
  Unsigned `orb.bin`: **0x2a0000 bytes**, **56% app-partition space free**.
- Home/list tests cover 1:1 held drags, 10px cancellation, directional finger/rim
  snapping, shell routing, boundary clamping, uniform metrics, every stable ID
  in a 24-device room and exact scroll restoration after Back.
- `shellcheck sim/home-shots.sh sim/home-polish-sheets.sh` and
  `git -c maintenance.auto=false diff --check`: passed.
- Isolated HomeRelay suite (`PYTHONPATH=hub`, the existing hub Python
  environment, `pytest -q hub/tests/test_home.py`): **49 passed in 3.26s**.
- Ruff format/check on both merged HomeRelay Python files: passed.
- Build/test logs: `sim/build/home-merge/logs/`.

## Screenshot evidence and design gate

All paths below are relative to `sim/build/home-merge/`. Each list surface was
rendered and inspected at **Default, AX3, and AX3+Bold+Reduce Transparency**;
independent review confirmed consistent rows without clipped text or fragments.

| Surface | PNG paths (each profile) | Visual result |
| --- | --- | --- |
| Rooms at start | `{default,ax3,ax3-bold-rt}/rooms.png` | Complete names and states; AX3 uses one complete stacked row. |
| Rooms after finger scroll | `{default,ax3,ax3-bold-rt}/rooms-scrolled.png` | Same row metrics; no displaced peripheral style or prior-row fragments. |
| Rooms at end | `{default,ax3,ax3-bold-rt}/rooms-next.png` | Final Dining row remains completely readable and reachable. |
| 24-device room at start | `{default,ax3,ax3-bold-rt}/busy-room.png` | Uniform native rows; positional edge presentation only. |
| 24-device room after finger scroll | `{default,ax3,ax3-bold-rt}/busy-room-scrolled.png` | Continuous spacing; complete readable rows without a tiny bottom fragment. |
| 24-device room at end | `{default,ax3,ax3-bold-rt}/busy-room-last.png` | Final device reachable; stable category ordering and state preserved. |
| Face accessibility regression | `face-gate.png` | Default/AX3+Bold+Reduce Transparency pair; no clipping or new decoration. |
| Card accessibility regression | `card-gate.png` | Default/AX3+Bold+Reduce Transparency pair; readable prose and controls. |

- **Two layers — pass:** lists/discs/readings are flat content; scene actions
  remain the existing bounded glass controls, with no glass on glass.
- **Canvas — pass:** true black; no decorative gradients or glows. Scroll-edge
  content masking follows HIG §2, rather than decorating the background.
- **Round-first — pass:** complete text fits viewport and safe chords; clipped
  previews are suppressed without changing native row size or snap positions.
- **Sizes — pass:** Settings metrics, Inter styles via `orb_text_apply`, and
  Caption 2 text retaining its 20px floor at the shared edge scale. AX3 reflows.
- **Colour — pass:** semantic yellow accompanies explicit state words; neutral
  off/unavailable readings and glyphs remain distinct without relying on hue.
- **Approvals/feedback — pass:** no approval changes; pending, failed and cached
  Home readings remain explicit. Physical-only writes/hold guards are tested.
- **Motion — pass in simulator:** shared finger tracking and snapping, Reduce
  Motion profiles, Bold Text and Reduce Transparency suites pass.
- **Navigation — pass:** BOOT, rim and twist remain available; no new visible
  back/paging chrome or competing semantic-action hit targets.
- **Craft — pass in simulator:** independent screenshot review found no
  inconsistent row sizes, middle compact row, internal gap or tiny fragments.

The lead's real-board screenshots via `scripts/orb-dev.sh shot` remain the
release gate. These host results do not claim measured board cadence or heap
headroom. No signing, flashing or merge completion was performed.
