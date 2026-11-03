# orb_shell

The device shell and system recovery UI share the firmware's LVGL/Inter/Framework7
renderer. No shell callback performs service work. `main.c` maps service snapshots,
queues actions, polls the model under the LVGL lock, and applies brightness outside
that lock. Setup, pairing, connection recovery, identify and active OTA retain
precedence over apps. Forget Wi-Fi and Unpair retain explicit confirmations.

```c
/* orb_model_init() before starting producer tasks; UI calls under lv_lock(). */
orb_shell_t *shell = orb_shell_create(lv_screen_active(), queue_action, context);
orb_shell_snapshot_t status = {.battery_pct = -1, .rssi = -127, .brightness = 70};
orb_shell_update(shell, &status);
orb_shell_poll(shell); /* Each UI tick: copies only when model revision changes. */
```

Navigation:

- Face: fixed 96 px clock, short date, Nunito rounded waiting/Mac complications,
  battery/Wi-Fi and Apps. The waiting complication opens a pending approval
  directly; lower bell/Stack controls and the Mac complication provide visible
  alternatives to the Face gestures. See `fonts/README.md` for licensed sources.
- Swipe up/down from Face: Stack/Notifications. Swipe inward from right edge:
  Controls. Swipe inward from left edge: Back. Debounced events enter through
  `orb_shell_button()`: PWR short opens Apps from Face and returns other shell
  pages to Face, PWR double opens Controls, and PWR hold opens the Power sheet.
  In apps, PWR short first offers the game's own pause sheet, falling back to
  the shell's Resume / Exit app sheet. BOOT short is Back, offered to the game
  after dismissing shell sheets/overlays; BOOT hold starts talk and release
  stops capture through the existing voice request queue. A persistent mic indicator stays above
  every surface, including system tasks, while the mic starts or records.
- Apps: a balanced six-icon honeycomb of flat Framework7 icons, with Agents
  captioned once below its icon. AX3 uses the continuous labelled list alternative.
- Agents, Dot, Settings and Notifications use `orb_round_list_apply/update` and
  scaled hit testing over native LVGL momentum scrolling. Peripheral rows become
  title-only previews when metadata would fall below 20 px; unreadable previews
  are hidden and reject taps. The central row retains project and static activity.
  A tappable rim control scrolls by a viewport section, returning to the top at
  the end. Content fades under floating navigation using `orb_scroll_edge_apply`.
- Session shows identity, state and exact pending command. The information action
  opens its full Summary, with the primary Review Command or Reply retained.
  Long prose scrolls inside a safe central chord without cutting or discarding
  the underlying UTF-8 text.
- Approvals retain source/project, exact command and scope beside equal Allow
  and Deny actions on one surface. They have no default, tint, reading barrier
  or automatic dismissal. Additional permissions use the exact hub option label
  in the same scrolling review. Stack's rim control pages cards only; a long
  command offers a distinct expand action to its complete review.
- Choices expose options immediately. Standard material controls have 20 px gaps;
  AX3 keeps the offered cancellation action visible while other options scroll.
  Messages keep Reply stable while their text scrolls.
- Queueing an answer shows Queued and the selected decision. A card removal
  preserves that receipt; the hub result supplies its recorded winning decision,
  accepted/delivered status and generic failure explanation. Recorded delivery
  failures never offer a replay. Only explicit hub cancellation is labelled
  Cancelled. Notifications show these outcomes; Clear All hides notifications
  without discarding pending decision receipts or denying a live approval.
- Settings retain Connection, Display, Accessibility, Updates and About.
  Brightness is deferred through the existing callback. Text Size/Bold Text use
  `orb_fonts`; Reduce Motion/Transparency use `orb_ui`; Contrast raises semantic
  colors and control rims. Accessibility and AX3 Controls have labelled states
  and rim navigation. Largest replaces the developer-facing AX3 name. Silent is
  local shell state for future feedback; Always On and Play remain explicit
  placeholders.

