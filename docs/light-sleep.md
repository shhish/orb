# Automatic light sleep: implementation and validation

This change enables ESP-IDF automatic light sleep only after the runtime gate
proves that Orb is eligible. It does not call `esp_light_sleep_start()`. There
was no board, serial-port, power-meter, flashing or live-hub access during this
work. Single-digit milliamps, the 24 KiB internal-heap floor, wake reliability,
panel appearance and Wi-Fi continuity remain real-board release gates.

## Runtime contract

`CONFIG_ORB_LIGHT_SLEEP` defaults to `y`. Boot creates and acquires the
`orb_awake` `ESP_PM_NO_LIGHT_SLEEP` lock before configuring PM. The lock is
released only for Sleep with a completed panel-off transition, known PMU power
state, armed GPIO/Wi-Fi wake, no active contact, no work inhibitors, and no USB,
voice/speech, OTA, ringing/finished alert, benchmark or game. The devtools USB
exception changes only the USB test; every other requirement still applies.
Unknown power state, a configuration refusal, wake arming failure or failed ISR
restoration leaves the device awake behind an independent fault veto. The
service owner retries with 1/2/4/8/16/30 s backoff, capped at one attempt per
30 s while a fault persists; a later success restores eligibility without
reboot. Repeated updates never recursively acquire the policy lock.

Wake uses touch INT GPIO11 low, PWR GPIO3 high and BOOT GPIO0 low, plus IDF
Wi-Fi wake and the existing RTOS/timer deadlines. GPIO wake and the ordinary
edge interrupt share the hardware interrupt-type register. The sleep callbacks
mask edge delivery while setting level wake, enqueue real input sampling after
wake, restore the original edge configuration, then sample again even if
the level is already idle. Button timestamp/level sampling and queue insertion
share one lock, preventing a newer ISR release from preceding a stale sample.
A held input rejects the
next sleep attempt. Ordinary ineligible states use the NO_LIGHT_SLEEP lock
without vetoing IDF's idle hook, preserving existing 80 MHz idle DFS on USB
and other awake states. A setup/restoration or PM API fault retains an
independent veto until recovery completes. The existing touch/button path still owns the entire first
contact, including its release; synthetic input never becomes physical wake
evidence or an Allow/Send/Unlock action.

The LCD sender releases the SPI device's acquired bus only after physical
panel-off is complete and DMA has drained. It acquires before the next panel
command or pixel write. `board_panel_power.c` remains on that same owner, so a
power transition cannot race another panel command. GPIO sleep selection is
disabled for the panel, touch, buttons, I2C and audio pins, preserving their
accepted active electrical state. PMU rails are not switched for light sleep.
The existing PA-low/audio-idle state and panel/touch retention remain intact.

Time deadlines are serviced by main independently of LVGL. The next timer,
alarm, snooze or reminder deadline limits its wait, so slowing the dark UI does
not postpone an alert. An interactive wake promptly restores the existing
20 ms input profile and gyro. Eligible sleep has one main-owned wake schedule:
IMU at 100 ms and calm/UI and full calendar service at 1 s (or an exact due deadline), with GPIO edges and nearer
button/Time deadlines shortening the wait. An empty button recognizer has no
periodic deadline; captured edges and pending debounce/gesture deadlines still
run immediately. Display/touch tasks park their dark
periodic polling; they are notified by the existing input paths.

Confirmed face-down permits 1 Hz IMU reads. The first plausible raw exit pose
requests 100 ms reads again. This is necessary because the detector rejects
gaps over 250 ms: one-second samples must neither prove a 300 ms exit dwell nor
leave a confirmed face-down latch impossible to clear. Exit alone still does
not wake or reveal content.

## Static lock and wake audit

These are source/configuration facts, not a captured PM dump or wake trace.
Check the regenerated configuration before relying on third-party defaults.

| Owner | Eligible idle behavior / cadence | Remaining reason |
| --- | --- | --- |
| `orb_awake` | Released only by the pure runtime policy | Held in every ineligible state; failed wake setup is fail-closed. |
| `spi_master` APB lock | No lifetime bus acquisition while physically asleep | A panel command reacquires first; command/DMA completion is fenced by the sender. |
| `display_band` CPU lock | No flush jobs while panel is off | Existing per-job references protect actual render/TE/DMA work. |
| `i2s_driver` | Existing idle gating disables RX/TX after 100 ms silence | Active capture/playback holds the driver lock; idle also detaches MCLK and leaves PA low. |
| I2C master | Transaction-scoped `ESP_PM_NO_LIGHT_SLEEP` lock (S3 default XTAL source) | IMU reads at 10 Hz / 1 Hz face-down; PMU/status at 1 Hz; touch is event-driven while dark. |
| Wi-Fi | Existing MAX_MODEM, listen interval 3 | Driver/AP beacon/DTIM and incoming traffic still determine radio wakes. Their current/duty cycle requires measurement. |
| Main / calm | 100 ms IMU, 1 s calm/status/power slot | Buttons, work changes and exact Time deadlines can wake sooner. No app input/publication, gesture recognition, gyro-bias learning, touch reads or devtools motion snapshots in the minimal IMU path. |
| Main / buttons | Event-only without a contact or pending gesture | Previously an independent 100 ms wait, offset from the IMU by its I2C completion time. GPIO notifications, 30 ms debounce, pending short/double and hold deadlines remain prompt; the whole waking contact stays consumed. |
| `orb_link` | Outgoing model queue pump 1 s, power check 5 s, report 60 s | Leaving eligibility immediately prompts a pump and restores 50 ms; a transient wake cannot be lost. Incoming frames are event-driven. |
| WebSocket keepalive | 45 s after background negotiation; otherwise 15 s | Existing negotiation/report ordering retained. The hub's negotiated background silence allowance remains 135 s. |
| Managed WebSocket task | Socket poll at 1 s | `esp_websocket_client.c` hardcodes a 1000 ms read-poll timeout; unchanged third-party code. |
| lwIP | TCP timer defaults to 250 ms with an open TCP connection | `LWIP_TIMERS_ONDEMAND` defaults to `y`; unused IGMP/MLD work is conditional. DNS/retransmit/DHCP/ARP/network events can add wakes. Stack timings are unchanged. |
| Managed mDNS | No timer during a stable connected link | `orb_net_find_hubs()` already serializes `mdns_init()`/query/`mdns_free()`. Its 100 ms periodic timer exists only during discovery; the previous audit incorrectly counted it as a persistent 10 Hz wake. Discovery timing is unchanged. |
| `orb_net` | Stable connected worker waits indefinitely | Reconnect/provisioning work uses its existing retry deadlines; interactive transitions still prompt it. |
| Voice worker | Waits indefinitely only when eligible and no voice/speech/BOOT/cancel/redo work exists | Model enqueue, speech callbacks, physical BOOT and link changes notify it. Ordinary 20 ms / recording 1 ms behavior remains outside this idle case. |
| Sound audio worker | One remaining 10 s analog-suspend deadline, then indefinite notification wait | Active sound, codec recovery or capture is real work and may wake more often. |
| Sound preference worker | Indefinite wait when generation is saved | An edit starts a 500 ms debounce; another edit restarts it. An edit racing a flash commit remains pending. |
| Development USB console | 5 s while eligible, 20 ms otherwise | Previously 1 s while eligible. Wi-Fi commands notify the command task immediately; this wait controls only otherwise idle USB polling. USB normally denies eligibility; the RAM-only bench exception can see up to 5 s console latency. |
| Fonts | Loader waits indefinitely | Its LVGL 10 ms completion timer exists only during an actual asynchronous load. |
| Storage | No polling worker in `orb_fs`; mutex waits indefinitely | Explicit writes/Time persistence still run as needed. No periodic storage task was added. |
| Owned `esp_timer`s | No new periodic timer | Display/touch precise deadlines are one-shot/event-driven; the OTA probation deadline is one-shot. |

