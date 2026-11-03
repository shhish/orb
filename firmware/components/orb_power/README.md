# orb_power

Stream C's allocation-free, deterministic power policy and synthetic-trace tests.
The design contract is [power-and-calm.md](../../../docs/power-and-calm.md).
The controller itself does not spawn a task, read hardware, configure wake
GPIOs, suspend Wi-Fi, send a panel command, or call ESP-IDF sleep. The explicitly
called board adapter dispatches existing board brightness commands only at the
consumer's drained display fence.
The firmware shell now consumes this policy in `orb_shell_power.c`; the
companion `sim/power_view.c` remains a separate preview fixture.

## Controller API

One service owner serializes the public `orb_power.h` calls. Allocate an
`orb_power_controller_t`, call `orb_power_init(&power, monotonic_ms)`, and publish
value snapshots to the shell. The board's existing `orb_power_t` remains the PMU
reading; these types intentionally have different names. Supply monotonic
milliseconds, e.g. `esp_timer_get_time() / 1000`; do not use SNTP/wall time.
Backward timestamps are rejected without changing the controller. Equal times
are allowed for independent events, but a duplicate IMU measurement cannot
manufacture a dwell. Init and every automatic wake are redacted.

- `orb_power_wake`: touch/PWR expresses deliberate viewing intent; the first
  contact/press from a dark, dimmed, Always On or redacted surface returns
  `consume_input=true`. Consume its **whole lifetime**, including release, so
  wake never clicks through to an approval or opens Apps. The next deliberate
  awake press may perform its normal action. While owner brightness is zero,
  every touch/PWR remains consumed to prevent invisible click-through, and
  brightness is never forced up. A classifier/test may inject the
  PICKUP reason; production motion input should use `orb_power_imu_sample` so
  it cannot skip rest arming. All automatic sources retain redaction, never
  reset the intentional-activity idle deadline. Automatic attempts while Active
  are rejected except a real attention event replacing a pick-up glance; that
  card gets its full window without replaying the wake transition.
  A pick-up glance is Active for 6 s and Dimmed through 9 s from its wake. It
  returns to Always On only if it came from Always On, the current preference
  and battery tier permit it, and the original idle Sleep deadline has time
  left; otherwise it sleeps. Deliberate touch/PWR converts it into ordinary
  viewing. Attention keeps 20 s Active and Dimmed through 30 s even in Low
  Power; critical still denies it. Attention is limited to one admitted wake
  per 30 s; pick-up has a 10 s cooldown plus the detector's stable-rest re-arm.
  Attention may replace an Active or Dimmed pick-up glance: the preceding untouched
  pick-up counts as ignored, but the attention window itself never counts.
  On battery, three consecutive pick-up glances without deliberate touch/PWR
  suppress pick-up for 5 min. Each further ignored glance doubles suppression
  to 10, 20, 40, then 60 min, capped at 60 min. Window and suppression deadlines
  are monotonic; rejected attempts do not renew them. Deliberate touch/PWR or
  USB plug resets the budget. USB cancels an in-flight glance's eligibility;
  glances begun on USB never count. Attention bypasses and never counts against
  this budget.
- `orb_power_note_activity`: record physical touch on an opaque timer/alarm
  alert without waking or revealing the retained task. It resets the pick-up
  budget, ends automatic bookkeeping and renews ordinary activity. The shell
  excludes synthetic pointers and preserves the existing alert contact fence.
- `orb_power_request_sleep`: explicitly sleeps only the display with the
  bounded calm fade. It preserves work inhibitors and ordinary automatic wake
  admission; Quiet is a separate setting.
- `orb_power_set_quiet(true)`: immediately selects the redacted desk layout;
  `false` expresses deliberate Active intent. Motion wake and Always On are
  independently configurable. The visible controls belong to the shell branch.
- `orb_power_set_preferences`: `always_on_brightness_pct` is the single named
  Always On brightness setting, default 9%, clamped to 1–15%. Active owner
  brightness caps every lit state, including owner zero. There is no ambient
  sensor or proportional brightness adaptation. The board accepts 0–100% and
  maps integer percentages to DCS `0x51` 8-bit levels; this is API support,
  not measured panel luminance or I/O success. Higher sustained brightness
  trades increased power/pixel exposure for glanceability; minute shifting
  does not eliminate AMOLED burn-in. Normal battery idle enters Dimmed at
  20 s, Always On at 30 s and Sleep at 120 s; USB Sleep remains 30 min.
  Disabling Always On sleeps at 30 s. Low-battery tiers retain their shorter
  deadlines.
