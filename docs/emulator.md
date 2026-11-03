# Orb desktop emulator

Run the real firmware UI on macOS, with the firmware LVGL fork, fonts, banded
RGB565 renderer, `orb_shell`, navigation, Home, Time and all four registered games.
SDL2 supplies a desktop window and input in place of the physical board.

From the repository root:

```sh
make emu
make emu EMU_ARGS='--scale 2'
```

The target configures `sim/build`, builds `orb_emu` with four workers and runs it.
It requires CMake, Ninja, zlib and Homebrew SDL2 (`sdl2-compat` 2.32 works).
Homebrew's `/opt/homebrew` prefix is supplied by `make emu`; nothing is installed
or downloaded by this target. To build without opening a window:

```sh
cmake -S sim -B sim/build -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build sim/build --target orb_emu -j4
sim/build/orb_emu --help
```

The 466 × 466 logical panel is circular, with a flat dark-grey surround. `--scale
2` doubles its window dimensions, preserving panel coordinates and nearest-pixel
presentation. Retina backing pixels follow macOS's display scaling. The UI runs
from LVGL's monotonic tick, with a 60 Hz desktop presentation loop. Idle UI does
not force LVGL redraws; SDL can still present the existing texture.

## Controls

| Input | Behaviour |
| --- | --- |
| Left mouse | Touch, tap, hold and continuous drag. Swipes and tangential rim drags use the firmware recogniser. A touch must start inside the disc; dragging beyond it stays on its edge. |
| P | PWR press: Apps from Face, Face from other shell surfaces, or pause in a game. A press on a dim/off display wakes it and is consumed without navigating. |
| Double P | Control Center, including an overlay that pauses a running game and returns to it on Back. |
| Hold P | Hold for 2 s to open the system power sheet (Sleep display / Restart), including during games. Sleep display changes only the fixture's display state; Restart has no host effect. |
| B | BOOT press is Back; hold for 350 ms to talk, then release to stop. The shell sends the production voice requests; silent fake audio finishes with the real `no_speech` result. |
| Arrow keys | Dip the corresponding edge while held, easing over 150 ms; release returns to level. This feeds the same gravity vector used by Marble and glass/parallax. |
| Option + mouse movement | Tilt from the pointer's position relative to the panel centre. No mouse button is required. Release Option to level. Entering this mode cancels a touch. |
| F | Toggle fake face-down: silence, quiet/ambient shell state and paused games. Press again to recover. |
| 1 / 2 / 3 / 4 / 5 / 6 | Explicit debug jumps: Face / Agents / Home / Apps / Stack / Notifications. |
| D | Toggle diagnostics. |
| S | Save the current SDL presentation as a PNG in `sim/build/emu-shots/`. |
| Q / Escape | Quit. Closing the window also quits. |

P and B remain raw button state while held. Each emulator step polls the same
`orb_buttons` recognizer as the board: 30 ms debounce, 300 ms after release to
distinguish short from double, and the shared hold thresholds. Its events route
through `orb_shell_button`, including BOOT voice requests and consuming PWR on
wake. No separate desktop press timer or PWR destination mapping is used.
Only the numbered debug jumps exit an app before restoring a requested surface.

Loss of window focus releases raw buttons and levels tilt; the shared recognizer
finishes any debounced press normally. An interrupted mouse gesture is cancelled
without clicking. Key auto-repeat never generates repeated presses or toggles.

The initial content comes from the existing simulated sessions/cards and Home
snapshot. Battery is 80%, USB is connected, and the clock uses the Mac's local
time. The UI and window title identify this as fixture/demo data. Answers,
messages, preferences and Home actions complete locally. Time, game rules,
navigation, holds, control layouts and drawing are the production code. Scores
and Marble keepsakes persist in bounded RAM for this run only.

## Diagnostics and screenshots

The overlay uses Inter on a plain black surface, with no glass or animation.
It reports desktop presentation fps and frame time, LVGL render time, dirty
area, and live/peak LVGL allocation payload. The label explicitly says **Mac
only / Not board speed**.

- Frame time includes the desktop loop and pacing; render time is the latest
  LVGL render interval, excluding SDL presentation. Idle frames can have no UI
  render. Headless frames run unpaced, so their throughput is not 60 fps timing.
