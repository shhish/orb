# Home polish review — 2026-10-28

Historical polish-branch receipt. The merged native-list implementation,
current validation and screenshots are described in
`HOME_MERGE_REVIEW.md`; its continuous lists supersede the paging notes below.

Uncommitted work on `orb/w2-home-polish`. No board, live HomeRelay, owner configuration, credentials or running services were changed. The implementation follows the exported Design 1.0 HTML, the Inter styles, and `docs/hig.md`. Simulator evidence is a review gate; the lead's real-board screenshots remain the release gate.

## Changes and reasons

- **Device lists:** stable category ordering avoids rows moving when brightness changes. At most five rows exist at Default, two at AX3; large rooms advance four devices with one row of context at Default, two at AX3. Vertical swipes, rim and case twist work through the actual shell, coalesce batched detents and clamp at the ends. A tiny virtual scroll viewport preserves previous-page gestures instead of letting Notifications capture them. BOOT restores the same room/page. Hidden semantic navigation stays available to tooling and has no competing touch target.
- **Names and state:** room titles and trailing values fit their circle chords. AX3 stacks device name and state. Long names ellipsize in rows; the room action surface shows the full bounded room name. Glyph discs are content, making the whole named row the target. Offline, unavailable, pending, unknown and failed values have explicit text; missing state is not invented as zero, Off or Locked. Empty homes, loading homes, empty rooms and no-favourite homes remain useful.
- **Writes:** room toggles require physical input. Deferred topology rebuilds reject old actions, preventing a stale displayed name from targeting a replacement alias. Pending values remain distinct; failures restore confirmed state and give a brief 3.5-second message. Unreachable/read-only row taps open explanatory details.
- **All off:** tapping the room title opens a flat content action for lights, switches and outlets. It sends explicit `on:false` values, drains the existing four request slots across up to 64 targets, never retries, records inaccessible targets and partial failures, and cancels unsent work on disconnect or replacement. Fans, locks and sensors are excluded. Pending, requested and partial-failure receipts are distinct. The flat action avoids an unnecessary wide glass cache.
- **Scenes:** scene-list and Rooms capsules show Running, Requested or a matching Scene failed message; an unrelated accessory error is not a scene failure. Physical-input and known-unlock guards remain enforced. Scene and All off controls visibly disable during conflicting work. Accepted means HomeRelay completed its request, not that every physical device reached its final state.
- **Dials and sound:** pending/failure feedback occupies a context lane rather than hiding the reading or device name. Failure plays `ORB_SOUND_FAILED` once; confirmations use `DONE`, toggles `HOME_ON/OFF`, and meaningful dial steps `DETENT`. No new legacy START/STOP aliases. Light-off icons and readings are neutral. Existing retained rim drawing and motion preferences remain intact.
- **Thermostat:** a neutral 72px sliders control exposes Off/Heat/Cool/Auto. Current temperature, target and mode are separate and explicit in °F; Fahrenheit changes are one degree and accepted relay values remain authoritative.
- **Locks and sensors:** pending locks say Locking/Unlocking and Awaiting Home, rather than prematurely claiming a final state. Fresh uninterrupted physical one-second unlock holds, cancellation and synthetic-input rejection are tested. Sensors show temperature, humidity, motion/contact and updated age; missing/future timestamps say Age unknown. Age refresh uses existing polls, with no extra timer.
- **HomeRelay client:** a delayed write reply cannot overwrite newer SSE fields or a replacement snapshot in the hub cache. Per-accessory serialization remains correct while shared write slots are busy. Tests use isolated fake relays and loopback-only listeners.
- **Frame cost:** idle polls check the revision before hashing/copying the 64-device model. Another room's state event preserves the visible row objects and press targets with zero invalidations and frames. Same-room state changes still rebuild this bounded row page; their real-device cost needs measurement.

## Validation