- `orb_power_set_battery`: pass the actual independent percentage, USB and
  charging facts; `-1` means unknown, not empty. Values below `-1` or above
  `100` are rejected. USB and battery changes never reveal or wake. Critical
  entry darkens immediately; later manual viewing lasts 10 s even during work.
  A known reading ≤2% off USB starts a 60 s monotonic cell-protection countdown.
  3% gauge jitter retains it; ≥4%, USB or unknown clears it and the advisory
  `shutdown_requested` snapshot output. After unknown, a fresh known ≤2%
  reading must start another full countdown. Unknown never starts or completes
  a request. The existing critical tier and `N% · Battery low` presentation
  precede it. The controller never cuts rails: main must fence writes and wire
  actual PMU soft-off. Battery settings retains known percentage, independent
  USB/charging state and validity-checked health/temperature in its existing
  template, without runtime estimates or idle timing copy.
- `orb_power_set_inhibitors`: VOICE/PLAYBACK/OTA/APP pause idle progression and
  give a fresh budget when that work finishes. DMA denies CPU sleep only:
  ordinary drawing must not continuously renew viewing intent. Face-down and
  critical still darken while work continues. No recording, transcript, pending
  answer or app data is discarded by this component. An automatic window may
  finish its ignored-wake accounting while inhibited, but cannot darken active
  work early; release preserves the fresh ordinary idle budget.
- `orb_power_imu_sample`: pass screen-axis g at 20 Hz. Report failed reads as
  nonfinite samples (e.g. `NAN,NAN,NAN`) to break dwell continuity; do not reuse
  the last successful sample with a fresh timestamp. A gap over 250 ms resets
  rest/candidate/dwell state. Invalid data never clears confirmed face-down.
  Rest arms after 800 ms in any orientation, including flat and 30–70° docks.
  A sustained orientation change (25° entry/20° hold for 150 ms) or magnitude
  change (0.18 g entry/0.10 g hold for 200 ms) admits one pick-up only when raw
  and filtered normalized screen z are both ≥0.30 at the end of the dwell.
  This viewing-pose gate leaves arming orientation unrestricted. Vertical
  pocket walk and face-down carry do not qualify; face-up hand carry can be
  indistinguishable from a real lift and may admit one glance. A new rest,
  10 s cooldown and ignored-wake budget bound repeats. Face-down needs raw and filtered
  normalized z ≤−0.8 for 300 ms, independently of the motion-wake preference;
  exit needs z ≥−0.6 for 300 ms and never wakes by itself.
  All host traces are labelled synthetic; real trace provenance is required
  before calling any threshold device validated.

Call `orb_power_tick` at policy deadlines and before using a fresh snapshot.
Event APIs also advance relevant policy, so a touch after a long scheduling gap
is consumed correctly without requiring a preceding tick. Calm wake lifts the
time by 2 px and scales newly appearing attention from 0.9 to 1.0 with opacity on a
critically damped spring, settling within 400 ms. Already-visible attention
interpolates its current size/opacity to avoid a discontinuity. Idle entry fades into the
Always On arrangement over approximately 250 ms; actual panel sleep fades the
safe face to black over 250 ms. Reduce Motion uses only a 150 ms cross-fade. All retained
elements interpolate from their current presentation, including burn-in
offsets; no novelty artwork or attention-line hide/show cut appears.
Transitions are anchored to their absolute deadline: a delayed tick past the
entire interval goes straight to its endpoint. The snapshot's
`next_deadline_ms` is the next policy/cadence endpoint, not the only scheduler
input: while `moment != NONE`, schedule at `min(now+frame_interval_ms,
next_deadline_ms)`. Both ordinary and Reduce Motion moments grant 16 ms frame
opportunities; only the former changes geometry. AOD grants at most one normal
update opportunity per second through `orb_power_frame_due`. Render only if
public time/count/shift actually changed. A bounded entry transition is the
exception to the steady Always On cadence. In the simulator compositor, admitted
attention-presence changes also request 16 ms frames during a bounded 250 ms
reflow (150 ms under Reduce Motion) via `power_view_frame_interval_ms()`.
Combine that request with the controller cadence; the minute burn-in shift
stays discrete throughout. Urgent privacy changes must mask/dark
immediately; they never wait behind a retained old task frame. At rest outside
AOD, the service has no continuous render requirement.

The four AOD offsets derive from monotonic `now_ms / 60000`, so touches and
state transitions cannot pin the same pixels by repeatedly resetting idle.
`revision` identifies presentation-policy changes and is a nonzero safe-frame
token; minute offsets/time/count are also ordinary renderer invalidation inputs.

## Safe display integration

`firmware/main/main.c` enables the LVGL-owned shell controller and feeds
PMU state, actual charging/USB facts, and timestamped IMU samples. The shell
retains a public-only overlay on the display's top layer, above app screens and
system sheets. Its time/count raster caches interpolate without temporary
transform layers. Default Always On follows the Design 1.0 geometry; AX3/Bold
reflows the same structure. The privacy overlay never navigates or modifies a
pending card, transcript, session, or app.

