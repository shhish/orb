# Orb

Orb is an OS for the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**: a 466 × 466
round touch display with an ESP32-S3, 32 MB flash, 8 MB PSRAM, microphones,
speaker codec, motion sensor and battery power management. Its LVGL interface
includes clock faces, agent session views, Home controls, timers and offline
games. [Architecture](docs/design.md) · [Device protocol](docs/protocol.md)

<p align="center">
  <img src="docs/readme/faces.png" alt="Four Orb clock faces: digital with weather and complications, analog Meridian, activity rings, and the colour picker" width="100%">
</p>

**Faces.** True black, round-first layouts and Liquid Glass controls, following
the [Orb HIG](docs/hig.md): a digital face with weather, battery and Home
complications, the analog Meridian face, activity rings, and per-face colour.

<p align="center">
  <img src="docs/readme/home.png" alt="Weather forecast, Home rooms, Home scenes and a running timer" width="100%">
</p>

**Around the house.** Weather with the next hours, Apple Home rooms and scenes
through HomeRelay, and timers.

<p align="center">
  <img src="docs/readme/agents.png" alt="A coding agent asking to allow a build, the Allow and Deny card, notifications from Codex and Dot, and the Marble game" width="100%">
</p>

**Agents and play.** Claude and Codex sessions surface on the Smart Stack, and
a permission request becomes an Allow / Deny card on the device.
Notifications, and offline games like Marble, tilt to play.

<sub>Faces, weather, scenes and timer are frames from the
[design system](docs/design/); Home rooms, agents, notifications and Marble are
renders of the real firmware UI from the desktop emulator (`make emu`).</sub>

An **always-on Mac** runs the Python hub in [`hub/`](hub/README.md). It pairs
devices, maintains their pinned-TLS links, signs and serves firmware updates,
and can connect agent sessions, Dot/MCP and local speech transcription.
The optional **HomeRelay** integration bridges Apple Home through the Mac;
HomeRelay is a separate service and its credentials stay on the hub.
The desktop simulator in [`sim/`](sim/README.md) runs the firmware UI with
synthetic device data.

## Getting started

