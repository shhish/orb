# Home polish simulator coverage

Synthetic data only. These tests never open HomeRelay, alter the owner's home,
or use either board.

The busy-room fixture has 24 named devices: 20 lights, two switches, a lock and
a temperature sensor. Its room and device names exercise the 32-byte wire
limit, wide letters, punctuation, apostrophes and accented UTF-8. It has no
favourites or scenes and includes an empty room. Tests visit every device by
stable ID, return to the same page, and exercise embedded vertical gestures and
the shell's normalized rim/twist value owner. Hidden semantic navigation is
used for deterministic page setup, alongside those real-input checks.

The behavior suite also covers physical versus synthetic scene, room-toggle,
All off and thermostat-mode input; pending scene duplicate suppression; failed
scene retry; truthful accepted receipts; thermostat failure rollback and receipt
expiry; Fahrenheit steps; timestamped sensor readings and future clock skew.
Room All off drains 22 explicit `on:false` writes through the existing four-slot
queue, never touches the lock or sensor, and preserves a partial-failure receipt.

Reproduce the behavior profiles with:

```sh
nice cmake --build sim/build -j4
nice ctest --test-dir sim/build -j4 -R '^home_view' --output-on-failure
```

The CTest Home screenshot matrix writes `sim/build/home-<state>-<profile>.png`.
`home-shots.sh` can also capture the full Home gallery plus pointer sequences in
a separate directory. After CTest, create comparison pairs with:

```sh
sh sim/home-polish-sheets.sh
```

The comparison output is `sim/build/home-polish-review/{default,ax3-bold-rt}/`.
Each PNG is generated using `python3 sim/png_tools.py concat`, with the canonical
Design 1.0 parent frame on the left and the real firmware render on the right.
Rooms, room and light dial use their exact parent frames. Added action, pending,
failure, empty, scene, thermostat and sensor states use the closest parent frame
to make structural changes reviewable; those extensions have no dedicated
Design 1.0 reference PNG.

The added screenshot states cover busy and empty homes, first/final room pages,
room actions and All off outcomes, pending light/switch/lock/row writes, scene
and scene-capsule outcomes, all thermostat modes in Fahrenheit, and known,
unknown and future sensor ages. Every state is captured at Default, AX3 and
AX3 + Bold Text + Reduce Transparency; behavior additionally runs with Reduce
Motion and the combined accessibility settings.

Visual gate review of the overview surfaces uses these comparison pairs in
both `default/` and `ax3-bold-rt/`:

| Surface | Evidence filenames |
| --- | --- |
| Rooms, no favourites, busy inventory | `rooms.png`, `no-favourites.png`, `busy-rooms.png` |
| Empty and loading Home | `empty-home.png`, `loading-home.png` |
| Room continuation and pending rows | `busy-room.png`, `busy-room-last.png`, `pending-room.png` |
| Empty room and room actions | `empty-room.png`, `room-actions.png` |
| All off request and outcomes | `all-off-pending.png`, `all-off-accepted.png`, `all-off-failed.png` |
| Scene request and outcomes | `scene-pending.png`, `scene-accepted.png`, `scene-failed.png` |
| Rooms scene capsules and outcomes | `capsule-pending.png`, `capsule-accepted.png`, `capsule-failed.png` |

For these surfaces: **Two layers** passes with flat content and glass confined
to actions; **Canvas** passes with true black and no gradients or glow;
**Round-first** passes with chord-fitted headers, names and state lanes;
**Sizes** passes with shared text styles and the AX3 two-row reflow;
**Colour** passes with state words independent of Home yellow;
**Approvals and agents** is not applicable to Home, while consequential actions
retain physical-input guards; **Motion** passes the retained-frame and Reduce
Motion behavior checks; **Navigation** uses physical Back plus vertical/rim/twist
without visible redundant paging buttons; **Craft** was reviewed in the paired
renders, including empty, pending, accepted and failed states. Device-specific
appearance and interaction judgment remains a board gate.

Simulator frames establish layout, retained rendering and behavior. They do not
establish panel appearance, frame time on ESP32-S3, sound quality, gesture feel,
or owner-budget headroom. The lead still needs real-board screenshots, owner
heap/rejection counters, and physical rim/twist/hold and audio review.
