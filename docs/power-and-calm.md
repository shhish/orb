# Power and calm — Stream C

Design contract, 2026-10-27. This follows `hig.md` §§1–9, `delight.md`,
`interaction-model.md` and research concept 7, Desk / Hand. All thresholds and
brightness percentages below are engineering starting points, **not measured
current, luminance, battery life or wake reliability**. The lead owns hardware
validation. Stream C supplies the controller and simulator-only presentation;
the shell branch consumes the C API without being refactored here.

## One object, two distances

On the desk Orb is a quiet clock with one aggregate attention mark. Pick-up
brings back a brighter, richer Face glance, but never reveals private text. Touch or
PWR expresses viewing intent; motion does not authenticate the owner. No power
transition navigates, answers, sends, unlocks, dismisses a card, discards a
transcript or interrupts a recording. A pending decision survives every state.

The shell supplies visible **Quiet / Active**, **Always On**, **Always On
brightness**, and **Wake on pick-up** controls in Control Center or Settings. Quiet selects the redacted
desk presentation immediately, silences cues and suppresses automatic pick-up
and attention wakes until deliberate touch/PWR or Active. Active is equivalent
to deliberate viewing intent. Turning motion wake off leaves touch/PWR available. These controls are
integration work for the shell branch, not hidden gesture requirements.

## State and timing contract

Times are elapsed monotonic milliseconds since the last intentional activity;
they are absolute deadlines, not chained timers. A delayed tick goes directly
to the correct state. Boot starts Active with private content redacted. Default
owner brightness is 70%; respect a lower owner setting in every lit state.
Brightness values are panel command percentages, not calibrated luminance.

| State | Normal entry deadline | Panel command | Presentation and redaction |
| --- | --- | --- | --- |
| Active | Intentional touch/PWR/Active; admitted pick-up or new attention wake | Owner setting, 70% default; selectable through 100% | Manual viewing may show the current task. Automatic wakes show the Face glance: time, date, aggregate waiting count, Home on-count, battery/hub status and a safe next-card title derived from its kind. Never expose raw card/session titles, agent steps, messages, transcripts, commands, Home names/values or notification bodies. |
| Dimmed | 20,000 ms idle | `min(owner,15%)` | Switch to the redacted desk layout before reducing brightness. Remove all private detail, actions and task previews. No moving complication or shimmer. |
| Always On | 30,000 ms idle | `min(owner,always_on_brightness_pct)`, 9% default | Time and one aggregate attention count only. No seconds, calendar, agent/source identity, notification title, body, transcript, Home content, controls or private artwork. Stable geometry after the entry transition, at most one update/second. |
| Sleep | 120,000 ms idle on battery; 1,800,000 ms with USB present | 0%, then request panel off | Nothing visible; retain all model/task state. Sleep does not mean power-off, disconnect or permission to suspend active work. |

If Always On is disabled, enter Sleep at 30,000 ms instead of Always On. Owner
brightness zero may darken any state; never force it upwards. Display redaction
is a separate flag, not inferred from brightness. Dimmed, Always On, Quiet and
Sleep revoke any deliberate reveal. Pick-up or attention never clear redaction.

**System polish decision (2026-10-28):** redaction and the minimal idle clock
are different things. An automatic Active wake now presents the richer Face
glance above the retained current task, without navigating away or clearing
`redact`. The first touch/PWR still consumes the entire contact before revealing
that task. The next-card line uses `Approval needed`, `Choose a response`,
`New message` or `Needs you`: raw titles are unstructured and can themselves
contain commands, messages or agent text. There is no protocol field declaring
a title public, so motion must not guess one is safe. Date and aggregate Home
on-count are public glance complications; unavailable/disconnected data is
omitted. Dimmed/Always On still remove those richer details. Automatic wake
remains silent and all safe-frame/DMA acknowledgement rules remain in force.

