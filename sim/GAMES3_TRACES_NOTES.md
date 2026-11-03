# Games 3 deterministic motion traces

`sim/tests/test_games_rest.c` exercises the production gravity helper, sensor-time
quiet detector and 60 Hz physics. `games_rest_traces` remains a CTest; its optional
path argument emits numeric CSV. CTest writes `sim/build/games3-beads.csv`; the
review copy is `sim/build/games3/beads-traces.csv`.

The final targeted run passed **1,169,275 checks**, taking **0.802 s** of host wall
time. Build command: `nice cmake --build sim/build --target test_games_rest -j4`.
Trace command: `nice sim/build/test_games_rest sim/build/games3-beads.csv`.
`nice` was invoked as required; macOS printed `setpriority: Operation not permitted`
and still executed each command. These host times are not board timings.

| Synthetic trace | Observed result |
| --- | --- |
| +35° tilt, held 3 s in the hand | 519 active particles; mean x −172.21 px; 95.0% in the low rim region (x<−120 px, radius>160 px). Gravity stays +sin(35°) g at every valid sample. |
| Reverse to −35°, held another 3 s | Mean x +172.26 px; 93.6% in the opposite rim region. All 519 particles move from x<−120 to x>120, across the middle. |
| Slow −0.5°→+0.5° sweep through level | All input lies inside the old freeze zone. Sparse moving particles preserve inertia; minimum speed while within ±0.1° of level is 7.1704 px/s. No force plateau and no abrupt velocity stop. |
| Flat/35°/85°/120° stand poses; 5/10/20 ms samples | Exact motionless clock forms at 1935–1940 ms, including flat from a fresh start. Each held clock stays exact for a further second; gyro-only yaw releases it. |
| Pick up, play, put down at the lead's measured (−0.087,0.040,0.916) g pose | Exact clock returns at 2990 ms after set-down, including observer recovery, 1.2 s quiet qualification and finite clock travel. No recalibration/Resume. |
| 30 s small random hand tilts and gyro jitter | ±0.35° accelerometer pose changes and ±0.8°/s gyro jitter never latch the clock. |
| Missing gyro | Gravity steering remains valid; accelerometer-only quiet cannot establish desk rest. |
| Sensor gaps, duplicate sample duration, NaNs and impact | Gaps/NaNs neither invent stillness nor pick up an already held clock. Impact releases the clock before any invalid physics step. |
| Dense maximum 640-particle pile at held 35°, 30 s; then observe another 5 s | Initial mean speed 0.00112 px/s; next-5-s peak mean 0.00130 px/s; peak per-sample p95 0.00300 px/s; worst bead 0.00777 px/s; largest displacement 0.00034 px. |

The poured fixture has a valid 10:09 clock, so the quiet rejection is tested while
clock latching is enabled. A deterministic independent pseudo-random gyro trace
represents a hand holding one pose; the projected gravity remains constant.
The dense rest fixture deliberately uses all 640 allocated particles, including
more than a particular clock's glyph sites, with no clock target to hide fluid
instability. Its speed assertions use the mean of **speed magnitudes**, rather
than cancellation of opposing velocity vectors. P95 and worst speed values are
the largest observed over the whole last five seconds.

Historical tests were replaced because they asserted the root cause: the neutral
learning a held tilt until its force became exactly zero. The old tests also
accepted accelerometer-only clock latching and comparatively large gyro vibration
as desk rest. Those expectations conflict with the owner's true-gravity toy and
gyro-confirmed rest requirements. Their useful coverage remains: independent
sensor cadences, any-angle stands, pickup, real measured resting pose, sensor gaps,
invalid values and impact rejection. Other games' captured-frame held-tilt tests
live in the host logic suite.

No visual surface is changed by this test-only work. Root captures and reviews
the production Beads renders against the mandatory design gate. No board is
flashed or measured by this subtask, and no Git write is performed.

The final executable also contains `maximum_particle_cpu_benchmark()`: 60 warm-up
steps, then 1200 independently timed `orb_game_step()` calls at all 640 particles,
clock disabled, ±0.5 g horizontal force alternating every 300 steps and 0.1 g
vertical force. It reports mean/p95/max host process CPU microseconds using
`CLOCK_PROCESS_CPUTIME_ID`. The two substeps and two iterations per substep give
four constraint passes per frame. There is no environment-sensitive timing gate;
the root's final build/run supplies those measurements for the review.
