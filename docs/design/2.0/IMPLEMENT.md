# Implementing Orb Design 2.0

Design 2.0 (owner review, 2026-10-28) makes the OS one family on the round display. The frames here are the source of truth. `Main.dc.html` is the system sheet, and the canonical, commentable canvas is https://claude.ai/artifact/BUtb5XDjfzC8A6Vn2jZcmU. Where 2.0 is silent, Design 1.0 (`docs/design/1.0/IMPLEMENT.md`) and `docs/hig.md` still apply; how to read a frame is unchanged from 1.0.

## Why 2.0 exists

The owner found circles of many sizes, irregular buttons, truncated titles, and behaviours that fire from the wrong place:
- Notifications opening from mid-screen scrolls.
- The rim silently becoming a volume knob.
- Taps firing during scrolls.

Two audits put numbers on it, and both are in `docs/agent-briefs/wave2/`:
- `consistency-audit.md` counted six disc sizes (60–88 px), ten capsule heights (36–72 px), three header architectures and five hand-typed greys.
- `behaviour-audit.md` lists 30 behaviour irregularities.

The root cause is that no shared component layer exists: every app builds its own controls. 2.0 fixes that first.

## The kit (orb_ui): every screen builds from these, nothing hand-rolled

| Role | Spec |
|---|---|
| Disc | 80 px for every standalone round control: app icons, rooms, accessories, Control Center toggles, play, pause and cancel. 56 px mini for utilities (Face apps, close). 40 px leading a list row. 180 px artwork only. Focus is a 3 px white ring 4 px outside the disc, never a bigger disc. |
| Primary button | Capsule, 64 tall, tinted with the app accent (accent text at about 26% tint plus an accent inner border), Headline 28 semibold. At most one per screen, centred on the bottom arc. |
| Secondary button | Capsule, 52 tall, neutral glass, Caption 2 25 semibold white. Replaces every text-only button ("Done"). |
| Glyph button | 80 px disc: tinted primary or glass secondary. A pair sits symmetrically on the bottom arc. |
| Rim title | Curved text along the top rim: baseline radius 200, centred at 12 o'clock, Footnote 1 23 semibold, caps, letter spacing about 1.2. Coloured with the app accent, or 60% secondary for system lists. It names the place, and counts belong in the content. At most 120° of arc (about 20 characters). Longer titles step down to Footnote 2, then ellipsise. When a full rim ring is drawn (Timer, a light dial), the title sits inside the ring on a 176 px baseline and never crosses it. It replaces every top header and truncated title. Use lv_arclabel. |
| Rim caption | Curved upright text along the bottom rim, baseline radius 212. Used for focused names (the Apps honeycomb) and hints. |
| Page position | Dots along the bottom rim (8 px, 14 px pitch), inside any rim ring. Scroll position is a 4 px arc on the right rim at radius 224, outside the widest chord-fitted rows. |
| Rim ring / gauge | Concentric with the bezel. The outer ring is 10–14 px wide at radius 213–218, inner rings sit 20 px inside it, and the track is the accent at about 18%. |
| Knob | Every arc slider uses the same 30 px white knob. |
| Row | The chord-fitted round-list row (Design 1.1), 72 tall, with a 40 px leading disc. Every list row has one, About and Accessibility included. |
| Colour | Tokens only: secondary #EBEBF5 at 60%, system grey #8E8E93. Accents follow Main.dc.html. Dates use iOS Calendar colours: the weekday #FF453A, the day white. |
| Press | One response for every tappable: 94% in 80 ms, glass brightens, 160 ms spring back. A cancelled contact (scroll) never activates. |

## Behaviour rules (system-wide)

- **Gestures:** v3 (`docs/interaction-model.md`). System gestures only start in the 40 px edge band. The centre belongs to content, except on the Face.
- **The rim** drives the visible content's own value or focus, or nothing. There are no global rim owners.
  - The volume HUD is display-only.
  - Volume is set in Control Center: tap the speaker chip, then the bottom arc controls volume. Tap the sun chip and it controls brightness again. Sound settings can also set it.
- **Buttons:**
  - PWR short locks; PWR double opens Control Center; PWR held 2 s opens the power sheet.
  - BOOT short is Back, and opens Apps on the Face. BOOT hold is talk.
  - Holding the Face opens the face gallery. The long-press Compass is retired; the Apps honeycomb covers it.
- **Motion and feedback:** use the Main.dc.html table.
  - Every app opens with an iris from its icon and closes back into it.
  - Layers arrive from their edge and leave the way they came.
  - Every place restores scroll, focus and page.
- **Send and Save** always require an explicit tap. Nothing submits on a countdown.

## Screens

- Faces: FaceOrb, FaceMeridian, FaceActivity, FaceGallery, FaceColor, FaceSlots and Lock.
- Home: HomeRooms, HomeRoom, HomeLight and HomeScenes.
- System surfaces: Apps, Control and ControlVolume, Notifications, Stack and Settings.
- Apps on the shared kit: Timer, Play, GameOver, Updates, Agents and Usage.
- Next: Weather. Weather data comes from the hub (Open-Meteo, for an owner-set location).

No text may overlap a ring, disc, hand or other text in any frame state. Check every screen in Default and AX3 by rendering it. `png/` holds the frame renders (`dc2png.py`).

Process: as in 1.0. Build to the frame, render the simulator in Default and in AX3 + Bold + Reduce Transparency, pass `docs/agent-briefs/wave2/design-gate.md`, and report what you could not match. The lead reviews board screenshots before anything ships.