Only intentional activity renews the ordinary idle deadline. An admitted
pick-up starts a redacted **glance**: Active for **6 s** from the wake, then
Dimmed until **9 s**. It returns to Always On only if it woke from Always On,
that preference and the current battery tier still permit it, and the original
intentional-activity Sleep deadline has time left. Otherwise it enters Sleep.
An automatic wake never resets that original deadline. A deliberate touch/PWR
during the glance converts it into ordinary viewing with fresh idle deadlines.

Attention wakes for real cards needing the user retain **20 s** Active and
Dimmed through **30 s**, including in Low Power; critical battery still denies
them. Attention may replace an Active or Dimmed pick-up glance, so a new card
gets the full admitted attention window. The preceding untouched pick-up
then counts as ignored; the attention window itself never counts.
Otherwise automatic sources are rejected while already Active; duplicates and
bursts neither extend a window nor replay its transition. Replacing an Active
glance does not replay its wake transition. Attention is admitted
at most once per **30 s**; pick-up has its separate **10 s** cooldown and
stable-rest re-arm. A card received while face-down, critical or cooling down
still enters the ordinary pending model; it simply does not wake the panel.
No automatic wake plays a sound in this stream.

On battery, count each pick-up glance that ends without deliberate touch/PWR.
After **3 ignored glances in a row**, suppress pick-up wakes for **5 min**.
Each further ignored glance doubles the next suppression to **10, 20, 40,
then 60 min**, capped at 60 min. Use monotonic window and suppression deadlines;
rejected wake attempts never extend them. Deliberate touch/PWR or a USB plug
resets the budget. USB also cancels the current glance's budget eligibility;
glances begun on USB or with owner brightness zero never count. Attention wakes bypass this budget and never
count against it. Work leases preserve their ordinary keep-awake semantics:
ignored-window accounting can finish during work, but must not darken active
voice, OTA, app or alert work early.

Touch from a low-power state is consumed as wake/viewing intent; its remaining
contact must not click the task under it. Physical interaction with an opaque
timer/alarm alert resets the ignored-wake budget and renews activity through
`orb_power_note_activity`, without revealing the retained task. Synthetic
pointers never call that hook; alert ownership and safe-frame consumption stay
unchanged. PWR similarly wakes before its normal
awake Apps action. Another deliberate press performs the ordinary action. The
shell must freeze semantic input ownership across this change.

## Always On face and burn-in protection

Use true black, semantic Inter Large Title for the time, tabular figures, white
ink, no colon blink and no seconds. With no waiting items, centre the clock on
the circle's equator at `(233,233)`, before the burn-in offset. With waiting
items, optically centre the time and attention line as one group around that
equator, accounting for their actual semantic font geometry at each size.
The line uses one Framework7-style orange hand and the aggregate text
`2 waiting` in Headline (28 px Semibold at Default; AX3/Bold follows the semantic
scale). Zero attention omits that entire mark; do not replace it with
celebratory text. Cap the visual
count as `99+ waiting`. The mark is a count of pending requests, not a count of
approved commands. Keep at least 16 px between the glyph and the text box.
This face is informational: the whole surface wakes, it has no tiny targets.

Do not draw a bezel ring, glass, battery complication, Apps button, background
card or animated working arc in Always On. These are intentional exceptions to
the richer active Face in `delight.md`: the HIG Always On rule for minimal,
stable, redacted content takes precedence. Colour has meaning and text carries
the same state. White and orange retain their HIG contrast on black even though
the physical panel is dimmed. Reduce Transparency needs no substitute material
because this presentation has no glass. Increase Contrast uses the HIG orange
variant. Bold Text and AX3 must reflow without clipping, never shrink the clock
or count below their semantic styles to recover space.

