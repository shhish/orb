# Delight board follow-up — 2026-10-27

Worktree `orb/w2-delight`; changes left uncommitted. No board access or flash.
This supersedes the ambient-cadence and pre-merge simulator notes in
[DELIGHT_RESULTS.md](DELIGHT_RESULTS.md), and addresses all three findings in
[delight-board.md](../docs/agent-briefs/wave2/delight-board.md).

## Root fixes

1. **Ambient redraw:** LVGL style setters invalidate even identical values.
   The 50Hz environment delivery wrote the Face's unchanged page transforms,
   and opacity changes on each 442×442 bezel arc invalidated its whole square.
   Minute shifts now write only changed coordinates. Working arcs stay fixed;
   a 32×32 clipped glint per busy session carries the two-second intensity
   shimmer, with an independent 100ms minimum update interval. The shared
   timer uses 100ms for ambient work and 16ms only for finite interaction
   feedback. A page replacement clears the departing scroll indicator's work.
   Off-screen, hidden, no-working, Reduce Motion, face-down, dimmed and off
   states pause the ambient timer. Power reaches the shell independently of
   IMU freshness, and main stops IMU glass/parallax updates while dimmed/off.
   The one-minute burn-in shift remains a discrete update in lit Always On.
2. **Command starvation:** UI is priority 5/core 1; LCD sender priority 6/core 1;
   devtools is priority 2/core 0 with a 20ms input poll; LVGL's draw workers are
   priority 3 and unpinned. Those priorities/affinities remain appropriate.
   The UI held the recursive LVGL mutex through a render/transfer iteration,
   then immediately reacquired it whenever the next refresh was overdue (or
   its converted wait rounded to zero ticks). It now unlocks and blocks at
   least one RTOS tick on every iteration. Thus an overloaded renderer cannot
   indefinitely prevent a waiting console task from acquiring LVGL, or keep
   draw workers from running. No lock is held during the new blocking interval.
3. **Merged simulator:** opening-benchmark source extraction ended at console
   dispatch, accidentally pulling in the subsequent game benchmark and its
   unrelated types. An explicit end marker now bounds only the production
   opening fixture; a configuration guard rejects missing/reversed markers.
   The generated include is uniquely named `devtools_open_benchmark.inc`.
   Opening and game fixtures compile separately with their own types.

HIG precedence: HIG §7 brings Always On motion to rest. The ring therefore
freezes in dim/Always On, satisfying the board brief's stop requirement, rather
than continuing delight §4's former 1Hz shimmer. Real state/clock changes and
minute burn-in shifts still render. No protocol changes.

## Captured verification

All figures below are **host evidence**, not device timing or power claims.
The redraw regression uses the actual shell/model/LVGL, one 466×84 band,
rounded panel windows, 20ms environment/poll and 1Hz unchanged status delivery.
Each case settles, then records actual flushed pixels for ten virtual seconds
inside a single minute. Frozen partial frames match fresh full renders byte
for byte, so the reduced dirty area does not leave stale pixels.

| Case | Frames / 10s | Maximum flushed pixels / frame |
| --- | ---: | ---: |
| Face, five working Codex sessions | 100 | 4,800 (2.210%) |
| Agents / detail, five working | 0 / 0 | 0 |
| Reduce Motion / Always On / dim-off / face-down | 0 each | 0 |
| Different active screen / hidden by setup | 0 each | 0 |
| RM off / power wake / restored active screen / shown again | 100 each | at most 5,052 (2.326%) |
| Face / Agents, no working sessions | 0 / 0 | 0 |

At an Always On minute boundary, exactly **one** burn-in shift frame renders.

