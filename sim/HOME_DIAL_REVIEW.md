# Home Rooms dial review — 2026-10-28

Implemented ROUND-FIRST.md §2 in `orb_home`, without changes to `orb_ui`, shell
sources or the signed fonts. No Git write commands, network access, board flash,
owner configuration changes or live Home requests were performed. Data is from
`orb_home_load_demo()` and bounded synthetic extensions of that snapshot.

## Behavior and design decisions

- Rooms use their snapshot order, starting at 12 o'clock, clockwise. A set has
  up to eight 80px discs at radius168; AX3 has six 88px discs. Remaining rooms
  continue in the next set when focus steps past the current set.
- Default focus is the first room with a present On light/switch/outlet, else
  the first room. Stable room ID restores focus after device/detail/scene Back,
  snapshot reordering and developer save/restore. Removed IDs resolve again.
- One tap focuses; a second tap on that disc, or the centre, opens the existing
  room device list. Selection does not send a Home command.
- The existing `orb_home_view_dial`/`rotate` normalized owner plugs directly into
  the shell's rim/crown/twist path. The shell supplies one tick per detent; Home
  does not double-play it. Ends clamp, as the existing normalized owner does.
- The centre shows the house caption, focused name and room-specific state,
  including a room temperature when present. AX3 drops the caption, wraps the
  name to at most two lines and uses compact count wording. Offline/loading
  removes the ring and shows the connection explanation with house caption.
- Pending and briefly failed writes use a 3px yellow/red focus rim plus explicit
  text. Resting focus is a 2px white rim at native90/99px (rounded1.12×), avoiding
  a separately transformed glass layer. Selected-disc press events reach the
  retained glass surface.
- Scenes opens a native round list. Physical provenance, moving/disabled guards,
  pending/offline/All off interlocks and model-level unlock refusal remain.
  Unsafe scene metadata is excluded from the list. Device lists, room All off,
  thermostat modes/dials, hold-to-unlock and rollback tests are retained.

**HIG precedence:** the sketch asks for glass/tint on every on-room disc, but HIG
§§1/2 and the design gate allow at most three glass controls and one tinted
control. Surrounding room discs therefore use flat semantic state content; the
focused room and Scenes are the only two glass surfaces. Only focus can tint.
Colliding pairs use existing Title3 emphasis; singles use existing Title2
emphasis. This keeps AX3 pairs inside their discs without adding font sizes or
glyphs. The 46px Scenes capsule is the HIG minimum target; its width144px and
centre(233,291) preserve20px gaps to the focused lower AX3 discs.

The device/scene row's existing340px width is behind `HOME_ROW_WIDTH`. The shared
`orb_round_list` chord-width stream still needs the lead's merge; no shared UI
source was modified here.

## Screenshots and inspection

All paths below are relative to this repository. Each row has three PNGs at
`sim/build/home-dial/{default,ax3,ax3-bold-rt}/NAME.png` and matching `.jpg`
inspection companions. `ax3-bold-rt` enables AX3, Bold Text and Reduce Transparency.

| NAME | Surface / specific check |
| --- | --- |
| `dial-on` | First on-room focus; caption/count, mixed-device state and temperature. |
| `dial-off` | Bedroom focus; neutral glass, white initial, explicit Off. |
| `dial-offline` | Ring absent; complete connection explanation and house caption. |
| `dial-pending` | Pending text and yellow3px focused rim. |
| `dial-failed` | Failure text and red3px rim, without a new control or glyph. |
| `dial-scenes` | Existing safe scenes in the native round list, no decorative glass rows. |
| `dial-eight` | Eight Default/six AX3 targets; Be/Ba collision initials. |
| `dial-next-ring` | Later set, Studio focus; no page dots or hidden room loss. |

Contact sheets: `sim/build/home-dial/review-{default,ax3,ax3-bold-rt}.jpg`.
The primary agent and an independent agent inspected all24 captures.
Some single-image preview presentations omitted repeated text/discs that exist
in the file. Independent PNG decoding matched the raw framebuffer byte-for-byte;
caption prefix/suffix pixels and complete JPEG contact sheets confirmed the
content. The simulator also compares every retained screenshot with a forced
full frozen redraw. Preview omission was not accepted as evidence of missing
firmware content.

