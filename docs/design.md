# Base OS design

Working codename **Orb** (rename freely). Status: design, with the board layer, the renderer (layer caching, Liquid Glass) and the Sessions demo running on the device (see Rendering). The full HIG-derived spec is in [hig.md](hig.md).

## What it is

A small round device that is a window onto the Mac mini. The Mac mini (the **hub**) does the real work: it runs the Claude Code and Codex sessions, transcribes speech, holds secrets and serves firmware. The device renders a native, Apple-quality UI, captures voice and touch, and stays thin. Two devices exist; both run the same OS and the hub gives each its own role.

```
 ┌───────────── Orb (ESP32-S3) ─────────────┐        Wi-Fi (LAN)        ┌──────────── Hub (Mac mini) ────────────┐
 │ Apps: Face · Agents · Voice · Play · …   │                           │ Device registry, pairing, roles        │
 │ Shell + UI kit (LVGL 9, design system)   │ ◀── pinned-TLS WebSocket ▶ │ Agents: Claude Code adapter, Codex     │
 │ Services: power · net · link · ota ·     │     WebSocket: JSON       │ Speech: mlx-whisper large-v3-turbo     │
 │   audio · input · notify · settings      │     + PCM audio frames    │ Firmware feed, signing, OTA rollout    │
 │ Board layer (1.75C today)                │                           │ Providers later: media, home, weather  │
 └──────────────────────────────────────────┘                           └────────────────────────────────────────┘
```

## Hardware (verified on the device)

| Part | Detail |
| --- | --- |
| SoC | ESP32-S3R8 rev 0.2, 2 × 240 MHz, 512 KB SRAM, 8 MB octal PSRAM, 32 MB QIO flash |
| Display | 1.75" round AMOLED, 466 × 466, CO5300 over QSPI (CS 12, CLK 38, D0–3 4–7, RST 1), tearing-effect line GPIO 13 |
| Touch | CST9217 at I2C 0x5A, INT 11, RST 2 |
| I2C bus | SDA 15, SCL 14; scan finds 0x18 ES8311, 0x34 AXP2101, 0x40 ES7210, 0x5A touch, 0x6B QMI8658 |
| Audio | ES8311 codec (speaker, PA enable GPIO 46) + ES7210 dual-mic ADC, I2S MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10 |
| Power | AXP2101 PMU: Li-ion charging, battery gauge, power key |
| Buttons | PWR (via PMU, also GPIO 3 through an inverter, high while held) and BOOT (GPIO 0) |
| Motion | QMI8658 6-axis IMU |
| Missing | No RTC chip (time comes from SNTP/hub, kept across light sleep), no vibration motor (feedback is sound + motion) |
| Motion sensor | QMI8658 read 0.46 g lying flat with ±4 g set: state left by the previous firmware (the power chip keeps the sensor powered across MCU resets). A soft reset at start fixes it (0.93 g); its axes are the screen's turned half a turn about z |

## Device OS layers

1. **Board** — one `board_t` interface (display start/lock, brightness, panel sleep, audio init, buttons, IMU, power readings, power off), modelled on Muse's `muse_board_t`. The 1.75C is the only implementation; a second board is a new file, not a fork.
2. **Services** — long-lived tasks with small C APIs and an event bus:
   - `power`: active → dimmed → always-on (black, low-brightness face) → light sleep; wake on touch, button, pick-up; battery and charge state.
   - `net`: Wi-Fi station, reconnect with backoff, mDNS discovery of the hub.
   - `link`: one encrypted connection to the hub; request/response, subscriptions, binary audio.
   - `ota`: download, verify, switch slot, self-test, roll back.
   - `audio`: push-to-talk capture (16 kHz mono), playback of UI sounds and speech; echo cancellation later via esp-sr.
   - `input`: touch gestures, button chords, IMU gestures (pick-up, face-down, shake, tilt).
   - `notify`: queue of alerts from the hub with priority, coalescing and quiet hours.
   - `settings`, `time`, `log` (logs stream to the hub, not just USB).
3. **Shell + UI kit** — navigation, system surfaces and the component library (see Design language).
4. **Apps** — compiled-in modules registered with a manifest (id, name, icon, entry, needs). The hub can also push **cards**: small declarative views (title, rows, actions) that the UI kit renders natively, so new glanceable things don't need a firmware release.

