# Wave 2 storage — lead handoff

Branch `orb/w2-storage`; changes intentionally uncommitted. No board, serial port,
live hub, owner settings, Keychain or launchd was accessed by this stream.

## Board-results follow-up — 2026-10-27

The lead's USB migration already succeeded. **Use app OTA for this fix; do not
repeat the migration or erase storage/NVS.** No new partition table or bootloader
is needed. The existing development signing key remains the trust anchor.

### Root causes and fixes

The exact existing `firmware/build/orb.signed.bin` and
`orb_assets-signed.bin` have matching block-zero signing keys, correct image
digests/CRCs and valid RSA-PSS signatures. Assets occupy `0xc20000` through
`0x117e000` before their signature sector. IDF's generic signature verifier hashes
with flash mmap; installed IDF's `flash_mmap.c` rejects addresses beyond
`0x1000000` without 32-bit cache addressing, which this build does not enable.
This is why a correct pack was rejected. Assets verification now hashes with
1 KiB `esp_partition_read` chunks, then calls IDF's same SBv2 trust/RSA verifier.
Running-app/eFuse trust, block-zero policy, payload digest and pack identity
remain enforced. This also fixes verification after assets OTA.

The boot verifier previously allocated an 8 KiB **internal** task stack and
signalled completion before self-deletion; idle-task cleanup could overlap later
startup. Its stack is now strictly PSRAM and the caller deletes/joins the worker
before continuing, including failure. A compile-time guard requires external
task stacks and PSRAM XIP (already enabled on this board). Its 4 KiB signature
copy also requires PSRAM, with no internal fallback. Asset reads/bounce buffers
are capped at 1 KiB. IDF trust discovery additionally reads a 1,216-byte app
signature block and its RSA encoder has small temporary allocations; the cap
does not describe the whole verifier's internal heap demand.

LittleFS's small default allocations previously selected internal RAM under the
1,024-byte generic threshold. The filesystem now requires strict PSRAM allocation
and fails closed if unavailable. Tests of the actual installed LittleFS core and
built config/table show **1,152 bytes** of caches at blank mount, format and normal
mount (512 + 512 + 128); one open file adds 512 bytes. Wrapper/VFS/mutex bookkeeping
and flash bounce buffers are additional. Format does not allocate in proportion
to the 10.875 MiB partition. These phases already run before display/radio/link
startup. Boot heap snapshots around verification/mount and worker stack margin
now make their costs visible in the lead's logs.

The reported 1,592-byte DMA historical minimum cannot isolate one allocation:
IDF sums low-water marks of separate heaps, potentially reached at different
times. The fixes remove known avoidable internal allocations and delayed cleanup;
**new physical heap minima still require board measurements**. Do not erase the
board's filesystem merely to reproduce a first format.

### Lead: build, sign and install over the hub

This worktree's ignored `firmware/sdkconfig` was updated and reconfigured. If
rebuilding in another existing checkout, explicitly select the strict allocator;
`sdkconfig.defaults` alone does not override a saved choice:

```sh
python3.13 - <<'PY'
from pathlib import Path
path = Path("firmware/sdkconfig")
lines = path.read_text().splitlines()
prefix = "CONFIG_LITTLEFS_MALLOC_STRATEGY_"
lines = [line for line in lines if not line.removeprefix("# ").startswith(prefix)]
path.write_text("\n".join(lines) + "\nCONFIG_LITTLEFS_MALLOC_STRATEGY_SPIRAM=y\n")
PY
scripts/idf.sh -DPROJECT_VER=0.3.0-storagefix-20261027 reconfigure build
```

The explicit version is necessary for this uncommitted recovery build: the
running `0.3.0-f97c5c6` rejects an app offer with the same version. The wrapper
accepts `-DPROJECT_VER`; choose another unique version for any later changed
release. The wrapper now carries this override for that invocation only; the
next ordinary `scripts/idf.sh build` restores the git-derived version and
discards old `PROJECT_VER:` cache entries. See [memory-online.md](memory-online.md).

The new `orb.bin` is unsigned. The two existing signed files were deliberately
left intact and still describe the lead's previous build. Only the lead, who can
read the **same existing development key**, should replace the signed app:

```sh
export ORB_SIGNING_KEY=/private/path/to/current-dev-signing-key.pem
scripts/sign-app.sh firmware/build/orb.bin -o firmware/build/orb.signed.bin
PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub release \
  firmware/build/orb.signed.bin --channel dev
```

Use the lead's established hub deployment/configuration on port 8444. Wait for
the new app's accepted hello and healthy/valid boot. The already-flashed assets
version 1 should now verify and restore normal fonts, so rebuilding/re-signing
assets is **not required for recovery**. To exercise assets OTA as requested,
publish the same pack with a new positive version, after the app update:

```sh
scripts/fonts.sh assets --version 2 -o firmware/build/orb_assets-signed.bin
PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub release \
  firmware/build/orb_assets-signed.bin --assets --channel dev
```

Use a version higher than the channel's existing assets version if it has already
advanced past 1. The hub release process must use that same development key as
well. Recheck `orb-hub devices --json`, normal panel fonts, assets version 2 in
hello, `scripts/orb-dev.sh heap` and `top`, then reboot and capture the new
`assets` / `orb_fs` boot heap lines and worker stack margin. Capture warm boot,
reconnect and OTA with the normal display workload. No USB write, key change or
second-board test is part of this recovery.

### New regression evidence

```sh
.venv/bin/python -m pytest -q tests/test_storage_artifacts.py
make -C firmware/components/orb_fonts/tests/host test
make -C firmware/components/orb_fs/tests/host test
```

The artifact tests compile the actual device port, installed IDF SBv2 trust/RSA
sources, and IDF's flash-hash algorithm/range guard against the generated build
configuration and exact partition table. OpenSSL implements host PSA primitives;
no signature-success stub is used. They verify the real signed artifacts and
reproduce the old mmap rejection, then sign the current built app and exact
assets envelope with a temporary RSA-3072 key outside the build directory.
Wrong-key, payload/signature/header/truncation and signed bad-payload-digest
cases reject. See [the harness notes](../firmware/components/orb_fonts/tests/artifacts/README.md)
for adapters and limits. The development key is never read by these tests.

Final local validation after rebuilding `0.3.0-storagefix-20261027`:

| Check | Result |
| --- | --- |
| `scripts/idf.sh build` | Passed (ESP-IDF process discovery required sandbox escalation) |
| Root pytest suite | 279 passed, including all 7 exact-artifact regressions |
| Fonts host suite, ASan/UBSan | Capped loader, 111 header + 33 port checks passed |
| Filesystem host suite, ASan/UBSan | 126 API + 188 mount + 912 actual core/config checks passed |
| OTA host suite, ASan/UBSan | 152 app logic + 84 assets transaction checks passed |
| New Python Ruff format/check; `git diff --check` | Passed |

The rebuilt unsigned `orb.bin` is 2,293,760 bytes, SHA-256
`dc637e9a4bf6c9513f642fb8a95e3f7d02fc298a9e81fd887b2ea60b9b31dd90`.
Its projected signed size is 2,297,856 bytes, leaving 3,993,600 bytes
(3.808594 MiB) free in the 6 MiB slot. The preserved signed app still has SHA-256
`d4cd5ac72e3e480b19d205b0a23467d3dd7c49e12b80b4dee7280bee461d947b`
and version `0.3.0-f97c5c6`; preserved assets SHA-256 is
`9398167dc2153d5d415f79899e076d9649067fe6db53283fcc51ab5f5403ba73`.
No rendered layout, font payload or app registry behavior changed in this fix.
Normal typography and new physical heap/stack margins await the lead's panel
and device checks. No commit or board/hub operation was performed.

## USB migration (lead only)