Each navigation releases the old page's widgets, observers and animations. The
model snapshot/store live in PSRAM on ESP, never the UI-task stack. At most three
visible controls use glass; content is plain and there is no full-screen blur.
Native scroll offsets survive non-navigation model rebuilds. Launches spring
from the selected icon; `orb_glass_materialise` develops glass over 550 ms and uses
an actual 150 ms pixel cross-fade for Reduce Motion. Reduce Transparency retains
opaque controls. The API and its existing renderer limitations are documented in
`orb_ui/README.md`.

App opening presents full-sized destination chrome before preparing content.
Apps builds one icon per refresh, Agents/Dot one row; other short surfaces build
after the first presentation. Recently opened Apps/Agents/Dot/Play can reuse two
bounded PSRAM content snapshots (868,624 RGB565 payload bytes). Live navigation
is excluded from those images. Consequential screens never use frozen previews.
Font/model/status/accessibility changes invalidate or restart preparation, and
Back or system UI can interrupt it. The content spring is deliberately shallow
so 21 px text stays at least 20 px during motion; navigation remains stationary.
`orb_shell_open_metrics()` exposes construction and transition state to the real
device `bench_open` command. `CONFIG_ORB_OPEN_LATENCY_FIXES` defaults on; disabling
it preserves instrumentation for the full-construction/lazy-font A/B control.
See `docs/agent-briefs/wave2/open-latency-results.md` for measurements the lead
must capture; simulator timing does not establish board response latency.

`CONFIG_ORB_MODEL_DEMO` is default-off. Enabling it builds an explicitly labelled
offline developer preview with synthetic sessions/cards; it does not connect,
provision, unpair, install updates or send test answers to the real hub. Normal
paired builds never silently load fixtures. The simulator explicitly loads them.

The link stream must feed `orb_model_upsert_session/card`, cancellation and expiry
from the shared v1.1 contract and drain `orb_model_take_answer()` with its transport
acknowledgement/retry policy. This shell stream does not change that wire protocol.
The queued receipt is local state, not proof of hub acceptance. Card/session
focus sets the voice target. The Voice surface queues mic press/release, Stop,
Cancel and explicit Send commands and renders every voice phase. Transcripts
remain complete and scrollable; no decision or confirmation expires. Errors
retain unconfirmed text, and ambiguous messages never resubmit automatically.
Model online/synced and can_message gate actions, including Send. Voice level
changes update the meter without rebuilding the page. Pressed mic objects stay
alive through recording revisions so release can always queue Stop. Direct
voice delivery receipts are published atomically by the model even when UI
polling coalesces SENDING and IDLE.

Build and verification are in `sim/README.md`; screenshots and the independent
round 3 per-surface gate reviews are in `docs/screens/shell/ROUND3.md`,
`ROUND3-REVIEW.md` and `ROUND3-LAYOUT.md`. The older `REVIEW.md` is historical. No board was used.

Page rebuilds are coalesced through `lv_async_call`: no input or font observer
deletes the page while LVGL is traversing its bubbling event lists. Pending
rebuilds are cancelled when the workspace dies. Swipes suppress a subsequent
click on the control beneath them. ASan/UBSan covers these navigation paths.

The Smart Stack derives its bounded order from current facts: unanswered cards
in wire priority order; a running/finished timer, running stopwatch, due reminder
or alarm today; capability-backed Now Playing; reachable lights known to be on;
relevant Claude/Codex usage; and a quiet local date/next alarm. Home rises above
media from 21:00 through 04:59. A paused utility stays below Home and media.
Usage requires an enabled provider, a current observation and known source,
and appears at 70% used or within 30 minutes of its reset. Stale, unknown,
offline and already-reset observations do not claim current pressure. With no
known date, local activity or supported telemetry the Stack retains its empty
state. Neighbor previews describe only actual Stack entries. The selector owns
no state or allocations and does not change the device/hub protocol.
