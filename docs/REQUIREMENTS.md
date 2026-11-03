# Orb requirements ledger

Every decision, resource and complaint the owner has given, from 2026-10-26 on, in one place. Agents read this before designing or building anything. If work conflicts with an entry, raise it; don't drift. Status: **done**, **partial**, **open**.

The visual reference is **Orb Design 1.0**, represented by the reference frames in `docs/design/`. Firmware must match it.

## 1. What Orb is

| # | Requirement | Source | Status |
|---|---|---|---|
| 1.1 | A base OS for the Waveshare ESP32-S3-Touch-AMOLED-1.75C (466×466 round AMOLED). The Mac mini is the powerhouse and Orb is a thin client over home Wi-Fi. | 10-26 | done |
| 1.2 | It must feel Apple-made: Apple HIG, the new Liquid Glass feel, and round-first layout. "As if Apple created this device." | 10-26, repeated | partial: Design 1.0 sets the bar |
| 1.3 | Signed OTA updates. Two boards: the dev board gets every test build, and the second board gets only finished software. | 10-26 | done (dev); second board open |
| 1.4 | "Go all in" on everything: fork, modify or rewrite upstream code when it falls short. Use things but twist them our way. Reuse Muse Gadgets (Apache-2.0). esp32-dot is unlicensed, so ideas only. | 10-26 | ongoing |
| 1.5 | Room on the device for offline things: games and utilities. | 10-27 | done (6 MiB slots, 59% free) |

## 2. Agents (Claude Code and Codex on the Mac)

| # | Requirement | Source | Status |
|---|---|---|---|
| 2.1 | **See a session streamed live from the Mac on Orb** (what the agent is doing and saying, as it happens) and **send it a transcribed voice request**. This is the primary agent feature. | 10-26 ("see, or send a voice transcription command for each session"), 10-28 ("best would be...") | partial: live session stream and voice send shipped in 0.5.0; no live partial transcript in protocol v1.1 |
| 2.2 | **Claude and Codex usage** at a glance: limits and how much is left, like Clawdmeter. | 10-28 | done (0.5.0): Usage rings, stale/unknown shown honestly |
| 2.3 | Approvals are secondary: the owner doesn't expect to use them often. Keep them reachable (a waiting card), but don't make them the centre of the Agents place. | 10-28 | design change |
| 2.4 | Both session paths: watch everything already running (desktop Code tab, CLI, Codex app), and start sessions from the hub for full voice control. Desktop sessions only get voice through the experimental inbox socket: test it without touching live sessions. | 10-26 | partial |
| 2.5 | Keep only focus sessions on the device and collapse idle ones (cost). | 10-27 | done |
| 2.6 | Real session titles, not "Claude session". | 10-27 | done |

## 3. Dot (ChatGPT)

| # | Requirement | Source | Status |
|---|---|---|---|
| 3.1 | Talk to ChatGPT Dot from Orb, as rsms/esp32-dot does: Orb as an MCP surface for Dot, with cards, answers and voice. | 10-26 | partial: the tunnel is live, Dot sees the Orb, but events need a plugin rescan |
| 3.2 | "Dots" belong on the Agents side next to Claude and Codex. | 10-28 | done (0.5.0): Dot in Apps and Sessions |

## 4. Home (HomeRelay)

