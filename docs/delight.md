# Delight, colour and play

The owner's verdict on the board, 2026-10-27: "The looks are very clean but the fun UI/UX element is not there, and there's a lack of colour. I don't want it colourful, but accent colours here and there to really give it that Apple feel." This document turns that into rules. `hig.md` still governs, and nothing here overrides its "never do" list. Delight comes from **colour with meaning, motion that answers you, and small moments of reward**, never from decoration.

## 1. Colour: black canvas, white type, one accent per thing

Use only the hig.md §5 system colours. Each colour carries a meaning, and an ordinary task view shows at most two accents. The launcher catalogue and Home accessory-state colours are explicit exceptions: app colours identify destinations, and accessory colours report the actual device state. They do not permit more than one tinted glass action. Colour goes on **glyphs, rings, small fills, the toggle-on state and the primary tint**. It never goes on backgrounds or text paragraphs. No gradients or glows anywhere, including touch feedback.

**App identity:** every app owns one accent, used for its icon disc, its title glyph and its primary action tint inside the app.

Flat identity and accessory discs use adaptive black/white Framework7-style glyph ink to meet hig.md §5 contrast. Bright discs must not be forced to use white glyphs. Keep glyph shape and text identity when a hue is neutralised.

| App | Accent | Glyph |
|---|---|---|
| Agents | orange `#FF9230` | terminal |
| Dot | indigo `#6D7CFF` | chat bubble |
| Voice | red `#FF4245` | mic |
| Play | green `#30D158` | game controller |
| Updates | blue `#0091FF` | arrow up circle |
| Settings | gray `#8E8E93` | gear |
| Home | yellow `#FFD600` | house |
| Time | teal `#00D2E0` (`ORB_TEAL`) | timer |
| Notifications | blue `#0091FF` | bell |
| Stack | gray `#8E8E93` | list |
| Compass | gray `#8E8E93` | arrow up right |

**Agent identity:** a small coloured glyph or edge dot per source, never a filled row.
- Claude Code: orange.
- Codex: mint `#00DAC3`.
- Dot: indigo.

Agent colours are not brand reproductions. Don't use other companies' logos or exact brand colours.

**State** (always paired with a symbol or word):

| State | Colour | Symbol |
|---|---|---|
| Needs you | orange | hand |
| Working | blue | animated arc |
| Done | green | check |
| Error | red | exclamation |
| Idle | gray | none |

**Face complications:** tint each complication's glyph or ring with its meaning. For example, an orange count for "needs you", and green or yellow or red battery by level, as iOS shows it. The clock stays white.

**Smart Stack cards:** black by default. A card takes its source's accent only on its glyph, plus a 3 px leading accent bar on approval cards, as in iOS notifications. Never fill a card with colour.

**Reduce Transparency and Increase Contrast:** use the hig.md §5 increased-contrast variants.

## 2. Motion that answers you

All motion uses the hig.md §6 springs, can be interrupted, and becomes a cross-fade under Reduce Motion.
- **Press:**
  - Every tappable thing reacts within one frame.
  - Glass swells 4% and changes its existing material/rim within the control bounds. App icons sink to 92% and spring back. No radial glow or halo is added.
  - Rows dim to 70%.
- **App launch:**
  - The icon zooms into the app, so the app's accent disc expands to the screen edge.
  - The app content fades in.
  - The disc then dissolves to black over about 250 ms.
  - Going back reverses it, landing on the icon.
- **Scrolling:**
  - Momentum with rubber-band overscroll at the ends.
  - The rim scroll indicator grows while you scroll and fades out 600 ms after you stop.
  - Rows ease into their curved scale.
  - Content dissolves under the scroll edge.
- **Paging (Smart Stack, idle list):**
  - Snap with a slight bounce.
  - The page dots on the rim stretch into a capsule for the current page.
- **Tilt:**
  - The parallax and specular highlight already exist.
  - Add a gentle shimmer on the Face's "needs you" complication when you tilt toward it.
- **State changes:**
  - Counts roll like an odometer: tabular digits slide vertically.
  - A finished agent ticks its check mark in with a short draw-on stroke.
  - A new approval slides its card up from the bottom with a soft chime (§3).

## 3. Reward moments, small and brief

- **Answers:** local acceptance gets a neutral press response and explicit “Queued / Waiting for your Mac” text. Only an authoritative Mac result that accepts and confirms delivery of the exact locally submitted answer may produce the final outcome cue, once. Recorded-but-undelivered, rejected and ambiguous outcomes never celebrate and are never silently replayed.
- **Approve / Deny delivered:** a compact green communication check sits in the receipt's normal layout flow, with at least 16 px clearance from its body. Denial remains explicitly labelled “Deny” / “Denied”; a delivered decision must not use failure colour. When the current card's authoritative outcome arrives, return after its brief 600 ms response. Reduce Motion uses the 150 ms fade contract; no touch glow is added. Pending decisions never time out or auto-dismiss.
- **Voice:** while listening, a live level ring around the mic glyph pulses in the Voice red with real mic amplitude, not decoration. On Send, the transcript flies toward the session's row.
- **All clear:** when the last waiting card is answered, the Face shows "All caught up" with a green check for 2 s, then returns to the normal Face.
- **Sounds** are ES8311 clicks and ticks under 150 ms, quiet, muted when Silent is on or the device is face-down (hig.md §8). Every sound is paired with a visual.

## 4. Living Face