The 1 ms FreeRTOS tick (`CONFIG_FREERTOS_HZ=1000`) is retained. Tickless idle
suppresses idle ticks; it does not eliminate the real wake deadlines above.
The previously independent main button and IMU schedules can each wake at
10 Hz: `next_buttons` was based on the loop's starting timestamp, whereas
`next_input` starts after the I2C read. Together with TCP at 4 Hz, socket polling
at 1 Hz, Link at 1 Hz and the former console at 1 Hz, these provide a concrete
source-level explanation consistent with about 27 entries/s. This is a
candidate attribution, not a measured decomposition: timers can coalesce,
and Wi-Fi, status/PMU/Time slots and incoming traffic add other wakes.

This pass removes the empty button wake stream and reduces eligible console
polling from 1 Hz to 0.2 Hz. It retains TCP's 250 ms timer because changing it
also changes retransmission timing, and retains the 1 Hz Link pump because it
expires cards and retries queued answers. The managed WebSocket client's
1000 ms socket poll is hardcoded; its `network_timeout_ms` controls frame I/O,
not this poll, so increasing that option would not remove wakes. Incoming
socket data interrupts the existing wait immediately. No library fork or TCP
timing change was introduced to reduce wake counts.

Voice and normal idle audio/preference workers already park indefinitely;
display/touch park their periodic handlers, fonts have no timer without a load,
and OTA probation uses a one-shot deadline. Audio initialization/idle failure
recovery can still retry at 100 ms in `orb_sound.c`; it is conditional work,
not the normal idle path. The parallel audio lane owns that recovery and the
board audio files; they were inspected but not changed in this pass.

TCP, socket and radio wakes mean the total CPU wake rate is greater than the
main service's 10 Hz budget. Runtime `power` output and board captures must
quantify the resulting sleep fraction; this document does not infer current.

Regression `sleep_waits` compiles the production button service, wait expression
and voice lease expressions. It verifies event-only idle, the wide deadline
conversion, unchanged debounce/hold/consumed contact behavior, 5 s console idle
with immediate notification, and confirming/error voice work blocking sleep.
Exact Time/approval schedules were not lengthened. Real-board entry rates and
residency after these changes remain unmeasured.

## Configuration and memory

Edit `firmware/sdkconfig.defaults`; the saved, ignored `firmware/sdkconfig` can
retain old explicit values and must be regenerated by the lead. A fresh bench
configuration/build can be generated without replacing a saved configuration:

```sh
nice scripts/idf.sh -B build-light-sleep -D SDKCONFIG=/tmp/orb-light-sleep.sdkconfig build
```

Use a new unused SDKCONFIG path for a genuinely fresh defaults application.
Review the resulting `sdkconfig.h` for `ORB_LIGHT_SLEEP`, PM enable/tickless
settings and the options below. Keep the bench version pinned as required by
the dev feed before the lead installs anything.

`PM_SLP_IRAM_OPT`, `PM_RTOS_IDLE_OPT`, `ESP_WIFI_SLP_IRAM_OPT`,
`PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` and `ESP_PHY_MAC_BB_PD` remain disabled in the
checked-in defaults. They are not enabled on the strength of a generic saving
claim. Option-specific map deltas and the final build's internal-memory totals
belong in the build evidence; a map does not prove the dynamic 24 KiB heap
floor. GPIO-reset and octal-PSRAM leakage workaround decisions are owned by the
runtime/configuration review and must be read with the regenerated config.

The runtime review compared local ESP-IDF 6.1 with the existing trunk build's
`config/sdkconfig.h`. That baseline had CPU power-down and cache-tag retention
enabled even though automatic light sleep was disabled. Turning on sleep with
those defaults would allocate retention memory during `esp_pm_configure`.
The new explicit CPU-power-down `n` avoids that cost. The following figures are
IDF help/source estimates, **not map deltas or measured savings**, and explain
why the options remain off pending a measured benefit within the heap budget:

| Option | IDF cost/benefit guidance | Decision |
| --- | --- | --- |
| `PM_SLP_IRAM_OPT` | About 2.1 KiB IRAM; up to 310 us entry/exit reduction at 160 MHz | Off. |
| `PM_RTOS_IDLE_OPT` | About 180 bytes IRAM; about 20 us reduction | Off. |
| `ESP_WIFI_SLP_IRAM_OPT` | About 7.4 KiB with the current Wi-Fi IRAM options off; selects additional sleep/PHY options | Off. |
| `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` | S3 CPU retention about 8.58 KiB, plus cache-tag retention up to 9 KiB; IDF quotes about 650 uA | Explicitly off to protect the dynamic internal-heap margin. |
| `ESP_PHY_MAC_BB_PD` | About 4 KiB Wi-Fi / 5.3 KiB combined retention | Off. |

`ESP_SLEEP_GPIO_RESET_WORKAROUND=y` is retained; it selects the GPIO sleep
configuration path, so the board pins explicitly disable sleep selection to
keep their active directions/pulls/routes. Flash and PSRAM leakage workarounds
and the MSPI pull-up requirement remain enabled. The baseline uses octal PSRAM
XIP for instructions/rodata with 32 KiB instruction / 64 KiB data cache. Although
`PM_SLP_SPIRAM_HALFSLEEP_ENABLED` is enabled, this IDF version's S3 octal-PSRAM
half-sleep entry/exit functions are empty; no half-sleep saving is claimed.

Watchdog settings are unchanged: the baseline interrupt watchdog is 800 ms on
both CPUs, and the task watchdog is 5 s checking both idle tasks, with panic off
and the hardware timer-group backend (`ESP_TASK_WDT_USE_ESP_TIMER=n`). The S3
watchdog's APB clock gates during hardware light sleep; IDF bounds its RTC wake
guard using the sleep duration plus timeout. No watchdog timeout was extended
or disabled to make sleep pass. Entry/exit callbacks are bounded and do not
wait on a task. Watchdog behavior still needs the real-board gate below.

The final successful board-target build produced these map/type measurements
(`firmware/build/orb.map`); retain the map with the completed build receipt:

| Measurement | Bytes | Interpretation |
| --- | --- | --- |
| `orb_light_sleep.c` internal static payload | 142 (33 data + 109 BSS) | Includes the 92-byte static policy mutex; no IRAM text. |
| IDF PM callback support versus trunk | +16 static bytes | PM implementation data +8 and BSS +8. |
| New model notifier registry | 16 static bytes | Map sum of internal data/BSS sections; no new task or queue. |
| Link / voice eligibility fields | +2 / +1 static bytes | Object map payload deltas versus trunk. Sound static payload is unchanged. |
| PM lock + two callback registrations | 88 heap payload bytes | ELF type sizes: 56-byte PM lock + two 16-byte callback nodes; allocator overhead is not measured. |
| Whole-image IRAM text | 85,091 -> 84,919 | Includes all simultaneous lane changes; not a controlled option A/B. |
| Whole-image DRAM data / BSS | 30,003 -> 30,035 / 50,640 -> 50,816 | Includes all simultaneous lane changes; do not attribute the delta to an individual config option. |