After the bounded entry transition, the controller grants one update opportunity per 1,000 ms in Always On; the
renderer invalidates only if time, count or shift actually changed. Static
content produces no redraws between changes. An admitted change between zero
and nonzero attention gently recentres the retained clock and fades the mark
over 250 ms (150 ms cross-fade under Reduce Motion). This bounded compositor
reflow requests 16 ms frames through `power_view_frame_interval_ms()`; combine
that with controller cadence in the preview consumer, then return to settled
1 Hz. The minute shift remains discrete even during reflow. At each elapsed minute move the
whole face around `(-1,-1), (1,-1), (1,1), (-1,1)` pixels, one corner per minute.
Each movement is exactly 2 px on one axis, never an animated drift; the initial
offset is `(-1,-1)`. No movement is larger than 2 px, no element is left behind,
and elapsed monotonic controller uptime rather than render frequency or a new
Always On entry determines the corner. Waking does not restart the pattern.
The same discrete burn-in shift remains under Reduce Motion.

## Always On brightness and AMOLED exposure

There is no ambient light sensor. **Always On brightness** is one named setting,
`always_on_brightness_pct`, default **9%**, clamped to **1–15%**. The owner's
Active brightness remains an upper bound, including zero. Do not derive this
setting proportionally from Active brightness or invent automatic adaptation.
Disabling Always On remains the direct way to avoid its sustained pixel use.

The board API explicitly accepts 0–100% commands. `orb_board_panel_brightness()`
in `board_ws175c.c` sends DCS `0x51` with `floor(pct * 255 / 100)`: 8%, 9% and
10% map to 20, 22 and 25 on the 8-bit command scale. The full 1–15% setting
range is representable. These are command levels, not measured luminance;
the driver returns `void` and logs I/O errors, so dispatch does not prove the
panel reached the requested brightness.

Increasing Always On brightness can improve desk glanceability but consumes
more display power and increases cumulative AMOLED pixel exposure and burn-in
risk. Sparse content, minute shifts and finite idle sleep reduce exposure;
they do not eliminate burn-in. USB supplies power but does not remove display
wear. Keep the 30-minute USB idle sleep deadline pending measured panel and
usage evidence rather than making Always On indefinite. No lux, current,
battery-life or burn-in lifetime is inferred from the simulator.

## Calm sleep and wake

Follow `interaction-model.md`: the Face answers pick-up with its own existing
content. On wake, the time lifts by **2 px** and a newly appearing attention mark scales
from **0.9 to 1.0** while its opacity rises. A mark already visible interpolates
from its current scale and opacity instead of disappearing or replaying an
entrance; this avoids doubled glyph edges during a lit wake. Use a critically damped, no-bounce spring
that settles within **400 ms**. Do not hide an existing attention line and then
restore it at the endpoint. If waking from Always On, interpolate from the
currently presented clock, count, opacity and burn-in offset; if waking from a
dark panel, fade the safe content in. No novelty artwork appears.

On idle entry the content settles and fades into the Always On arrangement over
about **250 ms**. Retained elements interpolate their position, scale and
opacity, including every element's burn-in offset. At the later actual Sleep
deadline the safe face fades to black over **250 ms** before the panel-off
request (150 ms under Reduce Motion). Privacy
masking precedes the first transition frame; private labels never fade through
an automatic wake. A delayed tick beyond a transition's absolute endpoint goes
directly to its settled state.

With Reduce Motion use only a **150 ms cross-fade**: no size animation, position
animation, depth, parallax or spring. The discrete minute burn-in shift still
applies equally to clock, hand and count. An input interrupts sleep immediately
and remains usable during wake. In-place awake touch, attention bursts and
brightness changes do not replay the transition. Face-down and critical-battery
sleep bypass it and go dark immediately. Accessibility changes must retarget
from the current presentation without exposing a stale private frame.

## IMU: deliberate wake, bounded false positives