Design gate results applying to each surface/profile above:

| Heading | Result and evidence |
| --- | --- |
| Two layers | Pass. Flat room state/list content; only focus+Scenes glass, maxone tint; offline has none. |
| Canvas | Pass. True black, bounded rims/press response, no gradients or glows. |
| Round-first | Pass for dial. Radius168, native80/88 targets, clockwise layout; later sets and centre remain inside the bezel. Device/scene row chord-width update remains the explicitly separate merge. |
| Sizes | Pass. Existing Inter styles via orb_text_apply; room targets80/88, Scenes46px minimum; no new fonts/glyphs. Text line boxes satisfy the16px bezel chord; offline AX3 uses two lines at376px within an approximately420px chord. Closest five-room AX3 lower-focus/Scenes gap is about20.1px. |
| Colour | Pass. Yellow indicates actual on state alongside readable state text; Off is explicit; pending/failure have words and rim changes; contrast uses dark ink on yellow and light ink on neutral. |
| Approvals/feedback | Pass. No approval changes; scene physical/unlock refusal preserved; pending/failure text is paired with the rim. |
| Motion/accessibility | Pass in simulator. Stable retained focus, geometric press, native list settle; AX3+Bold+RT and Reduce Motion tests pass. No navigation animation allocation on focus steps. |
| Navigation | Pass. No visible back/paging chevrons or page dots; shell BOOT, rim and twist work; semantic section navigation retained without stealing disc taps. |
| Craft | Pass in simulator. Independent review found no overlap, clipped initials, bezel clipping or state/Scenes collision after spacing fixes. Board appearance remains the lead's gate. |

Reproduce:

```sh
cmake -S sim -B sim/build -G Ninja -DORB_CJSON_DIR=/path/to/local/cJSON
nice -n 10 cmake --build sim/build -j4
nice -n 10 ctest --test-dir sim/build -j4 --output-on-failure
sim/home_dial_shots.sh
```

## Allocation receipts

`sim/home_memory.c` is linked only into the Home screenshot executable. It counts
requested LVGL payload and allocation calls, not process RSS or malloc headers.
The baseline was captured from the unmodified Rooms-column source copied before
editing; receipt: `sim/build/home-dial/baseline.log`. Counts include hidden
semantic navigation descendants. “Released with view” is live payload before
minus after destroying Home, excluding shared caches that survive it.

| Profile | Old Rooms objects | Dial objects | Old live LVGL bytes | Dial live LVGL bytes | Old released with view | Dial released with view |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Default | 35 | 41 | 343,626 | 98,224 | 261,635 | 20,017 |
| AX3 | 35 | 34 | 347,766 | 99,578 | 220,695 | 16,457 |
| AX3+Bold+RT | 35 | 34 | 347,766 | 99,578 | 220,695 | 16,457 |

80 synchronous normalized focus steps allocate **zero LVGL payload** in every
profile, including repeated changes between on/off rooms. Tests also preserve
object identity/count across later sets. Room labels preallocate128byte owned
strings and both existing initial styles during construction. Home updates only
bounded text buffers, fixed measurement caches, positions, existing style
properties and hidden flags. No control/string/style creation occurs on a step
or Home redraw. A documented Home-local helper refreshes LVGL9's private fixed
label measurement cache because the public setter allocates a one-shot display
event on each update. These labels never use scrolling/dots/translation.

The shared LVGL renderer still allocates its normal draw-task metadata/text
snapshots:130 calls Default,142 AX3 for the measured forced redraw. This is
reported separately, not represented as zero total renderer allocations.
Peak receipts include transient renderer/cache preparation and are not retained
view bytes. External fonts, model/view calloc, allocator overhead and actual
board headroom are excluded. The host view struct is3,152bytes versus2,216before;
the bounded snapshot/model size is unchanged. No orb_memory limit was raised or
bypassed. The retained desktop LVGL payload is below the existing512KiB LVGL
budget; this does not claim measured on-board PSRAM headroom.

