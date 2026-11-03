# Delight: independent simulator design review

This review uses the real shell, fonts, model, app registry and LVGL renderer,
with synthetic input and virtual time. It does not establish physical panel
quality, frame rate, speaker loudness, codec latency, battery life or combined
firmware heap headroom. No board or live hub was used.

The independent capture set is `sim/build/delight/review/`: 18 surfaces at
Default, AX3, AX3 + Bold Text + Reduce Transparency, and Reduce Motion, 72 PNGs.
The complete before/after catalogue is `sim/build/delight/{before,after}/`.
Motion evidence is `sim/build/delight/motions/{default,ax3-bold-rt-rm}/`.
Build directories are intentionally uncommitted; regenerate them with
`sim/shell-shots.sh` and `sim/delight-shots.sh`.

## Rules, precedence and limits

- The launcher is an explicit catalogue of all six app identities. Its six
  prescribed disc colours are the documented exception to delight's literal
  two-accent-per-screen limit. Other lists prioritize orange plus one semantic
  hue, keeping symbols and words when a source hue is neutralized.
- The white-glyph recipe conflicts with HIG §5/§8's contrast requirement on
  bright flat discs: white on orange is approximately 2.23:1 and on green
  approximately 2.02:1. **Contrast takes precedence:** the final shell chooses
  clean black ink where white would fall below 3:1, retaining white on darker
  discs and keeping every prescribed flat palette colour. Increased-contrast
  variants use the same rule. This is a documented glyph-polarity deviation
  from the white-only recipe, rather than a claim that its original colours
  already passed. A black keyline trial was rejected independently because it
  made the symbols read as outlined stickers; no keyline, shadow or glow remains.
- Shared glass retains HIG §2.2's approximately 6% swell rather than delight's
  4%. HIG wins, and the shared renderer belongs to the glass stream.
- Queued means local acceptance. Its visual choice feedback never claims
  delivery. The 600 ms return to Agents follows an accepted, delivered result;
  pending or failed decisions remain reviewable. Unanswered approvals never
  time out or auto-dismiss.
- All-clear requires a fresh authoritative successful result with no waiting
  cards. Its two-second interval and paired sound begin when the Face actually
  presents the check; replay, unrelated revisions and stale earlier results do
  not retrigger it.
- The working ring is a fixed segment with a slow intensity shimmer, never an
  indeterminate progress spinner. Agent rows retain static last-step text.
- Always On is an explicit shell input, currently derived by main from lit
  brightness at or below 10%. This is not a new hardware power mode. The host
  cadence regression verifies at most one update per second in that mode.
- Normal high battery remains the iOS-style neutral glyph to preserve the
  two-accent Face budget. Existing charge/low-power/critical status precedence
  remains in the status component.

## Face

Evidence: [Default](build/delight/review/face-default.png),
[AX3/Bold/opaque](build/delight/review/face-ax3-bold-rt.png),
[Reduce Motion](build/delight/review/face-rm.png).

- **Two layers:** pass; complications and ring are plain content, only Apps is glass.
- **Canvas:** pass; true black, flat semantic arcs, no gradient or glow background.
- **Round-first:** pass; side complications and bottom Apps clear the bezel and clock.
- **Sizes:** pass; count and battery remain separated at AX3/Bold; clock stays readable during rolls.
- **Colour:** pass within the stated battery policy; orange attention and blue working, white clock.
- **Approvals/agents:** pass; attention is a review shortcut and never submits a decision.
- **Motion/accessibility:** pass in host tests; RM stillness, 1 Hz AOD, burn-in shift and opaque controls.
- **Gesture alternatives:** pass; Apps, attention, notifications, stack and Controls have visible targets.
- **Apple craft:** pass for the sampled layout; earlier battery/text collision was corrected.

## Apps

Evidence: [Default](build/delight/review/apps-default.png),
[AX3/Bold/opaque](build/delight/review/apps-ax3-bold-rt.png).

- **Two layers:** pass; flat app discs are content; Back is the only glass control.
- **Canvas:** pass; true black and clean Framework7 glyphs on flat identity discs.
- **Round-first:** pass; radial default layout becomes a readable round list at AX3.
- **Sizes:** pass; app hit targets and enlarged text remain usable inside the circle.
- **Colour:** pass with documented catalogue and adaptive-ink deviations; actual rendered disc/glyph contrast is at least 3:1.
- **Approvals/agents:** pass; opening an app never answers a card or starts voice capture.
- **Motion/accessibility:** pass in sampled frames; sink/rebound, forward/reverse launch and RM cross-fade.
- **Gesture alternatives:** pass; each app is a visible button and Back is always available.
- **Apple craft:** pass for geometry and hierarchy; launch raster rectangles were corrected.

