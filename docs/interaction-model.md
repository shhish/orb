# Orb interaction model v3: edges navigate, the centre belongs to content

Owner revision, 2026-10-28: system gestures start at the bezel; centre scrolls must never become navigation. This revision supersedes v2 and its pull-at-list-top rule.

The owner, 2026-10-27: "I still don't see the UI/UX really making use of the round display and the fact that it's small… make use of gestures (swipes left, right, top, bottom), transitions… something smart and fun." The research is in `docs/research/round-ux.md` (on `orb/integration`). `hig.md` still governs; this document defines **how you move around and how it feels**.

## Principles

1. **Everything is a place, and your finger moves the places.** Every navigation is a direct manipulation: the screen tracks the finger 1:1 from the first pixel, the destination peeks in, and on release a spring either completes or returns, based on distance and velocity. Nothing waits for touch-up to start animating. Animations are interruptible and reversible mid-flight (grab it again).
2. **The centre is for reading, the rim is for moving.** Content sits in the centre. The circumference carries position (page dots, scroll indicator), relationships (orbiting bodies) and continuous input (the rim as a crown).
3. **One thing per screen.** On a 1.75" circle, each screen answers one question. Depth comes from places and focus, not from density.
4. **Motion answers a question.** It explains where something came from or went (iris open from a token), what's a peer (slide), what follows the finger (value), or that something finished (a brief settle). Never perpetual decoration. Idle is still; a real, named wait may breathe (`delight.md` §6).
5. **Navigation alternatives are physical.** Design 1.0, owner decision 2026-10-28: BOOT = Back (Apps on the Face), PWR short = lock, rim and twist scroll or step focus. These always-available controls replace redundant on-screen navigation chevrons.

## Gesture ownership (owner decision, 2026-10-28)

A contact is an **edge contact only from its DOWN point**: on the 466×466 panel,
its distance from (233,233) is at least 193 px (the outer 40 px). Moving into the
band later cannot promote a centre contact. The nearest cardinal direction by
angle (±45°) chooses top, bottom, left or right, including diagonal starts.
The first approximately 12 px retain the radial-versus-tangential arbitration;
then the recogniser owns the contact until lift or cancellation.

| Edge contact moving inward | Result |
|---|---|
| Top, down | Notifications |
| Bottom, up | Smart Stack |
| Left, right | Back, including dismissal of any layer |
| Right, left | Control Center |

Each system transition tracks the finger and retains its existing commit,
cancel, reversal, regrab and rubber-band behaviour. Outward travel does not open
a system destination. Back is identical for a left-edge swipe and BOOT: reverse
iris to the launching token when available, otherwise slide right. Every app
opened from Apps, including Updates, Battery, Sound, Settings subpages, Time,
Play, Home, Dot, Agents and Sessions, must have a return path.

Agents and Home are the Face's neighbouring places. Their shell-level Back
always returns to the Face, including entry through Apps, a complication,
Stack, Compass or developer `goto`. Home's room/accessory Back and Agents'
session/subpage Back unwind locally first. Other Apps destinations retain
their launching-token return path. This canonical Face parent supersedes the
earlier Home/Agents return-to-Apps behavior.

| Open layer | Edge dismissal |
|---|---|
| Notifications | Bottom edge, upward; or Back |
| Smart Stack | Top edge, downward; or Back |
| Control Center | Left edge, rightward; or Back |

**Centre contacts belong to the content, always:** scroll, tap, hold, value drags
and local carousels. They never open or dismiss system layers, invoke Back, or
move between places, including on the scrolling Home Rooms list and Agents. A list
pull at its top stays a content scroll. Its scroll position never changes gesture
ownership. Home opens rooms, Climate and Scenes from named list rows; horizontal
swipes on the landing list do nothing. Accessory pages remain local content, not
shell places.

The sole navigation exception is **Face (and the Always On Face)**, which has no
scrolling content. While locked, the first waking contact is consumed and cannot
navigate; after wake, its centre swipes keep the following map:

```
                    ↓ down: Notifications
   Agents  ← right  [ FACE ]  left → Home
                    ↑ up: Smart Stack
   PWR short: lock · double PWR: Control Center · BOOT short: Apps on Face
   Hold PWR 2 s: Power · hold BOOT 350 ms: talk · long-press: Compass
```

- **The rim as a crown.** An edge contact moving **tangentially** captures the
  current scroll, focus or value rotary owner. Clockwise means next, down or
  increase. A small arc indicator follows the finger, detents tick at items or
  steps, and a bounded fling can continue scrolling. Crown input never clicks,
  confirms an approval, sends, toggles or unlocks.
