# Orb Time polish — uncommitted review

Branch: `orb/w2-time-polish`. Nothing committed, signed, flashed, pushed or merged.
No owner configuration, Keychain, launchd service or live agent session changed.

## Changes and purpose

- Added one-time, weekday, weekend, daily and custom-day alarms, with civil-date scheduling across midnight, DST gaps/folds, timezone changes and clock corrections. The secondary line now describes the actual repeat and next occurrence. Configurable snooze uses monotonic elapsed time; its wall deadline can be restored after reboot.
- Added a fourth Time destination for the persisted reminder list, separate empty/future/due states, full-title review, one-hour snooze and Done. Long titles scroll inside their own region while actions remain fixed. Due reminders use `ORB_SOUND_NEEDS_YOU`.
- Made the voice setter reachable through a narrow documented `voice.begin` Time target. The Mac transcribes; the Orb parses only bounded supported phrases after explicit physical Save. Nothing auto-sends or forwards to an agent. Invalid, ambiguous, offline-clock and full-list cases explain the failure. The receipt says “Reminder added”, since flash persistence is debounced rather than already complete.
- Added 1/3/5/10/25-minute timer presets and a real last-used row. Presets never implicitly start a timer. Finished timers remain visible until dismissed and revoke their sound loops on dismiss/reset, including submission races.
- Preserved scrollable stopwatch laps, improved best/worst distinctions with arrows and teal/red, and removed false extrema when splits tie. Teal replaces green to honor HIG's two-accent limit.
- Added alarm options and custom-day editors, correct rotary initial values, nested physical Back, a reversible switch spring reusing the existing timer, and explicit palette tap/open/close/done cues. Removed the invented idle Alarm rim to match Design 1.0.
- Extended the shell's existing alarm interrupt to timer finishes and due reminders: wake from sleep/AOD, pause underlying apps, keep the alert awake, then restore the prior surface. Face-down stops alarms even when the due boundary arrives after the orientation edge. Double-tap safely stops alarms/timers.
- Added optional `hello.hour24`: the hub reads the Mac user's hour cycle once with CoreFoundation. Time uses it until an explicit local override is persisted. Alarm, reminder and shell Time receipts share formatting and use the existing synchronized timezone. Midnight is padded as `00:05` in 24-hour mode.
- Added atomic, checksummed local storage with a 500 ms quiet debounce and explicit failure/retry UI. Boot loads Time after storage mount; main flushes outside LVGL and the model lock. Sanitized UTF-8 titles and stable due ordering tolerate odd data without growing views.

## Screenshots and design gate

[Full evidence index](../docs/screens/time-polish/README.md) contains every surface's comparison path, reference mapping, per-heading gate notes and exact allocation/frame tables. Every reference/render pair was generated with `python3 sim/png_tools.py concat FRAME RENDER OUT` and inspected. Reproduce with `python3 sim/time_polish_sheets.py` after the simulator tests.

- [Default overview](../docs/screens/time-polish/default/contact.png) / [stress cases](../docs/screens/time-polish/default/stress-contact.png)
- [AX3 + Bold + Reduce Transparency overview](../docs/screens/time-polish/ax3-bold-rt/contact.png) / [stress cases](../docs/screens/time-polish/ax3-bold-rt/stress-contact.png)
- [Same settings + Reduce Motion overview](../docs/screens/time-polish/ax3-bold-rt-rm/contact.png) / [stress cases](../docs/screens/time-polish/ax3-bold-rt-rm/stress-contact.png)
- [SDL touch/rim emulator Time capture](../docs/screens/time-polish/emulator-time.png), taken with `ORB_EMU_TIME_SHOT=/tmp/orb-time-emulator.png SDL_VIDEODRIVER=dummy nice -n 10 sim/build/test_emu_flow` (**150 checks passed**).
- [Voice review](../docs/screens/time-polish/voice-review.png), [added receipt](../docs/screens/time-polish/voice-saved.png), [error](../docs/screens/time-polish/voice-error.png)

Simulator gate passes: black canvas; flat content and glass controls; one tinted action; no gradients/glows; circle-safe layouts; shared Inter and Framework7 styles; text/shape as well as color; reversible motion; physical navigation alternatives; readable AX3/Bold/RT/RM states. A second agent independently inspected the screenshots. Lead review on the real panel remains mandatory before merge.