Inputs are screen coordinates in g from `orb_board_accel()`: +z points out of
the glass and face-up rest reads approximately +1 g. Use a caller-supplied
monotonic timestamp; polling cadence is 20 Hz (50 ms). No task, ISR, allocation
or LVGL dependency is required in the detector. Gravity low-pass time constant
is 150 ms, with timestamp-derived alpha `dt/(150+dt)` and normalized filtered
vectors for angle comparisons. Reject non-finite values and raw norms outside
0.40–2.00 g; this plausibility range keeps vigorous lifts usable while the
separate, narrower rest range proves stillness. A gap over 250 ms invalidates
the candidate and stable-rest arm.
Missing/failed samples cannot prove a dwell. Invalid samples never wake or
clear an already confirmed face-down condition.

Pick-up uses stillness rather than a face-up arming gate:

1. Arm after **800 ms** continuous rest in **any orientation**: raw magnitude
   **0.85–1.15 g**, consecutive raw vector change **≤0.06 g**, and raw/filtered
   orientation drift **≤8°** from the rest-start direction. Freeze the gravity
   direction and resting magnitude as the baseline. Flat, 30–70° stands/docks,
   vertical and side-facing resting poses use the same test. Confirmed
   face-down silence takes priority and prevents arming until its dwell exit.
2. A tilt candidate starts when raw and filtered directions are both **≥25°**
   from baseline. Both raw and filtered
   directions must remain **≥20°** from baseline for **150 ms**. Independently,
   an acceleration candidate starts at an absolute magnitude deviation
   **≥0.18 g** from baseline and must remain **≥0.10 g in the same sign** for
   **200 ms**. Alternating impulses restart that dwell. Either sustained
   candidate emits a pick-up only when **raw and filtered normalized screen
   z are both ≥0.30 at the end of the dwell**. This is a viewing-pose gate,
   not a new arming condition: vertical-in-pocket and face-down carrying do
   not qualify. The lower hold thresholds provide hysteresis. The synthetic
   100 ms desk bumps and typing traces do not complete a dwell; real false-wake
   rates remain a board validation gate.
3. Emit one event, disarm, enforce **10,000 ms** cooldown, and require a new
   **800 ms** stable rest before another event. Carrying, oscillating through
   thresholds or a noisy cable must not repeatedly renew Active. A face-up hand
   carry can produce the same evidence as a real lift and may admit one glance;
   rest/cooldown and the ignored-wake budget bound repeated unattended wakes.

Face-down is independent of pick-up and stays enabled when motion wake is off:
raw and filtered normalized z ≤−0.80 continuously for 300 ms enters face-down. It masks
and sleeps the display, silences all cues, and skips the transition immediately after
confirmation. Exit needs raw and filtered normalized z ≥−0.60 continuously for 300 ms;
the hysteresis avoids chatter. Exit removes forced silence but **does not
wake** or reveal anything by itself. Subsequent deliberate touch/PWR, newly
armed pick-up or admitted attention can wake. Touch/PWR cannot override a still
confirmed face-down state; turning the device over remains the physical exit.

These thresholds cannot establish real-world wake accuracy. A lift with no
measurable acceleration or orientation change is unobservable to this detector.
Touch/PWR and visible Active remain alternatives. Host fixtures include flat
desk and 30–70° dock lifts, stand boundaries, desk bump, typing vibration,
pocket walk, bag jostle, face-up hand carry, lifts toward face-down and
face-down dwell. They are explicitly **synthetic**, not captured board traces.
Capture labelled board traces before claiming real wake reliability.

## Charging and low battery

Use the PMU's `usb`, `charging` and battery percentage as separate facts. USB
present does not mean charging; fully charged on USB does not show a charging
bolt. Battery `-1` is unknown, never empty or critical. Unknown voltage/percentage
does not invent a charge estimate. Neither plugging in nor unplugging reveals
private text or repeatedly wakes Orb; it recomputes the applicable deadline.

