# Orb developer workflow

Build on the Mac mini. The board is attached to the MacBook Pro, normally
`my-mac.local`, at `/dev/cu.usbmodem1101`.
Development scripts retain their existing CLI and use `orb-logd` when its socket
is available. All automated tests run locally against PTYs, fake executables,
and generated images. Installing services, SSH, flashing, and accepting hardware
goldens are separate owner deployment steps; none is part of `pytest`.

## Worktree setup and checks

```bash
/opt/homebrew/bin/python3.13 -m venv .venv
.venv/bin/python -m pip install -e '.[test]'
.venv/bin/python -m pytest -q
.venv/bin/ruff check devtools scripts tests
.venv/bin/ruff format --check devtools scripts tests
shellcheck scripts/*.sh scripts/orb-logd
scripts/idf.sh build
```

Runtime logging needs only `pyserial==3.5`. UI tests add `Pillow==12.0.0` and
`PyYAML==6.0.3`. The test extra pins pytest and Ruff. Crash decoding uses
ESP-IDF's `esp-coredump==1.17.2` and Xtensa tools; the optional `crash` extra pins
`esp-coredump` and `esptool==5.6.0` if a separate Python environment is preferred.
Do not install this package into the ESP-IDF environment just to run tests.

`scripts/idf.sh` uses `~/esp/esp-idf` and
`~/.espressif/python_env/idf6.1_py3.13_env`. For an existing build configuration,
apply the core-dump settings from `firmware/sdkconfig.defaults` with menuconfig:
changed defaults alone do not override values already stored in `sdkconfig`.

## Install the serial owner on the MacBook Pro

Copy or check out this version of the repository on the MacBook Pro when its
board is available. Run these steps **there**, after stopping any direct serial
monitor. The install script never changes Claude, Codex, Keychain, or hub config.

```bash
# Stage a dedicated venv and rendered plist; this DOES NOT load the agent.
scripts/orb-logd-install.sh install --port /dev/cu.usbmodem1101

# Inspect the staged file before loading it.
plutil -lint "$HOME/Library/LaunchAgents/com.orb.logd.plist"
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/com.orb.logd.plist"
"$HOME/.local/share/orb-logd/venv/bin/orb-logd" status
```

`install --load` combines staging and explicit loading. The checked-in
`deploy/com.orb.logd.plist.in` is only a template. The installer uses Python 3.13
at `/opt/homebrew/bin/python3.13`, overridable with `ORB_INSTALL_PYTHON`.
It installs into `~/.local/share/orb-logd/venv`; the repository need not remain
at its original path for the daemon to run. Existing agents/unowned venvs are
not overwritten. To remove this installation:

```bash
scripts/orb-logd-install.sh uninstall
```

Uninstall unloads only `com.orb.logd`, removes its plist and marked venv, and
preserves console logs and incidents. No installer or launchctl action has been
run as part of development validation.

For foreground operation from a checkout:

```bash
scripts/orb-logd serve --port /dev/cu.usbmodem1101
# Ctrl-C stops it and releases serial/socket resources.
```

An exact port reconnects when that path returns. A quoted pattern such as
`--port '/dev/cu.usbmodem*'` follows renamed ports only when exactly one matches;
multiple matches are an error, never an arbitrary device selection. For two
boards, use separate exact port selections, sockets, and log directories.

## Serial ownership, logs and incidents

The daemon deasserts DTR and RTS **before** open, clears `HUPCL`, and requests
exclusive serial access. It does not deliberately reset the chip during normal
open/reconnect. Driver-level line pulses still require verification on the real
USB hardware. Only the flash/crash tooling intentionally enters the bootloader.

Default locations on the MacBook Pro:

| Artifact | Location |
| --- | --- |
| Socket | `~/.local/share/orb-logd/orb.sock` |
| Console | `~/Library/Logs/orb-logd/console.jsonl` |
| Rotations | `console.jsonl.1` through `.5`, 5 MiB each by default |
| Incidents | `~/Library/Logs/orb-logd/incidents/*.jsonl` |
| Service diagnostics | `service.stdout.log`, `service.stderr.log` |