- **Time:** the largest display style, centred on x = 233 and slightly above the equator. The date sits below it in secondary text.
- **Attention:** an orange hand capsule replaces the date only when requests are waiting. Zero waiting has no mark. Agent attention never draws a partial bezel ring.
- **Complications:** one symmetric Home and battery row shares a type style, glyph size and baseline below the date/attention line. Home is yellow; battery retains its charge meaning.
- **Navigation:** one centred Apps glass capsule and the page dots; Apps contains Notifications and Stack. The Face shows a small offline indication only while the hub is unreachable.

## 5. Play: games that suit a round, tilting, touch device

Games are offline apps in `orb_apps`: they run with no hub, keep their scores on LittleFS, use the Play accent green in their chrome, and may use more colour inside the game itself. PWR opens their pause sheet; BOOT is Back. The Play catalogue uses each game’s own symbol and identity accent (the app catalogue exception in hig.md §5), keeping the same leading symbol rail even when peripheral metadata is omitted. It has no floating side chevrons.

1. **Marble** (blue `#0091FF`, filled circle): tilt a marble around a round tray to collect dots. The physics comes from the IMU (gravity, friction, wall bounce). It's the proof that games work.
2. **Orbit** (teal `#00D2E0`, concentric circle): tap to switch your dot between two concentric rings and dodge the obstacles coming around. It's quick to learn and suits the circle.
3. **Ring Snake** (green `#30D158`, circular arrows): a snake on a polar grid, steered by tapping the left or right half or by tilting.
4. **Pop** (orange `#FF9230`, sparkles): bubbles drift in, tap them before they reach the rim, and combos change the bubble colour.

Every game targets a steady TE-locked 30 fps or better. It draws only the moving sprites and dirty rectangles, never full-screen redraws. Assets come from small vector shapes or the assets partition.

## 6. Alive: the OS answers, wakes and breathes

The owner, 2026-11-01: the readable Home list lost its fun, and the whole OS should feel alive and breathable, "as if the OS is alive". Everything here follows hig.md §6 and the sections above. Where this section and an older promise above disagree, this section wins.

### The rules

1. **Caused.** Something moves because you did something or the world changed. Nothing moves to look busy.
2. **Idle is still, waiting breathes.** A settled screen draws zero frames and every timer is paused. Only a real, named wait may breathe, and it stops breathing after 30 s even if the wait goes on.
3. **Never lies.** Animation is presentation only. It never changes a model, sends a command, fires a detent or reports a false value through a getter or semantic trace. A value that was never reported stays blank and never sweeps.
4. **Never in the way.** All motion is interruptible, input is never blocked and a touch ends it at once. A breathing wait is state, not presentation, so it keeps breathing through a touch until the wait ends or its 30 s run out. A finger on a sweeping slider takes over from that point. No arrival lasts more than 450 ms after the transition ends.
5. **Fresh arrivals only.** Coming back with Back, returning from a sheet, rebuilding from a model update and restoring context show final values with no replay. Screens opened many times a minute (Control Center, the Face) do not sweep from zero; on wake a Face ring tweens from what you last saw to what is true now, and if nothing changed nothing moves.
6. **Small and cheap.** Prefer small dirty regions. No gradients, no glows. Measure on the board: at least 30 fps during any arrival, and no visible delay added to a transition.
7. **Quiet when asked.** Reduce Motion gives final values at once, or a 150 ms fade where something appears. Always On, dimmed, locked, covered or suspended surfaces show final values with no motion.

### The vocabulary

| Name | What it does | Where |
|---|---|---|
| **Sweep** | A level or ring grows from its minimum to the true value on a 420 ms spring, bounce 0.15, and its number counts with it | Home level and thermostat details, Settings Display and Sound, Agents Usage, Timer ring. Control Center has none (rule 5) |
| **Follow** | A value that really changed tweens there, and its number tracks the arc instead of jumping to the end | Every ring, level and OTA progress |
| **Roll** | Changed digits slide vertically, unchanged digits stay still. Whatever updates more often than once a second does not roll | Counts, temperatures, battery, Usage, Face minutes |
| **Light up** | A disc's colour grows from its centre in a flat fill, and the glyph pops about 10% and settles | Any real on/off change: room rows, accessory rows and details, Control Center toggles |
| **Pop in** | A control arrives from 80% scale and full transparency on a 350 ms spring, bounce 0.15. Layout and hit areas never move | Apps discs, Compass discs (outward from the finger), receipt and empty-state glyphs |
| **Draw on** | A check draws its two strokes in 280 ms | Real outcomes only: scene requested, All off done, agent step and delivered result, update ready |
| **Breathe** | A waiting word eases between 100% and 50% opacity every 1.6 s, at no more than 10 Hz | Loading, Turning off, Running, Transcribing, Checking for updates, Waiting for Dot's reply, Buffering. Never agent Working, which keeps its glint (hig.md: no indeterminate spinners for agent work) |
| **Scroll breath** | The rim indicator flashes on arrival, grows while scrolling and fades 600 ms after you stop | Every list |
| **Page stretch** | The current page dot stretches into a capsule and the neighbours make room on a spring | Every page-dot set |

### Home: the house wakes up

The named list stays exactly as it is: nothing reorders, nothing moves, one tap opens a room. On a fresh arrival, once the transition has finished, the room discs that really have something on light up in turn from the top, 45 ms apart. Off rooms stay gray. Only visible rows play; the rest are simply final. Any touch or scroll completes them at once. Back never replays it. A light that changes while you look lights up or goes dark in its row.

### Not yet

Tilt parallax on the Face, the rubber-band pull at a slider's ends, the Face lifting on pick-up and empty-state characters in the spirit of Pebble's sloth wait until the owner has felt this set on the board.
