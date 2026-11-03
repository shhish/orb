# UI scenario contract

Run with `.venv/bin/python scripts/orb-ui-test.py <scenario.yaml>`. Commands use
`scripts/orb-dev.sh` by default. `--command /path/to/simulator-driver` swaps the
executable for a future simulator implementing the same touch/shot CLI. Python
integrators can implement `orb_devtools.ui.Backend.perform(action, values)` and
`.shot(destination)`; `wait` is delivered to the backend so simulated time can
advance without sleeping.

A version 1 YAML mapping has `name`, optional `description`, optional `example`,
optional `size: [466, 466]`, and a `steps` list. Every step is a single-key mapping:

```yaml
version: 1
name: stable-screen
steps:
  - tap: [233, 400]
  - hold: [233, 200, 500]       # milliseconds
  - swipe: [233, 380, 233, 120, 300]
  - wait: 0.6                 # seconds
  - shot: settled             # unique simple capture name
  - expect:
      shot: settled           # defaults to most recent shot
      golden: shell/settled.png
      tolerance: 2            # CIE76 Delta E per pixel; default 2
      max_changed: 0.005      # fraction above tolerance; default 0
      masks: [[210, 20, 46, 25]]  # x, y, width, height; right/bottom exclusive
```

`expect: shell/settled.png` is shorthand with defaults. Image dimensions must
match; masks must fit the image and leave visible pixels. Perceptual differences
use sRGB converted to D65 Lab and CIE76 Delta E after alpha-compositing on black;
alpha changes are checked separately at 100 units over the alpha range. Pixels
transparent in both images do not count. There is no geometric alignment or blur,
so layout shifts remain visible. Diff PNGs highlight changed pixels in pink.

`--output artifacts/ui/review --goldens tests/ui/golden` chooses paths. Reports
contain `index.html`, machine-readable `results.json`, and copies of actual,
golden, and diff images. `--update` atomically replaces each referenced golden
with its capture. Review those PNGs before committing; an update run establishes
baselines and is not evidence that an older baseline passed. Exit codes: 0 pass
(or successful update), 1 comparison/capture failure, 2 malformed input/setup.
All scenario files are validated before any touch commands run.

For offline replay use `--backend fixture --fixtures /tmp/pngs`; sorted PNGs are
consumed once per `shot` across all scenarios, and waits do not sleep. This backend
never invokes `orb-dev.sh` or opens a serial port. Unit tests generate their own
images and fake command executable.