Disabling CPU power-down avoids the source-defined 8,800-byte S3 CPU-retention
allocation (549 rows of 16 bytes plus header), as well as its cache-tag buffer.
Those are dynamic allocations and therefore do not appear as reclaimed map
BSS. The current cache dimensions suggest a further 6,160-byte tag allocation,
but that figure is source-derived, not a captured heap delta. No option was
enabled without a map cost, and no dynamic heap-floor improvement is claimed.

## Poll/link/voice/model/sound file report

| File | Change |
| --- | --- |
| `firmware/main/orb_idle_policy.h` | Eligibility-only minimal policy; 100/1000 ms motion budget; independent button-deadline cap; raw face-down-exit fast sampling. |
| `firmware/main/orb_input_service.c` | Minimal physical IMU path returns before senses, touch, devtools and application publication; delivers power IMU under its existing lock. |
| `firmware/main/orb_input_service.h` | Sample result reports the required next poll interval. |
| `firmware/components/orb_shell/orb_shell_power.c` | Opt-in sleep IMU seam retains detector/clock/lock logic and only presents a changed revision; ordinary IMU behavior is preserved. |
| `firmware/components/orb_shell/include/orb_shell.h` | Declares the eligible dark IMU seam. |
| `firmware/main/tests/host/test_idle_policy.c` | Tests eligibility cadence, button deadlines and raw exit sampling, preserving existing contact-lifetime assertions. |
| `firmware/main/tests/host/test_input_modes.py` | Runs production input source and asserts minimal polls do not call app/touch/senses paths. |
| `firmware/main/tests/host/test_calm_imu_sleep.py` | Runs production calm IMU/lock functions with the real detector; steady Sleep samples make zero presentation calls, while pickup/face-down changes present once. |
| `firmware/components/orb_power/tests/host/test_imu_sleep_traces.c` | New synthetic 10 Hz Sleep traces cover dt-derived filtering, 150/200 ms dwells, 250/251 ms gaps, short pulses and 1 Hz face-down exit resumption. |
| `firmware/components/orb_link/include/orb_link.h` | Eligibility setter for local outgoing queue cadence. |
| `firmware/components/orb_link/link_radio.c` | Locked eligibility/cadence and latched prompt; exit immediately wakes the existing link queue owner. |
| `firmware/components/orb_link/link_radio.h` | Private cadence and prompt accessors. |
| `firmware/components/orb_link/orb_link.c` | Uses dynamic model-pump interval and consumes prompt without delaying existing transport deadlines. |
| `firmware/components/orb_link/tests/host/test_radio.py` | Production radio fixture covers 1 Hz eligibility, 50 ms restoration, quick wake/sleep and unchanged keepalive negotiation. |
| `firmware/components/orb_link/tests/host/Makefile` | Links the new production model registry in v1.1/memory fixtures. |
| `firmware/components/orb_link/tests/host/test_delivery.py` | Links the registry and supplies the new radio cadence seam to its extracted model-pump fixture. |
| `firmware/components/orb_link/tests/host/test_home_dispatch.py` | Links the registry in its direct model build. |
| `firmware/components/orb_link/tests/host/test_devtools.py` | Mocks/observes the notification that wakes the dark parked command task; rejected requests do not notify it. |
| `firmware/components/orb_home/tests/host/Makefile` | Links the production registry in the Home byte-exact wire fixture. |
| `firmware/components/orb_voice/include/orb_voice.h` | Eligibility and link/input wake hooks. |
| `firmware/components/orb_voice/orb_voice.c` | Eligible idle notification wait; model, reply, speech-stop/completion and physical BOOT producers wake it without losing a give-before-take notification. |
| `firmware/components/orb_voice/voice_wait.h` | Pure decision for event-only versus ordinary/recording polling. |
| `firmware/components/orb_voice/tests/host/test_voice.c` | Pure wait-policy cases for capture, transcription, send, speech, BOOT and cancellation/redo. |
| `firmware/components/orb_voice/tests/host/test_events.c` | Speech start explicitly verifies a worker wake. |
| `firmware/components/orb_voice/tests/host/test_events.py` | Links the production model notifier registry in its existing extracted-callback fixture. |
| `firmware/components/orb_model/orb_model.c` | One enqueue-completion notifier seam outside the model mutex; existing file-size debt does not grow. |
| `firmware/components/orb_model/include/orb_model.h` | Single service-owner notifier registration API. |
| `firmware/components/orb_model/orb_model_voice_notify.c` | Separate callback registry with an independent short lock; registration also wakes startup work. |
| `firmware/components/orb_model/orb_model_voice_notify.h` | Private enqueue-completion declaration. |
| `firmware/components/orb_model/CMakeLists.txt` | Builds the registry in firmware. |
| `firmware/components/orb_model/tests/host/test_model.c` | Tests startup/enqueue/full-queue notifications and re-enters the model from the callback to prove unlocked delivery. |
| `firmware/components/orb_model/tests/host/test_memory.c` | Isolates allocation tests from the separately tested notifier registry. |
| `firmware/components/orb_model/tests/host/Makefile` | Links the production registry in the model host suite. |
| `firmware/components/orb_sound/orb_sound.c` | Preference task becomes notification/debounce driven; volume/preferences and Silent changes notify it. |
| `firmware/components/orb_sound/orb_sound_save.h` | Pure generation/deadline state machine for debounce and racing commits. |
| `firmware/components/orb_sound/tests/host/test_sound.c` | Tests idle/debounce boundary, repeated edits and a change racing a commit. |

Integration, board transport, runtime PM/GPIO, devtools, Time and simulator
registration changes are additionally listed in the lead's completion report.
No screen layout, private-content redaction or safe-action allowlist changes
are intended by this lane.

## Host evidence

The following passed for this lane (ASan/UBSan where the existing suite enables
them):

| Check | Result |
| --- | --- |
| Standalone `test_imu_sleep_traces` | Pass: 10 Hz timing/filter/gap and face-down resumption traces. |
| Standalone `test_idle_policy` | Pass: existing states/contacts plus new eligible budgets. |
| `test_input_modes.py` | Pass: existing bias/mode behavior plus minimal-path call exclusions. |
| `test_calm_imu_sleep.py` | Pass: 20 steady Sleep samples skip presentation, state changes present once, normal IMU presentation remains unchanged. |
| Voice host `make -j4 test` | Voice 2511 checks; speech/DSP 637; request ownership 109; callbacks 57. |
| Model host `make -j4 test` | Model 727 checks plus 16,000 concurrent updates/coherent snapshots; allocation fixture 7206 checks. |
| Sound host `make -j4 test` | Sound 2012 checks; voice lease 35; preference storage 140. |
| Link `test_radio.py` | 46 checks, including eligible cadence and negotiation. |
| Complete Link host `make -j4` | Pass: core 28983, v1.1 891, memory 66705, callbacks 34211, radio 46, delivery 146, outgoing 436, Home dispatch 234, devtools routing 19/58 (compiled off/on). |
| Home independent host fixtures | Wire 1488, PSRAM 492, payload 524366, ESP payload 164 checks pass. The aggregate `make` also hits two pre-existing unrelated curly-versus-ASCII apostrophe expectations in `test_home.c:35` / `test_model.c:296`; production Home code and those expectations were left unchanged. |
| Mutation: remove minimal early return in a temporary copy | New production-input fixture fails at app/touch exclusion assertion. |
| Mutation: remove enqueue callback in a temporary registry copy | Model host test fails at notification-count assertion. |
| Mutation: route minimal calm IMU through ordinary presentation in a temporary copy | New calm fixture fails at the zero-presentation assertion. |