Documented differences: no approved standalone frames exist for the new presets/options/day/list surfaces, so their comparison reference is their parent Time/Alarm/Reminder frame. The fourth dot deliberately exposes Reminders. Available font styles round 20/18/15 px reference text to 21 px; counters use the existing 96 px face rather than the unavailable 120 px reference. No new fonts were allocated.

## Validation

All long build/test jobs ran under actual `nice -n 10`; builds and CTest used four workers.

- `cmake --build sim/build -j4`: passed with Orb warnings treated as errors.
- `ctest --test-dir sim/build -j4 --output-on-failure`: **778/778 passed**, 47.00 s on the final full run. This final run includes the receipt wording refinement and optional emulator capture hook.
- Time UI: **4,450 Default / 4,453 AX3 + Bold + RT / 4,629 with RM** checks; actual pointer/rotary paths, clipped-title ink, retained/full redraw equality, interrupted paging and switch reversal, alarm preemption, long data, all presets and reminder actions.
- Time model ASan/UBSan: **1,997 checks plus 10,000 concurrent invariants**, including DST, backward week/year correction, corrupt/failed storage and reboot restoration.
- Hub clock/focus/voice: **112 passed**. Voice host: **2,322 core / 625 speech / 109 request / 44 callback** checks. Link host ASan/UBSan: **28,946 checks, zero failures**.
- `scripts/idf.sh build`: passed. Unsigned `orb.bin` is **0x2a0000 bytes**; **0x360000 bytes (56%)** of the 6 MiB slot remain free.
- Ruff formatting/checks including ASYNC and `git diff --check`: passed.

## Allocations and frame cost

These are host requested-payload measurements, not board budget or frame-time claims. The model adds no heap/task/timer: its snapshot is 320 bytes (previously 304), reminder slots remain 88 bytes each, and persistence uses a bounded 684-byte stack buffer. The parser and device preference adapter allocate no heap. CoreFoundation releases its temporary locale/formatter references on the Mac; only the boolean result is cached.

A view uses one **808-byte** owner-accounted workspace allocation, plus its LVGL tree; a paging preview temporarily adds another view. The existing paused settle timer now also drives the switch. New live LVGL payloads: presets **6,717–7,378 bytes**, alarm options **40,088**, custom days **41,365–41,369**, empty reminders **3,091**, two-row reminder list **4,644–4,650**, long detail **5,103–5,131**, seven-row list **9,927–9,948**. These replace the active content tree; they do not accumulate one tree per page. The evidence index lists block counts and after-action variants. Timer's existing digit raster/tree is 146,894 bytes. Ringing Alarm avoids that raster.

Settled idle/paused/finished/editor/reminder states produced **zero frames and invalidations over two seconds**. Running Timer and Stopwatch each produced **8 frames over two seconds**; maximum flushed frame payload was 59,188 and 59,292 pixels respectively. No device latency/FPS is claimed.

## Owner and board work still required

1. Lead captures with `scripts/orb-dev.sh shot`, Default and AX3/Bold/RT, independent design review, and approval before merge/release.
2. `scripts/orb-dev.sh heap` for every new view and overlapping alert: owner admission/rejections, internal RAM/PSRAM floors, and main-task stack high-water mark during the 684-byte save. Simulator owner budgets are not enforced.
3. Audible 20-second ramp, needs-you cue, Silent behavior, speaker limiter and stop latency; physical face-down/double-tap/twist tuning; sleep/AOD wake and prior-screen restoration on the enclosure.
4. Reboot/power-cycle repeat days, snooze settings/deadline, hour override, last-used timer and reminders; storage failure/retry; clock resynchronization and DST behavior with device libc. An offline reboot cannot recover elapsed wall time without an RTC.
5. Deploy the matching updated hub and firmware through the lead, then exercise BOOT transcription and supported reminder phrases. No live service was restarted here.
6. Full locale localization is still limited: the device's C locale supplies English weekday/AM/PM strings and UI copy. Mac hour cycle and supported timezone synchronization are implemented; arbitrary localized strings/fonts are not. Hub hour-cycle preferences are cached until the hub restarts.