Install ESP-IDF 6.1 for firmware builds (default `$HOME/esp/esp-idf`, override
`IDF_PATH`), CMake, Ninja and SDL2 for the simulator, and Python 3.13+ for the hub.
See [hub installation](hub/README.md#install) for its dependencies and optional
local provider packages. From the repository root:

```bash
nice make fw-build             # ESP-IDF firmware, at most four build jobs
nice make sim                  # desktop simulator
nice make emu                  # interactive firmware shell
nice make hub-test PY=/path/to/hub-venv/bin/python
nice make sim-test
nice make lint                 # guardrails, ruff and shellcheck
```

For signed USB flashing, see the workflow below and
[partition migration](docs/storage-migration.md). `scripts/remote.sh flash`
handles a board on an SSH-connected USB host; `scripts/idf.sh -p <serial-port>
flash monitor` is the local IDF path. Build/generate the assets and sign the app
before flashing: [font/assets pipeline](firmware/components/orb_fonts/README.md).
Wi-Fi credentials are entered privately through `orb-hub setup` over BLE.

## License

Orb's own code is licensed under the [MIT License](LICENSE),
Copyright (c) 2026 shhish. Dependencies and fonts retain their own licenses;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including the
Espressif-product restriction in `esp_new_jpeg` and outstanding provenance items.

## Hardware details

| | |
| --- | --- |
| Chip | ESP32-S3R8, 8 MB octal PSRAM |
| Flash | 32 MB, QIO |
| USB | Built-in USB-Serial/JTAG; choose the serial port reported by your host |
| SDK | ESP-IDF v6.1 in `$HOME/esp/esp-idf` by default |
| Graphics | LVGL 9.6, local fork in `firmware/third_party/lvgl__lvgl` (see `ORB_PATCHES.md`) |
| Board support | `firmware/components/orb_board`: panel at 80 MHz, touch, audio codecs, power chip, buttons and motion sensor, using drivers directly |

## Build, flash, monitor

The ESP-IDF project is in `firmware/`. For a board attached to a separate USB host, the remote helper copies `build/` over SSH and runs esptool there. Set `ESP_REMOTE_HOST` (default `my-mac.local`) and `ESP_REMOTE_PORT` for that host.

```bash
scripts/idf.sh -j4 build
ORB_SIGNING_KEY=/path/to/test-key.pem scripts/remote.sh flash
scripts/remote.sh monitor
```

`scripts/remote.sh flash` signs the app before copying it and includes the font pack.
For development use a throwaway RSA-3072 key via `ORB_SIGNING_KEY`; the owner
initializes the permanent signing key. A board bootstrapped with the test key
needs a USB flash signed by the permanent key before it can accept that key's OTA releases.

`scripts/idf.sh` passes its arguments to `idf.py` (`menuconfig`, `add-dependency`, `size` and so on). If the board is plugged into the Mac mini instead, use `scripts/idf.sh -p /dev/cu.usbmodem… flash monitor`.
The wrapper defaults to four build jobs, exports the IDF/Ninja and CMake job
limits, and accepts `-j4` or `-j 4` (one through four jobs).

The app version defaults to `0.3.0-<seven-character git HEAD>` on each build.
For a one-build recovery/release identity, use
`scripts/idf.sh -D PROJECT_VER=0.3.0-recovery-20261027 build` (or set
`ORB_PROJECT_VER` for that invocation). The wrapper passes the override only to
that process; the next ordinary build returns to the git version and reconfigures
automatically. Cached `PROJECT_VER` values are ignored and removed. Use the
wrapper for overrides, because a direct CMake `-D PROJECT_VER=...` cannot be
distinguished from an old cached value. `CONFIG_APP_PROJECT_VER_FROM_CONFIG`
remains IDF's explicit persistent override for isolated benchmark configs.

Stable application releases must use the isolated stable configuration, which
compiles out both USB and wireless devtools:

```bash
scripts/idf.sh --channel stable -j4 build
# Owner release step, with a unique version supplied to the build as above:
orb-hub release firmware/build-stable/orb.bin --channel stable
```

The wrapper uses `firmware/sdkconfig.defaults` plus `firmware/sdkconfig.stable`
and saves its configuration in `firmware/build-stable/sdkconfig`; the normal
development configuration is untouched. CMake rejects a stable build if a
saved or overridden configuration enables `CONFIG_ORB_DEVTOOLS`. If that guard
fires, remove only `firmware/build-stable/sdkconfig` and repeat the stable
command. `orb-hub release` signs and publishes the supplied image; it does not
build it or change its configuration when `--channel stable` is selected.

## Simulator

`sim/` runs the UI on the Mac: the LVGL fork with the firmware's settings (`sim/lv_conf.h`), and `orb_ui` and `orb_demo` as they are, over a fake board. It renders the way the device does, in the same 84-row bands in the panel's pixel format, cut to the circle and kept clear of glass by the same code (`orb_display_geom.c`, `orb_glass_band_guard`), so what the renderer does to the UI, glass artifacts included, shows here as on the panel. Outside the circle is black.

```bash
sim/run.sh               # in a window (installs SDL2 with Homebrew if it's missing)
sim/shot.sh              # docs/screens/demo-*.png, headless
sim/shot.sh -n 120       # the first 120 frames, one per 16 ms, into sim/build/frames
```

In the window, the mouse is the finger. The arrow keys dip that edge of the device 5° further (0 levels it), and Shift- or Option-drag tilts it as far as the mouse moves until the button is let go; either way the reading goes to the UI as the motion sensor's does, at 50 Hz through `orb_demo_tick()` into `orb_ui_set_gravity()`. `m` toggles Reduce Motion, `s` saves a screenshot (into the current directory, or `--out DIR`), `q` quits; the title bar shows the tilt.

Headless, time is virtual: it jumps from one LVGL timer to the next, so every run gives the same pixels and a shot lands on its exact moment. For other moments and tilts: `sim/run.sh --headless --tilt 1200=25,0 --shot 2000=tilted.png` (`--help` lists the options).

Unlike the device: pthreads instead of FreeRTOS (still two draw threads), plain C instead of the S3's SIMD fills, LVGL's allocator instead of PSRAM. No live hub, audio or physical power hardware is modeled. `orb_shell_sim` renders synthetic system/power states with the same fonts and status widgets as firmware (see [sim/README.md](sim/README.md)). `sim/conf-diff.sh` checks `sim/lv_conf.h` against the last firmware build's LVGL settings; `ctest --test-dir sim/build` runs the fake board's tests and a headless render.

## Partitions

`firmware/partitions.csv`: two 4 MB OTA app slots with rollback enabled, an 8 MB `assets` partition (fonts, images, sounds, updated separately), about 14.8 MB of `storage`, and a 1 MB `coredump` partition. The crash partition takes space from the end of storage; NVS, OTA and assets offsets stay fixed.

## Developer tools

[docs/devtools.md](docs/devtools.md) covers continuous serial logging with `orb-logd`, safe flash leases, screenshots and touches, verified crash decoding, YAML UI regressions, and Perfetto profiling. Host-side tools live in `devtools/orb_devtools/`, alongside the `hub/orb_hub` package. Local tests use PTYs and fakes and never require a board.

```bash
/opt/homebrew/bin/python3.13 -m venv .venv
.venv/bin/python -m pip install -e '.[test]'
.venv/bin/python -m pytest -q
```

## Private machine settings

Use environment variables for local endpoints and board identities; keep any
shell file exporting them in `.cache/` or another ignored location. Public
examples use synthetic data and do not identify a physical board.

| Variable | Purpose / default |
| --- | --- |
| `ESP_REMOTE_HOST` | SSH USB host; `my-mac.local` (add your SSH user if needed). |
| `ESP_REMOTE_PORT` | Remote serial port; `/dev/cu.usbmodem1101` (select your own port). |
| `HOMERELAY_URL` | Optional Apple Home API override; otherwise Bonjour discovery then `http://127.0.0.1:18796/api/v1`. |
| `ORB_DEV_BOARD` | Development board MAC/device ID for partition migration; required for execution unless `--board` is given. |
| `ORB_DEV_DEVICE_ID` | Development device ID for speed A/B and sound validation; default is the synthetic `orb-020000abcdef`. |
| `ORB_DEV_REMOTE_HOST` | Expected USB host for the restricted speed-profile harness; `my-mac.local`. Set it alongside `ESP_REMOTE_HOST`. |
| `ORB_LAUNCHD_PREFIX` | Generated hub LaunchAgent label prefix; `org.orb`. See [launchd setup](hub/README.md#running-under-launchd). |
| `ESP_REMOTE_BACKUP` | Private full-flash backup path on the remote USB host; required for restore. |

## Factory backup and recovery

Before the first flash, save your own full 32 MB factory image with esptool.
It can contain Wi-Fi credentials and other NVS data, so keep it private.
To restore an existing backup on the remote USB host, set
`ESP_REMOTE_BACKUP` to its path and run `scripts/remote.sh restore`.
This overwrites the entire flash; use the image from that board.