- Dirty area is the sum of flushed, band-rounded rectangle pixels for the last
  presentation frame, divided by 466². It includes band padding and repeated
  flushes; it measures renderer traffic, not the number of changed image pixels.
- An emulator-only CLIB allocator tracks requested LVGL bytes across draw
  threads. It excludes malloc metadata, SDL, fonts allocated outside LVGL and
  other firmware allocators. There is no finite desktop free-pool/fragmentation
  reading. The headless image-test allocator and firmware allocator are unchanged.
- PNG encoding uses LVGL's allocator too, so taking screenshots can raise its
  reported peak. Headless frame time also includes that encoding work.
- The overlay refreshes four times a second and therefore adds redraw work when
  visible. Hide it when inspecting idle damage. Screenshots include the overlay
  if shown, the circular mask and the grey surround.

Use `--out DIR` to change the screenshot directory. PNGs are taken from SDL's
renderer before presentation, at the renderer's actual backing resolution.

## Offscreen execution and tests

```sh
SDL_VIDEODRIVER=dummy sim/build/orb_emu --headless-frames 60 --out /tmp/orb-frames
sim/build/orb_emu --headless-frames 60 --scene agents --text-size ax3 --bold --out /tmp/orb-agents
sim/build/orb_emu --headless-frames 60 --scene card --reduce-transparency --out /tmp/orb-card
cmake --build sim/build -j4
ctest --test-dir sim/build -j4 --output-on-failure
```

`--headless-frames N` forces SDL's dummy video driver, creates a hidden SDL
window/software renderer, advances LVGL at deterministic 60 Hz ticks and saves
every presentation as `frame-000001.png` through `frame-NNNNNN.png`. It uses the
same texture upload, mask, renderer and PNG path as the visible window. Reusing
an output directory replaces matching frame filenames. Use around 60 frames
for settled animation evidence; a few frames deliberately capture startup.

Scene shortcuts: `face`, `agents`, `home`, `apps`, `stack`, `notifications`,
`card` (an approval), `play`, `marble` (the real registered app). Accessibility
flags: `--text-size default|larger|ax3`, `--bold`, `--reduce-transparency`,
`--reduce-motion`. Settings in the UI also apply for this run.

CTest includes SDL dummy-driver smoke runs at 1× and 2×, raw input and pointer
tests, memory-accounting tests, and fixture/controller interaction coverage.
The flow test drives double P into Control Center, a two-second P hold into the
power sheet, BOOT Back/talk, wake consumption, and the same gestures in games.
The existing PNG suite remains independent of SDL window creation.

## Hub mode and physical limitations

`--hub` is a documented stub: it prints an explicit unsupported message and
exits with status 2 before SDL or any network activity. It never silently falls
back to fixtures or reads the board's credentials. No hub code was changed.

The existing protocol could support a separately paired emulator without new
hub routes, but requires a host pinned-TLS WebSocket client, its own unique
device identity and pairing/token storage, request deadlines/keepalives, and
bounded sessions/cards/Home snapshot decoding. The reusable Python client is a
test fixture rather than a production transport. This work is intentionally
left for a separate integration; do not use the dev board's token or disable
certificate verification. See [protocol.md](protocol.md).

No Wi-Fi/BLE, pairing, OTA, microphone capture, network transcription, physical
power management or real home/session actions run in fixture mode. The host
sound backend does not emit audio. Face-down demonstrates the shell/app policy;
it does not emulate the PMU or switch off a Mac display. Mac timings do not
predict ESP32 speed, memory placement, DMA, panel scan, tearing or AMOLED colour.
All three gravity axes reach the production app controller, including the
face-down key's negative z axis and the games' captured viewing pose.
The lead's independent screenshot review and physical-board design gate remain
necessary before merging or shipping firmware UI changes.

## Design evidence

The button integration preserves the firmware's layouts. The merge validation
captures Face, Agents and an approval card through SDL, and Control Center and
the power sheet through the flow test's raw keyboard input. Each accessibility
variant runs in a fresh process. Paths below are relative to `sim/build/`.