Mutation runs did not change production source. The new 10 Hz trace is a
compatibility check for the existing detector, which already derives alpha
from dt; no threshold or detector-code change was needed. Nice was requested
on each test/build command; this sandbox reported `setpriority: Operation not
permitted` while still executing the bounded commands. The lead/root owns
the final full simulator, guardrail and IDF build results.

## Real-board gates for the lead

1. Regenerate/review the configuration and build before installing on the
   single dev board. Verify internal minimum heap stays at least 24 KiB during
   reconnect, speech, alarms, navigation and OTA; static map figures alone are
   insufficient.
2. On USB, `scripts/orb-dev.sh --wifi power` must report USB blocked and no new
   sleep entries. For a deliberate Wi-Fi bench only, use
   `scripts/orb-dev.sh --wifi lightsleep usb on`; inspect eligibility, blocking
   reason, actual entry count, slept microseconds and IDF lock residency. Use
   `lightsleep usb off` afterwards; reboot must clear the override. No serial
   transport reliability claim is made while this exception is enabled.
3. Repeat on battery with the panel physically off. Compare whole-device
   current and the PM dump before/after. `spi_master` must no longer retain its
   prior 80–97% acquisition through panel sleep. Capture actual sleep fraction
   and remaining driver wake cadence; do not infer single-digit current from
   the host tests.
4. Test short and long touch/PWR/BOOT contacts, a held button and bounce at
   sleep entry, and queued press/release edges. The first dark/redacted contact
   must be consumed until release. The following deliberate action must work;
   no approval, send or unlock may occur from wake or synthetic input.
5. Test flat/dock pick-up, noise, confirmed face-down with a slow turnover,
   cooldown and rejected/invalid IMU samples. Capture labelled board traces;
   the host traces are synthetic.
6. Keep Wi-Fi/hub connected through several negotiated 135 s silence windows;
   capture pings, attention delivery, reconnects and wake latency. Test timer,
   alarm, reminder and snooze deadlines while asleep, including a deadline
   between the periodic slots and a wall-clock/time-zone correction.
7. Start voice/speech, OTA and a game before or during a wake and confirm
   eligibility revokes, the 20 ms interactive profile/gyro returns, and neither
   task nor interrupt watchdog fires. Check panel/touch/I2C/PA signals and rails
   for glitches. Capture the real-panel sleep/wake screenshots required by the
   repository release gate; host-only rendering cannot pass that gate.


## Integration, board, Time and tooling file report

Paths below are relative to the repository. The preceding table covers the
input/link/voice/model/sound changes individually.

| File | Change |
| --- | --- |
| `firmware/main/main.c` | Main owns eligibility and the shared dark wake slot, exact Time waits, physical wake replay and immediate profile restoration. Uses the latest sampled VBUS, including an immediate policy refresh after PMU reads. Dark UI drains queued buttons before returning and retains the full view mailbox until ordinary presentation resumes. |
| `firmware/main/orb_power_services.c`, `.h` | Live OTA/voice/alert/game/inhibitor checks, radio hints and deadline service; no LVGL presentation in the service policy query. |
| `firmware/main/orb_panel_policy.c`, `.h` | Existing display-fence adapter extracted intact to keep main within its file/function guardrails. Owns panel acknowledgement/readiness; no change to privacy or safe-frame admission. |
| `firmware/main/orb_light_sleep.c` | Boot lock/configuration, fail-closed wake preflight, read-only idle veto, paired GPIO mode switching, real input replay and actual sleep callback statistics. |
| `firmware/components/orb_power/include/orb_light_sleep.h`, `orb_light_sleep_policy.h` | Runtime API, pure eligibility/lock/held-level state machines and RAM-only development override declaration. |
| `firmware/main/orb_boot.c` | Initializes the awake lock before requesting automatic PM sleep. |
| `firmware/main/Kconfig.projbuild` | Default-enabled `ORB_LIGHT_SLEEP`, dependent on PM/tickless idle; enables paired PM callbacks. |
| `firmware/sdkconfig.defaults` | Enables runtime sleep, explicitly disables the five optional RAM-cost features and retains GPIO/flash/octal-PSRAM leakage safeguards. |
| `firmware/main/CMakeLists.txt` | Builds runtime/panel modules and declares GPIO/PM dependencies. |
| `firmware/components/orb_board/board_ws175c.c` | Lazily acquires LCD SPI before commands/pixel windows; drained release seam removes the asleep bus acquisition. |
| `firmware/components/orb_board/board_panel_power.c` | Confirms asleep readiness only after successful SPI release; retries safely if release fails. |
| `firmware/components/orb_board/board_buttons.c`, `include/orb_board.h` | Sleep-exit physical pin capture into the existing edge queue; does not inject gestures or yield within the sleep callback. |
| `firmware/components/orb_board/tests/host/test_button_edges.c` | Verifies physical sleep-exit held/release snapshots use the existing queue without invoking the yielding ISR notifier. |
| `firmware/components/orb_board/tests/host/test_panel_power.py`, `test_panel_timing.py` | Production-source bus release/reacquire, readiness and failure regression checks. |
| `firmware/components/orb_display/orb_display.c`, `include/orb_display.h` | Serialized eligible-dark callback and notification-only wait; no independent UI deadline or full LVGL timer handling while eligible/dark. Processes pending real touch and resumes ordinary handler on wake; explicit queued developer callbacks receive one ordinary handler pass without restarting periodic dark timers. |
| `firmware/components/orb_display/orb_display_touch.c`, `.h` | IRQ-only dark idle, existing 16 ms held polling, real pending-input reads, initialization readiness and non-yielding physical wake notification. |
| `firmware/components/orb_display/tests/host/test_touch_poll.py`, `test_ui_fairness.py` | Actual source tests for parked timers, real touch/release handling and ordinary cadence preservation. |
| `firmware/components/orb_time/orb_time.c`, `include/orb_time.h` | Exact next timer/snooze/alarm/reminder deadline API using monotonic and fractional wall time; saturates unrepresentable timer deadlines. |
| `firmware/components/orb_time/orb_time_clock.c`, `orb_time_internal.h` | Existing UTF-8 helper moved unchanged to keep the time-service file below its size cap. |
| `firmware/components/orb_time_app/tests/host/test_time.c` | Deadline tests: pause/resume, fractional clocks, reminders, snooze, exact expiry and overflow. |
| `firmware/main/tests/host/test_sleep_service.py` | Production dark-UI admission and deadline ordering, including a PWR tap released before UI delivery. |
| `firmware/components/orb_power/tests/host/test_light_sleep_policy.c` | Every blocker, all inhibitor bits, USB exception boundaries and lock/held-level transitions. |
| `firmware/components/orb_power/tests/host/test_light_sleep_runtime.py` | Production runtime under GPIO/PM stubs; enabled/disabled configurations, setup failure classes, real entry/exit ordering, rejected sleep, held levels, counters and sticky restoration failure. |
| `firmware/components/orb_devtools/orb_devtools.c` | Registers Wi-Fi command, wakes command task on network requests and reduces eligible console polling to 1 Hz. Delegates power diagnostics to keep existing guardrail debt from growing. Queued UI commands request one explicit handler pass so a parked display still accepts wake/screenshots/bench requests. |
| `firmware/components/orb_devtools/orb_devtools_power.c`, `.h` | Power receipt with eligibility/blocker/readiness/override/entries/slept-us plus IDF mode/lock dump; strict RAM-only `lightsleep usb on/off` executor. |
| `firmware/components/orb_devtools/orb_devtools_wire.h` | Exact paired-wire allowlist and string serialization for the new command. |
| `firmware/components/orb_devtools/CMakeLists.txt` | Builds power diagnostics and declares the public power API dependency. |
| `firmware/components/orb_devtools/README.md` | Wi-Fi-only USB bench procedure and truthful power receipt fields. |
| `firmware/components/orb_devtools/tests/host/test_async.py`, `test_wake.py` | Verifies queued UI work requests its one-shot service wake, keeps the ordinary guarded wake path, and retains existing timeout/cancellation cleanup. |
| `firmware/components/orb_devtools/tests/host/test_wire.c`, `test_power.py` | Strict command validation, no setter on malformed commands, report fields and remote parser checks. |
| `scripts/orb_dev_remote.py` | Strict command shape and bounded timeout for `lightsleep usb on/off`. |
| `docs/protocol.md` | Documents the development-only wire command before implementing either side. |
| `hub/orb_hub/devtools.py`, `hub/tests/test_devtools.py` | Narrow hub allowlist update and loopback TLS command/CLI/invalid-argument tests. The live hub was not changed; deploy this whitelist with the firmware before Wi-Fi bench use. |
| `sim/sim_display.c` | Simulator counterpart for the explicit UI-service notification; simulator owners already dispatch LVGL continuously. |
| `sim/CMakeLists.txt` | Registers the new/existing production host tests and the model notifier source. |
| `docs/light-sleep.md` | Implementation, static wake/lock audit, memory evidence and remaining physical release gates. |