## Agents

Evidence: [Default](build/delight/review/agents-default.png),
[AX3/Bold/opaque](build/delight/review/agents-ax3-bold-rt.png).

- **Two layers:** pass; session rows are plain content; navigation floats separately.
- **Canvas:** pass; black rows and canvas with small glyph/state accents only.
- **Round-first:** pass; rows shrink/fade toward the bezel; rim More remains separate.
- **Sizes:** pass; AX3 uses two-line session names and retains project/state context.
- **Colour:** pass; two-hue priority budget retains state words and source symbols when neutralized.
- **Approvals/agents:** pass; busy work shows its last step, with no indeterminate spinner.
- **Motion/accessibility:** pass; live overscroll, rim fade, pressed rows and state draw-on have RM behavior.
- **Gesture alternatives:** pass; visible rim More advances the scroll; each readable row is a button.
- **Apple craft:** pass; no filled source rows, decorative borders or extra glass.

## Idle sessions

Evidence: [Default](build/delight/review/idle-page-default.png),
[AX3/Bold/opaque](build/delight/review/idle-page-ax3-bold-rt.png).

- **Two layers:** pass; results and More are content, Back is glass.
- **Canvas:** pass; plain black, no decorative tint or background treatment.
- **Round-first:** pass; the lower More row enters the readable central chord when scrolled.
- **Sizes:** pass; AX3 preserves full session identity rather than crowding a side icon rail.
- **Colour:** pass; static source/state copy preserves meaning even when glyphs are neutral.
- **Approvals/agents:** pass; loading/error/end are explicit words, not agent-work spinners.
- **Motion/accessibility:** pass; received pages settle through the shell's spring/RM path.
- **Gesture alternatives:** pass; visible More and rim advance support finger-independent paging.
- **Apple craft:** pass; short, relevant results retain a clear route back.

## Session detail

Evidence: [Default](build/delight/review/session-default.png),
[AX3/Bold/opaque](build/delight/review/session-ax3-bold-rt.png).

- **Two layers:** pass; title/project/state/command are content; Back, Info and primary action are glass.
- **Canvas:** pass; true black, one primary tint and no tinted text paragraphs.
- **Round-first:** pass; diagonal navigation and bottom primary do not intersect the central prose.
- **Sizes:** pass; enlarged title and command stay legible and the full summary remains scrollable.
- **Colour:** pass; orange Agents identity and black primary caption preserve semantic tint contrast.
- **Approvals/agents:** pass; Review Command opens the exact approval; it is not an Allow shortcut.
- **Motion/accessibility:** pass in sampled states; text settings, opaque controls and RM remain effective.
- **Gesture alternatives:** pass; Back, Info, review/reply and prose advance have visible controls.
- **Apple craft:** pass; one clear next action with source and last-step context.

## Smart Stack

Evidence: [Default](build/delight/review/stack-default.png),
[AX3/Bold/opaque](build/delight/review/stack-ax3-bold-rt.png).

- **Two layers:** pass; the card stays black; only navigation and equal decisions are glass.
- **Canvas:** pass; source glyph and 3 px leading approval bar carry the accent.
- **Round-first:** final enlarged-heading clearance is regression-tested; page capsule follows the rim.
- **Sizes:** pass; AX3 decisions stay stacked and visible while command context scrolls.
- **Colour:** pass; source tint does not become a filled card or a default decision.
- **Approvals/agents:** pass; both decisions remain neutral and no unresolved approval expires.
- **Motion/accessibility:** pass; snap/arrival use spring motion, RM uses content cross-fade.
- **Gesture alternatives:** pass; rim More changes cards, explicit review/advance handles long prose.
- **Apple craft:** enlarged title/Back collision identified independently and fixed before final capture.

## Approval and choice cards

Evidence: [Approval Default](build/delight/review/approval-default.png),
[Approval AX3/Bold/opaque](build/delight/review/approval-ax3-bold-rt.png),
[Choice AX3/Bold/opaque](build/delight/review/choice-ax3-bold-rt.png).

