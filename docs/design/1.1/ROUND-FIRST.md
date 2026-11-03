# Orb Design 1.1: round-first

Owner feedback, 2026-10-28: Home "does not fit on the screen … this layout would be
great on a squared display but this is round so the sides are just being wasted …
some [screens] might need rethinking". Design 1.1 revises the screens that were
laid out as rectangles inside the circle. Everything not named here stays Design 1.0.

Evidence: board captures in `work/board-review/round3/` (real fonts, assets v5).
Part of what the owner saw was not layout. The bench board ran its 28 px rescue
fonts because the signed font pack did not match the firmware, so every large style
collapsed and headers wrapped. That is fixed and is not a design change, but it
means each review must confirm `assets: … Signature verified` in the boot log.

## Principles

These restate `docs/hig.md` §3 and §7 as checks:

1. **Width follows the chord.** A row, capsule or line box is as wide as the
   chord at its *current* height allows (capsule end circles inside the safe
   circle, 16 px text margin). Fixed 340 px rows are a rectangle; they leave
   about 80 px per side unused in the middle band. hig.md §7 already says "row
   width follows the chord at that row's height", and the code did not.
2. **One focal element at the centre.** Secondary items surround it, peeking at
   the sides or arranged on a ring, and are not stacked in a column.
3. **The rim is a surface.** Progress, scales, selection and position live on
   the rim as arcs, not as horizontal bars or dots.
4. **Headers fit the top chord on one line.** Detail that does not fit moves
   into the content, not into a wrapped second line.
5. **Nothing touches a ring or the bezel.** Text keeps 16 px from any arc it
   sits inside, measured at both ends of each line.

## 1. Lists (Settings, Sound, Battery, Home devices, Notifications, Sessions)

- **Rows:** row capsules size to the chord at their current centre, continuously
  as they scroll. In the middle band a 72 px row is about 418 px wide; near
  the rim it narrows to the capsule bound and then takes the existing edge
  scale and fade.
- **Implementation:** one shared rule in `orb_round_list`, replacing
  per-surface widths (Sessions' own chord rule moves there). Accessories stay
  right-aligned to the row's current trailing edge, and titles get the extra
  width before truncating.
- **Performance:** must stay a cached presentation. Use stepped widths
  (precomputed per row height), not a relayout every frame. Prove it with the
  existing scroll benches.

## 2. Home: Rooms dial

This replaces the Rooms column (Design 1.0 `HomeRooms`). The precedent is the
watchOS 1 Friends circle: a ring of discs, rotated by the crown.

- **Ring:** up to 8 room discs, 80 px each, on a 168 px radius ring around the
  centre, starting at 12 o'clock and running clockwise.
  - Glass disc. Tinted Home yellow with a dark glyph when any light, switch or
    outlet in the room is on; neutral glass otherwise.
  - The disc shows the room's initial (Title2 emphasised), as the Friends circle
    did. Use two letters ("Be", "Ba") when rooms share an initial. The icon set
    has no room glyphs, and adding any would change the signed font pack.
  - More than 8 rooms continue on further rings. The rim steps past the last
    disc into the next set, with no page dots.
- **Focus:** one room is focused.
  - Its disc scales to 1.12× with a 2 px white rim.
  - The rim (drag or crown) and twist step focus around the ring, with one
    detent sound per step.
  - Tapping a disc focuses it; tapping the focused disc or the centre opens
    the room.
  - The default focus is the first room that is on, else the first room.
  - Focus is restored after Back.
- **Centre** (inside the ring, about a 236 px circle):
  - Caption: "Demo Home · 6 on" (Footnote, secondary).
  - Room name: Title3 emphasised, one line, fitted to the chord.
  - State: "3 lights on · 71°F", yellow when on, or "Off".
  - One capsule at the bottom of the centre circle: **Scenes**. It opens the
    existing scene list as a round list, with the same safety rules as today.
- **States:**
  - Offline or loading: the ring is absent and the centre reads "Connect your
    Mac to load Home." with the header caption.
  - Pending and failure feedback stays on the focused disc's rim, as it does on
    today's rows.
- **AX3:**
  - 6 discs per ring at 88 px.
  - The centre drops the caption, and its text wraps to two lines at most.

## 3. Home: room detail

Keep the device list with the §1 chord-fitted rows. Keep All off and the
thermostat dial (already round).

## 4. Usage: concentric rings

This replaces today's left/right label pairs, which collide with the ring
("week 70% · Wed 1:44am · stale" crosses it).

- **Rings:**
  - The outer rim ring (10 px, 10 px from the bezel) is Claude's 5-hour use,
    in Claude orange.
  - An inner ring (6 px, 16 px inside the outer one) is Codex's 5-hour use,
    in Codex blue.
  - Both rings start at 12 o'clock and fill clockwise, over a track at about
    12% white.
  - A missing provider draws its track only.
- **Pages:** the rim or vertical paging switches the centre between providers
  (Claude, then Codex). Both rings stay visible throughout, so they can always
  be compared at a glance.
- **Centre** (every line fits the inner ring's chord with the 16 px rule):
  - Provider name, in its colour (Headline).
  - The 5-hour percentage, large (Title1, tabular).
  - "resets 7:49pm".
  - "Week 85% · Wed 3:59pm", ending in "· stale" when stale.
  - "Not available" replaces the percentage when there is no data.
- **Bottom chord:** "1 working", as today.

## Unchanged

These were reviewed and are already round-first: Face, Apps honeycomb, Play
picker, Time pages, Control Center (with its rim brightness arc), Dot,
Notifications, Updates, Battery header and Agents live. Smart Stack cards stay
watchOS-shaped (317 px wide) by design.
