# orb_nav

The UI thread owns an `orb_nav_t`. The portable core allocates no memory and
never sends an action. Register screen IDs with their spatial role, feed pointer
down/move/up/cancel and a timer tick, and render the returned gesture through the
compositor adapter. `orb_shell` keeps LVGL as the only owner of settled content.

- A contact belongs to the outer 40 px band only when its DOWN radius is at
  least 193 px from (233,233). Its nearest angular edge (±45°) stays fixed for
  the contact. Radial versus tangential travel arbitrates after 12 px, and the
  chosen owner stays captured until lift/cancel. Inward top/bottom contacts open
  Notifications/Smart Stack; inward left/right contacts are Back/Control Center.
  Tangential contacts use the registered rotary owner; outward travel is content.
- Centre drags always belong to content, including non-scrolling surfaces and
  lists at their top. Only Face (including its Always On presentation) retains
  centre navigation: finger left to Home, right to Agents, up to Stack and down
  to Notifications. Stack dismisses downward from the top edge; Notifications
  dismisses upward from the bottom edge. Back dismisses every layer, including
  Control Center. Centre contacts cannot regrab an in-flight system transition.
- Blank-area holds emit the legacy Compass gesture at 400 ms; Design 2.0's shell
  maps a Face hold to the face gallery. The explicit compatibility Compass route
  uses direction selection only for navigation; a centre release cancels it. Travel
  of 10 px cancels hold admission permanently for that contact, before the
  12 px directional arbitration and even if the finger returns to DOWN.
- A bounded back stack stores the source screen and optional iris token. Peek is
  read-only, so cancelled drags preserve history. Pop only after navigation
  commits. Layer history is separately bounded. Reopening a sheet already in
  the active layer stack unwinds to that identity and retains its original
  plain parent; repeated system-layer routes cannot accumulate duplicates.
- Rotary callbacks only change scroll/focus/preview. They never click, send,
  approve, toggle, or unlock. Fling is capped at two turns/second and stops on a
  changed owner, new contact, cancellation, or UI stall above 100 ms. Identical
  pointer polls preserve velocity for 80 ms; changed-coordinate samples use
  actual elapsed time. Detents use the supplied quiet sound callback alongside
  visible feedback.

The compositor has its own header and owns three RGB565 planes in PSRAM. Firmware
reserves them before fonts and network startup; the shell claims that boot pool.
Committed routing swaps plane descriptors so the previous place survives for
Back, without a copy or a new capture. Content/font changes invalidate
the epoch; selecting another session/card invalidates that context explicitly.
The adapter recaptures Face status at admission because its clock/telemetry can
update in place without a route/model revision.
A renderer supplies settled captures, and `render_band` reads them directly into
the existing display band. Warm drag/tick/render paths do not build LVGL objects
or allocate a full-screen output plane. Capture admission and allocation failure
must remain explicit in the shell adapter. Invalidating any consumed plane,
including an iris/layer's prepared backdrop, retires its active presentation
before that plane can accept another capture. Host timings are not board FPS.

Portable contracts:

```sh
cmake -S firmware/components/orb_nav/tests -B /tmp/orb-nav-tests
nice -n 10 cmake --build /tmp/orb-nav-tests -j4
nice -n 10 ctest --test-dir /tmp/orb-nav-tests -j4 --output-on-failure
nice -n 10 python -m pytest -q firmware/components/orb_nav/tests/test_nav_host.py
```

The normal simulator CTests also include `nav_core` and `nav_compositor`.
Board throughput and frame cadence remain device measurements, using
`bench_nav <gesture> <ms>` through the normal development transport.

Back and sheet bands copy their immutable foreground/background as disjoint
spans. They read exactly one PSRAM source per output pixel, avoiding a full
background copy immediately overwritten by the translated plane. Pixel oracles
cover all directions/byte orders; the device work test counts source pixels.
