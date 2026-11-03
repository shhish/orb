# Compiled offline apps

This is a small allocation-free registry and lifecycle controller, independent
of LVGL. `orb_apps_init(capabilities)` offers the optional compiled provider
`orb_apps_register_builtins()`; the weak default returns false and registers the
example Marble manifest. Linking `orb_games` supplies four offline games instead.
The original proof controller remains available in builds without that component.
Additional compiled modules register static-lifetime `const` manifests through
`orb_apps_register()`. There are at most eight manifests, with unique IDs, names,
Framework7 glyphs, flat icon colours, entry/exit/pause/tick callbacks and declared
IMU/audio/network/storage needs. Capabilities mean available services, not current
network connectivity. An app requiring unavailable capabilities cannot enter.
Storage directories are prepared on launch when storage is mounted; a storage
failure blocks launch rather than pretending saves work.

The main task owns every registry/lifecycle API. Shell code must queue requests
to it instead of calling under the LVGL lock. Apps are compiled trusted modules:
callbacks must not block, start private input loops, reenter this API, or retain
the input pointer. A failed enter calls exit for partial cleanup. Only one app
runs at a time. Main samples input at 20 ms and calls:

```c
orb_apps_poll(now_us, false, screen_on, accel_x, accel_y, accel_z, motion_valid);
```

The three acceleration components are screen-space specific force in g:
+x right, +y up, +z out. At rest the vector points away from Earth, so
downhill movement in LVGL coordinates is `-accel_x/+accel_y`.

Screen-off pauses the app and stops ticks; wake resumes it unless a manual pause
is still active. Main routes debounced PWR and BOOT gestures through
`orb_shell_button()`. PWR short opens the game's own pause sheet when its hook
handles the request; other apps use the shell's Resume / Exit app sheet. PWR
hold opens the system Power sheet. BOOT short is Back, offered to the game before
the generic app exit, and BOOT hold starts talk. Visible Close/Exit controls queue
`orb_apps_request_exit()` for main to consume. The shell returns to its own app
list when `orb_apps_active()` becomes NULL. Apps release audio/storage resources
on exit and stop work on lifecycle pause.

The poll API retains its raw `pwr_held` escape for isolated controller users:
holding it for 1.2 seconds revokes the app before cleanup, including while paused.
Production passes false because the debounced system button route owns PWR.

Marble demonstrates a tilt controller and immutable snapshot for a future shell
renderer. `orb_apps_enter("marble")` starts it; `orb_marble_snapshot()` returns
unit-circle positions. The model clamps to an inset circle, rejects nonfinite
motion and caps elapsed-time steps; pause resets the clock so wake never jumps.
`orb_marble_nudge()` supplies the same movement for visible directional controls,
so tilt has a button alternative (hig.md sections 7–8). It creates no screen,
widgets, glass, animations, fonts or app launcher changes. A later renderer must
use true black content, a white marble, proper Close/directional targets and pass
the screenshot design gate at default/AX3/Reduce Transparency before it ships.
No rendered proof app is claimed by this stream.

Registry and Marble use 96 bytes of static BSS including alignment in the current
ESP32 build map (56 registry/control, 40 Marble), no heap, no task and no audio
buffers. This is `firmware/build/orb.map` around `s_power_since`/`s_marble`, not a
live device measurement. No dynamic allocations occur per frame.

```sh
make -C firmware/components/orb_apps/tests/host test
```

ASan/UBSan checks cover manifest validation/duplicates/capacity, capability/storage
failure, partial-entry cleanup, one-owner lifecycle, pause/resume, escape timing
including timestamp zero/release/repress/paused apps, and finite bounded Marble
motion plus button movement without an IMU.

## Queued UI integration

After startup registration is complete, the shell may enumerate the immutable
manifest catalogue and use `orb_apps_request_enter(id)` under its UI lock. This
bounded atomic request is consumed by main's next poll, including when no app is
active. An exit request replaces any pending launch and is consumed even while
paused. Launch acceptance does not imply resource/capability checks have passed;
the shell observes `orb_apps_active()` for actual ownership. These requests do
not call lifecycle callbacks on the UI task. See `orb_games/README.md` for the
renderer, controls, score-save contract and shell-owner integration boundary.