| # | Requirement | Source | Status |
|---|---|---|---|
| 4.1 | A Home app over HomeRelay, round-first, clean and beautiful. | 10-27 | done (0.5.0) |
| 4.2 | **It must be obvious which light and room is which**: full room names and state readable at a glance. Home opens on a stable scrolling room list; one tap opens the named room, with no focus step or hidden room groups. Climate and Scenes use named rows. | 10-28, 11-01 | implemented in `orb/w2-rooms`; 1,265 simulator tests, build and design gates passed; independent physical-panel review pending |
| 4.3 | Temperatures in °F (from the Mac's locale). | 10-27 | done |

## 5. Fun, games and utilities

| # | Requirement | Source | Status |
|---|---|---|---|
| 5.1 | Fun with restraint: accent colours here and there, not a colourful UI. | 10-27 | partial |
| 5.2 | **All the games designed and built to the same bar**, not just Marble: Marble Conservatory, Orbit, Ring Snake, Pop, plus a tilt bead/fluid toy (Johannes Tscharn's "this replaces scrolling") that can settle into the time. | 10-27, 10-28 | done (0.5.0): Play picker plus Marble, Beads, Orbit, Ring Snake, Pop to Design 1.0 |
| 5.3 | Marble tilt must feel right (it was inverted). No on-screen buttons while playing. | 10-28 | done: tilt fixed; no buttons while playing |
| 5.4 | Timers, alarms and reminders (Capy-style) as a Time app. | 10-26 research, 10-27 | Timer/Stopwatch/repeating Alarm/Reminders implemented on `orb/w2-time-polish`; real-board review pending |

## 6. Interaction

| # | Requirement | Source | Status |
|---|---|---|---|
| 6.1 | Use the round display and the small size: gestures (swipes left, right, top, bottom), transitions and something smart and fun. | 10-27 | partial |
| 6.2 | **The physical buttons must do things**: PWR (press, double-press, hold) and BOOT (press, hold to talk). | 10-28 | done: PWR Apps/Face, double Control Center, hold power sheet; BOOT back and hold to talk |
| 6.3 | There's no vibration motor, so soft sound replaces haptics. | 10-26 | done |

## 7. Performance

| # | Requirement | Source | Status |
|---|---|---|---|
| 7.1 | No lag anywhere: app open, scrolling, transitions, input. "Optimise from the core." | 10-27, repeated 10-28 | partial |
| 7.2 | Think out of the box: rewrite core libraries or change the architecture if that's what it takes. Don't hold back. | 10-27, 10-28 | Engine 1.0 planned |
| 7.3 | Smoother scrolling animations. | 10-27 | partial |

## 8. Visual defects the owner saw on the panel (must stay fixed)

- The glass button edges were pixelated (the Reply button). Done.
- The battery and other glyphs didn't look iOS-like. Done.
- The Face: a broken half rim arc, nothing aligned, a tiny clock and mixed fonts (a key-signing regression). Fixes in flight.
- Dark greys lift and tint green on the panel; this needs a calibration pass. Open (docs/ux-feedback.md).

## 10. Sound, voice and the rest of the hardware

Owner, 10-28: "what about sound and speaker? Anything else the device offers that we're not configuring yet?" The audit is docs/research/hardware-audit.md. Designs are the Sound and Senses sheet, plus the SoundSettings, VolumeHUD, SpokenReply, AlarmRinging, NowPlaying, Battery and Onboarding frames.

| # | Requirement | Status |
|---|---|---|
| 10.1 | A sampled, Apple-quality system sound palette (the Sound and Senses table), replacing synthesised sine beeps. The ES8311 limiter and volume are always on, and Silent mutes everything except alarms and timers. | partial: sampled palette and mute rules on the board; audible review pending |
| 10.2 | Spoken replies: Dot's and agents' answers read aloud by the Mac's voice, streamed to the speaker, and stopped by talking over them (echo cancellation) or by a double-tap. | partial: device and hub built; hub flag off until the audible test |
| 10.3 | Volume control from any screen by twist or rim, with an overlay. | open |
| 10.4 | The gyroscope on: twist the Orb lying flat to scroll or set values; steadier tilt in games. | partial: gyro on, per-board bias learned; tune on the enclosure |
| 10.5 | Double-tap on the case (the QMI8658 tap engine) for the screen's safe primary action. NEVER Allow, Send or Unlock. | partial: allowlisted safe actions; tune on the enclosure |
| 10.6 | Both microphones with noise suppression. A wake word stays opt-in and later. | partial: both mics read with noise suppression; recording review pending |
| 10.7 | Now Playing from the Mac as a Smart Stack card, with Mac volume on twist. Later: a Bluetooth media knob when Wi-Fi is down. | open |
| 10.8 | Battery health, temperature and a desk charge limit (AXP2101). | partial: telemetry and desk limit API; no UI yet |
| 10.9 | Always On in the display's 8-colour idle mode with partial refresh, if board power measurements justify it. | to evaluate |
| 10.10 | First-run onboarding teaching the swipes, BOOT and PWR. | open |
| 10.11 | microSD and Bluetooth presence: later, only if they earn it. | later |

## 9. Process (how the work is done)

- Helpers, in this order: Antigravity (reviews and short tasks), Codex (build streams), DeepSeek (cheap reviews, paid), then Claude. Claude handles design, board work and merges, and never compromises quality.
- Set things up without asking. Commands the classifier blocks go to the owner, named plainly.
- **Every change is reviewed as screenshots from the real board before it reaches the owner.** Passing simulator tests is not enough.
- Every agent follows hig.md, Design 1.0 and this ledger. UI work must match the Design 1.0 frames: check it with the screenshot diff tool.

## Resources the owner provided

- X posts (10-26): Robert Bye's web-connected desk device; zathvarun's Capy companion on this exact board (hold to talk, timers, alarms, reminders); Johannes Tscharn's tilt bead simulation; the Muse Gadgets announcement.
- github.com/rsms/esp32-dot (Dot over MCP; ideas only).
- code.claude.com/docs/en/hooks (session and approval events).
- HomeRelay (the owner's Apple Home LAN API on the Mac mini).
- The owner's panel video (IMG_8895.mov) and photos: docs/feedback-*.png.
- Research found from those (10-26): Puckling, Claude Desktop Buddy (and its 1.75C port), Clawdmeter / clawdmeter-plus (usage meters), agent-display, pixel-agents-esp32, agent-buddy, awesome-esp32, Xiaozhi, Home Assistant knobs.
