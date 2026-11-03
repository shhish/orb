# Agents and Dot polish — simulator design review

Reviewed 2026-10-28 against `docs/hig.md`, the mandatory wave-2 design gate and
the exported Design 1.0 frames. These are isolated fake sessions and throwaway
wire events; the live hub, owner sessions, settings and boards were untouched.
Simulator evidence does not satisfy the physical-board release gate.

## Evidence

Run `nice python3 sim/agents_contact.py --out work/agents-polish` after the simulator
build. This runs the production wire decoder, model and shell in both variants,
then runs `python3 sim/png_tools.py concat DESIGN RENDER PAIR` for every sheet.
The helper now uses only the Python standard library. Reference matching uses
the longest screen name so TalkSend is compared with its own countdown frame.

Each link below is a lossless Design | firmware pair. The **right side** is the
rendered firmware. Full sheets are [Default](../work/agents-polish/contact-default.png)
and [AX3 + Bold + Reduce Transparency](../work/agents-polish/contact-ax3-bold-rt.png).
The [manifest](../work/agents-polish/image-validation.json) records the sheet row,
reference and channel extrema for all 48 nonblank 466×466 renders.

| Surface | Default | AX3 + Bold + Reduce Transparency | Specific review |
|---|---|---|---|
| Agents | [PNG](../work/agents-polish/default/Agents-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Agents-pair.png) | All three steps remain visible at AX3; static running/done glyphs, real line counts, stable sentence preview, retained mic. Full sentence opens in SessionLive. |
| SessionLive | [PNG](../work/agents-polish/default/SessionLive-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/SessionLive-pair.png) | Scrollable complete content, stable header, current capability gates voice. |
| Sessions | [PNG](../work/agents-polish/default/Sessions-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Sessions-pair.png) | Priority order and long-name fitting; compact idle row preserves its slot at any active count. Default fourth idle row matches reference y=340. AX3 scrolls full-size rows. |
| Talk | [PNG](../work/agents-polish/default/Talk-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Talk-pair.png) | Persistent actual amplitude meter and transcript; release explicitly says review. |
| TalkSend | [PNG](../work/agents-polish/default/TalkSend-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/TalkSend-pair.png) | Transcript, Cancel and Send remain separate; countdown requires a physical recording and an actual grant. |
| Usage | [PNG](../work/agents-polish/default/Usage-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Usage-pair.png) | Explicit am/pm and weekly reset day/time; model-specific windows never masquerade as five-hour quota. At AX3 the plain content scrolls within the fixed rings. |
| Usage, scrolled | [PNG](../work/agents-polish/default/UsageDetails-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/UsageDetails-pair.png) | Exact Codex rolling and weekly reset remain reachable at AX3. |
| Dot | [PNG](../work/agents-polish/default/Dot-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Dot-pair.png) | Last question/reply retained independently of another session's sends. |
| Approval | [PNG](../work/agents-polish/default/Approval-pair.png) | [PNG](../work/agents-polish/ax3-bold-rt/Approval-pair.png) | Equal neutral decisions, Deny present, command/context reading region, physical-input guard and 1-of-N paging. AX3 context continues through the reading region's fade and is reachable by scrolling. |

Additional inspected state pairs, in both `default/` and `ax3-bold-rt/`:

- `SessionsNeedsYou-pair.png`: waiting before busy; details retain state words.
- `SessionsIdleOnly-pair.png`: zero active sessions no longer underflow or displace the idle row.
- `AgentsMany-pair.png`: 24 legacy sessions can rotate focus without indexing beyond eight rim positions.
- `UsageStale-pair.png`, `UsageUnknown-pair.png`, `UsageModelOnly-pair.png`: explicit stale/unavailable states, unknown is never 0%, duration-only quota heading.
- `SendQueued-pair.png`, `SendDelivered-pair.png`, `SendFailed-pair.png`: truthful receipts; queued/failed/unknown receipts remain until Close. Sending offers Close because a committed request cannot be retracted.
- `SessionEndedSend-pair.png`: removed session disarms the countdown, preserves transcript, neutralizes/disables Send and offers Cancel.
- `TranscriptionError-pair.png`: failed transcription is displayed as a failure with its safe reason.
- `DotDisconnected-pair.png`: previous exchange stays readable while “Dot not connected” remains explicit.
- `DotAbsent-pair.png`: absent ChatGPT connection shows a centered explanation and hides unavailable voice affordances.
- `DotIdleDiscovery-pair.png`: Dot outside the eight-session focus set is discovered through idle pages; its per-device summary and actual messaging grant are used.