| Condition | Policy, brightness and recovery |
| --- | --- |
| USB present | Normal timings, 30-minute idle sleep; preserve face-down/Quiet; use actual charging flag. |
| Battery ≤20%, USB absent | Low Power: Active capped at 30%; Dimmed after 10 s, Always On after 15 s, Sleep after 60 s. Recover at ≥23% or USB. |
| Battery ≤10%, USB absent | Preserve Low Power; disable Always On and sleep at 15 s. Re-enable at ≥13% or USB. |
| Battery ≤5%, USB absent | Critical: immediately sleep, no transition, pick-up or attention wake. Manual wake capped at 15%, with a newly acknowledged frame required; intentional viewing expires directly to Sleep after 10 s. Recover at ≥8% or USB. |
| Battery ≤2%, USB absent | Start a 60 s monotonic cell-protection countdown after a known reading. At its deadline expose `shutdown_requested`; actual PMU soft-off remains the lead's integration responsibility. |

Battery state uses those hysteresis latches, so 19/20/21% noise cannot pulse the
screen or change policy repeatedly. Manual viewing may deliberately reveal
while critical; a previously revealed framebuffer is never reused on wake.
Low battery does not abandon voice, OTA, pending answers or model updates.
It requests safe service throttling; it never cuts rails mid-write. Cell
protection retains its countdown through 3% gauge jitter. A reading ≥4%, USB
or unknown `-1` clears the countdown and request; after unknown, a fresh known
reading ≤2% must start another full 60 s. Unknown never starts or completes
cell protection. The existing critical presentation precedes the request.
`shutdown_requested` is an advisory snapshot output only: the lead must fence
flash/data writes and wire the actual PMU soft-off safely in main.

The existing richer Face/status widget owns battery visuals: charging green
with bolt, low power yellow, charge ≤20% red, paired with its symbol/percentage
per `orb_status` precedence. Always On omits them to keep pixels sparse. A
manual low-battery glance can expose a normal-size percentage in the richer
shell, never a mandatory modal or automatic dismissal of a pending decision.

At the first known reading at/below 20% on battery, show a quiet, silent banner
linking to Battery. At/below 5%, replace it with `N% · Battery low`. Each appears
once per crossing, using the policy's 23%/8% recovery hysteresis; sensor noise,
duplicate readings and unknown samples never re-arm it. USB or charging clears
the notice and re-arms both crossings. A critical notice waits for manual wake,
since a notice never wakes Orb or defeats critical sleep. Tapping opens Battery;
a case double-tap dismisses locally, preserving the task and all pending cards.
The Face, Battery, Charging and Control Center already expose battery facts,
so they omit the duplicate banner. Battery keeps its existing template: known
percentage, independent USB/charging state, and health/temperature only when
valid. Unknown telemetry never becomes a health estimate, current reading or
runtime promise; the screen quotes no idle timing.

## C service and safe display/sleep integration

The new `orb_power` component is a deterministic caller-owned controller, named
`orb_power_controller_t` because the board already defines `orb_power_t` for a
PMU reading. Its small API accepts time, intentional activity, wake reason,
attention, PMU status, preferences, work inhibitors and accelerometer samples;
it returns an immutable snapshot with state, redaction, forced silence,
brightness, panel request, cadence/next deadline, pixel offset and the optional
bounded moment. It has no shell, LVGL, network or task dependency. Serialize its
calls in the consuming service; tests use fakes and synthetic timestamped traces.

Render privacy changes **before** applying wake brightness. The shell must
render the snapshot's current surface (redacted for automatic wake), finish all
display DMA and acknowledge that snapshot generation before the adapter can
brighten or wake the panel. During a lit handoff from an already acknowledged
safe frame, keep its current brightness until the replacement drains, then
apply the new level. This lets Active become Dimmed and Always On without a
black interval. An already visible private frame can remain for that extra
frame while its redacted replacement renders. For redacted-to-private content,
never raise brightness before acknowledgement; if the new target is lower,
lower the old surface first so incoming private pixels cannot inherit excess
brightness. Brightness-only adjustments to the same surface remain immediate.
A stale acknowledgement cannot enable a later generation. Likewise never send
a brightness command while a write is on the wire. The board adapter uses
`orb_board_panel_brightness()` only at a caller-provided display fence. A failed
or missing safe-frame acknowledgement from a dark or unknown framebuffer
leaves the screen dark; a known lit safe frame retains the handoff above.
Face-down and critical forced-off remain immediate. It is not an excuse to
wake on a previous task framebuffer.