- **Tap versus scroll.** A tap travels less than 10 px and lasts less than 500 ms.
  Once travel reaches the drag threshold, or scrolling/dragging begins, that
  contact can never click even if it returns to its DOWN point. Remove its
  pressed state immediately, without a release flash. Starting a list scroll on
  a button or toggle scrolls its parent; it never activates the control. This
  applies to lists, Home Rooms/room/accessory surfaces, Settings toggles, Time,
  Play, Sessions and Notifications. Deliberate long holds retain their own
  actions and are cancelled by movement.
- **Long-press: the Compass.** Holding a blank area for 400 ms opens four destinations at N/E/S/W: Agents, Home, Play, Settings. Drag towards one and release to go, or release in the centre to cancel. It's also reachable from a visible button in Apps. It's for navigation only.
- **Physical buttons:** see the exact mapping below. In Apps, the touched icon names itself, and tapping opens it with an **iris**.
- **Pick up / set down:**
  - **On the desk**, the Face is quiet: big time and one attention mark.
  - **Picked up**, it wakes into the richer glance: complications and the next card.
  - **Face-down** silences it.
  - There is a visible Quiet/Active toggle in Control Center. Motion never navigates, approves or sends.

## Transition vocabulary

| Relationship | Motion | Implementation (cheap first) |
|---|---|---|
| Peers (places, pages) | **Slide**, tracking the finger, with a 6% parallax gap; rubber-band at the ends | Cached full-screen snapshots of the neighbours in PSRAM; translate them per frame. No rebuild while dragging. |
| Opening an object (app icon, agent body, home satellite, card) | **Iris**: a circle grows from the token's position to the full disc, revealing the destination; the token's glyph rides along and becomes the header | Analytic circle spans over the cached destination snapshot; only the ring band changes per frame |
| Back | **Reverse iris** to the token if it was opened that way; otherwise a slide right | Same snapshots |
| Layer over a place (Stack, Notifications, Control Center) | **Sheet** from its arc; the place behind scales to 92% and dims to 60%, with no blur (cheap, true black) | Background snapshot scaled once, cached |
| Value follows the finger | Immediate tracking, then a spring settle on release | Arc and value region only |
| Done | A brief settle (≤ 300 ms) into a still receipt | One small sprite or shape change |

Springs per hig.md §6 (response 0.35–0.55 s, damping 0.8–1.0). Reduce Motion means a 150 ms cross-fade and no scale or parallax. Every transition must hold **≥ 45 fps on the board while dragging**: it moves cached pixels, it doesn't re-render content.

Reverse iris starts where its aperture first reaches the round panel, skipping
only the provably invisible rectangle-corner prefix. Include panel edge coverage
and the entire rim in that proof. The visible aperture/rim pixels remain the
same; drag distance, release velocity and regrabbing use the remaining interval.

## Places, redesigned around the circle

- **Face:**
  - Time dominant.
  - A strict centre axis at x = 233: the largest clock, then the secondary date ("Tue 27").
  - Waiting requests replace the date with one orange hand capsule ("2 waiting"); zero waiting has no label.
  - One row of Home and battery complications mirrored around the axis, sharing a type style, icon size and baseline. No session/rim arc.
  - A small offline indicator appears only when the hub is unreachable. Connected/Wi-Fi text is omitted.
  - One centred Apps glass capsule and bottom page dots. Apps contains Stack, Notifications and Compass as visible routes.
- **Agents (the orrery):**
  - Up to four session bodies on fixed orbital slots, with the focused one large at the centre showing its name, state and last step.
  - Use the rim or a swipe to rotate the orbit and bring the next body into focus. Tap it to iris into the session.
  - Overflow and idle sessions sit in a "More" body that opens the focus-lens list.
  - At AX3 or under Reduce Motion it's just the list.
- **Home (named room list, owner revision 2026-11-01):**
  - Rooms stay in model order as named rows with state subtitles and optional temperatures.
  - One tap irises into that room; list rows never toggle an accessory. Vertical drag, rim and twist scroll.
  - Climate and Scenes are conditional rows after the rooms and push their existing pages. Back restores the list scroll; fresh entry starts at the top.
  - Horizontal centre swipes do nothing; the landing title has no hidden action.
- **Smart Stack:** one card per page (vertical snap, rim scroll), with a **focus lens**: the centred card is full, and its neighbours peek as thin arcs above and below.
- **Notifications:** newest at the top. Later, the **Time Tide** history scrub (the rim moves through the day), once the hub supports history.
- **Apps:** the honeycomb stays, sized to the circle. Each icon names itself under the finger, and opens with an iris.
- **Play: Marble Conservatory** (later). Tilt the marble; the rim rewinds the marble's own path.

## Fun, with restraint

- **Caused, brief, still at rest.** An agent body settles with a small bounce when its agent finishes. The approve fill and check stay as today. When the last card is answered, "All caught up" arrives as the bodies settle into a calm ring.
- **The Face notices you:** when picked up, the time lifts subtly (2 px parallax) and the attention mark gently scales in. It settles within 400 ms.
- **Sound** pairs with motion at meaningful moments only: detent ticks, iris open (a soft whoosh under 120 ms), approve. Silent mode is respected.

