# Games3 audit: Marble, Orbit, Ring Snake and Pop

These notes cover the pure game logic audit. The root results document combines
them with the shared sensor, Beads, runtime, sound, renderer and build evidence.
No git write, flash, owner configuration or live-state operation was performed.

## Findings and changes

| Game | Finding and disposition |
| --- | --- |
| Marble | The shared adaptive neutral cancelled a held angle; root removes that adaptation and retains the captured round-start frame across Pause/Resume. The game integrator itself continuously applies force: damping is uniform (`0.980`), with no near-level velocity cutoff. Speed is capped at 210 px/s (3.5 px/step), below the 12 px ball radius, and four constraint passes use the same rounded arc geometry as rendering. Existing public-input tests solve all three progressively faster, narrower-gap stages in tilt and touch modes. Rewind stores ten seconds at 30 Hz, consumes history at the same cadence, advances gates forward and resolves the release locally with zero restored impulse. No extra collision or difficulty tuning was justified. Runtime agent owns restoring the documented rim-rewind interaction. |
| Orbit | Steering is intentionally tap-to-change-orbit; accelerometer force affects lighting, rather than lane selection. Added a ten-second shared-input trace to ensure the input remains honest. Comet speed already ramps with earned score from 1.05 to a bounded 1.85 rad/s. Hazards change lanes only half a relative revolution away, and a single contact has 0.8 s recovery, preventing overlapping hazards from costing several lives at once. Fixed a real feedback defect: lane-switch trail discs were placed on the positive y axis regardless of the planet angle. They now lie along the planet's actual radial switch path. Added angle and radial bounds assertions. |
| Ring Snake | The shared neutral adaptation could falsely re-arm a held tilt. Root removes it. The intentional 0.15 g engage / 0.055 g release hysteresis is a discrete steering gesture, with two consistent engage steps and 50 ms neutral to re-arm; it does not zero moving-body velocities. A held angle produces one turn, rather than repeatedly turning into itself. Earned-score cadence already ramps modestly from 200 to 167 ms for tilt and 133 to 100 ms for touch. Grid collisions allow the vacating tail unless eating, and rendering continuously interpolates between grid cells, including angle wrap. Found an unfair visibility defect: optional 72 px arrow controls at offsets (±46,112) covered the inner r140 lane. Runtime agent moves the view to (±46,60); logic's exclusion geometry is updated to match. A new regression samples all three complete tracks at 1° increments with the full 12 px head clearance. Food no longer needs relocation away from a playable inner-track site. |
| Pop | It is intentionally touch-first and does not use tilt steering. Three initially separated targets and bounded 48-attempt spawning prevent a newly spawned covered target. Drift uses fixed 60 Hz steps, small smooth sway and shared HUD exclusion geometry. Speed ramps with earned score from 15 to a capped 31 px/s. Clearing the field refills within 300 ms, the round lasts 45 s, and timeout returns before a last-frame miss can also decrement lives. Existing tests cover direct taps, nearest-target fallback, miss/combo reset, finite maximum-score speed, round-end and restart. No additional rule change was justified. |

## Deterministic traces and checks

`held_tilt_ten_seconds()` in the production logic test runs Marble, Orbit and
Snake at three initial poses (flat, 35° viewing angle and a compound in-hand
pose), each at 5/10/20 ms sensor cadence independent of 60 Hz physics. A held
20° roll lasts ten seconds. Every sample must retain the captured neutral
byte-for-byte; after 0.5 s filter travel, the force must stay nonzero and vary
by less than 0.0001 g for the remaining hold. The final force immediately
accelerates Marble in an unobstructed lane. Orbit keeps moving without an
invented tilt lane switch. Snake stays latched while traversing a circular lane,
without synthesizing a second gesture as the pose becomes quiet. Existing
short tests separately cover the initial two-step tilt engagement.

Orbit's trail regression checks all four switch discs have the same angle as
the planet and lie strictly between the old and new lane radii. Snake's
regression checks all 1,080 lane/head samples clear the controls.

The ASan/UBSan game-logic target compiled with all existing strict warnings:

```sh
nice -n 10 make -C firmware/components/orb_games/tests/host -j4 build/test_game_logic
```

An isolated ASan/UBSan runner then invoked the new held traces and the existing
Orbit, Snake, Pop, speed-bound, scene/polish and chrome geometry tests:
**3,769,398 checks passed**. All 27 held traces retained exactly
**−0.274507 g** after filter travel, through the full ten-second hold.

The sandbox printed `nice: setpriority: Operation not permitted` and ran the
compiler/test successfully. Root records the final combined execution results
and fresh simulator evidence after shared-input changes are integrated. The
two visual fixes require the Orbit switch and Snake touch-control screenshots
to pass the same HIG gate in default and AX3/Bold/Reduce Transparency profiles.
