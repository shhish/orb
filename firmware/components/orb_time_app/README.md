# Time

Timer, Stopwatch, repeating Alarm and Reminders, registered as `time` / **Time**
with the teal Time identity. Timer and Stopwatch work offline; alarms and
reminders use the system wall clock supplied by the hub or SNTP.

## Ownership

`../orb_time/orb_time.c` is the device-lifetime service, independent of LVGL and
app admission. Its public domain/voice headers and pure calendar/record helpers
live in that LVGL-free component. `orb_time_app` retains the view and the small
registry/poll provider in its local `orb_time.c`; explicit archive linkage keeps
the provider active after the split.
`orb_apps_poll()` invokes its utility hook even when another app is active or
the screen is asleep. Leaving Time never pauses it. The shell owns app entry,
the Apps iris, Face complication and Smart Stack receipt. The view owns no
deadline or background worker.

All service APIs are thread-safe. Commands and snapshots take explicit monotonic
microseconds. ESP critical sections protect only the static model; civil-time
conversion and sound run outside them. Tests inject clocks and use no live state.

Presentation sampling stays at most every 250 ms. Only differing labels and rim
bands are damaged. The view has one paused page-transition timer. Kit switches
own their finite transition and kit lists prepare entering rows outside scroll
and draw callbacks; resting, paused and finished screens generate no idle frames.

## Interaction

- Swipe horizontally between Timer, Stopwatch, Alarm and Reminders. The bottom arc's dots
  are also 46 px tap targets. The page follows the finger and settles on a
  critically damped spring; a settling page can be grabbed again. Reduce Motion
  uses a 150 ms cross-fade. Left-rim inward Back and PWR are system navigation.
- Timer and Alarm: digits rest white. Tap a digit group to target it (teal plus
  an underline, so focus never rests on colour alone), then turn the rim; a
  rim turn marks its group the same way. Tapping the targeted group adds one. There are no field tabs, +/- controls or navigation chevrons. Relative rim capture waits for tangential motion
  so touching the rim cannot jump a value or consume radial system Back.
- Timer: one second through 24 hours. The ring is the time left: full while
  set, emptying clockwise from 12 o'clock over the kit's 18% accent track. Below an hour, Timer
  shows `M:SS` or `MM:SS` (`5:00`, `59:59`); at an hour it shows
  `H:MM:SS` (`1:00:00`). Separators are ASCII colons and the main counters
  never show hundredths. Timer starts with minutes selected and no redundant
  hour group. Turning the minute rim beyond `59:59`, or tapping the selected
  minute group to carry past it, reveals hours naturally. Every readout sizes
  its leading group to the digits it has and centres as one string, so a
  countdown that drops below an hour (or below ten minutes) re-centres once.
- The native 96 px tabular clock role is retained for ordinary M:SS values.
  Wider H:MM:SS rows fit against the strictest visible digit-row chord measured
  by `orb_chord_width()`, inset to the ring's inner edge plus 16 px. Returning
  below an hour restores the native hero. Civil AM/PM belongs to the same
  measured line; a ringing alarm uses an intact static line without a raster.
- Stopwatch: Start/Pause/Resume, Lap while running and Reset while paused.
  Counter, pinned header and laps share one precision, `MM:SS.hh` below an
  hour and `H:MM:SS.hh` after (`orb_time_format_stopwatch`). The newest split
  is a centred capsule in the caption slot that opens the laps; scroll with a
  finger, the rim or that capsule. Laps
  are flat rows curved, scaled and faded at the rims by
  `orb_kit_list_apply()`: count above its tabular split with hundredths in the
  bounded title/subtitle lanes of a canonical 72 px row with a 40 px leading disc.
  Rows belong to the actual scroll viewport, so the kit retains only visible
  row sources and admits entrants from its preparation timer. The ordinary
  counter content scrolls in the same clipped viewport without becoming a row
  raster; the app's scroll callback allocates nothing.
  Up/down marks identify fastest/slowest alongside teal/red, keeping the two-accent HIG limit; equal splits receive no false best/worst mark. The newest 16
  splits are retained. As the controls scroll away, their glass dematerialises
  and their captions fade completely before viewport clipping. A compact
  curved Stopwatch title and secondary live elapsed time replace the counter.
  The Timer's track ring sweeps each elapsed minute from the service snapshot
  and stops while paused. There is no bottom footer label.
- Timer quick presets: tap Quick timers while idle for 1, 3, 5, 10 and 25 minutes. Last used appears after a timer has actually started. Choosing a preset edits the duration; Start remains explicit.
- Alarm options: tap Options. Select Repeat or Snooze and turn the rim or twist
  to adjust; at AX3 values sit under their labels. Custom days opens a seven-day
  checklist: an empty circle is off, a check is on. BOOT/Close returns through
  editors before leaving the app. Time toggles 12/24-hour format.
- Alarm: one time, weekdays, weekends, every day or selected custom weekdays.
  Recurrence advances civil calendar days, never fixed 24-hour intervals. An
  unavailable spring time moves forward through the gap (02:30 → 03:30); a
  repeated fall hour rings at its first occurrence only. A correction back
  across the most recently fired civil day does not ring that day again;
  corrections to earlier dates recompute the next matching local occurrence.
  A forward clock jump rings one overdue occurrence and schedules the next
  future matching day.
  Snooze uses monotonic time while running, with a configurable 1–60-minute
  default (9 minutes initially). Without a valid wall clock, a new alarm cannot
  arm; timers and the stopwatch keep working.
