# Shared bezel Crown

`orb_rim` contains Home's extracted 270° bezel geometry, flat retained arc and
tick drawing, local damage, continuous pointer reducer and visual-detent sound
feedback. Home and Time use the same implementation.

The value travels clockwise from 135° to 405°. `orb_rim_config_t.gaps` reserves
the leading diagonal Back circle (1) or both diagonal toolbar circles (2).
Values compress across those gaps; touching or crossing one does not discard a
step. A new drag starts only within the existing 179–229 px rim band and outside
the bottom opening. The caller owns gesture arbitration, model writes, the
touch-only glass knob and its spring settle. The component creates no timers.

Time uses `start_angle = 270, sweep_angle = 360, gaps = 0`: a complete ring
starting at twelve o'clock, clockwise. A zero `sweep_angle` preserves Home's
geometry. `orb_rim_drag_begin_relative` anchors the drag at the selected digit
group's value, so touching a different point on the rim never changes it before
the finger moves. Full-circle configurations accept the entire rim band.

Pass coordinates local to the 466×466 screen to `orb_rim_drag_begin` and
`orb_rim_drag_update`. Begin returns whether it claimed the rim; use
`orb_rim_value(config, drag.angle)` for its initial position. Update unwraps the
0° boundary and bounds elastic overtravel to 7.5% (hard clamps with Reduce
Motion). Release or cancellation sets `drag.active = false`; cancellation
restores the caller's last committed value.

`orb_rim_detent` updates a selected tick only when it changes. When `audible` is
true it requests a quiet Start tick or Stop endpoint cue from `orb_sound`, which
enforces Silent, face-down and quiet hours. Always redraw the matching visual
detent when it returns true (hig.md §8).

The geometry and fake-sound tests run without LVGL, the board or network:

```sh
make -C firmware/components/orb_rim/tests/host test
```
