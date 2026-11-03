# orb_faces

Design 2.0 face and editor presentation, shared by the workspace and the locked
power surface. The shell supplies public aggregate values and handles routes,
physical buttons, system edge arbitration, power and burn-in displacement.
`orb_faces_snapshot_model` derives the common usage/working definition; weather
comes from `orb_model_weather_snapshot` unless a deterministic preview overrides it.
No session title, request or other private text enters a face snapshot.

All disc, button, gauge, rim text, focus and position roles come from `orb_kit.h`.
The analog clock's ticks and hands are content-specific LVGL lines, backed by
fixed point arrays in the view state. Hands render before opaque subdial discs,
so their motion cannot cross the subdial text. A missing cloud/rain/fog/snow icon
has a condition label instead of the old question mark.

The native face builders also produce editor/gallery artwork. A preview scales
canonical kit controls and bounded cached rim/clock slices individually, avoiding
three retained full-frame bitmaps. Preview discs intentionally shrink with their
face artwork; focused controls retain their native size and use the kit outline.

Two frame conflicts follow the mandatory role/HIG rules:

- Colour's exported40px standalone swatches become80px kit discs. Nine fit a
  270-degree arc at radius174, leaving the top title arc clear and at least20px
  between adjacent discs. Selected state is a check and the shared focus ring.
- Locked text uses the same shared140px clock as Orb. The128px Always On font
  is Light, which HIG forbids; lower opacity plus thinner rings/hands supplies
  the locked presentation instead.

The component charges its fixed state to `ORB_MEMORY_WORKSPACE`. LVGL objects,
shared glass and prepared rim rasters retain their existing kit owners; use the
host faces matrix for measured deltas and the lead's board heap report for the
actual device gate. The component's draw path has no callback and no allocation.
The only smooth clock callback changes retained hand points at20Hz while the
Meridian is active, visible and not reduced-motion. Lock/Always On pauses that
timer and refreshes only when the shell supplies its1Hz snapshot. No preview
starts a smooth timer. The shell supplies the existing minute burn-in shift.

Choices (Orb3, Meridian4, Activity3 slots) live in a fixed16-byte version2 record under `orb_fs` app `faces`, key
`preferences`. A successful atomic write updates the in-process committed cache,
which the locked view reads without storage I/O. Colour Back keeps the editor
open on write failure and shows "Not saved". Destruction follows LVGL ownership:
deleting the root/parent deletes its timer and frees its state; an explicit
`orb_faces_destroy` is the same root deletion.

Rotary paths retain their presentations. Gallery builds all three real face
artworks once, then repositions them and updates the kit name/dots. Colour builds
all nine check glyphs once, then changes focus, visibility and the retained clock
accent. Slots prepares all seven provider forms per handle, including fixed
string buffers; turning only selects a form and updates the kit caption/scroll
position. The kit's documented rim text content cache is the sole cache rebuild
on those paths. Route or font changes may rebuild the view while idle.

Meridian and Activity complication editors show the actual dimmed180px face
artwork above canonical80px editing handles. This keeps the native ticks/full
rings intact inside the artwork and reserves the standard outer title/caption
baselines. Meridian's four handles form a2x2 grid with20px vertical gaps.