`orb_board` currently exposes brightness but no panel sleep primitive. Therefore
brightness 0 is only a dark fallback, **not proof the panel is physically off**.
An optional platform hook requests panel awake/off and reports unsupported or
failure truthfully. Physical panel-off and its resume sequence remain the lead's
integration gate. Do not send guessed panel opcodes from this service.

Sleep policy is advisory. Never call `esp_light_sleep_start()` automatically:
the lead must first establish touch/PWR/IMU wake readiness and preserve hub
attention delivery through an appropriate network wake policy. Voice capture,
playback, OTA/flash, interactive app work and display DMA inhibit system sleep.
Voice, playback, OTA and interactive app keep-awake leases pause normal idle
transitions and resume with a fresh idle budget when they end. DMA only denies
CPU sleep and unfenced panel commands: it never renews the user's idle budget,
since ordinary Face rendering must not prevent dimming forever. Face-down still
forces privacy, silence and a dark
panel while the work continues. An unsatisfied wake source, unsupported panel
off, unacknowledged safe frame or outstanding inhibitor denies light sleep.
The controller does not reconfigure GPIO, PMU, Wi-Fi, launchd or owner settings.

An explicit display lock or Quiet can darken the display during work without
removing any system-sleep inhibitor. The controller's separate display-lock
latch survives IMU evaluations and clears only on an admitted wake. Pending
voice confirmation and retry/error screens retain the VOICE viewing lease;
work masks are installed before preference, battery or attention evaluations.

## Evidence and lead gates

The simulator must render Always On, no-attention, shifted minute positions,
sleep and wake PNG sequences at Default and AX3 + Bold + Reduce Transparency;
also render Reduce Motion sequences. Review each against every design-gate
heading. The new preview is a stub view outside the live shell: it proves
layout and deterministic state output, not integrated hardware power savings.

Host tests cover exact timer boundaries, delayed ticks, privacy on every wake,
bounded automatic wake, face-down suppression and hysteresis, failed/missing
samples, desk noise, tilt pick-up, carrying/cooldown/re-arm, battery hysteresis,
charging truth, inhibitors, safe-frame acknowledgement and hook failure. Keep
synthetic traces deterministic and preserve provenance if measured ones arrive.

Before merge the lead must wire the shell/service adapter, complete independent
PNG review, and capture on the single dev board: idle current in every state,
panel-off truth/resume, no missed touch/PWR/IMU wakes, bounded false wakes on
desk/stand/cable/carry traces, charging/low-battery behaviour, silence while
face-down, no private old frame during wake, and no recording/OTA loss. Stream C
does not claim these board gates passed. Exact lead commands and simulator
paths belong in the implementation results alongside captured pass counts.

## Independent review decisions

Accept the review's stand wake defect, purposeful wake/sleep motion, complete
burn-in shifting, empty-state equator centring and larger attention typography.
The named 9% Always On brightness setting addresses its brightness concern
without assuming measured office-light readability.

Reject the suggested replacement arming gate `z >= 0.45`: it still excludes
steeper stands and other resting orientations. Stillness in any orientation
implements the lead's broader requirement. Reject proportional Always On
brightness and its fixed floor: the lead selected one independent named
setting, and an owner brightness of zero must remain dark. Reject the review's
150 ms ordinary sleep recommendation in favour of the lead's approximately
250 ms transition; 150 ms remains the Reduce Motion duration. Retain the
30-minute USB idle sleep limit rather than indefinite Always On because USB
does not remove cumulative AMOLED exposure, and sustained panel operation has
not been measured. The review's lux, physical readability and burn-in claims
remain hypotheses until captured on the panel.