- Clock text adopts the Mac owner's hour cycle from optional `hello.hour24` until an explicit local override is selected. It shares a durable 12/24-hour preference across Time and the shell's
  Time receipts. AM/PM markers and custom weekday abbreviations follow the
  firmware's supported C locale (English); civil-time conversion follows the system timezone. No API
  changes the owner's system clock.
- Reminders has an offline empty state and a list of eight entries, ordered by due time. Tap a row to review its full title, due time, Done and In 1 hour actions; long titles scroll without moving those actions. Due entries latch until
  Done or Snooze. Snooze starts at the current time, so an overdue reminder does
  not immediately ring again. Done entries provide reusable capacity. Titles
  are bounded to 63 UTF-8 bytes at a character boundary; malformed input is
  sanitized before rendering or persistence. The explicit local voice target
  calls `orb_time_reminder_from_voice()` and then `orb_time_reminder_set()`;
  unsupported or ambiguous commands return a visible reason.
  Hold BOOT while Time is open, review the transcript and tap Save. Supported
  examples: “remind me in five minutes to take a break”, “remind me in an hour
  to call home”, and “remind me at 6:30 PM to lock up”. Numeric relative seconds,
  minutes, hours and days are bounded to 365 days; words one through twelve and
  a/an are accepted. A 24-hour preference also accepts bare `H:MM`; a 12-hour
  preference requires AM/PM for ambiguous hours. Voice needs the Mac's cached
  transcriber; saving, persistence and due handling are local. Time never
  auto-sends or forwards a reminder transcript to an agent or Dot.

Every control, title, ring, row and indicator comes from `orb_kit.h`. Glyph
actions are 80 px, sheet Close is a 56 px mini, and focus adds the shared ring
without scaling. Time uses one teal identity and at most one tinted primary;
other actions use neutral glass. Reminder and ringing receipts use 64 px
primary and 52 px secondary capsules. Rim titles use baseline 176 inside full
rings and 200 elsewhere; dots remain inside those rings and scroll arcs stay
outside the rows at radius 224. Semantic action names remain attached to icon
controls. Reduce Transparency and Reduce Motion use the shared UI policy.

A ringing alarm, a finished timer and a due reminder share one modal alert:
full rim ring, inset title, readout or reminder body, a neutral capsule
(Snooze, Repeat, In 1 hour) above the tinted six o'clock action (Stop, Stop,
Done), no page dots and no page swipe. A failed save shows "Couldn't save"
under the rim title with a Retry capsule; Alarm keeps Options beside it.

Timer and Alarm finishes remain latched until explicit Stop. Their sampled
sound loops fade up over 20 seconds through `orb_sound`; timers and alarms bypass
Silent/quiet hours. Due reminders use the needs-you cue under normal sound
policy. A suppressed cue is never replayed. Face/Stack preserve the receipt.

## Persistence and memory

Boot calls `orb_time_init()` after app storage mounts. The model performs no heap
allocation and has no background task. Commands only mutate a bounded static
model. `orb_time_persist_service()` runs on main, outside the LVGL lock, and
atomically replaces `time/state-v1` after 500 ms without a new durable change.
A fixed 684-byte wire record contains recurrence, alarm/snooze state, snooze
length, the clock format, last-used timer and all reminder entries. Explicit
field validation and a checksum reject damaged or future-version records without
erasing them. The write buffer is bounded stack storage, not a retained view
allocation. Timer/stopwatch running time is deliberately not resumed after a
reboot; the last-used timer duration is restored idle.

Save failures remain visible. A new configuration change queues another save;
`orb_time_persist_retry()` retries an idle failure immediately. There is no busy
retry loop on damaged/unavailable storage. A snooze survives reboot through its
saved wall deadline and waits for a valid clock sample; without a hardware RTC,
an offline reboot cannot recover elapsed wall time. Each view holds only the
small snapshot; `orb_time_reminder_get()` reads an outstanding entry by rank
instead of allocating another eight-entry array.

## Validation and screenshots

```sh
make -C firmware/components/orb_time_app/tests/host -j4 test
make -C firmware/components/orb_rim/tests/host -j4 test
cmake --build sim/build -j4
ctest --test-dir sim/build -j4 --output-on-failure
IDF_PY_BUILD_JOBS=4 scripts/idf.sh build
swift -module-cache-path /tmp/orb-time-swift-cache sim/time_contact.swift \
  sim/build/time-default sim/build/time-ax3-bold-rt sim/build/time-ax3-bold-rt-rm
```

`orb_time_sim OUTDIR [ax3 bold rt rm]` uses real-pointer interactions and captures
states and start/pause/finish/lap/page/rim sequences. `orb_time_shell_sim` checks
Apps, system exits, Face, Stack and background completion. The tests compare
retained pixels with frozen full redraws and check actual ring/list ink,
tabular advances, geometry and idle/running frame counts.
See [Time polish review](../../../sim/TIME_POLISH_REVIEW.md) for current screenshots, allocation measurements, validation and board gates; [Time results](../../../docs/TIME_RESULTS.md) records the earlier implementation.