The layout and arithmetic are in [design.md](design.md#ota-and-releases).
NVS remains `0x9000` / `0x6000`; OTA metadata remains `0xf000` / `0x2000`;
PHY remains `0x11000` / `0x1000`. The metadata is reinitialized to select `ota_0`.
The migration erases the new inactive slot, assets, storage and coredump. It is a
one-time repartition, not an in-place preservation of future offline app files.

1. Own the development board's transport lease and use external power. The script
   uses `scripts/orb-transport.sh --exec`, which holds one lease for the entire
   esptool worker; no independent monitor/flash may run alongside it. It opens one
   ROM/stub session and leaves the app stopped until verification finishes.
2. Use the **existing private signing key trusted by the development board** and
   its matching public PEM, from private paths outside the repository. The board
   currently trusts the isolated development key; generating a new/permanent key
   changes trust and is a separate owner operation. This stream's throwaway test
   artifacts under `/tmp/orb-w2-storage-signing` are test-only.
3. From this worktree, build and sign both artifacts:

   ```sh
   scripts/idf.sh reconfigure build
   export ORB_SIGNING_KEY=/private/path/to/current-dev-signing-key.pem
   scripts/sign-app.sh firmware/build/orb.bin -o firmware/build/orb.signed.bin
   scripts/fonts.sh assets --version 1 -o firmware/build/orb_assets-signed.bin
   scripts/migrate-partitions.sh --dry-run
   scripts/migrate-partitions.sh --execute \
     --public-key /private/path/to/current-dev-public-key.pem
   ```

   Replace the two example key paths with private existing files. `--dry-run`
   prints every offset and never invokes SSH, serial or signing tools. `--execute`
   validates all five artifacts, the exact new table, assets envelope and both
   block-zero signatures locally **before** contacting the USB host. Set
   `ESP_REMOTE_HOST` (default `my-mac.local`) and `ESP_REMOTE_PORT` for your USB
   host. Execution requires `--board <MAC-or-device-id>` or `ORB_DEV_BOARD` with
   your development board's identity. A dry run uses the synthetic identity
   `02:00:00:ab:cd:ef` / `orb-020000abcdef` when none is supplied.
4. The worker checks ESP32-S3, MAC and 32 MiB flash, then checks that the installed
   NVS/OTA-metadata/PHY geometry matches. It reads NVS, PHY, old table and old OTA
   metadata into private backups and fsyncs them before any erase/write. It writes
   bootloader at `0`, table at `0x8000`, initial OTA metadata at `0xf000`, signed app
   at `0x20000` and signed assets at `0xc20000`. All writes are verified; NVS and
   PHY are read again and compared byte for byte while the app is still stopped.
5. Keep the local backup directory printed by the tool, normally
   `crashes/partition-migration/<UTC>-020000abcdef/`. The remote private staging
   directory is retained as well. `verified.json` records success, image hashes
   and the NVS hash. Backups contain Wi-Fi/pairing secrets: retain them privately.
   Any flash/compare failure leaves the board in ROM/stub without resetting;
   inspect/recover from the saved artifacts before retrying. A transport failure
   still attempts to retrieve the remote backup. No automatic NVS rewrite occurs.
6. After the success reset, verify Wi-Fi/pairing survive, the UI uses normal fonts,
   `orb_fs` mounted and created `/storage/apps`, and hello reconnects. Capture heap
   and task-stack evidence with `scripts/orb-dev.sh heap` and `top`, including
   Default/AX3, reconnect and OTA. Only the lead can make those physical claims.
   `scripts/remote.sh flash` requires `ORB_PARTITIONS_MIGRATED=1` for later routine
   USB flashing; the initial install must use the migration tool.

## Assets and app OTA follow-up (lead only)

Use the updated hub code through the lead's existing hub deployment procedure;
this stream does not restart the owner's port 8444 process. Missing `kind` still
means app OTA. An `assets` hello field opts into independent assets offers;
legacy clients never receive them. App releases have priority over assets so a
new app can establish its generated pack identity before matching assets arrive.

```sh
PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub release \
  firmware/build/orb.signed.bin --channel dev
PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub release \
  firmware/build/orb_assets-signed.bin --assets --channel dev
```

The hub's signing operation must use its existing matching development trust key.
Use a new positive integer assets version when publishing changed content. A
valid signed pack for a different app safely yields rescue typography until its
matching app/pack pair is installed. Verify an ordinary app OTA, an assets OTA,
bad-signature rejection, interrupted assets recovery and NVS preservation on the
development board. Do not treat host tests as physical power-loss evidence.

## Original migration checkpoint: build, size and RAM

The final complete local build produced a 2,297,856-byte signed app, leaving
3,993,600 bytes (**3.808594 MiB**, 3.99 decimal MB) free in its 6 MiB slot. The
signed assets image is 5,632,000 bytes, leaving 2,756,608 bytes (2.628906 MiB) in
the assets partition.

Built-in generated font data is 111,893 bytes, down from 1,680,494. The raw pack
is 5,624,460 bytes. Moving faces changes lifetime: PSRAM caches inflate fonts on
demand and release unused sets. All 54 Default packed faces' inflated record
buffers together are 1,600,396 bytes; AX3/Bold's 56 records total 2,111,232 bytes.
Converted LVGL metadata and allocator overhead are additional, plus a 932-byte
pointer cache, up to 69,017 bytes of compressed input and roughly 11 KiB inflate
workspace while loading. These are source-derived bounds, not board heap usage.

Build-map static BSS: apps 96 bytes; `orb_fs` 100 bytes; LittleFS wrapper 16 bytes.
LittleFS configured caches/lookahead need 1,152 bytes plus bookkeeping; an open
file adds a 512-byte cache. Apps allocate no task/heap; hold-PWR is polled by the
existing main task. Assets OTA reuses the 8 KiB installer stack and needs a
4 KiB chunk plus HTTP/TLS allocations. Assets trust bookkeeping adds 236 bytes
of static variables before linker alignment (including two static semaphores).
Boot signature verification originally used a transient 8 KiB internal worker
stack, including its 1 KiB hash buffer. The board-results fix above moves the
worker stack to PSRAM and joins its deletion before continuing boot. The existing
3,584-byte main stack is unchanged. Exact combined heap/stack minima need the
lead's board measurements.

## Simulator design evidence

No shell, UI, display or LVGL files were changed. Marble is a controller and has
no rendered surface. Font relocation preserves the existing normal pack pixels.
Captures reviewed in this worktree:

| Surface | Default | AX3 | Reduce Transparency | AX3 + Bold + Reduce Transparency |
| --- | --- | --- | --- | --- |
| Face | `sim/build/w2-storage/face-default.png` | `face-ax3.png` | `face-opaque.png` | `face-ax3-bold-opaque.png` |
| List | `sim/build/w2-storage/agents-default.png` | `agents-ax3.png` | `agents-opaque.png` | `agents-ax3-bold-opaque.png` |
| Card | `sim/build/w2-storage/approval-default.png` | `approval-ax3.png` | `approval-opaque.png` | `approval-ax3-bold-opaque.png` |

All abbreviated paths share `sim/build/w2-storage/`; overview:
`sim/build/w2-storage/design-gate.png`. All fixtures are synthetic. Reproduce:

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build
sim/build/orb_shell_sim face sim/build/w2-storage/face-default.png
sim/build/orb_shell_sim agents sim/build/w2-storage/agents-ax3.png ax3
sim/build/orb_shell_sim approval sim/build/w2-storage/approval-opaque.png rt
ctest --test-dir sim/build --output-on-failure
```

Self-review against the design gate (applies to Face, List and Card above):

| Heading | Face | List | Card |
| --- | --- | --- | --- |
| Two layers | Pass: one glass Apps control | Pass: glass Back only | Pass: Back/Allow/Deny, no tint |
| Canvas | Pass: black, no gradient/glow | Pass: plain black content | Pass: plain black content |
| Round-first | Pass: inset clock/status/actions | Pass: chord-fitting text and Back | Pass: context and equal decisions inside rim |
| Sizes | Pass: Inter and existing targets | Pass: AX3 wraps/pages without clipping | Pass: AX3 stacks decisions without clipping |
| Colour | Pass: waiting count plus orange | Pass: state words plus icons | Pass: neutral decisions; no colour-only state |
| Approvals/agents | Pass: static waiting count | Pass: static last step | Pass: Deny persistent, neither tinted/default |
| Motion/accessibility | Pass: AX3/Bold/opaque captures | Pass: AX3/Bold/opaque captures | Pass: AX3/Bold/opaque captures |
| Gestures | Pass: visible Apps/navigation controls | Pass: visible Back and disclosure | Pass: visible Back/Allow/Deny |
| Apple-made review | Pass: restrained existing Face | Pass: existing round list | Pass: legible persistent review context |

Static captures cannot establish spring behavior or touch hit areas; the existing
simulator input/design contracts cover those. Independent lead design review is
still required before merge. Rescue mode intentionally uses one body-size face
and icons for recovery; full Dynamic Type/Bold aesthetics require valid assets.

## Verification

All commands were run in this worktree; test servers use private temporary state
and loopback listeners. No actual migration or board workload was run.

| Command | Result |
| --- | --- |
| `scripts/idf.sh reconfigure build` | ESP-IDF 6.1 build passed; unsigned `orb.bin` 2,293,760 bytes |
| `ORB_PYTHON="$PWD/.venv/bin/python" .venv/bin/python -m pytest -q tests` | 272 passed, including 18 migration tests |
| `cd hub && ~/.venvs/orb-hub/bin/python -m pytest -q` | 249 passed, app OTA and independent assets channel |
| `tools/fonts/.venv/bin/python -m pytest -q tools/fonts/tests` | 64 passed, including real RSA wrong-key/tamper checks |
| `scripts/fonts.sh check` | 6/6 sets, 732 preview lines, 2,136 status checks, 40,000 concurrent bitmap decodes; no failures |
| `tools/fonts/.cache/lvgl_check/orb_fonts_check --rescue-tests <missing-or-corrupt-pack>` | 164 checks each for missing and corrupt packs; zero failures |
| `make -C firmware/components/orb_fonts/tests/host test` | Capped-loader regression; 111 header + 30 production-port checks; ASan/UBSan clean |
| `make -C firmware/components/orb_fs/tests/host test` | 126 filesystem + 73 erased/corrupt/missing mount checks; ASan/UBSan clean |
| `make -C firmware/components/orb_apps/tests/host test` | 88 registry/lifecycle/marble checks; ASan/UBSan clean |
| `make -C firmware/components/orb_ota/tests/host test CJSON_DIR="$PWD/firmware/managed_components/espressif__cjson/cJSON"` | 152 existing OTA logic + 84 actual assets transaction checks; ASan/UBSan clean |
| `cmake -S sim -B sim/build -G Ninja && cmake --build sim/build` | Simulator build passed |
| `ctest --test-dir sim/build --output-on-failure` | 220/220 passed |
| `scripts/migrate-partitions.sh --dry-run` | All offsets printed, no transport/key access |

Offline end-to-end preflight also used the actual five ESP-IDF artifacts, real
throwaway-key signed app/assets and matching public key; all sizes, table entries,
envelope integrity and first-block signatures passed before any SSH invocation.
Ruff format/check (including ASYNC), shellcheck and `git diff --check` passed.

Primary review points:

- `firmware/partitions.csv:2`: protected geometry and full 32 MiB layout.
- `scripts/migrate_partitions.py:109`: local validation; `:157`: one-session
  worker, durable backups, identity check, byte comparison and failure retention.
- `firmware/components/orb_fonts/port/orb_assets_esp.c:47`: signature/hash
  verification; `:121`: transient boot worker; `:155`: update/read exclusion.
- `firmware/components/orb_ota/orb_ota_assets.c:7`: actual fault-tested write
  transaction; `orb_ota.c:840`: pinned HTTPS assets installer.
- `hub/orb_hub/firmware.py:95` and `hub/orb_hub/cli.py:375`: signed envelope
  publication and `--assets`; only minimal release plumbing changes in link/server.
- `firmware/components/orb_fs/orb_fs_port_esp.c:18` and `orb_fs.c:175`: erased-only
  formatting and bounded atomic writes.
- `firmware/components/orb_apps/orb_apps.c:86`: screen-off pause and held-PWR exit.
- Shared-file changes are limited to `firmware/main/main.c:429` boot/poll wiring,
  its CMake component dependencies, the generated dependency lock, and removing
  obsolete `CONFIG_ORB_FONTS_FLASH_PACK` from `sdkconfig.defaults`.

Remaining lead validation: actual USB migration/NVS receipt, normal and rescue
typography on the panel, app/asset OTA, power-cut recovery, live heap/stack minima
and independent screenshot design review. Rendered Marble remains a later shell
task; its registry/controller and storage/lifecycle contracts are complete here.