The source-level scheduler regression compiles the production UI loop body
with virtual RTOS/mutex/render adapters under ASan/UBSan: **14,000** overdue or
sub-tick refresh iterations, 14,000 unlocked command handoffs, at 100Hz and
1000Hz tick rates. With a simulated 35ms render and the existing 20ms console
poll, its maximum modeled latency is **65ms**. This establishes the mandatory
scheduling opportunity, not a measured USB response bound. Physical tap/shot
response within one second, actual lock-hold durations, and throughput/power
remain board-owner verification; snapshot/USB costs are not modeled here.

Passed on the final code:

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build -j 6
ctest --test-dir sim/build --output-on-failure -j 6 # 352/352
scripts/idf.sh build                            # unsigned build; no flash
make -C firmware/components/orb_display/tests/host test
make -C firmware/components/orb_shell/tests test # ASan/UBSan, 46 checks
ruff check firmware/components/orb_display/tests/host/test_ui_fairness.py
ruff format --check firmware/components/orb_display/tests/host/test_ui_fairness.py
git -c maintenance.auto=false diff --check
```

The opening benchmark runs 400 consecutive opens plus failure cleanup/recovery.
The firmware build required sandbox escalation for ESP-IDF's `sysctl` query;
it completed without modifying owner configuration or Git state.

## Design-gate screenshots

Fresh captures from the final simulator, visually inspected at Default, AX3
and AX3/Bold/Reduce Transparency. These are synthetic local fixtures. The
lead's independent review is still required before a merge.

| Surface | Default PNG | AX3 PNG | AX3/Bold/opaque PNG |
| --- | --- | --- | --- |
| Face | [face-default](build/delight-board/face-default.png) | [face-ax3](build/delight-board/face-ax3.png) | [face-ax3-bold-rt](build/delight-board/face-ax3-bold-rt.png) |
| Agents list | [agents-default](build/delight-board/agents-default.png) | [agents-ax3](build/delight-board/agents-ax3.png) | [agents-ax3-bold-rt](build/delight-board/agents-ax3-bold-rt.png) |
| Approval card | [approval-default](build/delight-board/approval-default.png) | [approval-ax3](build/delight-board/approval-ax3.png) | [approval-ax3-bold-rt](build/delight-board/approval-ax3-bold-rt.png) |

| Gate heading | Face | Agents list | Approval card |
| --- | --- | --- | --- |
| Two layers | Pass: black content, one glass Apps control. | Pass: plain rows, glass Back only. | Pass: plain card, Back and equal neutral choices. |
| Canvas | Pass: true black, no gradient/glow; fixed ring. | Pass: true black; glyph accents only. | Pass: true black; source glyph/leading bar only. |
| Round-first | Pass: inset ring, side complications and 6-o'clock Apps. | Pass: chord-aware rows, diagonal Back, rim paging. | Pass: chord-fit text and actions; diagonal Back. |
| Sizes/type | Pass: semantic Inter/Nunito/Framework7; AX3 and Bold fit; battery's documented small-digit exception. | Pass: AX3/Bold title wraps; visible paging target and legible body. | Pass: AX3/Bold fits; large capsule actions have separation. |
| Colour | Pass: semantic orange/blue; white clock and readable labels. | Pass: semantic accents plus symbol/word state, readable contrast. | Pass: orange source/bar plus explicit labels; choices remain neutral. |
| Approvals/agents | Pass: fixed working segments, no spinner. | Pass: static last-step text and Needs you symbol/word. | Pass: Allow and Deny visible; no default/tint or unanswered dismissal. |
| Motion/accessibility | Pass: bounded shimmer; RM/AOD/off static; AX3/Bold/opaque preserved. | Pass: no ambient redraw; AX3/Bold/opaque preserved. | Pass: finite feedback only; AX3/Bold/opaque preserved. |
| Gesture alternatives | Pass: Apps, Stack and Notifications visible. | Pass: Back and paging visible. | Pass: Back, Allow and Deny visible. |
| Apple-made review | Pass: uncluttered clock hierarchy and small fixed liveness cue. | Pass: curved sparse list with clear source/state hierarchy. | Pass: direct decision context and balanced actions. |
