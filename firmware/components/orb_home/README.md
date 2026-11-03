# Device Home

The LVGL-free model, staging and request implementation and `orb_home.h` now
live in `../orb_home_data`. This component contains presentation and depends on
that service. The host tests/fixtures stay here; their Makefile follows the new
service paths. Home payload ownership is documented in
`docs/agent-briefs/wave2/refactor4-results.md` from the repository root.

`orb_home_data` is a bounded, LVGL-free PSRAM store: 64 accessories, 16 rooms,
32 scenes and four pending requests. Writers publish revisions; UI readers copy
only changed snapshots. Chunked snapshots commit atomically. Unknown fields and
missing state never become invented readings. There are no direct network calls
or HomeRelay credentials in this component.

## Design 2.0 migration

Home compositions use `orb_kit.h` for controls, rim text, rows, indicators and
sliders. Home opens to a fixed-order list of named rooms, with words for state,
room-specific glyphs and sensor temperatures. A single tap opens that room;
landing rows never send commands. Climate and safe Scenes are ordinary rows
after the rooms, opening nested pages. The Demo Home title is passive.

Room, Scenes, sensor and mode lists use kit rows and the kit scroll arc.
Every accessory detail uses the same radius-200 rim title, 80 px identity disc,
emphasized value and context/status/battery lanes. Adjustable accessories use
the bottom level arc at radius 213 with its 30 px knob. The thermostat disc
opens Mode; light, fan, switch and outlet discs toggle power; the lock disc owns
the deliberate hold. All identities come from `orb_home_accessory_icon()` and
the shared font pack; there are no component-private vector glyphs. A cover is
Blinds, a contact is Door, and an explicit garage-named cover/contact uses Garage.
Heat, Cool and Auto use Flame, Snowflake and Repeat. The model has no separate
door/garage subtype, so that display choice does not change commands or protocol.

Rooms with lights, switches or outlets end with a named All off row. It opens
the room actions page without sending a command; that page's All off button
is the single 64 px bottom primary. Rim titles are passive throughout Home.
Every contact uses the kit
response and cancellation, in addition to Home's physical-input guards.
Scroll is retained separately by stable room ID and for Scenes, sensors and
accessory pages. Home exposes the visible detail list to the shell's rim owner.
Rooms rim/twist scrolls the native list. Back retains its scroll position,
while fresh entry uses the computed resting offset in the y=66..440 viewport.
At Default, three whole rows fill the circle beneath the title; top padding
still lets the first row reach the equator. The resting offset saves as zero
(fresh); native zero saves as -1 so Back can distinguish it. Room names own
the full title lane, with state and temperature together in the subtitle and
no trailing room value. Climate keeps its trailing temperature. Prepared live
subtitles and native disc overlays update without allocation or moving rows.
A light with reported zero brightness reads Off even when its power flag lags;
pending dials retain a dimmed accent indicator over a quiet neutral track.

## Safety and authority

UI actions enqueue requests; one write per accessory may be pending. Failure
restores confirmed state, and newer authoritative events win over old replies.
A replacement snapshot or disconnect invalidates pending requests. No Response,
offline and unsupported accessories cannot write. Pending and failed operations
retain explicit words, independent of colour. Opening a missing accessory keeps
an explicit Accessory not found detail; BOOT returns to the prior Home context.

Room All off sends explicit off only to lights, switches and outlets. Its four
slots drain a bounded 64-bit target mask, never retry, and count inaccessible
or rejected writes as failures. Scenes cannot run during the batch. Scenes
marked `contains_unlock` are refused; every scene needs a physical explicit tap.

Locks reject toggles. Unlock requires a fresh physical initiating pointer and an
uninterrupted one-second hold on the lock disc, with `deliberate_unlock=true`.
Movement, leaving the target, lost contact, hiding any ancestor and navigation
cancel the hold. Synthetic events never gain physical provenance from a timer.

Demo fixtures are process-local and never contact the owner's Home. The model
and protocol remain unchanged by this UI migration. Temperatures follow locale;
thermostat target stays the same quantity during preview and rest, and Celsius
half steps are visible. White temperature remains mireds on the wire and Kelvin
or a colour name in the UI.

## Validation

Build/test with four workers under `nice`. `sim/rooms-shots.sh` renders the
room-list accessibility profiles and generates frame-left / firmware-right contact
sheets using the offline `sim/home_contact_sheets.py` compositor. The manifest
records scene, profile, reference and execution status. Simulator payload counts
exclude native malloc overhead, font storage and board headroom. The lead must
capture the real board and check `orb_memory` owner budgets before release.