- **Two layers:** pass; command/choice content remains plain, glass count stays at or below three.
- **Canvas:** pass; black cards, source glyphs and an approval-only narrow accent bar.
- **Round-first:** pass; enlarged actions fit the central chord, clear Back and preserve gaps.
- **Sizes:** pass; 72 px consequential controls and full long-command scrolling remain available.
- **Colour:** pass; risky Allow/Deny are equal and untinted; post-choice reward is explicit feedback.
- **Approvals/agents:** pass; Deny remains present, no automatic answer, replay or unanswered dismissal.
- **Motion/accessibility:** pass; touch fill/check and deny settle have RM alternatives; returns await authority.
- **Gesture alternatives:** pass; choices, Deny, Back and reading advance are visible buttons.
- **Apple craft:** pass; the visual reward never replaces the truthful Queued/Sent receipt.

## Message and attention cards

Evidence: [Message AX3/Bold/opaque](build/delight/review/message-ax3-bold-rt.png),
[Attention AX3/Bold/opaque](build/delight/review/attention-ax3-bold-rt.png).

- **Two layers:** pass; message and completion prose remain content, actions/navigation alone are glass.
- **Canvas:** pass; true black with a small source glyph, no filled source background.
- **Round-first:** pass; body wraps in the central safe area and Reply clears the bezel.
- **Sizes:** pass; AX3 body remains readable; long content has an explicit advance control.
- **Colour:** source-derived Dot/Agents primary tint follows the same black-caption contrast policy.
- **Approvals/agents:** pass; Reply enters confirmation-capable voice and never sends automatically.
- **Motion/accessibility:** pass; no fake agent activity or decorative looping motion.
- **Gesture alternatives:** pass; Reply, Back and long-content advance are explicit controls.
- **Apple craft:** pass; informative cards stay quieter than an approval.

## Notifications

Evidence: [Default](build/delight/review/notifications-default.png),
[AX3/Bold/opaque](build/delight/review/notifications-ax3-bold-rt.png).

- **Two layers:** pass; history rows are content; Back and Clear All are separate controls.
- **Canvas:** pass; black list with small state symbols and no coloured row fills.
- **Round-first:** pass; list clipping/fade follows the circle; title clears Back at enlarged size.
- **Sizes:** pass; AX3 rows retain source/receipt copy; full details remain reachable by tapping.
- **Colour:** pass; semantic budget limits simultaneous hues and state remains textual.
- **Approvals/agents:** pass; clearing presentation does not remove a queued decision or repeat an answer.
- **Motion/accessibility:** pass; rim indication grows during scroll then rests after its timed fade.
- **Gesture alternatives:** pass; Clear All, each row and rim advance are visible actions.
- **Apple craft:** pass; receipts remain distinguishable from new attention.

## Control Center

Evidence: [Default](build/delight/review/control-center-default.png),
[AX3/Bold/opaque](build/delight/review/control-center-ax3-bold-rt.png).

- **Two layers:** pass; no glass panel behind controls and no glass-on-glass arrangement.
- **Canvas:** pass; true black with plain informational labels and standard toggle material.
- **Round-first:** pass; three glass controls at Default; AX3 pages prevent crowding.
- **Sizes:** pass; +/- stay 72 px, toggle captions remain readable and physically separated.
- **Colour:** pass; state labels accompany toggle symbols rather than relying on hue alone.
- **Approvals/agents:** pass; no hidden consequential operation or decision default.
- **Motion/accessibility:** pass; Silent really gates sounds and Reduce Motion takes effect immediately.
- **Gesture alternatives:** pass; brightness, toggles, Back and paging all have buttons.
- **Apple craft:** pass; controls show symbols, names and values without extra decorative containers.

## Settings and Sessions preferences

Evidence: [Settings AX3/Bold/opaque](build/delight/review/settings-ax3-bold-rt.png),
[Sessions AX3/Bold/opaque](build/delight/review/sessions-settings-ax3-bold-rt.png).

- **Two layers:** pass; settings rows and toggle tracks are plain content, Back is glass.
- **Canvas:** pass; black, gray Settings identity, no new decorative colour.
- **Round-first:** pass; rows follow chords and enlarged labels clear the toolbar.
- **Sizes:** pass; AX3 names/values and source toggles remain readable without crowding.
- **Colour:** pass; green on-state accompanies explicit On/Off words and knob position.
- **Approvals/agents:** pass; source preferences never manufacture agent activity or send decisions.
- **Motion/accessibility:** pass; Bold, text size, RM, RT and contrast changes remain reachable/live.
- **Gesture alternatives:** pass; each setting, toggle, Back and advance has a visible control.
- **Apple craft:** pass; familiar hierarchy and restrained chrome.

## Voice

