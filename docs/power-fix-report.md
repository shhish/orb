# Power follow-up review — 2026-10-31

Changes are left uncommitted on `orb/w2-pwr-fix`. No board, serial port,
flashing, live hub, owner configuration, Keychain or launchd access was used.
The audio lane's production files were not changed.

All seven core review findings were confirmed against this worktree:

| Finding | Resolution and regression |
| --- | --- |
| 1. PWR reveals a retained task beneath an opaque alert | Physical PWR uses activity-only accounting, preserves redaction/lock and consumes its full lifetime. Manual reveal entry points reject opaque alerts. The safety fixture dismisses the alert before release and checks UP/SHORT/DOUBLE/HOLD remain consumed. |
| 2. Pickup inherits an attention window's Always On origin | Only attention replacing pickup inherits the earlier origin. A pickup from attention's Dimmed phase returns to Sleep. |
| 3. Zero-brightness pickup spends the ignored budget | Such a pickup is ineligible for accounting. The review's claim that suppression survives a deliberate PWR was incorrect: PWR already resets the budget. |
| 4. Lock discards system inhibitors | A separate `display_locked` latch suppresses display leases while retaining VOICE/PLAYBACK/OTA/APP/DMA system blockers. Repeated locked IMU samples cannot renew display activity. Alert dismissal preserves non-APP work bits. |
| 5. Pending voice confirmation loses its viewing lease | CONFIRMING and ERROR join the VOICE lease in shell, radio-work and runtime admission checks. Ending the lease restores a fresh ordinary idle budget. |
| 6. Suppression deadline saturates | An unrepresentable future suppression expires rather than becoming an indefinite ban. This is a representability boundary, not a practical uptime failure. |
| 7. New work arrives after idle evaluation | Work masks are installed before preference, battery and attention evaluation; status updates enter that same ordered poll. |

No complete finding was rejected. The existing touch-glance, cell cutoff and
alarm-bypasses-budget behavior remains covered by the controller suite.
For finding 4, the current main runtime also independently checks live
voice/speech/OTA, so the review overstates the demonstrated production failure
for those services: the cited `orb_power_board_try_light_sleep()` helper has
no production main caller. Erasing the inhibitor mask is still a real
controller/API defect, including lost PLAYBACK/DMA bits, and is fixed and
regression-tested independently of that extra runtime fence.

The panel adapter holds a known lit safe frame during a content handoff and
returns `WAIT_SAFE_FRAME`, preserving screen invalidation. It applies the new
brightness after acknowledgement/DMA drain. Private-to-redacted may retain an
already visible frame for the extra render; redacted-to-private cannot raise
brightness early and clamps to a lower target before private pixels arrive.
Dark/unknown starts and owner-zero wakes still wait in black; face-down and
critical forced-off remain immediate. No layout or redaction policy changed.

The second boot TLS connection came from firmware's deliberate dev-channel
refresh after hello. Compiled devtools support is now offered in the initial
hello and the forced teardown is removed. The hub still grants permission
only from its saved dev channel, and firmware checks the accepted channel.
The hub's duplicate-socket replacement and identity-guarded cleanup were
already correct; no hub production change was needed. The reported readable
data/clean-close warnings are consistent with tearing down amid initial hub
traffic, but their disappearance still needs a board boot capture. Legacy
firmware without the optional capability remains accepted.

The wake audit found independent empty-button and IMU 100 ms schedules; the
I2C completion timestamp lets them drift apart. Empty button service is now
edge-driven, preserving exact gesture deadlines, and eligible USB-console
polling is 5 s instead of 1 s. Wi-Fi commands still notify immediately. An
independent synthetic 3 ms I2C schedule produced 19 main-loop events/s before
and 10 after this change; those are not hardware sleep-entry measurements.
TCP's 250 ms timer, managed WebSocket's 1 s socket poll, Link's 1 s model/answer
pump, main status/Time/PMU slots and Wi-Fi remain. mDNS was already freed after
discovery; the old audit's persistent 10 Hz attribution was wrong. Conditional
100 ms audio recovery remains owned by the parallel audio lane. The detailed
remaining-wake rationale is in [light-sleep.md](light-sleep.md).