The IDF architecture follows the [ESP32-S3 sleep-mode guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/system/sleep_modes.html): modem sleep plus automatic light sleep retains connectivity. Implementation details were checked against the installed ESP-IDF 6.1 source, including the shared GPIO interrupt-type field and the PM callback ordering.

## Simulator visual evidence

The display/input reviewer read `docs/agent-briefs/wave2/design-gate.md` and
`docs/hig.md` §§1–9, then opened the following generated PNGs after the complete
simulator run. These are actual simulator renders, not real-panel screenshots.
This change intends no layout, artwork, typography or redaction change.

| Surface | Default evidence | AX3 + Bold Text + Reduce Transparency evidence |
| --- | --- | --- |
| Face | `sim/build/shell-face-default.png` | `sim/build/shell-face-ax3-bold-rt.png` |
| List, showing a pending save | `sim/build/shell-sessions-pending-default.png` | `sim/build/shell-sessions-pending-ax3-bold-rt.png` |
| Approval card | `sim/build/shell-approval-default.png` | `sim/build/shell-approval-ax3-bold-rt.png` |
| Sleep/wake preview | `sim/build/power-default/sleep-500.png`, `sim/build/power-default/wake-500.png` | `sim/build/power-ax3-bold-rt-rm/wake-150.png` also includes Reduce Motion |

The checklist below records observations per surface. “Unchanged” describes
the scope of the code change, not a claim that a screenshot proves target
geometry, contrast ratios or animation physics.

| Gate heading | Face | List | Card | Sleep/wake |
| --- | --- | --- | --- | --- |
| Two layers | Pass: plain clock/complications; Apps is the separate control. | Pass: plain content rows and ordinary switch controls. | Pass: text stays on black; glass belongs to the two actions. | Pass: clock/count only; no glass. |
| Canvas | Pass: black, no observed gradient/glow. | Pass: black, flat rows, no observed gradient/glow. | Pass: black, no observed gradient/glow. | Pass: sleep is black; wake uses black behind white/orange ink. |
| Round-first | Pass: clock, curved date and complications fit the circle. | Pass: visible rows and rim scroll indicator fit; AX3 labels remain inside rows. | Pass: heading, wrapped copy and both actions fit. | Pass: centred clock/count stay inside the round safe area. |
| Sizes | Pass visually: AX3/Bold labels remain legible without clipping; target geometry unchanged. | Pass visually: AX3/Bold names and pending label fit; target geometry unchanged. | Pass visually: AX3/Bold copy wraps and action labels fit; target geometry unchanged. | Pass visually: larger/bold clock/count remain unclipped; no targets. |
| Colour | Pass visually: meaningful date/complication/status accents and readable labels. | Pending controls/labels are visibly dimmed by their existing disabled state; no new colour treatment. | Pass visually: orange identity, white copy, two untinted actions. | Pass visually: white time and orange hand/count; state also has text/symbols. |
| Approvals & agents | N/A: no approval or agent-work spinner. | Pass: static `Saving…` label; no indeterminate work spinner. | Pass: Deny remains present; neither action is tinted or presented as a default. | N/A: no private agent content or approval action appears. |
| Motion/accessibility | Default and AX3/Bold/RT endpoints inspected; animation curves require temporal evidence. | Default and AX3/Bold/RT inspected; pending state remains legible as a state. | AX3 wraps copy cleanly; RT controls remain distinct; no decision is hidden. | Black sleep and settled wake inspected; the RM 150 ms endpoint is legible and redacted. |
| Navigation alternatives | Apps remains visible; physical mappings unchanged. | No redundant navigation chevrons added; physical/rim alternatives unchanged. | No redundant Back control; existing paging dots retained. | N/A: redacted information surface; physical wake semantics unchanged. |
| Feels Apple-made | Restrained clock hierarchy; no new visual defect observed. | Clear provider identities and repeated row geometry; no new clipping observed. | Clear question/copy/actions hierarchy; no new visual defect observed. | Quiet, minimal wake presentation without private text. |

No introduced visual defect was identified in this static review. The pending
list's disabled-state contrast was not numerically measured; screenshots also
cannot establish computed-backdrop contrast, hit areas, spring behaviour,
physical OLED colour or wake latency. The independent lead review and
real-panel sleep/wake captures remain required before release.


## Final verification receipt