Evidence: [Recording Default](build/delight/review/voice-recording-default.png),
[Recording AX3/Bold/opaque](build/delight/review/voice-recording-ax3-bold-rt.png),
[Confirmation AX3/Bold/opaque](build/delight/review/voice-confirming-ax3-bold-rt.png).

- **Two layers:** pass; mic/level/transcript remain content, navigation and explicit actions are glass.
- **Canvas:** pass; true black, Voice red only, no decorative waveform or glow.
- **Round-first:** pass; the level ring is centered on the mic and does not cross Listening or toolbar glyphs.
- **Sizes:** pass; transcript wraps/scrolls at AX3; persistent recording indicator remains visible.
- **Colour:** pass; real PCM level drives the red arc; black primary ink passes rendered RT tint contrast.
- **Approvals/agents:** pass; transcript confirmation has no timeout; Send disables immediately after consent.
- **Motion/accessibility:** pass; amplitude information remains under RM with fixed geometry; send flight cross-fades.
- **Gesture alternatives:** pass; Hold to Talk, Stop, Cancel, Send and transcript advance are explicit controls.
- **Apple craft:** pass; earlier off-center ring and redundant intersecting mic glyph were corrected.

## Play

Evidence: [Empty Default](build/delight/review/play-default.png),
[Registered AX3/Bold/opaque](build/delight/review/play-registered-ax3-bold-rt.png).

- **Two layers:** pass; registry rows/empty state remain content; Back/Close are navigation controls.
- **Canvas:** pass; true black with Play green glyphs, no coloured list fill.
- **Round-first:** pass; registry entries use the existing curved list and empty copy stays central.
- **Sizes:** pass; AX3 registered app name and offline description fit without clipping.
- **Colour:** pass; Play chrome uses green regardless of the registered game's internal colour.
- **Approvals/agents:** pass; launch requests are copied/bounded and dispatched on main, outside LVGL.
- **Motion/accessibility:** pass for shell chrome; game rendering/motion belongs to the sibling games stream.
- **Gesture alternatives:** pass; Close and main-owned held PWR both exit, including a paused application.
- **Apple craft:** pass for list/empty/launch status; this does not certify an unfinished game's visuals.

## Motion samples and functional checks

Inspect the ordered PNGs rather than only final settled frames:

- `app-launch-{016,096,176,320,600}.png` and `app-reverse-*.png`: flat disc expands and dissolves cleanly; no cached-row black rectangles.
- `icon-press-*.png`, `apps-press-*.png`, `scroll-drag-*.png`, `scroll-release-*.png`: immediate feedback, overscroll and active rim geometry.
- `stack-page-*.png`, `count-roll-*.png`, `agent-done-*.png`, `approval-arrival-*.png`: paging, tabular roll, draw-on and non-stealing arrival.
- `approve-queued-*.png`, `deny-queued-*.png`, `approve-delivered-*.png`, `deny-delivered-*.png`: local choice feedback stays distinct from authoritative receipt.
- `voice-level-zero-*.png`, `voice-level-full-*.png`, `voice-send-*.png`: actual level endpoint mapping and recipient-directed send feedback.
- `all-clear-*.png`, `all-clear-expired-2200.png`: visible two-second reward and restoration of the ordinary Face.
- Matching `ax3-bold-rt-rm` sequences retain information while replacing spatial/depth motion with cross-fades.

`test_delight` drives real pointer input and checks Face cadence/muting/lifetime,
counter/clock pixels, battery separation, live voice geometry, rendered primary
contrast, actual launcher disc/glyph contrast and single-polarity ink, enlarged header clearance, timed rim fading, authoritative-only
all-clear, and the registry launch/service/exit boundary. `test_shell` retains
its unchanged-status and flat-motion zero-repaint checks by isolating ambient
working sessions in those two tests; its rim geometry assertion measures an
actively scrolling indicator, with idle fade tested separately.

Commands: `cmake --build sim/build --target test_delight test_shell`,
`sim/build/test_delight`, `sim/build/test_shell`,
`make -C firmware/components/orb_shell/tests test`.

Final independent rerun: **223 delight UI checks**, **1,105 shell contract
checks**, and **46 ASan/UBSan shell service checks** pass. The two existing noise
tests each report zero frames and zero invalidations. Fresh Smart Stack and
Notifications AX3/Bold/opaque captures confirm that title ink clears Back;
the Dot Reply capture confirms indigo material with black emphasized ink.

Independent review is evidence for lead review, not permission to merge. Keep
the explicit launcher catalogue/adaptive-ink deviations visible in the handoff.
