# Home: Apple Home on a round display

The optional HomeRelay service serves Apple Home over the LAN from an always-on
Mac. Orb supports named rooms, lights, switches, sensors, thermostats, locks,
fans, scenes and doorbell notifications through the hub. Fixtures use synthetic
homes and generic room/accessory names; they are not a household inventory.

`hig.md` governs, including Control Center (§7), toggles, and security actions,
which require deliberate input. `delight.md` governs colour and motion.

## Architecture: the device never talks to HomeRelay directly

The hub holds the HomeRelay control token, stored in `~/.local/share/orb-hub/home/homerelay.token` with mode 600 and never sent to the device. The hub keeps one SSE `/events` stream, and the device only talks to the hub over the existing pinned-TLS link. This keeps a home-control credential off a device people carry, adds no second HTTP client or TLS session on the ESP32, and lets the hub filter, compact and coalesce.

Protocol: a v1.1 `home` capability, written into `docs/protocol.md` first.

**Messages:**
- `home.snapshot` event (chunked like sessions): `rooms[]`, `accessories[]` and `scenes[]`. Each accessory is `{id (short alias), name, room, kind, reachable, state:{on, brightness, hue, sat, ct, temp, target, mode, humidity, motion, contact, locked, speed, position, battery, low_battery}, writable[]}`. Fields are bounded like sessions: names ≤ 32 bytes and at most 64 accessories on the device.
- `home.update` event: per-accessory partial state, coalesced by the hub to ≤ 4/s per accessory.
- `home.set {id, values}` request. It replies with the accepted state (optimistic on the device, reverted with a message on error).
- `home.toggle {id}` request.
- `home.scene {id}` request.

**What the hub sends:**
- Hidden: bridges, the camera feed, `custom.*` values and scenes named `Trigger<digits>`.
- Excluded: the "Admin" and "Server Room" rooms, unless the owner pins something from them.
- Favourites are hub-side preferences, set with `orb-hub home favourites …` and later from the device. The defaults: lights and switches in Living Room, Bedroom, Kitchen and Office, plus the thermostat, lock, fan, and scenes Good morning / Evening / Good Night / I'm leaving / I'm home.

**Doorbell:** a doorbell press becomes an **attention card** on the device, with a chime (§Sound), "Front Door" and the time. No video; the camera stays on the phone.

## App identity

- **Home:** accent yellow `#FFD600`, a house glyph on a yellow disc in the Apps ring. Move the future Timer app to cyan.
- **Glyph ink:** choose black or white Framework7-style ink for contrast on each flat disc, including actual lamp colours (hig.md §5); bright yellow does not require white ink. Home accessory-state colours are a meaningful exception to the ordinary two-accent limit, not permission for extra tinted glass actions, gradients or glows.
- **Accessory colour comes from the thing itself:**
  - A light that's on fills with its actual colour (hue and saturation, or a warm white mapped from colour temperature), at full opacity for its glyph disc and 25% for its tile.
  - Switches and outlets that are on use Home yellow.
  - Fans use mint, and the thermostat uses orange (heat) or cyan (cool).
  - Off is dark glass grey, with the glyph in secondary label colour.
  - **No Response:** dimmed to 40%, labelled "No Response" in Apple's wording.

## Screens

### 1. Home: Rooms (the landing page)

Home opens on a plain vertical list under the existing **Demo Home** rim title,
using the same chord-fitted kit rows as the room and Scenes lists. The title is
not a control. Rooms stay in the model's order; live state never moves a row.

- Every room has its full name (or **Unnamed room**), a recognisable room glyph,
  and a state subtitle: **2 lights on**, **1 device on**, or **Off**. When a
  temperature is reported, it follows that state in the same subtitle, for
  example **1 light on · 22°**. A pending request shows **Updating…**, and a
  failure shows **Couldn’t update** in red; neither includes a temperature.
  The subtitle conveys state without relying on colour.
- A leading flat disc uses Home yellow with black ink when anything is on, and
  the shared neutral off fill otherwise. Room names choose from existing glyphs
  for bathroom, kitchen, garage, office, bedroom, entry and outdoor spaces.
- The room name owns the whole title lane and wraps as needed. Room rows have
  no trailing value lane. Live state and temperature change in place without
  rebuilding or shifting the rows; turning on a light never moves its room.
- **One tap opens exactly that room**, with an iris from the row. The landing
  list never sends a Home command. Scrolling a row never opens it.
- The rim, twist and vertical drags scroll the list. Horizontal centre swipes
  do nothing. There is no room focus, hidden group, carousel or page indicator.