Each console record carries a UTC host timestamp, boot identity, kind and text.
ROM/reset banners identify boots, including fast crash loops. Guru Meditation,
panic, assert, abort and backtrace lines trigger a separate incident containing
the recent console prelude and the current boot's ELF SHA when observed.
Incidents close at reboot, after 2 seconds of silence, or at a 4 MiB bound.
Logs continue during commands and screenshot transfer. The serial stream is
unavailable while an external owner holds a lease; lease boundaries are logged.
A daemon starting mid-boot cannot reconstruct an ELF SHA it never observed.

Socket/log directories are private and the socket/log files owner-only. Console
rotations are bounded; incident files and launchd service diagnostics are retained
until the owner archives/removes them. Review logs before sharing: firmware may
log sensitive values. There is no TCP listener or unauthenticated network service.

```bash
# On the board's Mac:
scripts/orb-logd logs --tail 200 --filter 'main:'
scripts/orb-logd logs --follow --seconds 20 --filter 'Guru' --filter 'Backtrace'
scripts/orb-logd logs --tail 100 --json

# On the Mac mini: same persistent stream, Ctrl-C to quit.
scripts/remote.sh monitor
# Bounded filtered capture, including errors and crashes:
scripts/orb-transport.sh --logs 20 'main:|E (|Guru|Backtrace'
```

Filters are case-sensitive literal substrings with OR semantics. No regex is
executed. Slow subscribers fail explicitly and can recover from the disk tail;
they never block the serial recorder indefinitely. `remote.sh monitor` now
streams logs and exits with Ctrl-C; it is not an interactive IDF monitor with
Ctrl-] shortcuts or automatic symbolization (use `orb-crash.sh backtrace`).

## Build and flash

```bash
scripts/idf.sh build
scripts/remote.sh flash
scripts/dev-flash-monitor.sh 20 'main:|display:'
```

`remote.sh flash` copies build artifacts to `~/esp-remote/esp32s3-firmware` on
the board's Mac, then runs its existing `~/.venvs/esptool/bin/esptool`.
`info` and `restore` keep their existing CLI. Before an esptool operation, the
transport obtains a daemon lease and waits until serial is closed. It holds
the socket until the subprocess exits, including failure or Ctrl-C. The child
inherits the lease descriptor so killing the wrapper alone cannot cause the
daemon to reopen serial while the child is still using it. Reclaim/reconnect
is automatic after the last holder exits.

Only absent/refused sockets permit direct serial fallback. Busy, permission,
protocol, or command errors fail; they never justify a second serial owner.
A timed-out command blocks later commands until its exact acknowledgement or a
new boot arrives. A flash lease may clear that state because it resets the board.

`ESP_REMOTE_HOST` overrides the SSH target. `ESP_REMOTE_PORT` selects the direct
fallback port. **When logd is running, its configured port is authoritative**;
select another board by configuring another daemon and `ORB_LOGD_SOCKET` (a path
on the remote Mac, forwarded safely through SSH). A custom socket path can begin
with `~`, which is expanded remotely. Do not point two daemons at one board.
`ORB_DEV_LOCAL=1` bypasses SSH for a locally attached device or PTY;
`ORB_PYTHON` overrides the local transport's default `.venv/bin/python`.

The storage partition is smaller in this version. Deploying the partition table
is a migration: preserve any existing storage data first and recreate/migrate
its filesystem for the new size. Flashing the app alone does not add the partition.

## Screenshots, gestures and snapshots

```bash
scripts/orb-dev.sh shot shots/review.png
scripts/orb-dev.sh tap 233 400
scripts/orb-dev.sh hold 233 233 500
scripts/orb-dev.sh swipe 233 380 233 120 300
scripts/orb-dev.sh top
scripts/orb-dev.sh heap
```

Coordinates are panel pixels, 0–465. Gesture durations are 1–60000 ms.
Screenshots retain the indexed base64/CRC32 wire protocol, retry damaged
transfers up to three times, and atomically publish PNGs only after validation.
Pixels outside the round display are transparent. The PNG represents LVGL's
rendered scene; it does not reveal panel tearing or brightness. Every command
requires the complete firmware acknowledgement, not merely its prefix.