Threading: LVGL and the shell on core 1; Wi-Fi, link and audio on core 0. Draw buffers in internal DMA RAM, full-frame buffers and images in PSRAM, flushed in bands synced to the panel's tearing-effect signal (as Muse does) for tear-free 60 fps motion.

## Design language

The bar: someone picks it up and assumes Apple made it. The detailed, sourced rules (units, type scale, colours, springs, glass parameters, components) are in [hig.md](hig.md); this section is the summary.

- **Round first.** This is not a rectangle with the corners cut off. Layouts are radial: content sits in concentric rings and the inscribed circle, controls hug the rim (toolbar buttons at 10 and 2 o'clock, one primary action at 6), lists curve with the bezel (rows shrink and fade towards top and bottom, like a watchOS list bent to a circle), page dots follow the rim, progress is a ring, and every shape's corner radius is concentric with the bezel. Text never runs into the curve.
- **Two layers, as Apple defines Liquid Glass.** Content (sessions, text, artwork) fills the canvas edge to edge; controls and navigation float above it on Liquid Glass. Glass is never used for content and never stacked on glass.
- **Liquid Glass material.** Backdrop blur of what's beneath, a lift over true black (about systemGray6), a hairline rim, and a specular highlight that slides around the rim as the device tilts (the motion sensor drives it). Regular glass for controls, tinted glass only for the screen's one primary action, a darkening layer under clear glass over bright content, scroll-edge fades instead of dividers. Reduce Transparency swaps the material for an opaque surface.
- **Parallax.** The motion sensor shifts layers by depth, as on iOS: wallpaper and content furthest back move most, glass and text least, a few pixels in total (content 8 px, controls 2 px, against the pose the device has rested in lately, so a steady pose looks centred). Reduce Motion turns it off.
- **Canvas.** True black (pixels off on AMOLED). No gradients or glows anywhere, including touch feedback; depth comes from the glass and real content. Press response uses geometry and a bounded material/rim change.
- **Units.** 1 pt = 1.634 px (466 px over 1.75 in is 266 ppi; Apple's point is 1/163 in), so a 44 pt target is as big under the finger here as on an iPhone: 72 px. Minimum 28 pt (46 px).
- **Type.** SF Pro and SF Symbols are licensed for Apple platforms only, so: **Inter** with SF-like tracking, and an open icon set redrawn at SF Symbols weights. watchOS text styles converted to px (body 28 px, headline 28 px semibold, large title 65 px).
- **Colour.** Apple's dark-mode system palette as re-tuned for Liquid Glass (blue #0091FF, orange #FF9230, red #FF4245, green #30D158, …), used sparingly for meaning. Ordinary task views use at most two accents; launcher identity discs and actual Home accessory-state colours are explicit exceptions. Flat discs use adaptive black/white glyph ink for contrast. Labels on tinted glass are semibold or heavier; at most one glass action is tinted. `hig.md` §5 governs these exceptions.
- **Motion.** Springs (SwiftUI's default: response 0.55 s, damping 1.0; brisk 0.15 bounce), interruptible and following the finger; zoom-from-icon launches; glass shapes morph rather than cut. Reduce Motion swaps springs for cross-fades.
- **Feedback.** No Taptic Engine, so short quiet clicks and ticks (silent face-down or in quiet hours) plus a visual press response (glass swells and brightens).
- **Navigation.**
  - **Face** is home: time, date and complications around the rim (agents needing you, next timer, battery).
  - Swipe up: **Smart Stack** of live cards. Press PWR: **Apps**. Hold PWR: power menu.
  - Swipe down: **Notifications**. Swipe in from the right: **Control Center**. Swipe from the left edge: back.
  - Hold BOOT anywhere: **talk**, to whatever is in focus.
  - Pick up to wake; face-down to silence and sleep.
- **Accessibility.** Larger text, bold text, increased contrast, Reduce Motion, Reduce Transparency; nothing conveyed by colour alone.

## Typography

`firmware/components/orb_fonts` holds the type and icons; its [README](../firmware/components/orb_fonts/README.md) has the details and [screens/fonts](screens/fonts) the previews.

- Apple's ten watchOS text styles in Inter (continuous optical sizes, Apple's tracking and leading), at three Dynamic Type sizes (Default = xLarge, Body 28 px; Larger = xxxLarge, 31 px; AX3, 38 px) and with Bold Text (every weight one step heavier). Each style has its emphasized weight and a tabular-figures variant. Labels styled through `orb_text_apply()` follow the setting by themselves.
- About 100 icons from Framework7 Icons 5.0.5, offset to match the stem weight of the text they sit next to and sized like SF Symbols, usable inline in any label (`ORB_ICON_MIC " Listening"`). A small documented set retains Phosphor artwork where Framework7 has no equivalent. Existing icon names and codepoints stay stable.
- Live battery and Wi-Fi indicators come from `orb_status`: LVGL geometry with proportional charge, charging and low-power colours, optional tabular percentages, RSSI bars and a disconnected state. They follow the text style and baseline.
- Two clock faces (140 and 96 px) with tabular figures.
- Prerendered at 4 bpp from pinned sources by `scripts/fonts.sh`. Normal faces at every size live in a 5,624,460-byte font pack inside signed assets. The app retains 111,893 bytes of rescue Inter 28 / Framework7 28 and compact status figures. Assets are verified before use, read into PSRAM a font at a time, and checked against the app's generated pack identity. Missing, incompatible or corrupt assets leave recovery text/icons available.

## Rendering

Measured on the device (ESP32-S3 at 240 MHz, 8 MB octal PSRAM at 80 MHz):

| | |
| --- | --- |
| Full-screen write to the panel, QSPI 40 MHz (Waveshare default) | 26 ms |
| Same at 80 MHz through esp_lcd | 13.7 ms, faster than the panel's 16.8 ms scan |
| Same, circle only, one queued chain per band (Orb) | 10.7 ms for the 376 KB the circle needs |
| Full-screen fill into PSRAM (SIMD or C) | 8.4–9.0 ms |
| Full-screen fill into internal RAM (SIMD) | 0.17 ms |
| Reading PSRAM (SIMD copy, uncached) | ~86 MB/s: a 310 × 104 card in 0.74 ms |

So nothing is drawn in PSRAM. LVGL 9.6 (a local fork, `firmware/third_party/lvgl__lvgl`, changes in `ORB_PATCHES.md`) renders in bands of up to 84 rows into one internal-RAM buffer; LVGL waits for DMA completion before drawing into it again, and each band is held until its write won't cross the panel's scan, so nothing tears. Bands cover only the columns the round panel shows. A glass surface always lands whole in one band, as its blur reads all of it.

**Layer caching**, as Core Animation's `shouldRasterize` (`lv_obj_set_raster`): a widget and its children are drawn once into a bitmap in PSRAM and copied to the screen frame after frame. Moving it keeps the bitmap; a change inside it (style, text, a child) redraws it the next time it's on screen. The bitmap is split into an opaque core in the panel's format, a plain SIMD copy, and small blended edges (A8 with a color for text, ARGB8888 for the rest); a widget that always sits on the black canvas is drawn over it once and is opaque throughout. Liquid Glass is the other half: LVGL's blur (averaged downsampling, 8-bit filtering, bilinear reconstruction and anti-aliased coverage) plus two 8-bit masks drawn from signed distance fields once per size (shadow with darkened edge, and the rim, whose highlight is relit only on its own pixels).

Where the time went (the Sessions demo: two glass circles and a capsule over ten rows), and what moved it:

- Before, every frame drew everything; LVGL's profiler put ~40 % of the draw threads' time in the glass's box shadows (recomputed for every band and tile) and most of the rest in rounded fills, borders, arcs and text.
- Rows, heading and labels became raster caches: copies. That only paid once LVGL's image decoder, which took a global lock for every image the two draw threads drew, was bypassed for ready-made bitmaps (fork).
- Glass: SDF masks instead of LVGL's box shadows, borders and arcs, and the tint mixed in by the blur pass itself.
- One render tile per band (tiles cut glass and measured no faster), touch read off the UI task, motion handed to the UI without the LVGL lock.
- The wire: one queued chain per band instead of esp_lcd's blocking command transactions (~45 µs each, seven per band): 15.0 → 10.7 ms a full frame.
- Internal RAM: 84-row bands instead of 104 (a glass control still fits one), 4 KB draw-thread stacks, no Waveshare BSP.

Now, measured with `scripts/dev-flash-monitor.sh 44 "main:"`:

| | Before (8c6df88) | Now |
| --- | --- | --- |
| Full-screen scroll with full glass | 26–28 fps, frame 31–32 ms | 51–52 fps at speed (44 where the eased scroll slows), frame 18.5–19 ms |
| Glass only (light sweeping every frame, press) | — | 61–62 fps, frame 9.0 ms |
| Wire per full frame | 14.0 ms (424 KB) | 10.7 ms (376 KB) |
| Torn bands | 0–2 per 2 s | 0–1 per 2 s |
| Internal RAM free | 78 KB with the demo (88 KB after display init) | 109 KB with the demo (125 KB after display init) |

Before the smooth-blur and round-row refinements, a full-screen frame was ~13 ms of drawing (glass blur ~2.5 ms, glass masks ~4 ms, row copies ~3 ms) against ~11 ms of wire; with two band buffers the two only partly overlap: bands with glass take longer to draw than to send and the rest the other way round, which costs ~4.5 ms a frame. A third buffer would hide most of it but costs 78 KB of internal RAM that networking needs, as larger bands would; that, not the CPU, is what stands between this and 60 fps full-screen.

## Hub (Mac mini)

A Python asyncio service under launchd, laid out like `xteink-companion`: its own venv, logs in `~/Library/Logs/orb-hub/`, data in `~/.local/share/orb-hub/`.

- **Devices.** Registry with id (MAC), name, role, update channel, last seen. First pairing: the device shows a 6-digit code, you confirm on the Mac (`orb-hub pair 123456`), the hub issues a token and the device pins the hub's certificate.
- **Wi-Fi setup.** Over BLE from the Mac mini itself (no phone app): the hub scans for an unprovisioned Orb, sends the home Wi-Fi credentials, and pairing follows on Wi-Fi.
- **Speech.** Push-to-talk audio streams to the hub and is transcribed with `mlx-whisper` using the `whisper-large-v3-turbo` model already cached on the Mac mini. The transcript is shown on the device for a second to confirm or cancel before it is sent.
- **Firmware feed.** Signs and publishes builds, tracks which device runs what, and pushes "update available".

## Agents integration

What Claude Code and Codex actually allow today (researched 2026-10-26):

| | Claude Code (desktop Code tab + CLI) | Codex |
| --- | --- | --- |
| Live session list and status | `claude agents --json` lists every session (desktop ones included) with busy / waiting / idle and why it's waiting; plus HTTP hooks (SessionStart/End, Notification, Stop, UserPromptSubmit) pushed to the hub | Hub-run sessions: full events from `codex app-server`. ChatGPT-app sessions: read-only, by tailing `~/.codex/sessions` rollouts |
| Approve / deny | **Yes, for all sessions.** A blocking HTTP `PermissionRequest` hook waits for the device's answer; first answer wins, so the Mac prompt still works too | Hub-run sessions: yes, via app-server approval requests. ChatGPT-app sessions: `PermissionRequest` hooks, unverified there |
| Send a message (voice) | Sessions the hub starts (Agent SDK / background sessions): fully supported. Sessions already open in the desktop app: only via the session's inbox socket, which is undocumented and arrives as a cross-session message | Hub-run sessions: `turn/start` when idle, `turn/steer` mid-turn. ChatGPT-app sessions: `codex queue`, unverified |

So the device can watch and approve everything you already run, and can talk to any session the hub runs. Sending voice into a session you started in the desktop app is the one gap; it can be tried behind an "experimental" flag.

Notes for setup: `~/.claude/settings.json` has no hooks today, so adding the hub's hooks changes nothing else. Codex's single `notify` slot is already used by `SkyComputerUseClient`, so the hub must chain to it rather than replace it. Codex isn't on PATH; the CLI ships inside ChatGPT.app.

Privacy: the device receives session titles, state, the pending tool call and a short summary of the last reply, never full transcripts or file contents.

## Protocol

Specified in [protocol.md](protocol.md). In short:
- **Discovery:** the hub advertises `_orb-hub._tcp` over mDNS.
- **Transport:** a WebSocket over TLS pinned to the hub's self-signed certificate. This replaces the Noise plan: it uses mature libraries on both sides (esp-tls/mbedTLS, Python `ssl`) and needs no custom crypto.
- **Pairing:** the device shows a 6-digit code derived from the certificate fingerprint and both sides' nonces; `orb-hub pair <code>` confirms it and issues the device token.
- **Messages:** JSON request/response/event envelopes.
- **Audio:** binary frames (v2).

## OTA and releases

- **Partitions** (32 MiB flash; all ends exclusive):

  | Partition | Offset | Size | End |
  | --- | --- | --- | --- |
  | NVS, preserved | `0x9000` | `0x6000` | `0xf000` |
  | OTA metadata, unchanged geometry | `0xf000` | `0x2000` | `0x11000` |
  | PHY, preserved | `0x11000` | `0x1000` | `0x12000` |
  | `ota_0` | `0x20000` | 6 MiB | `0x620000` |
  | `ota_1` | `0x620000` | 6 MiB | `0xc20000` |
  | `assets` | `0xc20000` | 8 MiB | `0x1420000` |
  | `storage` (LittleFS, subtype `0x83`) | `0x1420000` | `0xae0000` = 10.875 MiB | `0x1f00000` |
  | `coredump` | `0x1f00000` | 1 MiB | `0x2000000` |

  Arithmetic: `0x20000 + 2*0x600000 + 0x800000 + 0xae0000 + 0x100000 = 0x2000000` (32 MiB). The first `0x20000` includes bootloader, table, NVS, OTA metadata, PHY and alignment gaps. A table relocation requires the lead's USB [migration procedure](storage-migration.md); app OTA never changes it.
- **Signing:** apps are signed (Secure Boot v2 format, RSA-3072) with a key kept in the Mac mini's Keychain, never in the repo (Muse commits a dev key, so don't reuse theirs). Verification runs on update (`SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`), against the key in the running image's own signature block, so USB-flashed images are signed too (`scripts/sign-app.sh`). Secure Boot and flash encryption burn eFuses irreversibly, so they stay off for now.
- **Flow:** `idf.py build` (unsigned) → `orb-hub release --channel dev` signs and publishes → the hub tells the devices on that channel → the device downloads from the hub over the link, checks the signature and SHA-256, switches slot and reboots → the new image must bring up the UI and reconnect to the hub within 60 s to mark itself valid, otherwise the bootloader rolls back.
- **When:** immediately on the dev channel; otherwise when idle and charging, or on a tap.
- **Channels:** per device, so one Orb can run dev builds while the other stays stable.
- **Recovery:** USB flashing (`scripts/remote.sh`) always remains, and the original Waveshare image is backed up.
- **Versions:** semver plus git SHA in the app descriptor, shown in Settings → About.
- **Signed assets:** `ORBASSET` format 1 is a bounded 64-byte header (kind, positive content version, payload/signed lengths, payload SHA-256), font pack and erased padding to 4 KiB, followed by an RSA-3072 Secure Boot v2 signature sector. The device hashes with bounded partition reads, then uses IDF's running-app/eFuse trust and RSA verification before checking payload SHA-256 and pack identity. Partition reads are essential: the pack crosses physical 16 MiB, outside this board's 24-bit flash mmap range. Boot verification uses a strictly PSRAM 8 KiB worker stack and 4 KiB signature copy; the caller joins/deletes it before display/network startup. No unsigned assets flash target exists. `scripts/fonts.sh assets --version 1` creates a signed image; `orb-hub release <image> --assets --channel dev` publishes its independent channel. A device advertises its verified assets version separately from its app version.
- **Assets OTA:** verified single slot with built-in rescue coverage. The updater serializes against flash reads, disables new pack-font loads before erasing, verifies the complete download and signature, and reboots to activate it. Cached font blocks remain owned in PSRAM; reads after an update attempt use rescue until reboot. A failed verification invalidates the first sector. A power cut during erase/write leaves the unchanged app bootable with rescue fonts; the hub can retry assets. App boot selection, probation and rollback metadata remain independent.
- **Offline app storage:** `orb_fs` pins `joltwallet/littlefs` 1.22.3 (MIT wrapper, BSD-3-Clause LittleFS), mounts `/storage` at boot, and formats only completely erased media. Nonempty mount failures preserve data. LittleFS requires PSRAM with no internal allocation fallback; its core caches use 1,152 bytes at mount/format and 512 more per open file, independent of partition size (wrapper/VFS/flash bookkeeping is additional). App IDs and keys cannot contain paths; eight namespaces, 32 files/app, 64 KiB/file, 256 KiB/app and a 128 KiB filesystem reserve bound use. Writes stage, fsync, close and atomically rename; metadata wear levelling remains enabled (512 block cycles).
- **Offline apps:** `orb_apps` registers up to eight compiled manifests (id/name/Framework7 icon/flat palette colour/entry/capabilities). Main owns enter/exit/pause/tick and polls PWR independently: holding it for 1.2 seconds exits even a paused app. Marble is an allocation-free tilt controller plus snapshot and visible-control nudge API; the shell may launch/render it later after its design gate. It creates no UI surface in this stream.

## Developer workflow

- **Desktop simulator** (`sim/`, see the README). The UI kit and apps build for macOS on the same LVGL fork and settings, rendered in the device's bands into an SDL2 window, with the board faked (mouse as touch, keys and drags as tilt), or headless in virtual time into PNGs for design review (`docs/screens`). UI work happens at desktop speed; connecting it to the real hub comes later. This is the main lever for getting the polish right.
- **Repo layout:**
  ```
  firmware/   ESP-IDF project: components/orb_board, orb_display, orb_ui, orb_demo, orb_lv_simd; third_party/lvgl__lvgl (fork)
  sim/        macOS LVGL build of the UI
  hub/        Python service, launchd plist
  docs/       design.md (this), hig.md (Apple HIG for this device)
  scripts/    idf.sh, remote.sh
  ```

## What to take from Muse and Xiaozhi

- **From Muse Gadgets** (Apache-2.0, keep attribution; it pins ESP-IDF 6.0.1, but lifting files into 6.1 is fine):
  - the 1.75C board file and its button wiring;
  - the AXP2101 driver;
  - duplex ES8311/ES7210 audio setup;
  - the banded, tearing-effect-synced LCD flushing;
  - `noise_core`;
  - their power-management sdkconfig.
- **Not from Muse:** Link, pairing, chat and voice, which are all wired to Meta's servers and token. Nothing here talks to Meta, so the Muse SDK terms (non-commercial, 50 devices) don't apply.
- **From Xiaozhi** (MIT), later: the esp-sr audio front end (echo cancellation, noise suppression, voice activity detection, wake word) and its Opus encoder.

## Milestones

1. **Foundation:** board layer complete (PMU, buttons, IMU, audio) — panel, touch, power chip, buttons and motion sensor done; renderer with layer caching done; desktop simulator (UI only) done; power states; Wi-Fi + BLE setup from the Mac; hub link with pairing; signed OTA with rollback; the simulator talking to the hub. After this, USB is only for recovery.
2. **Shell and design system:** face, apps, Smart Stack, Notifications, Control Center, Settings, UI kit components, type and motion tuned on the device.
3. **Agents:** session list and detail, approvals, push-to-talk replies to hub-run sessions, experimental replies to desktop sessions.
4. **Play and more:** IMU fluid/particle toys and games, second-device role, hub cards for media server health and home control.

## Open decisions

- Codename.
- How sessions you want to talk to are started: from the device/hub (fully controllable), or keep starting them in the desktop app and accept the experimental inbox path for voice.
- Whether the second device runs the same role or a different one (for example a fixed status display).

The historical rendering timings above predate the smoother reconstruction, cached row shrinking and integrated single-buffer memory budget. The integration removed the second 78,288-byte DMA buffer because BLE provisioning could not start alongside it. The final setup-screen smoke is recorded in `HANDOFF.md`; sustained scrolling throughput with Wi-Fi and Bluetooth is not established by those static-screen measurements. Simulator wall time does not establish panel FPS.
