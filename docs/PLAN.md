# Orb master plan (2026-10-27)

This is the single plan every agent works from. It consolidates the owner's goals and decisions from this project, the research (`docs/research/round-ux.md`), the architecture decision (`docs/architecture-rendering.md`), the design documents (`hig.md`, `delight.md`, `home-app.md`, `interaction-model.md`) and the independent UX review (`docs/ux-review.md`). If something conflicts with this plan, raise it; don't drift.

## The goal

Orb is a round 466×466 companion that feels Apple-made: **round-first, gesture-first, alive but calm, fast**. It lets the owner:
- watch and steer their Claude Code and Codex agents (approve, deny, voice);
- talk to ChatGPT Dot;
- control Apple Home through HomeRelay;
- play small offline games;
- update itself over the air, safely.

## Decisions already made (don't relitigate)

- **Design.** hig.md wins over any brief. True black, Liquid Glass only on controls, accents by meaning (delight.md), no gradients or glows.
- **Navigation.** interaction-model.md:
  - Face in the centre, Agents to the left, Home to the right.
  - Smart Stack up, Notifications down.
  - Left rim for Back, right rim for Control Center.
  - The rim acts as a crown. The Compass opens on long-press.
  - Transitions are iris, slide and sheet, all finger-tracking and interruptible.
- **Renderer.** Hybrid: LVGL's band engine with large 84-row bands, its tuned blur, and rows drawn at size. Transitions move cached snapshots. No engine rewrite.
- **Security.** The hub holds every credential: HomeRelay, Claude hooks and the signing key. The device never auto-approves, unlocks or sends. Answers are honest: queued, delivered or failed.
- **Units and copy.** Temperature follows the Mac (°F here). Copy is plain watch language.
- **Agent workforce, in this order:**
  1. Antigravity (`agy`), with its model choice per task.
  2. Codex (`codex exec`, gpt-6.1-sol ultra).
  3. DeepSeek (`dsh-run`, paid and sparing).
  4. Claude: orchestration, design judgment, all board work, merges.

## Agent and model assignment

| Kind of work | First choice | Fallback |
|---|---|---|
| Design-heavy UI implementation (new places, motion, apps) | agy `claude-opus-4-6-thinking` | Codex |
| Rendering, performance and firmware internals (LVGL fork, display, memory) | Codex gpt-6.1-sol ultra | agy Opus |
| Bounded fixes, tests, hub/Python, tooling | agy `gemini-3.8-flash-high` | Codex |
| Independent code review (a different family from the author) | agy Opus or DeepSeek v4-pro | Codex read-only |
| Independent design review (screenshots against hig.md) | agy Opus | Codex |
| Research and web | Codex (network) | agy |
| Board flashing, measurement, merges, design calls | Claude | — |

One writer per set of files. Each stream gets its own worktree from the trunk. Claude commits and merges after re-running the tests.

## Trunk and merge discipline

- **Trunk:** `orb/w2-next` is the firmware trunk. Every stream branches from it and merges back through Claude only after:
  - sim and host tests pass;
  - `idf.sh build` passes;
  - the design gate passes (for UI);
  - board gates pass, where a stream has them.
- **Hub:** the hub trunk is `orb/integration`, which also runs the live hub service. Its firmware catches up from `orb/w2-next` at releases.
- **The one board** is a serialized resource. Claude queues board runs. Streams must be verifiable in the simulator and leave exact board commands.

## Streams

| # | Stream | Owner | Depends on | Done when |
|---|---|---|---|---|
| A | **Navigation round 2**: overlapped transitions, rim as crown on the scroll path, crescent and iris glitches, Agents stalls (in progress) | Codex | — | Transitions ≥ 55 fps, rotary ≥ 45 and Agents scroll ≥ 45 on the board, with no stall over 33 ms |
| A0 | **Trunk consolidation**: merge `w2-nav` (navigation and scroll) with `w2-next` (UX fixes and offline) | Claude, with Codex resolving conflicts | A | One trunk passes everything |
| B | **Navigation phase 2: Agents orrery and Home satellites** (interaction-model.md), plus the iris from real tokens | agy Opus | A0 | Design gate, ≥ 45 fps on the board, AX3 and Reduce Motion use the list |
| C | **Power and calm**: idle dimming, Always On face (redacted, 1 Hz, pixel shift), sleep and wake on pick-up and face-down (IMU), battery and charging states, low-power behaviour, a quiet "sleep character" moment (wave 2 item never built) | agy Opus (UI) and Gemini (power plumbing) | A0 | Board: idle current dropped, no missed wakes, AOD sensitive text redacted |
| D | **Reliability**: DMA and internal minimum headroom (DMA min-free reaches 8 B), a 2-hour soak (scroll, transitions, voice, OTA, reconnects) with leak tracking, coredump capture to the hub, watchdog review | Codex | A0 for the soak | Internal ≥ 12 KB, DMA ≥ 4 KB under all benches, a 2-hour soak with zero leaks or crashes |
| E | **Hub polish**: real session titles (in progress, agy), interrupted-download traceback (in progress, DeepSeek), mDNS under launchd, "Done · N min ago" recency in session payloads | agy Gemini | — | Hub tests pass and the live hub logs are clean |
| F | **Offline utility apps**: Timer, Stopwatch and Alarm as round dials (rim as crown, iris from the Apps ring), with Face and Smart Stack integration for running timers | agy Opus | A0 for nav hooks; the UI can start now | Design gate passes, works offline, zero frames when idle |
| G | **Games 2: Marble Conservatory** (tilt marble, rim rewinds the marble's path, keepsake garden), per research concept 10 | agy Gemini, reviewed by Opus | — | Steady 30 fps on the board via bench_game; it saves and restores |
| H | **Wireless devtools**: screenshots, taps and bench over the hub link, so the board doesn't need USB (planned in wave 2) | Codex or agy Gemini | — | `orb-dev.sh --wifi shot/tap/bench` works and is authenticated through the paired link |
| I | **Design QA loop**: after each merge, an independent review of board screenshots against hig.md and interaction-model.md, findings turned into tasks | agy Opus (reviewer); Claude decides | each merge | No open P0/P1 |
| J | **Release hygiene**: merge the trunk into `orb/integration`, update HANDOFF, `orb-hub release` to dev; stable channel and second board when the owner says | Claude | each milestone | Board on the release; the HANDOFF is current |
| K | **Owner-gated**: Dot (the owner sets up the ChatGPT tunnel), the permanent signing key (`orb-hub keys init`), second-board migration, real-speech voice test, Gitea CI | Owner, with Claude preparing | owner | — |

## Order

1. **Now, in parallel:**
   - A: Codex (running)
   - E: agy Gemini and DeepSeek (running)
   - F: Timer UI, agy Opus
   - G: Marble, agy Gemini
   - H: wireless devtools, agy Gemini, in a separate worktree
   - C-design: the Always On and sleep design, agy Opus, design first
2. **When A lands:**
   - Claude consolidates the trunk (A0) and runs board gates.
   - Then B (agy Opus), C-implementation and D (Codex) start in parallel.
3. **After each merge:** I (design QA) runs, then J (release) when a milestone is green.
4. **Owner-gated items (K):** prompted when they block nothing else.