## Engineering shape

A new **`orb_nav` component** owns the interaction model:
- **Gesture recogniser:** a state machine with ownership, radial-versus-tangential arbitration, edge detection, velocity tracking and cancellation.
- **Spatial navigator:** the place graph, plus the layer stack (sheets) and the back stack.
- **Transition compositor:** cached PSRAM snapshots of the current and neighbouring screens, slide, iris spans, sheet scale and dim; interruptible springs.
- **Rotary service:** a per-screen owner, detents, bounded fling.

`orb_shell` and the apps register places, tokens and rotary owners through `orb_nav`, instead of hand-building transitions. Screens are snapshotted when they settle, so a drag only moves pixels.

Memory: two to three full-screen RGB565 snapshots at about 434 KB each in PSRAM (about 3.6 MB free) are acceptable; snapshot admission is bounded and falls back to a live slide.


## Physical buttons (owner revision, 2026-10-28)

PWR is GPIO 3, active high; BOOT is GPIO 0, active low. Main samples every 20 ms even when LVGL is idle, the display is dimmed/off, or an app is paused. The portable recognizer requires 30 ms of stable input, reserves a short press for a second press beginning within 300 ms of release, and emits one hold per contact. Singles therefore arrive 300 ms after release; a double replaces both singles.

| Input | Result |
|---|---|
| PWR press, display off or at ≤10% | Restore the last brightness above 10% (70% initially). Consume the whole waking contact, including its release/hold; it only wakes and cannot lock again or confirm anything. |
| PWR short, display active | Lock through the shared Always On power path: show the dimmed, redacted Face and lock touch until a wake. |
| PWR double | Open Control Center. |
| PWR hold 2 s | Power sheet: Sleep display / Restart, with a visible close glyph. Neither action runs until a physical touch selects it. |
| BOOT short (double also) | Back, using the same shell Back path as the left-edge gesture, including layer dismissal. On the Face with nothing behind it, open Apps. A double produces one Back. Close an open power/pause sheet first. |
| BOOT hold 350 ms | Show a red mic + Listening immediately on every screen and start the existing voice flow for the focused agent/card, otherwise Dot. |
| BOOT release after hold | Stop capture and transcribe. Keep the existing transcript review and explicit physical Send; release never confirms an answer. |
| PWR short while a game is running | Pause the game, then use the same Always On lock path. The lock does not leave the game running behind the Face. |

Idle auto-lock uses the same lock path as PWR short. Tap-to-wake, raise or PWR wakes the display; the waking contact is consumed before content can act. Locking or waking never confirms, sends or unlocks an accessory. Physical-input provenance guards remain in force.

Sleep display turns brightness to zero without stopping input polling. PWR wakes it. PWR hold never calls the PMU power-off function: the AXP2101's independent 10 s hardware cut remains available. This revision replaces the earlier PWR Apps/Face and pause-sheet shortcuts; BOOT keeps Back.

Developer commands: `buttons` prints the last 16 raw, debounced or synthetic edges, with monotonic millisecond timestamps and source labels. `press pwr|boot short|double|hold` injects a synthetic gesture through the same main/UI mailbox. Control Center and voice review pause and overlay a full-screen app; Back restores that same app and its earlier pause state. Voice receipts remain visible until explicitly dismissed. A failed voice start shows a slashed mic and “No voice” while BOOT remains held.

Developer `wake`, `goto` and touch commands renew both the power controller's
activity deadline and LVGL's inactivity clock. The power observer follows all
pointer devices, including the developer pointer created after the physical
one. A touch command wakes explicitly before injection; the waking command
does not grant physical-input provenance. `goto` wakes and uses the ordinary
shell navigation path in one UI transaction, retiring pending transitions and
replacing open navigation layers. Agents/Home receive the same canonical Face
history through this route. A rejected wake rejects `goto` too.

A synthetic BOOT hold previews Listening for 250 ms without capturing audio or sending anything. Synthetic power gestures can open the sheet, but cannot choose Sleep/Restart; synthetic touches cannot submit card decisions or Send. Both commands are developer-build-only and available over USB and the authenticated developer transport.

The old owner-visible failures had several causes: raw PWR edges had no debounce/hold/double interpretation or wake policy; the shortcut opened Apps from every non-Apps place rather than returning home; shell BOOT feedback was gated to Home and merely stored a flag, while a separate voice worker owned another hold detector; and public navigation cancelled only presenting snapshots, leaving a pending asynchronous prepare able to replace a later button route. The new recognizer has one owner, the UI mailbox preserves gesture order, and public navigation retires pending/presenting/fallback transitions before routing.

Agents centre swipes remain local content: the header can select Sessions or Usage, and vertical content swipes, rim and twist step session focus. They never navigate to another shell place or open a system layer; BOOT returns.