- `nice -n 10 cmake --build sim/build -j4`: passed.
- `nice -n 10 ctest --test-dir sim/build -j4 --output-on-failure`: **861/861 passed**, including four Home behavior profiles, Default/AX3/AX3+Bold+Reduce Transparency captures, combined Reduce Motion behavior, and actual workspace-shell input tests.
- `nice -n 10 scripts/idf.sh build`: passed; unsigned `firmware/build/orb.bin` is 0x2a0000 bytes with 56% app-partition space free. This is a build artifact, not a release.
- Sanitized Home host suites: 143 model/link, 225 model, 483 PSRAM and 1,488 byte-exact wire checks passed. HomeRelay/CLI pytest: 90 passed using this worktree via `PYTHONPATH=hub`.
- Ruff format/check including ASYNC, shellcheck on both screenshot scripts, and `git diff --check`: passed.
- Retained-render tests: settled Home surfaces produce zero idle frames/invalidations. The rim sequence renders eight frames; largest invalidation 18,240 px Default / 20,160 px AX3; largest band-rounded flush 36,516 / 40,728 px. These are host damage receipts, not ESP32 FPS or latency measurements.

## Sheets and visual gate

Run `sh sim/home-polish-sheets.sh` after CTest. Every pair uses `python3 sim/png_tools.py concat` with the canonical parent PNG on the left and the firmware render on the right. There are **64 overview comparison pairs** under `sim/build/home-polish-review/{default,ax3-bold-rt}/`. Detail pairs live under `sim/build/home-polish/detail/`. Added states have no dedicated reference frame and use the closest parent.

| Surface | Default / accessible evidence |
|---|---|
| Rooms, no favourites, many rooms, offline/loading/empty | `home-polish-review/{default,ax3-bold-rt}/rooms.png`, `no-favourites.png`, `busy-rooms.png`, `empty-home.png`, `loading-home.png` |
| Device lists, 24 devices, final page, pending, empty | Same directories: `room-living.png`, `busy-room.png`, `busy-room-last.png`, `pending-room.png`, `empty-room.png` |
| Room action and All off receipts | Same directories: `room-actions.png`, `all-off-pending.png`, `all-off-accepted.png`, `all-off-failed.png` |
| Scene list and Rooms capsule receipts | Same directories: `scene-{pending,accepted,failed}.png`, `capsule-{pending,accepted,failed}.png` |
| Light, colour, white, fan, failed/pending details | `home-polish/detail/design-dial-{default,ax3}.png`, `{light,light-off,colour,white,fan,error,long-error,wide-error,pending-light,pending-switch}-review.png` |
| Thermostat and modes | `home-polish/detail/thermostat-review.png`, `thermostat-{heat,cool,auto,off,modes}-review.png` |
| Locks, unknown/offline and pending | `home-polish/detail/{lock,unlocked,unknown-lock,offline-lock,offline-unlocked,pending-lock}-review.png` |
| Sensors and ages | `home-polish/detail/{temperature,humidity,motion,contact,sensor-aged,sensor-unknown-age,sensor-future-age}-review.png` |
| Full emulator Home, plus Face/card accessibility regression | `home-polish/emulator/home-{default,ax3-bold-rt-rm}-sheet.png`; `{home,face,card}-{default,ax3-bold-rt-rm}/frame-000060.png` |

The surfaces above were rendered and visually inspected; independent agents reviewed the overview and detail groups. The final emulator Home/Face/card captures were also inspected.

- **Two layers — pass:** lists, readings, discs, scene execution and All off are flat content. Glass stays on controls/navigation, no glass on glass, at most three visible shapes and one tint on touched device surfaces.
- **Canvas — pass:** true black, no new gradients/glows or touch halos. The emulator's dark-grey area outside the round panel is its desktop surround.
- **Round-first — pass:** headers, names and bottom-row state lanes fit actual line-box chords; Mode uses the approved (365,101) diagonal. All off receipts have a separate lane; previous bezel clipping and footer overlap were corrected.
- **Sizes — pass:** existing `orb_text_apply` styles, Default and AX3 reflow, 72px mode choices, full row targets, no interactive 34/42px discs. Peripheral rows and the existing 64px dial power control meet the 46px minimum. No ad-hoc fonts.
- **Colour — pass:** state has words/symbols as well as colour; Home yellow and the real accessory-state exceptions remain semantic. Off/unknown/offline do not claim active colour.
- **Motion — pass in simulator:** interrupted rim/spring damage checks and Reduce Motion profiles pass. Reduce Transparency and Bold are honored; no extra animation timer was added.
- **Navigation — pass:** physical BOOT/PWR and real rim/twist/vertical paths work. No new visible back or paging controls; hidden semantic actions do not intercept physical touches.
- **Feedback/accessibility — pass:** pending, accepted, failed, unknown and cached states are distinct. Synthetic actuation and unsafe unlock paths remain rejected; sound is supplemental. Long/wide strings and future timestamps are covered.
- **Craft — pass for simulator review:** the accessible layout preserves hierarchy rather than squeezing text. Panel colour, cadence, speaker quality and real input feel remain board gates.