- Required simulator configure/build: passed (`cmake -S sim -B sim/build -G Ninja`, `nice cmake --build sim/build -j4`).
- Required full CTest: **1,156 / 1,156 passed**, 115.01 s, `nice ctest --test-dir sim/build -j4`. This includes the production-source runtime, UI/service, IMU, input, devtools and Link host fixtures plus the existing rendering/parity tests.
- Required `python3 scripts/guardrails.py`: passed, no regressions; existing `file_size=13` debt unchanged.
- Required `nice scripts/idf.sh build`: passed, Ninja `-j4`, ESP-IDF 6.1; generated app is unsigned and was not installed.
- Hub devtools: **96 passed**, using this worktree via `PYTHONPATH="$PWD/hub"` and throwaway loopback TLS fixtures. No live hub access.
- Ruff check/format for all changed/new Python files and `git -c maintenance.auto=false diff --check`: passed.
- Supplemental devtools host suite passed; wake fixture now verifies the explicit UI-service wake, including queue rejection and timeout cleanup. Mutation checks are described above.
- Final targeted physical-capture/runtime/UI/service/devtools regression rerun: 5 / 5 passed after extending the button capture fixture.
- No commits, pushes, serial/board access, signing-key access or owner configuration changes were made.

The final map contains 84,919 bytes IRAM text, 30,035 bytes DRAM data and
50,816 bytes DRAM BSS. These are static image sections, not live free heap.
The runtime module remains 142 bytes of internal static payload and no IRAM
text. Final frame-size inspection of the generated objects found `ui_tick`
2,800 bytes (trunk comparison 3,904), `orb_power_services_policy` 2,448,
`orb_power_services_apply` 800 and `app_main` 272; these are compiler stack
frames, not measured runtime high-water marks. The main task retains its
existing 6 KiB allocation, and the device stack/heap gates still apply.

The lead must regenerate any saved `sdkconfig` from `sdkconfig.defaults`,
review the five explicit `n` values and retained workarounds, and deploy the
narrow hub devtools whitelist update before using the new Wi-Fi command.
Current draw, the 24 KiB runtime heap floor, GPIO/panel/PA electrical behavior,
watchdogs, physical wake reliability and a >135 s connected idle run remain
unverified. USB insertion is recognized by the existing 1 Hz PMU sample;
the latest sampled VBUS value revokes eligibility immediately.

## 2026-10-31 power follow-up receipt

The follow-up removes empty button polling, reduces eligible USB-console
polling to 5 s, preserves system inhibitors under display lock, and corrects
the discovery-only mDNS attribution above. No alarm/approval deadline changed.
The [file-by-file report](power-fix-report.md) records all seven peer-review
findings, lit-frame handoff, initial-hello reconnect fix, mutation evidence and
remaining board gates.

- Required simulator configure/build passed with `-j4`.
- Final full `nice ctest --test-dir sim/build -j4 --output-on-failure` passed
  **1,167 / 1,167** tests in **242.13 s**.
- Required `nice scripts/idf.sh build` passed after the last firmware edit;
  generated configuration enables automatic light sleep, PM and tickless idle.
- Guardrails passed; existing `file_size=13` debt unchanged. Ruff, Python
  formatting and whitespace checks passed. Hub Link/devtools loopback tests:
  **132 passed**.

These are host/build results. No board, serial, flashing, live-hub or power
measurement was performed in this follow-up. Updated residency, entry rate,
single-handshake boot and blink-free panel transitions require lead captures.
## Codec analog idle suspend (2026-10-31)

The audio owner now keeps the existing 100 ms silent tail/transport gate and
adds a single deadline at 10 s since the last cue, speech or capture activity.
At that deadline, with no pending/active voices or speech, it powers down both
codecs through checked register writes. The worker then waits for a producer
notification indefinitely. Preference notifications do not restart the idle
deadline. ES7210 rev21 p10 note 4 recommends register shutdown before stopping
the input clock. The 10 s transition therefore briefly restores transport
clocks under mute/PA-low, writes both off sequences, then stops/detaches clocks
again; success requires both the register writes and final transport stop.
Capture takeover retains its existing exclusive lease and destroys
the idle playback pair; microphone open recreates/configures it at 16 kHz.
Speech playback restores the duplex 32 kHz profile and both mic channels.

| State | ES8311 | ES7210 | External state |
| --- | --- | --- | --- |
| Playback/speech | DAC enabled, configured volume/DRC/ramp | MIC1/2 ADC paths and microphone bias enabled for speech interruption | I2S/MCLK on; PA enabled only at nonzero volume |
| Short idle, 100 ms to 10 s | `0x31[6:5]=11` mute; analog profile retained | Analog/mic bias profile retained | PA low; RX/TX disabled; MCLK detached |
| Analog suspended, 10 s onward | Vendor suspend: `0x0E=FF`, `0x12=02`, `0x14=00`, `0x0D=FA -> FC`, `0x00=00 -> 1F`, `0x01=30 -> 00`, `0x02=10 -> 00`; ADC/DAC gains zero, `0x15=00`, `0x45=00` | Vendor stop: `0x47..4C=FF`, `0x40=C0`, `0x01=7F`, `0x06=07` | PA/clocks off; ALDO1 and retained digital supplies on; I2C/register retention still powered |
| Wake in progress | Saved reset/clock/analog/gain values restored, still muted | Saved clock/analog/mic power values restored; `0x00=71 -> 41` restarts the digital path | Clocks restored first; PA low through full silent DMA prime and 10 ms settling allowance |
| Failed wake | Snapshot retained until success; any partially restored power state invalidates the completed-off flag | Same; a later suspend rewrites every off domain | PA low; transport stopped; worker logs and retries after a notification-bounded 100 ms wait |

This is the lowest **vendor-supported register sequence**, not zero codec
current. Analog blocks, microphone bias/PGAs/modulators/references, VMID and
internal clocks are targeted for shutdown; supply leakage, I2C and register
retention remain. ES8311 rev10 p18 gives a contradictory description of
`0x0D[2]` (`PDN_VREF`): its table says 1 enables the reference, whereas the
vendor suspend ends at FC and Linux uses an inverted Vref power control. The
implementation retains FC; it does not claim an independently proven internal
reference state or substitute F8 based on that ambiguous table.

Driver evidence is pinned locally in
`firmware/managed_components/espressif__esp_codec_dev` (1.5.11):

- `device/es8311/es8311.c:288` (`es8311_suspend`), `:313` (`es8311_start`),
  `:383` (mute), `:653` (enable). Start omits restoration of divider `0x02`
  and overwrites ramp `0x37`; snapshot restoration avoids both regressions.
- `device/es7210/es7210.c:296` (start), `:313` (stop), `:380` (enable),
  `:405` (constructor/open). Construction powers analog even before an opened
  wrapper enables it. Explicit shutdown also runs on construction unwind and
  deletion, including an unopened microphone wrapper.
- `esp_codec_dev.c:147` (open), `:371` (mute), `:503` (close). Close ignores
  codec/data disable errors; open marks wrapper state before hardware success;
  wrapper mute and ES8311 mute hide write failures. Idle suspend retains the
  wrappers and uses checked raw writes instead. Driver-managed PA is disabled
  with `pa_pin=-1`; only the board enables GPIO46 after successful restoration.