| Changed file | Purpose |
| --- | --- |
| `docs/power-and-calm.md` | Documents lit handoff privacy, invisible-pickup accounting and separate display/system leases. |
| `docs/light-sleep.md` | Corrects mDNS attribution and records removed/remaining timers and unmeasured hardware gates. |
| `docs/protocol.md` | Clarifies first-hello devtools support and authoritative channel permission. |
| `docs/power-fix-report.md` | This review and verification receipt. |
| `firmware/components/orb_power/include/orb_power.h` | Adds the independent display-lock latch. |
| `firmware/components/orb_power/orb_power.c` | Corrects automatic-window origin, budget boundaries and locked display leases. |
| `firmware/components/orb_shell/orb_shell_power.c` | Protects alert contacts, retains system inhibitors, adds pending-voice leases, orders work before idle evaluation and polls once per status wrapper call. |
| `firmware/main/orb_power_services.c` | Keeps confirming/error voice work prompt and ineligible for system sleep. |
| `firmware/components/orb_power/orb_power_board.c` | Holds lit safe content across frame handoff and limits pre-ack private brightness. |
| `firmware/components/orb_power/include/orb_power_board.h` | Documents the adapter's revised acknowledgement contract. |
| `firmware/main/orb_panel_policy.c` | Comment-only hook clarification; waiting handoffs still invalidate the screen. |
| `firmware/components/orb_devtools/orb_devtools.c` | Offers compiled devtools on first hello; slows only eligible USB fallback polling. |
| `firmware/components/orb_link/orb_link.c` | Removes dev-channel-triggered reconnect. |
| `firmware/main/main.c` | Parks empty eligible button polling and widens the milliseconds-to-microseconds multiplication. |
| `firmware/components/orb_buttons/include/orb_buttons.h` | Documents GPIO edge/deadline-driven service. |
| `firmware/components/orb_power/tests/host/test_policy.c` | Automatic-origin, invisible-budget, max-range suppression and work/Quiet regressions. |
| `firmware/components/orb_power/tests/host/test_power.c` | Lit idle handoffs, reveal brightness directions, owner-zero fencing and immediate forced-off regressions. |
| `firmware/components/orb_board/tests/host/test_power_adapter.c` | Updates asynchronous handoff expectations. |
| `sim/tests/test_power_brightness.c` | Replaces the obsolete expectation of a black lit handoff. |
| `sim/time_shell_main.c` | Exercises full physical alarm PWR contacts and distinct reveal/lock/wake presses after presentation dismissal. |
| `firmware/main/tests/host/test_calm_power_safety.py` | Compiles production calm functions with the real controller for alert, lock, pending-voice and ordering regressions. |
| `firmware/main/tests/host/test_sleep_waits.py` | Runs production button/console waits and voice classifiers under deterministic clocks. |
| `firmware/components/orb_link/tests/host/test_hello.py` | Production initial-hello and channel/capability regressions with devtools compiled on/off. |
| `firmware/components/orb_link/tests/host/Makefile` | Runs the new hello fixture. |
| `hub/tests/test_devtools.py` | Loopback TLS tests for one retained socket and legacy capability omission. |
| `sim/CMakeLists.txt` | Registers calm/wait regressions and the complete power host suite. |

Targeted sanitizer evidence: power 19 cases/5,275 checks, IMU 35/5,556,
policy 11/457; asynchronous adapter and simulator brightness 2,238 checks;
production calm/wait fixtures; complete Link host suite, including hello
59/68 checks with devtools off/on. Hub Link/devtools loopback tests passed
132 tests. Thirteen policy/shell temporary-copy mutations rejected every core
defect and extra alert/order/locked-IMU/locked-attention/duplicate-poll regressions. Two adapter mutations
rejected black handoff and excess reveal brightness; two hello mutations
rejected either old reconnect behavior; three wait mutations rejected the old
button timer, narrow deadline product and old console interval. Production
sources were not overwritten for mutation runs.

The final required simulator configure/build and the 9-test integrated
Time/power/calm/waits/host-power rerun passed. The required final ESP-IDF 6.1
build passed after the last firmware edit, with automatic light sleep, PM and
tickless idle enabled in its generated configuration. The unsigned app is
`0x2d0000` bytes and fits its 6 MiB slot with 53% free; this does not measure
runtime heap. Initial IDF configuration required a sandbox escape for a
read-only system-information query; the final incremental build passed in the
normal sandbox. No signing key was accessed and no image was installed.

Build/test logs are `/tmp/orb-power-fix-sim-build.log`,
`/tmp/orb-power-fix-targeted.log`, `/tmp/orb-power-fix-idf-final.log` and
`/tmp/orb-power-fix-ctest-final.log`. The final full CTest run passed
**1,167 / 1,167 tests in 242.13 s**, including the registered power host suite.
Guardrails passed with the existing `file_size=13` debt unchanged; Ruff,
Python formatting and `git diff --check` passed. The earlier full run caught
and led to fixing the locked-attention integration regression before this
final clean run.

Design-gate images were newly rendered and inspected at
`/tmp/orb-panel-handoff-review/`: `face-default.png`, `face-ax3-bold-rt.png`,
`list-default.png`, `list-ax3-bold-rt.png`, `approval-default.png` and
`approval-ax3-bold-rt.png`. Face, pending Sessions and Approval pass the static
two-layer, black-canvas, round-fit, type/size, semantic-colour, approval/agent,
accessibility, navigation and restrained-appearance checks. Approval retains
Deny and equal untinted actions; AX3 wraps cleanly. No gradients/glows or
unintended layout change were observed. Hit geometry is unchanged; disabled
list contrast was not numerically measured, and static PNGs cannot prove
animation curves or physical panel brightness. The lead's real-board design
gate remains open.

Remaining limits: no power meter or board access means no measured new wake
rate, residency, current, actual TLS handshake count or physical blink result.
The controller still cannot tell whether a nonzero-brightness glance had every
frame withheld by the adapter; only owner-zero budget eligibility is fixed
here. The lead should capture idle brightness history, a single boot handshake,
light-sleep entries/residency, alert-over-private lock behavior, confirming
voice, locked OTA/audio and complete waking-contact lifetimes on the dev board.
No alarm or approval deadline was lengthened. `nice` was requested on long
jobs; this sandbox refused priority adjustment, and concurrency stayed bounded
to `-j4`.