Intentional reference differences: fixture names/counts/readings differ from the static mockup; extensions have no dedicated Design 1.0 frame; AX3 uses fewer rows and stacked state. Default room title maps 24px to the existing 23px Footnote 1 style. Small reference text below 20px maps to the existing 21px minimum, and peripheral state stays legible rather than inheriting the reference's half-opacity text. These follow HIG §§3–5 over the smaller/less legible sketch values. Long device names ellipsize within the wire's 32-byte limit; no new protocol field was invented for full names or timestamps.

## Allocation ledger

These are retained payload sizes from the actual Xtensa compiler flags and object/style probes, **not measured board heap headroom**. Allocator headers and fragmentation are excluded.

| Existing payload | Before → after | Growth |
|---|---:|---:|
| One model store (same PSRAM allocation) | 30,040 → 30,128 B | 88 B |
| Snapshot, each existing retained copy | 14,688 → 14,728 B | 40 B |
| Home view (same PSRAM allocation) | 1,704 → 1,720 B | 16 B |
| Device detail, including its snapshot | 15,160 → 15,208 B | 48 B |

Across the store, link chunk, workspace copy, view, view snapshot and open detail, existing payload allocations grow **272 B** total (224 B without detail). No additional model/view payload allocation call was added; the four pending slots stay 164 B each. The batch uses a 64-bit mask.

New UI allocation groups:

- Virtual room viewport + extent: two bounded LVGL objects, local styles and child storage, **250 B gross payload** across ten allocations and one 4-byte parent-child-list growth. No raster buffer or callback allocation. Ordinary visible rows are limited to five/two.
- Flat All off button + label: **533 B gross LVGL payload**, including text/styles/events/child storage. This is a separate action page and does not coexist with the virtual room viewport. No glass/SDF/material allocation.
- Visible thermostat Mode: existing button/label reused; **100 B** new glass state plus **120 B** ordinary LVGL metadata/text delta. Its 72px mask sizes are already shared/prewarmed. Current configuration has fused and retained materials disabled, so no new retained backdrop/material buffer.
- Existing labels still allocate bounded string storage when their content changes. No per-device heap objects were added for the 24/64-device inventories. Simulator fixtures/tests allocate temporary host snapshots only.
- The hub's race tracking is bounded to at most eight in-flight writes with at most 17 characteristic keys each and is removed in `finally`; it adds no firmware allocation.

The flat All off action avoids an otherwise unprewarmed wide glass cache (72,896 B at rest; up to 302,600 B across press sizes). Scene capsules retain their original dimensions to reuse existing masks.

## Lead/owner and board work still required

1. Lead: sign a dev-only build using the existing release process, flash/OTA only the dev board, and capture real `scripts/orb-dev.sh shot` evidence for Rooms, a busy room, All off, thermostat/modes, pending/failure, locks and sensors in Default and AX3+Bold+Reduce Transparency. No second-board tests.
2. Measure `orb-dev.sh heap` owner rejections and internal/PSRAM floors before/during/after repeated Rooms → room → detail → action navigation. Pay attention to the added Mode glass metadata and ordinary LVGL allocation groups; simulator budgets do not enforce these limits.
3. Measure real frame/input latency while the current room receives HomeRelay traffic and during long rim/twist dial drags. Same-room row revisions still rebuild only the bounded page; host results do not prove its target-device cadence.
4. With the owner, review detent/toggle/done/failed sounds, Silent behavior, thermostat normalized accepted values, one-second lock-hold cancellation and enclosure twist feel. Use a deliberately controllable light and a guarded lock for live checks.
5. Confirm allowed scene intent: HomeRelay's opaque scene API does not expose actions, so arbitrary scenes cannot be certified unlock-free from metadata. Known unsafe flags and explicit physical taps are enforced; Orb must not present scenes as an unlock affordance.
