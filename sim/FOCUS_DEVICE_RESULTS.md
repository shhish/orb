# Focus device implementation evidence

Branch: `orb/w2-render`. Changes are uncommitted. No board, hub, owner configs,
Git metadata, orb_net or main.c was modified.

The local `docs/protocol.md` predates focus. The exact per-device focus contract
was read from `../integration/docs/protocol.md` (Per-device focus and preferences)
without changing that worktree. Defaults are all sources enabled and 10 minutes;
opaque idle cursors are bounded to 80 UTF-8 bytes. No protocol changes were needed.

## Changes

- Link decodes hello preferences, authoritative eight-session focus snapshots,
  summaries and idle pages; sends partial preferences and cursor requests.
- Model retains confirmed preferences until completion, exposes pending/errors,
  keeps one separately allocated eight-session idle page and rejects stale replies.
  Conflict restarts at the first page; exhaustion stops requests.
- Agents ends with a secondary idle disclosure. Idle More/error rows can reach
  the central chord and retain their scroll position through pending/failure;
  successful replacement starts at the first row. Leaving or hiding the idle
  view frees both model and UI page copies. One selected session is retained for
  its detail/voice target when it leaves focus, labelled Outside focus.
- Settings → Sessions offers Claude Code, Codex and Dot switches, and cycles
  Show recent for through Off/5/10/30 min. Confirmed changes spring; Reduce Motion
  cross-fades. Only a touched knob becomes glass. System interruptions release it.
- Face counts enabled focus sessions needing attention. Source cancellation never
  submits an answer. Voice confirmation retains the actual idle-session recipient.
- Stable voice error codes reach the model. No speech offers Didn't catch that /
  Hold and speak with a persistent Hold to talk retry. The first 3,200 PCM bytes
  (100 ms at 16 kHz mono PCM16) are discarded per capture before framing/metering.

## Validation