## Crash capture and matching symbols

The new table keeps NVS at `0x9000`, OTA slots at `0x20000` and `0x420000`, and
assets at `0x820000`. Storage still starts at `0x1020000` but is `0xED0000` bytes;
`coredump` is 1 MiB at `0x1EF0000`. No OTA or NVS offset moved. The last 64 KiB
of the 32 MiB flash remains unused as before.

Defaults enable flash core dumps, boot integrity checking, a dedicated 2048-byte
panic stack, up to 64 tasks, and full 64-character application ELF SHA logging.
ESP-IDF 6.1 selects ELF format and SHA-256 integrity. Dumps retain the latest
crash, not an unbounded history. PSRAM content is not a complete process image;
large stacks/task sets can still exceed the partition. The configuration follows
[Espressif's core-dump guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/core_dump.html).

On the MacBook Pro the capture helper requires `esptool==5.6.0` in the existing
`~/.venvs/esptool` environment. The owner can install/verify that version during
deployment; this task has not changed that environment. On the Mac mini:

```bash
# After deploying the new table and a crash-enabled build:
scripts/orb-crash.sh capture --elf firmware/build/orb.elf --output crashes/failure.raw

# Offline, against the exact ELF retained with the crashed release:
scripts/orb-crash.sh decode crashes/failure.raw --elf /path/to/matching/orb.elf
scripts/orb-crash.sh backtrace crashes/incident.jsonl --elf /path/to/matching/orb.elf
```

Omit `--elf` to select the single `firmware/build/*.elf`; ambiguity is an error.
Capture holds one lease and one bootloader session, reads the device's actual
partition table, validates its MD5 and core partition bounds, then reads that
partition **before running the app again**. It resets the chip on exit and
releases the lease. The remote helper uses the documented
[esptool scripting API](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/scripting.html).
No dump is erased. Output is refused if the requested raw file already exists.

The decoder validates the dump checksum, extracts the full ELF SHA from its
application identity note, and compares it with the local ELF's SHA-256 before
starting `esp-coredump`. It rejects missing/truncated identities and unknown
formats. This targets IDF 6.1's ESP32-S3 ELF dump format, not legacy binary dumps.
Plain panic/JSONL decoding uses only PC addresses from PC:SP pairs with the
Xtensa toolchain's `addr2line`. Older logs without a full SHA require `--app-sha`
from a trusted record of the **crashed** app; the current build's hash is not proof.

Artifacts beside the raw dump: the original partition table, a length-trimmed
verified dump, text report, and JSON receipt recording the ELF SHA and decoder.
Raw captures are kept even when validation fails. Tool paths are selected from
PATH or the IDF install; pass `--decoder`, `--gdb` or `--addr2line` to disambiguate
multiple toolchains. For an OTA rollback, use the crashing image's archived ELF,
not necessarily the app currently running. Retain release ELFs on the hub.

## UI regression scenarios

```bash
.venv/bin/python scripts/orb-ui-test.py tests/ui/scenarios/*.yaml \
  --goldens tests/ui/golden --output ui-results/review
# Explicitly establish/refresh baselines after visual review of the target state:
.venv/bin/python scripts/orb-ui-test.py tests/ui/scenarios/*.yaml --update
```

Both checked-in scrolling-demo scenarios are **examples**. The current demo
animates continuously and changes lighting with the IMU; freeze/synchronize those
inputs and define the starting state before treating these as regression gates.
No hardware golden PNGs are claimed or shipped. `--update` records the resulting
captures; review the report and PNG changes before committing them.

The [scenario schema](../tests/ui/README.md) supports `tap`, `hold`, `swipe`,
`wait`, `shot`, and `expect`. Comparisons use CIE76 Delta E in Lab, with per-pixel
`tolerance`, a permitted `max_changed` fraction, and `[x,y,width,height]` masks.
Transparent round corners are excluded; alpha loss, dimension changes, missing
goldens and fully masked images fail. Masks must cover only known variable areas.
Reports include HTML, JSON, captured/golden PNGs and highlighted diffs; update
reports preserve the previous baseline for review. The harness exits nonzero on
failed expectations or captures and validates all scenarios before touching a device.

`--command /path/to/simulator-driver` replaces `orb-dev.sh` with the same command
CLI. A Python simulator can implement `Backend.perform()` and `.shot()`, including
advancing simulated time for `wait`. `--backend fixture --fixtures /tmp/pngs`
replays sorted local PNGs without serial or SSH. Tests use both fake command
executables and fixture backends.

## Profiling and Perfetto

Enable `CONFIG_LV_USE_PROFILER=y` and `CONFIG_LV_USE_PROFILER_BUILTIN=y` with
menuconfig and turn off `CONFIG_LV_PROFILER_BUILTIN_DEFAULT_ENABLE`. Profiling is
opt-in; a normal build returns an actionable error for `profile`. The current
demo also performs a boot capture when built with profiling; wait for it to finish.

```bash
scripts/orb-profile.sh 400 profiles/scroll.json
scripts/orb-profile.sh --input saved-console.jsonl profiles/replay.json
# Entire converter option set, including a different frame scope:
.venv/bin/python scripts/orb-profile-convert.py capture.ftrace trace.json \
  --frame lv_display_refr_timer --top 20 --strict
```

The command accepts 1–5000 ms. Capture writes `.ftrace`, trace JSON and a summary
text table. Load JSON in Perfetto or a Chrome Trace viewer. CPU 0/1 have distinct
process tracks with original LVGL task IDs. Nested scopes produce inclusive and
self times; per-frame summaries include both cores' overlapping work, with full
function totals in `otherData.frames[].functions`.

These are scoped **wall times**, not scheduler CPU accounting. A scope can
include blocking/preemption. A task migrating between cores is assigned to its
entry core and flagged, since ftrace markers cannot reveal the switch instant.
Incomplete, malformed or mismatched scopes produce warnings rather than invented
durations; `--strict` fails when warnings exist. Prefer short captures: the
192 KiB profiler buffer auto-flushes when full, and serial output can perturb
measurements. The BEGIN marker now precedes collection, including auto-flushed
records. The committed sample is synthetic in LVGL's recorded format; it is not
a hardware performance claim.

## Hub integration contract

This checkout adds `orb_devtools`, not `hub/` or `orb_hub`. Keep USB ownership on
the board's Mac. The hub can call these scripts over SSH or install this package
on that host and import `orb_devtools.client`. Do not open serial from `orb_hub`.
For Python/async hub code, move blocking client/subprocess calls off its event
loop. Do not expose raw flash/devtools operations as Dot MCP tools; the product
link and Dot boundaries in `protocol.md` and `dot.md` remain unchanged.

Protocol v1 is UTF-8 NDJSON on a private Unix socket, one operation per connection:

```json
{"op":"status"}
{"op":"logs","tail":100,"follow":true,"seconds":20,"filters":["main:"]}
{"op":"command","command":"shot","timeout":40}
{"op":"lease"}
```

Responses have `type`: `status` (version/connected/paused/port/boot/error), `log`
(`record` with time/boot/kind/text), `data` (base64 raw serial bytes), `leased`
(`port`), or terminal `done`/`error` (`error` string). `shot` and `profile` are
raw framed serial responses; the existing helper validates/converts them.
Commands are serialized; busy is an error, not an implicit queue.

A lease closes serial before sending `leased`. Hold that socket until the external
owner exits, then close it or send `{"op":"release"}`. With subprocesses, use
`client.lease(with_fd=True)` and pass the descriptor through `pass_fds`.
The ordinary context sends release on exit; the `with_fd` context uses EOF so a
surviving child retains ownership. Reclaim is asynchronous; poll `status` for
`connected`. Normal logs/follow clients never own or pause serial.

Hub work after merging: archive each release's exact ELF and SHA, associate
incident boot IDs with release/device identity, and choose a specific socket per
board. No hub dependency, config, service or live session was changed here.

## Validation handoff

See [devtools-validation.md](devtools-validation.md) for command results, test
counts, and the exact sandbox errors blocking firmware compilation and Git
staging in the development session. Host tooling is tested; hardware deployment
and accepted visual goldens remain owner integration work.