Main observes the exact generation at RENDER_START/REFR_READY, drains the DMA
queue from the next UI turn, checks transfer error counters, and calls
`orb_power_board_apply_snapshot`. A missing/stale generation cannot brighten.
The optional `orb_shell_power_display_ready` handshake also consumes subsequent
contacts/presses until the current frame is acknowledged and allowed to emit.
Owner brightness is stored separately from the dim/AOD panel command, so idle
policy never overwrites the owner's control value.

The controller stays on the serialized UI owner. Main's service task continues
network/OTA/buttons/apps independently; it no longer runs a second raw-button
brightness policy. Explicit shell preferences expose Quiet/Active, Always On,
Always On brightness and motion wake. The following shows the equivalent
service-owner integration contract:

```c
/* Serialized service owner; publish the immutable snapshot to the UI owner. */
orb_power_tick(&power, monotonic_ms);
orb_power_snapshot_t snapshot = orb_power_get_snapshot(&power);
/* UI owner masks private labels immediately and renders this snapshot. */
render_power_snapshot(&snapshot);
/* Forward the exact snapshot token with the rendered frame to the display owner. */
submit_power_frame(snapshot.revision);

/* Sole orb_display owner, after the safe frame's final DMA completion fence.
 * Check the token against the current serialized controller generation. */
orb_power_apply_result_t result = orb_power_board_apply(
    &power, &adapter, &platform_port, frame_revision);
```

The board adapter must run **only on the display owner**, after all pixel DMA
has drained. Main invokes it only from its UI timer after `orb_display_drain`, while the
serialized display owner is outside a refresh; never invoke it from the service
task while `orb_display` can own a transfer. The existing `orb_display_brightness()`
queues brightness onto that same owner; the lead can use that queue as the
integration point. `orb_power_board_apply()` calls the existing
`orb_board_panel_brightness()` API there, never while another owner writes.
That board API returns `void`; successful API dispatch is not measured
brightness or proof an I/O write succeeded.

Initialize the adapter with `orb_power_board_adapter_init`. A missing or stale
safe-frame token makes every new lit generation dark and returns
`WAIT_SAFE_FRAME`, including a privacy-changing dim. An exact token permits
brightness/panel changes. Never acknowledge a token for a frame the shell did
not render; never acknowledge a newer generation using an older frame.
Until the current frame is actually presented and the adapter permits emission,
consume every contact/press, including a second contact during wake or a failed
panel operation. The controller's requested brightness does not prove the
screen is visible.

`platform_port.panel_awake` is optional. Without it, sleep dispatches brightness
zero and returns `PANEL_UNSUPPORTED`; black pixels are **not physical panel
off**. The platform hook must preserve brightness zero on wake and preserve the
acknowledged safe pixels. If RAM cannot accept pixels while asleep, prepare or
wake the panel dark first, render/drain the safe frame, then enable emission.
Whether the CO5300 can do this, and its actual panel-off/resume sequence, are
lead board gates. This component sends no guessed panel opcodes.

`orb_power_board_try_light_sleep` is a separate opt-in platform hook. It denies
sleep unless all of these hold: logical Sleep, completed moment, physical panel
known off, current safe-frame token, no work/DMA inhibitor, a nonzero bounded
sleep interval, and TOUCH/PWR/TIMER/ATTENTION wake readiness. IMU readiness is
also required when motion wake is enabled. **Polling the IMU before sleep does
not establish IMU wake readiness.** The platform must prove an interrupt or
bounded timer scheme and preserve network attention delivery. Defaults deny
CPU sleep. Hook failure is reported; no controller transition assumes success.

## Host verification

```sh
nice make -C firmware/components/orb_power/tests/host -j4 test
```

Clang uses `-Wall -Wextra -Werror -Wshadow -Wconversion`, AddressSanitizer and
UndefinedBehaviorSanitizer. The fake board records only brightness calls; panel
and CPU sleep hooks record dispatch/admission and return deterministic failures.
The tests cover timer boundaries/delayed ticks, consumed wake input, redaction,
frame cadence and minute shifts, charging and all battery hysteresis levels,
glance deadlines and ignored-wake suppression, cell-protection debounce,
critical wake budgets, work versus DMA inhibition, stale tokens, missing/failed
hooks, synthetic desk/dock lifts, pocket walk, bag jostle, face-up hand carry,
face-down lifts, desk jitter/tilt/stand traces, startup face-down,
hysteresis, invalid samples, discontinuities, cooldown and stable re-arm.

No board current, FPS, luminance, physical panel-off, wake latency or false-wake
rate is measured by these tests. Physical wake GPIO/PMU/network policy,
independent visual review and single-dev-board measurements remain integration
gates before a power saving or wake reliability claim.