Commands run from the worktree root:

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build
ctest --test-dir sim/build --output-on-failure -j 4
sim/build/test_shell
sim/shell-shots.sh sim/build/focus-device
make -C firmware/components/orb_model/tests/host test
make -C firmware/components/orb_link/tests/host test
make -C firmware/components/orb_voice/tests/host test
ruff check firmware/components/orb_link/tests/host/test_delivery.py firmware/components/orb_voice/tests/host/test_events.py
ruff format --check firmware/components/orb_link/tests/host/test_delivery.py firmware/components/orb_voice/tests/host/test_events.py
shellcheck sim/shell-shots.sh
git -c maintenance.auto=false diff --check
scripts/idf.sh build
```

- CTest: **311/311 passed**. Shell: **1,097 public-contract checks**.
- Model ASan/UBSan: **496 + 183** checks, including ESP allocation paths;
  16,000 concurrent updates and 16,000 coherent snapshots.
- Link ASan/UBSan: **28,928 + 641 + 65,159 + 14,237 + 87 + 367** checks.
- Voice ASan/UBSan: **2,291 + 97 + 32** checks.
- Ruff, formatting, ShellCheck and whitespace checks passed.
- ESP-IDF build passed. `firmware/build/orb.bin` is unsigned, 0x230000 bytes,
  with 64% of the smallest app partition free. The first sandboxed attempt was
  blocked by psutil process enumeration; the permitted build outside the sandbox
  and a final repeat completed successfully.
- Full screenshot script: **277 PNGs**, 87 scenes plus accessibility/flow frames.
- Logs: [CTest](build/focus-device/ctest.log), [ESP-IDF](build/focus-device/idf-build.log).

## Design gate

Independent screenshot/code review passed after correcting tail-row overlap,
last-row centering, pending/error scroll retention, recipient labels, focus-removal
copy and touched-toggle interruption lifetime. No unresolved actionable finding.
HIG and brief agree; the stricter gate limit of three glass elements was followed.

Each table cell below is a pass note for that checklist heading and surface.

| Heading | Face | Agents / idle lists | Sessions settings | Card / voice |
|---|---|---|---|---|
| Two layers | Plain complications; one glass Apps control. | Content rows plain; Back alone glass. | Plain rows/tracks; Back and touched knob only glass. | Prose plain; approval has three untinted controls; voice at most three. |
| Canvas | True black; no gradient/glow. | True black; no gradient/glow. | True black; flat semantic tracks. | True black; no gradient/glow. |
| Round-first | Rim layout and fixed clock remain clear. | Chord scaling; final disclosure can rest centrally. | Chord scaling; recent title wraps at AX3. | Scroll prose within central chord; actions clear of rim. |
| Sizes | Dynamic Type/Bold and existing targets retained. | Semantic text ≥20 px when readable; 72/46 px targets. | Semantic text; entire row is the toggle target; AX3 two-line recent heading. | AX3/Bold text readable; persistent 72 px actions. |
| Colour | Enabled focus count uses text plus state colour. | Secondary idle state and Framework7 chevrons. | On/Off text plus knob position; semantic green. | Equal approval choices; errors use explicit text. |
| Approvals & agents | Counts focus needs-you only. | No spinner; Loading/failed/end states explicit. | Pending keeps confirmed values; no automatic source action retry. | Deny remains; no default/tint/auto-answer; no-speech retries explicitly. |
| Motion | Existing spring/RM behaviour preserved. | Existing scroll springs/RM preserved. | Confirmed knob uses 500 ms spring; RM 150 ms fade; RT opaque material. | Existing control materialisation/RM retained; RT/Bold verified. |
| Gesture alternatives | Apps and attention buttons remain. | Back, More and rim scroll control visible. | Back, row switches and recent selector visible. | Back, mic hold/release, Stop/Cancel/Send as appropriate. |
| Apple feel | Glanceable round hierarchy. | Short relevant focus plus quiet idle disclosure. | Familiar switches and restrained navigation. | Explicit review and friendly persistent retry. |

Representative gate frames (all also have Default/AX3/AX3+Bold+RT variants):

- [Face](build/focus-device/face-ax3-bold-rt.png)
- [Focus / idle disclosure](build/focus-device/agents-idle-row-ax3-bold-rt.png)
- [Idle More](build/focus-device/idle-more-ax3-bold-rt.png)
- [Settings](build/focus-device/sessions-settings-ax3-bold-rt.png)
- [Touched switch](build/focus-device/sessions-touched-ax3-bold-rt.png)
- [Recent window](build/focus-device/sessions-recent-ax3-bold-rt.png)
- [Approval](build/focus-device/approval-ax3-bold-rt.png)
- [No speech](build/focus-device/voice-no-speech-ax3-bold-rt.png)

## New-state PNG matrix

All paths are under `sim/build/focus-device/` and generated by the script above.
Each new state was captured at all three requested settings.

| State | Default | AX3 | AX3 + Bold + Reduce Transparency |
|---|---|---|---|
| agents-idle | [PNG](build/focus-device/agents-idle-default.png) | [PNG](build/focus-device/agents-idle-ax3.png) | [PNG](build/focus-device/agents-idle-ax3-bold-rt.png) |
| agents-idle-row | [PNG](build/focus-device/agents-idle-row-default.png) | [PNG](build/focus-device/agents-idle-row-ax3.png) | [PNG](build/focus-device/agents-idle-row-ax3-bold-rt.png) |
| idle-loading | [PNG](build/focus-device/idle-loading-default.png) | [PNG](build/focus-device/idle-loading-ax3.png) | [PNG](build/focus-device/idle-loading-ax3-bold-rt.png) |
| idle-page | [PNG](build/focus-device/idle-page-default.png) | [PNG](build/focus-device/idle-page-ax3.png) | [PNG](build/focus-device/idle-page-ax3-bold-rt.png) |
| idle-more | [PNG](build/focus-device/idle-more-default.png) | [PNG](build/focus-device/idle-more-ax3.png) | [PNG](build/focus-device/idle-more-ax3-bold-rt.png) |
| idle-end | [PNG](build/focus-device/idle-end-default.png) | [PNG](build/focus-device/idle-end-ax3.png) | [PNG](build/focus-device/idle-end-ax3-bold-rt.png) |
| idle-empty | [PNG](build/focus-device/idle-empty-default.png) | [PNG](build/focus-device/idle-empty-ax3.png) | [PNG](build/focus-device/idle-empty-ax3-bold-rt.png) |
| idle-error | [PNG](build/focus-device/idle-error-default.png) | [PNG](build/focus-device/idle-error-ax3.png) | [PNG](build/focus-device/idle-error-ax3-bold-rt.png) |
| idle-more-pending | [PNG](build/focus-device/idle-more-pending-default.png) | [PNG](build/focus-device/idle-more-pending-ax3.png) | [PNG](build/focus-device/idle-more-pending-ax3-bold-rt.png) |
| idle-more-error | [PNG](build/focus-device/idle-more-error-default.png) | [PNG](build/focus-device/idle-more-error-ax3.png) | [PNG](build/focus-device/idle-more-error-ax3-bold-rt.png) |
| idle-detail | [PNG](build/focus-device/idle-detail-default.png) | [PNG](build/focus-device/idle-detail-ax3.png) | [PNG](build/focus-device/idle-detail-ax3-bold-rt.png) |
| idle-reply | [PNG](build/focus-device/idle-reply-default.png) | [PNG](build/focus-device/idle-reply-ax3.png) | [PNG](build/focus-device/idle-reply-ax3-bold-rt.png) |
| session-outside-focus | [PNG](build/focus-device/session-outside-focus-default.png) | [PNG](build/focus-device/session-outside-focus-ax3.png) | [PNG](build/focus-device/session-outside-focus-ax3-bold-rt.png) |
| sessions-settings | [PNG](build/focus-device/sessions-settings-default.png) | [PNG](build/focus-device/sessions-settings-ax3.png) | [PNG](build/focus-device/sessions-settings-ax3-bold-rt.png) |
| sessions-codex | [PNG](build/focus-device/sessions-codex-default.png) | [PNG](build/focus-device/sessions-codex-ax3.png) | [PNG](build/focus-device/sessions-codex-ax3-bold-rt.png) |
| sessions-dot | [PNG](build/focus-device/sessions-dot-default.png) | [PNG](build/focus-device/sessions-dot-ax3.png) | [PNG](build/focus-device/sessions-dot-ax3-bold-rt.png) |
| sessions-recent | [PNG](build/focus-device/sessions-recent-default.png) | [PNG](build/focus-device/sessions-recent-ax3.png) | [PNG](build/focus-device/sessions-recent-ax3-bold-rt.png) |
| sessions-pending | [PNG](build/focus-device/sessions-pending-default.png) | [PNG](build/focus-device/sessions-pending-ax3.png) | [PNG](build/focus-device/sessions-pending-ax3-bold-rt.png) |
| sessions-error | [PNG](build/focus-device/sessions-error-default.png) | [PNG](build/focus-device/sessions-error-ax3.png) | [PNG](build/focus-device/sessions-error-ax3-bold-rt.png) |
| sessions-off | [PNG](build/focus-device/sessions-off-default.png) | [PNG](build/focus-device/sessions-off-ax3.png) | [PNG](build/focus-device/sessions-off-ax3-bold-rt.png) |
| sessions-touched | [PNG](build/focus-device/sessions-touched-default.png) | [PNG](build/focus-device/sessions-touched-ax3.png) | [PNG](build/focus-device/sessions-touched-ax3-bold-rt.png) |
| sessions-recent-off | [PNG](build/focus-device/sessions-recent-off-default.png) | [PNG](build/focus-device/sessions-recent-off-ax3.png) | [PNG](build/focus-device/sessions-recent-off-ax3-bold-rt.png) |
| sessions-recent-five | [PNG](build/focus-device/sessions-recent-five-default.png) | [PNG](build/focus-device/sessions-recent-five-ax3.png) | [PNG](build/focus-device/sessions-recent-five-ax3-bold-rt.png) |
| sessions-recent-thirty | [PNG](build/focus-device/sessions-recent-thirty-default.png) | [PNG](build/focus-device/sessions-recent-thirty-ax3.png) | [PNG](build/focus-device/sessions-recent-thirty-ax3-bold-rt.png) |
| voice-no-speech | [PNG](build/focus-device/voice-no-speech-default.png) | [PNG](build/focus-device/voice-no-speech-ax3.png) | [PNG](build/focus-device/voice-no-speech-ax3-bold-rt.png) |
| voice-network | [PNG](build/focus-device/voice-network-default.png) | [PNG](build/focus-device/voice-network-ax3.png) | [PNG](build/focus-device/voice-network-ax3-bold-rt.png) |
| voice-unsupported | [PNG](build/focus-device/voice-unsupported-default.png) | [PNG](build/focus-device/voice-unsupported-ax3.png) | [PNG](build/focus-device/voice-unsupported-ax3-bold-rt.png) |

Physical ADC/transcription quality, panel optics, runtime heap and live hub
end-to-end behavior remain device validation for the board owner. Simulator PNGs
and host timings do not establish physical performance. No deployment occurred.