| Surface | Default | AX3 + Bold | AX3 + Bold + Reduce Transparency |
| --- | --- | --- | --- |
| Face | `emu-merge-sdl/face-default/frame-000060.png` | `emu-merge-sdl/face-ax3-bold/frame-000060.png` | `emu-merge-sdl/face-ax3-bold-rt/frame-000060.png` |
| Agents list | `emu-merge-sdl/agents-default/frame-000060.png` | `emu-merge-sdl/agents-ax3-bold/frame-000060.png` | `emu-merge-sdl/agents-ax3-bold-rt/frame-000060.png` |
| Approval card | `emu-merge-sdl/card-default/frame-000060.png` | `emu-merge-sdl/card-ax3-bold/frame-000060.png` | `emu-merge-sdl/card-ax3-bold-rt/frame-000060.png` |
| Control Center | `emu-merge-buttons/default/control-center-default.png` | `emu-merge-buttons/ax3-bold/control-center-ax3-bold.png` | `emu-merge-buttons/ax3-bold-rt/control-center-ax3-bold-rt.png` |
| Power sheet | `emu-merge-buttons/default/power-sheet-default.png` | `emu-merge-buttons/ax3-bold/power-sheet-ax3-bold.png` | `emu-merge-buttons/ax3-bold-rt/power-sheet-ax3-bold-rt.png` |

To capture the keyboard surfaces again, create an output directory and run:

```sh
ORB_EMU_FLOW_SHOT_DIR=/tmp/orb-button-shots ORB_EMU_FLOW_SHOT_VARIANT=ax3-bold-rt \
  sim/build/test_emu_flow
```

`ORB_EMU_FLOW_SHOT_VARIANT` accepts `default` (also the default when unset),
`ax3-bold`, or `ax3-bold-rt`. Screenshot mode runs a short capture flow; leave
`ORB_EMU_FLOW_SHOT_DIR` unset for the complete 120-check interaction regression.

Diagnostics at 2×: `sim/build/emu-smoke-2x/frame-000024.png`.
The implementation and an independent agent both inspected these captures.

| Design-gate heading | Evidence / result |
| --- | --- |
| Two layers | Pass across the captured surfaces: matte content, glass only on controls/navigation, no stacked glass; approval has three glass controls and no tinted default. |
| Canvas | Pass: true black inside the disc, flat #2C2C2C outside, no added gradients or glow; SDL preserves the firmware pixels. |
| Round-first | Pass: circular SDL mask; labels and controls fit their chords, including enlarged power-sheet labels. |
| Sizes | Pass: shared Inter styles and Framework7 icons; AX3 list reflows and approval actions stack without bezel clipping; power controls retain 72 px height. |
| Colour | Pass: status text accompanies meaningful accents; approvals and system power actions remain untinted. |
| Approvals & agents | Pass: Allow and Deny stay explicit, no automatic answer/dismissal and no work spinner. Demo responses occur only after interaction. |
| Motion | Shared production springs and accessibility settings; settled screenshots show stable layouts and opaque variants. Interaction tests exercise drags, rim control and game pause/resume. Panel motion is unmeasured. |
| Gesture alternatives | Pass: Face Apps, list navigation, card actions, Control Center Back and power-sheet close/actions remain visible. |
| Feels Apple-made | Pass for host presentation: consistent hierarchy, restrained controls, stable round layouts. Lower default-size Agents rows intentionally fade/truncate until scrolled. |

No device pixels or UI layout were redesigned for the emulator. These results
establish desktop presentation and fixture interaction; they do not substitute
for the required independent lead/physical-panel review before firmware merges.

Validation on `orb/w2-trunk` with the `orb/w2-emu` merge content:

- `cmake --build sim/build -j4`: passed with warnings treated as errors for Orb code.
- `ctest --test-dir sim/build -j4 --output-on-failure`: **651/651 passed**
  (646 existing tests plus all five emulator tests), 21.94 s on the final Mac run.
- Emulator tests: `emu_smoke`, `emu_smoke_2x`, `emu_input` (34 checks), `emu_flow`
  (120 checks), `emu_memory` (215 checks and 8,000 concurrent reallocations).
- Nine SDL scene/accessibility captures and three raw-button capture runs passed.
- No GUI window or board access, no Git writes, and no deletion of build contents.