Long-title/activity PNGs are also generated beside these pairs, including Agents,
SessionLive, Sessions and Approval. Their harness checks both measured text fit
and the visible circle chord. Repeated stream updates keep label identity and
do not redraw the whole screen; idle work does not animate.

Session ordering preserves the protocol's authoritative snapshot order within
waiting, working and remaining-session groups. Metadata timestamps, title changes
and summary updates never move an already waiting prompt; a dedicated two-prompt
regression proves that the older queued prompt stays first.

## Mandatory gate

The following shared pass notes apply to every surface above; the table records
the surface-specific adaptations and the extra state pairs prove their edges.

- **Two layers — pass:** flat content and bubbles; glass belongs only to controls. At most two glass controls on these task views; no glass on glass.
- **Canvas — pass:** true black; no gradients, glows, tinted background panels or decorative work animation.
- **Round-first — pass:** measured title/step ellipsis preserves the chord; list rows and prose scroll through the existing round/fade treatment. The idle row's local-coordinate correction was re-rendered and inspected.
- **Sizes — pass:** signed Inter/Framework7 styles, effective mic target 72 px including its 4 px extension, minimum controls 46 px, text at the established signed minimum or larger. AX3 keeps all three steps and uses reading scroll for complete text/reset data.
- **Colour — pass:** provider identity and done/failed glyphs follow the approved exception; state also has text/glyph shape. One tinted action at most. Idle essential text retains contrast instead of applying the reference sketch's 50% whole-row dimming: HIG §5 wins.
- **Approvals and agents — pass:** neutral risky decisions with Deny, no automatic approval or timer dismissal. Physical input is required for Allow, Deny and Send. A Mac-side card cancellation removes decision controls; paging reaches the next prompt. Running work uses a static terminal glyph instead of Design 1.0's spinner: HIG §7 wins.
- **Motion/accessibility — pass:** retained labels avoid stream flicker; actual meter levels, static work, Bold Text and opaque Reduce Transparency controls. Physical-only talk countdown stops when its target becomes unavailable. AX3 scrolls content instead of shrinking text.
- **Navigation — pass:** BOOT/PWR, touch, rim and twist continue to provide alternatives. No added chevrons or generic page dots.
- **Feels Apple-made — simulator pass:** concise status, stable content, explicit send receipts and quiet empty states. Lead's independent screenshot review and physical panel review remain required before merge/release.

## Validation and allocations

`nice cmake --build sim/build -j4 --target orb_agents_sim` and
`nice ctest --test-dir sim/build -j4 -R agents_design1 --output-on-failure` passed.
The final isolated harness reports **949 Default / 946 AX3** wire/render/input
checks. Its additions cover needs-you order, idle-only layout, 24-session focus,
explicit timezone text, model-only quota, Dot idle paging/absence, ended-session
send, queued receipt persistence, cancel, synthetic Send and Mac-side cancellation.
The full simulator, hub and IDF results are reported by the lead.

`ruff format` and `ruff check` pass for `sim/agents_contact.py`; PNG checks confirm
lossless/nonblank pairs. This sandbox prints `nice: setpriority: Operation not
permitted` while running the commands, so lowered priority is not asserted here.

New allocation sites/usage, **not device heap measurements**:

- AX3 Usage adds one plain LVGL scroll container plus its existing scroll-edge event state (`scroll_edge_t` is two int32 values and a bool, plus allocator/event bookkeeping). No bitmap or full-frame cache was added.
- Opening Dot can now allocate the existing bounded idle-page payload under `ORB_MEMORY_WORKSPACE` while it discovers an out-of-focus Dot. A compiled host `sizeof(orb_idle_page_t)` is **3,392 bytes**; the lead must capture the actual device owner peak/rejections.
- Two cached-selection booleans track fresh paged grants and navigation restoration. Existing fixed buffers hold all display strings; no new transcript/session history heap was introduced.
- Relevance sorting uses a bounded 24-entry unsigned stack array (**96 bytes** on the target's 32-bit unsigned ABI).
- Rim objects are capped at eight even for legacy 24-session snapshots, reducing the prior worst-case widget count and removing an out-of-bounds angle lookup.

The simulator does not enforce owner budgets. Capture `orb-dev.sh heap` owner
peaks/rejections and physical screenshots on the dev board. Live permission-hook
delivery, BOOT microphone/transcription/audio quality, owner-timezone readback,
hub restart/reconnect and the ChatGPT plugin rescan remain lead/owner checks;
these simulator fixtures do not claim live-provider or panel validation.