## Validation and integration status

- **316/316 Home CTests pass** (2.83s), including all Home view/shell/board
  profiles and screenshot fixtures. Coverage includes physical rim and
  twist, first-on/all-off default focus, initials, tap focus/open, radius/size,
  eight/six capacity, multiple sets, ID restoration, offline/loading, pending /
  failure expiry, retained-frame equality and no accidental Home writes.
- All306 Home screenshot fixtures pass, including existing locks, light detail,
  thermostat, sensor-age and All off cases. Home UX sequences pass73checks.
- Full CTest run: **890/897 passed**,31.81s in the final run under effective `nice -n10`. Seven legacy assertions below require
  migration outside this task's edit scope; no tests were disabled or weakened.
- Simulator builds with warnings-as-errors. Shellcheck and diff whitespace checks
  are recorded with the final validation logs.
- Firmware clean build: **passed**,2020/2020 Ninja steps, bounded at`-j4`.
  `firmware/build/orb.bin` is2,752,512bytes (`0x2a0000`); app partition has56% free.
  Latest Home source/header predate the compiled object and binary. SHA256:
  `7a91c2341a8402bbf5aacc3ee21e204bc5cbb021a274a3af40369cd53d19363b`.
  Build used only already-cached managed components with component downloads
  disabled. A temporary CMake include restored the dependency edges normally
  supplied by the component manager; no global include overrides remained.
  The only warning was an existing unused function in orb_devtools.
- `shellcheck sim/home_dial_shots.sh sim/home-shots.sh sim/home-polish-sheets.sh`
  and `git -c maintenance.auto=false diff --check`: passed.

Logs are copied under `sim/build/home-dial/logs/`. Initial sandboxed `nice` calls
reported that macOS refused `setpriority`; `-j4` remained bounded. The final
firmware build and complete CTest run were repeated outside that restriction,
with effective `nice -n10`, and produced the same results. The offline firmware command
was:

```sh
IDF_COMPONENT_MANAGER=0 nice -n 10 scripts/idf.sh \
  -D EXTRA_COMPONENT_DIRS=/tmp/orb-home-components \
  -D CMAKE_C_FLAGS= \
  -D CMAKE_PROJECT_INCLUDE=/tmp/orb-home-offline-dependencies.cmake clean build
```

The temporary dependency hook is copied to `logs/offline-dependencies.cmake`.
Cached dependencies stay outside this worktree. Standard online dependency
resolution was never invoked.

### Required lead updates outside this edit scope

| Test / file | Current assertion | Required migration |
| --- | --- | --- |
| `offline_contract_{default,ax3_bold_rt,reduce_motion}` / `sim/tests/test_shell.c:2054` | Cached Living Room label and old Last known Home state while offline. | Expect absent ring and connection explanation; preserve shell offline restrictions. |
| `scroll_fixtures` / `sim/tests/test_scroll_fixtures.c:315` | Home Rooms prepares a native scrolling-list fixture. | Remove Rooms from scroll-list fixture enumeration; use dial stepping coverage from Home tests. Device-list scroll coverage stays. |
| `nav_shell` / `sim/tests/test_nav_shell.c:305` | Home Rooms returns a non-null native scroll owner. | Assert normalized dial owner and focus change; `sim/tests/test_home_shell.c` already exercises actual rim+twist routing. |
| `list_scroll{,_ax3}` / `sim/tests/test_list_scroll.c:208` | Home Rooms supports native vertical list scrolling. | Stop treating Rooms as a list; retain room-device scroll checks. |

Production rim integration needs **no new shell hook**. The existing normalized
Home owner is sufficient. The legacy scroll-fixture implementation and shared
chord-width integration remain with the lead/other stream under the stated scope.

The lead must still review real-board screenshots via `scripts/orb-dev.sh shot`
and verify the signed-font boot receipt. No board was flashed or measured here.