Every touched register is saved before the first shutdown write; partial
shutdown never replaces that snapshot. Unchanged serial format, limiter, ramp,
mic PGA/digital gains and sound preferences survive. Volume edits made while
suspended update the saved output gain. Zero/nonzero transitions require a new
silent prime. The worker resumes before rendering/dequeuing a cue or reading
speech; a failed resume therefore cannot consume its opening samples. Real
timer/alarm cues remain admissible during output recovery, and active alerts
retain their existing loop until explicitly stopped. A TX transfer fault can
still lose samples already consumed by the mixer; no sample-integrity claim
is made for a broken transport. Recovery does not change Silent, Always On,
quiet-hours or zero-volume policy.

ALDO1 stays on. In the [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf),
ALDO1 is A3V3 and supplies all ES7210 rails, including VDDP, plus ES8311 AVDD.
ES8311 digital supplies and the shared SDA/SCL pull-ups R23/R24 remain on
VCC3V3. [ES7210 rev21 p9](https://files.waveshare.com/wiki/common/ES7210_DS.pdf)
limits digital inputs to VDDP + 0.3 V; no unpowered-I/O tolerance is documented.
Cutting ALDO1 while I2C remains pulled high presents a back-power/overvoltage
path even with I2S off and the panel asleep. Stopping MCLK alone does not make
the rail safe to cut. `axp2101_keep_rails` only clears enables and is not a
reversible runtime rail API. No PMU/panel/sleep-policy changes are made here.

The [ES8311 datasheet](https://files.waveshare.com/wiki/common/ES8311.DS.pdf)
and [Linux ES8311 driver](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/es8311.c)
were also checked. The added 10 ms silent settling allowance, successful host
ordering tests and lack of a driver startup delay do **not** prove pop-free
audio or a <60 ms physical cue start. No board, sound, serial, current, heap
floor or power-residency measurements were possible. Required real-board gates
remain current before/after 10 s, first-cue waveform/latency, repeated suspend
cycles, microphone first-frame quality, speech interruption and fault recovery.

Host evidence for this codec change:

- The production board fixture passes 54,884 checks with ASan/UBSan: all 13
  constructor stages, all 54 suspend operations, every resume operation,
  partial/short writes, retry/re-suspend, full register restoration, zero-volume
  transitions, and clock -> restore -> silent prime -> settle -> unmute -> PA
  ordering. Nine temporary board-source mutations fail runtime assertions,
  including removal of the shutdown clocks and the codec power writes.
- The production worker fixture passes 13 deterministic scheduling/fault cases:
  exact 10 s deadline, notification/deadline preservation, a new cue resetting
  the deadline, cue/speech/mic wake, idle/suspend/resume/open/TX/volume failures,
  unread cue/speech openings on wake failure, and alerts during recovery.
- Sound core/policy tests pass 2,023 checks, voice lease tests 35 and preference
  storage tests 140. Existing mic profile tests pass all ten read/write failure
  stages. Nine worker/core mutations fail runtime assertions, including
  rendering before wake, rejecting recovery alerts, skipping retry, ignoring
  volume failure and clearing an active alarm on lease release.
- The board fixture has a one-line CTest registration in `sim/CMakeLists.txt`;
  the worker and sound core remain in their existing CTest registrations.
- Final simulator configure/build passes; all **1,165 / 1,165 CTests pass**
  (`nice ctest --test-dir sim/build -j4`, 233.60 s), including the new board
  fixture and existing render/parity, privacy, contact and voice coverage.
- `nice scripts/idf.sh build` passes with `ninja -j4`; the final image is
  unsigned, 0x2d0000 bytes, with 53% free in the smallest app slot. The sandbox
  initially blocked dependency-manager process inspection; the authorized build
  passed outside that restriction. Guardrails retain `file_size=13` with no
  regressions; Ruff and whitespace checks pass. No firmware was installed.


## 2026-10-31 automatic-sleep peer review follow-up

This receipt concerns this worktree's code and installed ESP-IDF 6.1 source,
not the peer's older line numbers. The owner's reported 835 entries / 29.7 s
slept in about 33 s with Wi-Fi up predates these changes; it was not remeasured
here. No board, serial, flashing, live hub or power-meter access was performed.
No display content, layout, redaction, gesture intent or power-down configuration
was changed.

| Finding | Disposition and source evidence |
| --- | --- |
| 1 — one-shot fault | Fixed. Setup, GPIO restore, callback registration, PM configure and PM-lock failures remain fail-closed but retry automatically. One attempt per service deadline; delays are 1, 2, 4, 8, 16, then 30 s repeatedly. There is no permanent retry-exhausted latch. PM ownership is committed only after successful acquire/release. IDF `components/esp_pm/pm_locks.c:97–137` returns errors without changing ownership. Enter/exit callback nodes are registered separately and remembered because `pm_impl.c:245–301` can install enter before failing exit allocation. |
| 2 — cache/FromISR | Crash/misuse claim rejected; contract clarified. Public PM callbacks surround `esp_light_sleep_start()` (`pm_impl.c:865–905`); `sleep_modes.c:1419–1441,1739–1741` restores flash/cache before returning. `esp_pm.h:290` documents idle-task context and no blocking. FreeRTOS suspends scheduling for tickless sleep (`tasks.c:4373–4400`). Its FromISR notification defers ready-list changes and yield (`tasks.c:6194–6241`), unlike normal task notifications; callbacks must never request an ISR yield. Keep the existing flash/PSRAM/CPU power-down settings off. The IRAM wrapper alone does not make these public callbacks cache-off hooks. |
| 3 — masked release | Fixed for a release between the first wake sample and restored edges: a second real-level sample is queued regardless of whether the contact remains active. This also runs after a failed restoration, and successful retry samples the current level before eligibility returns. Board sampling and enqueue are serialized against the other core's ISR. No PWR gesture, duration or intent is fabricated. |
| 4 — I2C | Rejected. S3 `components/soc/esp32s3/include/soc/clk_tree_defs.h:348` defaults I2C to XTAL. `esp_driver_i2c/i2c_common.c:279` therefore keeps `ESP_PM_NO_LIGHT_SLEEP`, acquired/released around transactions in `i2c_master.c:687–688,733–734`. This matches the owner's `I2C1 NO_LIGHT_SLEEP` dump. Even APB_MAX excludes LIGHT_SLEEP in `pm_impl.c:527–537`; the peer's APB premise is also incorrect. |
| 5 — confirmation | Rejected as stale. This branch already includes `ORB_VOICE_CONFIRMING` and `ORB_VOICE_ERROR` in both voice-work predicates in `orb_power_services.c`. `sleep_waits` compiles those production predicates and fails when CONFIRMING is removed. Confirmation intentionally has no expiry; see `orb_voice_core/README.md`. |
| 6 — USB | Retained as a documented bounded limitation, not a new regression. Main samples VBUS every second and updates eligibility immediately afterward. There is no USB insertion wake source in this board contract. Reducing observation latency would need a separate PMU/USB wake design and board verification. |
| 7 — zeroed GPIO model | Fixed. Removed the redundant pure GPIO state model and its disconnected tests. The real armed state is atomic and participates directly in the skip veto, including calls from the other core's idle hook (`pm_impl.c:1034–1052`); callbacks still restore edges on rejected/too-short attempts. Fault state uses the independent atomic fence. |
| 8 — nested configure | Fault-path reconfiguration removed. Policy and recovery run after releasing LVGL; only shell/panel snapshot capture stays under that lock. The policy mutex still serializes runtime setup. No IDF reverse dependency on LVGL/Orb was found, so the alleged deadlock was not substantiated. |

### Changed files

| File | Change |
| --- | --- |
| `firmware/main/orb_light_sleep.c` | Retry/deadline/fault state, staged setup, actual PM ownership, real atomic armed veto, second physical sample and removal of fault-path configure. |
| `firmware/components/orb_power/include/orb_light_sleep.h` | Fault/report fields and explicit idle/service callback contract. |
| `firmware/components/orb_power/include/orb_light_sleep_policy.h` | Remove the disconnected GPIO gate model; pure admission/lock policy retained. |
| `firmware/main/orb_power_services.c` | Release LVGL after snapshot capture and before PM policy/recovery. |
| `firmware/components/orb_board/board_buttons.c` | Serialize timestamp/level capture and queue insertion. |
| `firmware/components/orb_board/include/orb_board.h` | Document sleep/recovery snapshot contract. |
| `firmware/main/main.c` | One comment explaining why the existing FromISR notification remains; no behavioral hook. |
| `firmware/components/orb_devtools/orb_devtools_power.c` | Add fault/error/retry/recovery report fields. |
| `firmware/components/orb_power/tests/host/test_light_sleep_runtime.py` | Production-adapter regression/fault coverage in four build configurations. |
| `firmware/components/orb_power/tests/host/test_light_sleep_policy.c` | Remove tests for the deleted, disconnected GPIO model; actual veto tested in runtime fixture. |
| `firmware/components/orb_power/tests/host/test_power_services.py` | New production service lock/admission regression. |
| `firmware/components/orb_board/tests/host/test_button_edges.c`, `.py` | Real recognizer, lifetime-consumed release and cross-core snapshot-order regression. |
| `firmware/components/orb_devtools/tests/host/test_power.py` | Healthy/fault/deadline report assertions. |
| `sim/CMakeLists.txt` | Register `power_services`; other regressions extend existing registrations. |
| `docs/light-sleep.md` | Current recovery contract, review dispositions, source evidence, test receipt and physical limits. |

### Recovery/report contract

`power` retains the existing eligibility/readiness/residency fields and adds
`sleep_fault=none|lock|callback|config|wake|restore`, `sleep_error` (last numeric
IDF error, retained after recovery), cumulative `sleep_failures`, `sleep_retries`
and `sleep_recoveries`, plus `sleep_retry_ms` (nonnegative remaining delay,
zero when recovered or due). Status and its 64-bit retry timestamp are copied
under the status spinlock. Retry count increases only for an actual due attempt;
repeated polls do no GPIO, PM-lock or configuration work before that deadline.

A successful recovery first owns `orb_awake`, completes every missing callback
registration/configuration stage, restores GPIO/wake sources, and samples the
physical level; only then is the fault fence cleared and policy reconsidered.
No task, timer, allocation or blocking wait was added to the sleep callbacks.
Recovery cannot initialize a missing touch driver: false boot readiness remains
a genuine unavailable prerequisite until the existing caller supplies readiness.

### Deterministic regression evidence

- `light_sleep_runtime` compiles the production adapter in four configurations
  (sleep on/off, devtools on/off, DFS on/off). Seventeen scenarios cover transient
  lock creation/acquire/release, PM configure, skip/exit callback allocation,
  GPIO retention/arming/source and Wi-Fi setup failures; persistent capped
  backoff; exact retry boundaries; PWR/BOOT release and late contact during restoration; held
  contacts; rejected sleeps; restore failure/retry; residency counters; and
  USB exception boundaries.
- Temporary production-source mutations fail at runtime when retries, the
  second physical sample, actual armed veto, transactional PM ownership,
  callback-stage tracking, the backoff cap or late-contact sampling are removed. These checks modify
  temporary copies only.
- `board_button_edges` uses the production recognizer with ASan/UBSan. It
  verifies physical DOWN/UP lifetime consumption without SHORT/HOLD and an
  adversarial cross-core release ordering. Restoring pre-lock sampling fails.
- `power_services` asserts snapshot capture under LVGL and runtime work outside
  it, every voice phase, speech blocking and use of the latest USB sample.
  It failed before the lock-boundary fix. Existing `sleep_waits` verifies the
  confirmation predicate and fails with CONFIRMING removed.
- `devtools_power` verifies both healthy and retrying reports, numeric errors,
  counters, remaining delay and saturation at zero after a passed deadline.

### Physical limits and remaining gates

A contact already released before the first post-wake sample cannot be safely
reconstructed from the aggregate GPIO wake cause: it does not identify touch,
PWR or BOOT. S3's `esp_sleep_get_gpio_wakeup_status()` reads RTCIO status used by
a different wake setup; this lane does not rely on undocumented digital latch
history or invent a PWR press. The two samples preserve all observed first
contacts and their releases; contacts shorter than the existing debounce remain
filtered without stretching their duration. Absolute first-contact capture for
a pulse wholly before the first sample remains a real-board gate, not a proven
host result. No synthetic input path or approval/send/unlock path changed.

The lead still needs repeated physical PWR/BOOT/touch taps around sleep entry,
release during wake restoration, contact bounce and whole-contact consumption
on dark/redacted surfaces; fault-injected retry/readback; Wi-Fi continuity;
residency/current; wake latency; and runtime heap/stack evidence. There is no
power meter here, and no electrical or battery-saving claim is made from host
tests. UI screenshots are not intended to change; existing render/parity gates
exercise that constraint, and the lead owns real-panel captures.

### Build/test receipt for this follow-up

- `cmake -S sim -B sim/build -G Ninja` and the `nice` simulator build with
  `-j4` passed. Final incremental build after the header comments also passed.
- `nice ctest --test-dir sim/build -j4 --output-on-failure`: **1,169 / 1,169
  passed**, 163.62 s, including render/parity and all regressions above.
- `nice scripts/idf.sh build`: passed, ESP-IDF 6.1, `ninja -j 4 all`; unsigned
  image `0x2d0000` bytes, smallest app slot 53% free. No signing or installation.
- Generated `sdkconfig.h` enables Orb light sleep, PM callbacks and tickless
  idle; flash and CPU power-down remain disabled. The existing S3 octal-PSRAM
  half-sleep define remains enabled with empty implementation functions
  (`esp_psram/esp32s3/esp_psram_impl_octal.c:461–472`); no power-down option changed.
- `orb_light_sleep.c.obj` symbol inspection sums **177 bytes of mutable static
  data**, including the existing 92-byte mutex. This is an object-file/static
  measurement, not live internal heap, stack high-water or current consumption.
- Seven runtime behavior mutations were rejected by assertion failures;
  board snapshot-order and service-lock mutations also failed. CONFIRMING
  removal failed both the existing lease and new service fixtures.
- Guardrails: no regressions, existing `file_size=13` debt unchanged. Ruff
  check/format and `git -c maintenance.auto=false diff --check` passed.
- The sandbox refused priority adjustment for the first simulator build;
  the final incremental build, full CTest and IDF build used authorized `nice`
  execution outside that restriction, each with at most four jobs.
- No commits, pushes, board/serial access, live-state changes or owner
  configuration changes. Real-board and power-meter gates above remain open.