- The list viewport spans **y=66..440**. A fresh entry starts at its computed
  resting position, `rooms_rest`, with at least three whole rows under the
  title at Default text size and the first row's top at or below **y=104**.
  Top padding still lets the first room be pulled down to the equator for
  reading. The resting scroll is saved as **0**, meaning **fresh**, rather
  than making a fresh list start at native scroll 0. An explicit pull to
  native scroll 0 is saved distinctly, so Back also preserves that position.
- Back from a room, Climate, Sensors or Scenes returns to this list at its
  previous scroll position, including the resting position. Back on the list
  returns to the Face. BOOT provides Back without redundant on-screen arrows.

When content exists, the rooms are followed by **Climate** (a thermostat or
sensor is present) and **Scenes** (a scene without an unlock is present). These
are named rows that push their existing pages. They do not control devices.
The Climate row retains its trailing temperature value.
Loading, no-rooms, offline and unavailable compositions are retained. A stale
Home snapshot keeps the existing loading composition until it is synchronised;
Climate and other retained detail pages keep their shared Last known header.

### 2. Inside a room

Named accessory rows show state and open accessory details. Toggling stays
inside the room. Rim titles are labels throughout Home: tapping a room title
does nothing.

When the room contains a switchable accessory, its final list row is named
**All off**. Rooms without switchable accessories do not show this row. It
remains fully reachable by scrolling at Default, Larger and Largest text
sizes. Tapping the row opens the existing room actions page, where the
**All off** button must still be pressed to request any changes. Scrolling to
the row, tapping it or reaching the actions page never changes an accessory.
Additional devices continue in the same scrolling list, and Back restores the
room's scroll.

### 3. Climate and Scenes

**Climate** is a pushed page with its own **Climate** rim title. It retains the
current-temperature glance, thermostat detail route and Sensors list. **Scenes**
retains its named scene list and request feedback. Back unwinds these pages to
the room list without changing the landing scroll position.

### 4. Detail controls: the bezel is the Digital Crown

The signature interaction for a round device: **drag around the rim to set a value.**
- **The control is a thick arc hugging the bezel** from 7 o'clock to 5 o'clock (270°), with the value filling it in the accessory's colour.
- **The knob is glass, appearing only while touched** (§7 toggles and sliders).
- **Ticks:** a click every 10% and a firmer tick at the ends (§8), plus a visual tick mark.
- **Reading:** the value is large in the centre (e.g. "60%"), with the accessory name above it and an on/off glass button at 6 o'clock.
- **Rubber-banding** at both ends, and a spring settle when you let go.

By kind:
- **Light:** brightness on the rim.
  - A second page (swipe up) shows **colour**. The rim becomes a hue ring; for white-only lights it's a warm-to-cool colour-temperature arc. Drag the knob around it, and the centre disc previews the colour.
  - A third page holds the light's scenes, if any.
- **Thermostat:** the target temperature on the rim, coloured orange to cyan by mode. The centre shows the current temperature large and "Heating to 22°" small. Off, Heat, Cool and Auto are glass segments along the bottom arc.
- **Fan:** speed on the rim. The centre glyph spins slowly while it's on, at a rate proportional to the speed and capped (Reduce Motion: static).
- **Lock:** a large lock glyph in the centre, with the state "Locked" or "Unlocked".
  - **Unlocking needs a deliberate hold:** press and hold the centre for 1 s while a ring fills around it. Locking is a single tap.
  - Never unlock from a list toggle or a scene capsule. hig.md §7: security actions require deliberate input.
- **Switch or outlet:** one large toggle disc in the centre, plus power or "In Use" if reported.
- **Sensor:** a read-only glance: the value, the last change ("Motion · 4 min ago") and battery. A low battery gets a red battery glyph and the words "Low Battery".

### 5. Elsewhere in Orb

- **Control Center:** a "Home" row with the time-appropriate scene and the first two favourites as toggles.
- **Face:** an optional complication, "3 on", tinted yellow, that opens Home.
- **Smart Stack:** a contextual card when it's relevant: "Lights still on in Living Room · Turn Off" after 11 pm, or the doorbell.

## States and feedback

- **Optimistic writes:**
  - The control changes immediately.
  - If the hub reports failure, it springs back with a brief red message: "Couldn't reach Floor Lamp".
  - "No Response" accessories are disabled, never optimistic.
- **Every toggle** gets a short tick (on: rising, off: falling) and the press-and-fill animation. Scenes get a soft two-note chime.
- **Offline (hub unreachable):** Home shows the last-known states greyed out, with "Home is offline · Your Mac isn't reachable". No writes are allowed.

## Performance budget

- Home screens are mostly static: zero frames when idle.
- Only the affected disc or arc region redraws on updates.
- The rim drag redraws only the arc band and the centre label at up to 60 fps, with no blur under the moving knob except its own small glass.
- Snapshot parsing goes into PSRAM, with the same bounds and rules as sessions.
