# Link-online memory — lead handoff

Branch `orb/w2-render`, changes uncommitted. This stream did not access the board,
the live hub, owner settings, Keychain or launchd. No UI pixels/layouts changed;
the design screenshot gate does not apply to this allocation/diagnostics fix.

Source entry points: `orb_link/link_memory.c:17` installs the hooks;
`orb_link/orb_link.c:962` attributes parsing and `:1071` routes fragments;
`orb_mem/orb_mem.c:92` initializes diagnostics and `:179` emits phase receipts;
`orb_devtools/orb_devtools.c:689` appends them to `heap`. These paths are under
`firmware/components/`. `firmware/main/main.c:419` performs early initialization;
`orb_voice/orb_voice.c` marks real audio open and routes request contexts;
`orb_ota/orb_ota.c:558` coordinates OTA measurements. Headers and component CMake
files wire these dependencies, including standalone link test apps. New/extended
host tests live under `orb_link`, `orb_model`, `orb_voice` and `orb_mem`.
Version selection is `firmware/project-version.cmake:4`, included by the main
firmware CMake file, with invocation handling in `scripts/idf.sh:15` and nine
regressions in `tests/test_project_version.py`. README, devtools/link component
READMEs and the prior storage handoff document the updated behavior.

## Attribution and allocation policy

The real parser's host regression measures a representative 2,365-byte snapshot
chunk containing six sessions: **177 cJSON allocations, all ≤1 KiB, 6,462 bytes
peak requested payload** before allocator headers. Under the existing generic
1,024-byte internal preference those nodes, keys and values could use internal
RAM. This verifies the allocation source, not the board's recovered headroom.

`app_main` now installs process-wide cJSON hooks before any JSON service worker.
Every JSON node/string/printed envelope requests `SPIRAM | 8BIT`; PSRAM OOM fails
closed without borrowing internal memory. The hooks stay immutable while trees
exist. Audited cJSON callers are link, voice, OTA and main's link callbacks, all
task context, never ISR or cache-disabled work. Standalone link apps initialize
the same policy. Fragment growth, certificate copies and voice request contexts
also require PSRAM, including copies smaller than the generic threshold.

The model store, snapshot scratch and UI snapshot copies already require PSRAM.
Snapshot decoding already processes one ≤4,096-byte envelope at a time, stages
at most 24 sessions and publishes only the final chunk. It does not accumulate
DOMs across chunks. Tests exercise four chunks, full 24-session/16-card model
copies, parser allocation failure at every allocation and clean retries.

mbedTLS already uses `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`; the installed IDF
allocator requests PSRAM explicitly. The managed WebSocket API exposes no caps
or caller-owned receive-buffer option. Its 4,096-byte RX/TX generic allocations
prefer PSRAM with the current threshold, but can fall back internally on PSRAM
pressure; smaller IDF transport/control bookkeeping can still use internal RAM.
No vendor patch, TLS-size reduction or global malloc-policy experiment was added.
DMA/ISR/RTOS allocations retain their requirements. Audio still needs at least
5,760 internal sample-buffer bytes plus descriptors, driver and codec overhead.

## Measurements available to the lead

With `CONFIG_ORB_DEVTOOLS=y`, the existing command now appends phase receipts:

```sh
scripts/orb-dev.sh heap
```

- `@@HEAP` keeps historical internal/DMA/PSRAM free/largest/min-free output.
- `@@HEAPPHASE historical internal=... target=24576 pass=...` compares the
  boot-wide minimum with the 24 KiB target. It does not prove all workloads ran.
- Phase rows report completed `count`, currently `active` windows, and worst
  internal/DMA/PSRAM minima across those invocations. Unobserved phases are omitted.
- `@@HEAPFAIL` reports allocation failure count and last requested size/caps,
  both per active phase and globally. No pointers, payloads or function strings
  are retained or printed. No row means zero captured failures.

Phases: `link-connect`, `tls-handshake`, `hello`, `json-message`, `snapshot-parse`,
`card-show`, `voice-service`, `voice-start`, `ota`. TLS includes client setup,
TLS handshake and HTTP upgrade because the public API signals completion after
all three. `card-show` covers decoding and model insertion; page rendering is
still included in the boot-wide historical minimum. `voice-start` covers the
actual codec/I2S open, distinct from allocating the voice worker at boot.

Snapshot/card envelopes are classified after parsing; their measurements include
the initial DOM peak and cleanup. An undecodable envelope remains `json-message`.
Failure counters remain attributed to phases active when the failure happened,
so a pre-classification failure appears under `json-message`, not its later alias.

The central monitor uses IDF 6.1's local minimum reset/restore API at phase
boundaries, under one static mutex. Overlapping windows include concurrent
display/radio/service allocations; these are time-window measurements, not
per-task ownership. IDF sums per-region lows that may occur at different times,
so these conservative minima must not be added together. Internal and DMA overlap.
IDF allocates a small monitor bookkeeping array while a window is active; it is
freed at the last end. The diagnostics retain bounded internal metadata and no
payload copies. Main/console historical reads and OTA's attempt readings use the
same coordinator so one reader cannot reset another's window. OTA still reports
current-attempt minima; its internal pool now matches the console's INTERNAL
capability set rather than INTERNAL|8BIT. Diagnostics compile out when disabled.

## Lead-only board verification

1. Acquire the existing exclusive development-board lease. Use only the already
   paired development board `orb-020000abcdef`. Keep the running hub on 8444 and
   its enabled real-agent providers; do not restart/reconfigure it or erase NVS,
   storage, assets, bootloader or partition table.
   Before publishing, inspect `orb-hub devices --json` and confirm this is the
   only device assigned to `dev`: a release targets the whole channel, including
   devices that reconnect later. If the channel is not already isolated, use the
   lead's separately coordinated deployment path; do not change or test the
   second board for this run.
2. Choose a unique one-build version and publish it as the current dev release,
   so the feed cannot replace the experiment with an older app. Build and sign
   with the **same existing development key**,
   using the established app-only hub deployment procedure:

   ```sh
   scripts/idf.sh -DPROJECT_VER=0.3.0-memfix-20261027 reconfigure build
   # The lead supplies the existing dev key path privately; no Keychain change.
   ORB_SIGNING_KEY=/private/path/to/current-dev-signing-key.pem \
     scripts/sign-app.sh firmware/build/orb.bin -o firmware/build/orb.signed.bin
   ORB_SIGNING_KEY=/private/path/to/current-dev-signing-key.pem \
     PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub release \
     firmware/build/orb.signed.bin --channel dev
   ```

   Wait for accepted hello, healthy/valid boot and the expected firmware identity.
   This is app OTA only; the storage migration is already complete. The memory
   test starts after the new app boots, so earlier install history is excluded.
3. Save logs through connect, TLS, hello, full real-session snapshot and replayed
   live cards. Check provider status and device status using the established hub:

   ```sh
   PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub agents status
   PYTHONPATH="$PWD/hub" ~/.venvs/orb-hub/bin/orb-hub devices --json
   scripts/orb-dev.sh heap
   ```

   Confirm the source has at least 24 real sessions and the device shows the
   bounded snapshot without reconnect churn. `snapshot-parse count` counts chunks,
   **not sessions**. Do not substitute the two-session `demo-cards` workload.
4. Keep the display running. Open the replayed live cards and a session that
   truthfully supports messaging. Hold/release BOOT or its mic control to start
   real capture, then cancel the transcript without sending a message/approval.
   Save another heap receipt. Require `card-show count>0`, `voice-start count>0`,
   and no active windows left once settled. Capture screenshots only after the
   heap acceptance run because screenshot allocations change historical lows.
5. Accept only if both internal and DMA historical minima remain ≥24,576 bytes,
   all required phases ran, and no allocation failures, display errors, panics,
   watchdogs or unexpected reconnects occurred. Repeat a fresh boot and reconnect
   with the same 24-session/card/voice workload. Compare phase minima and steady
   free/largest values for leaks. If headroom fails, use the lowest phase and
   failure caps to select the next source fix; do not call the host result a pass.

The ≥24 KiB target and physical-display behavior remain **unverified here**.

## Host validation

`scripts/idf.sh build` passes (ESP-IDF 6.1); `orb.bin` is unsigned and 0x230000
bytes before any lead signing. An actual override build reported
`0.3.0-memcheck`; the next ordinary build restored `0.3.0-<HEAD>` with no
`PROJECT_VER:` cache entry. See the root README for the invocation-only override.

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build
ctest --test-dir sim/build --output-on-failure
for component in orb_apps orb_board orb_display orb_fonts orb_fs orb_link \
                 orb_mem orb_model orb_ota orb_ui orb_voice; do
  make -C "firmware/components/$component/tests/host" test
done
make -C firmware/components/orb_net/tests/host
```

Results: 230/230 simulator tests; memory diagnostics 239 enabled + 3 disabled
checks; link core 28,928, v1.1 1,414, memory/faults 67,035, callback 14,237,
delivery 53, outgoing 367; model 445 + 151 checks and 16,000 concurrent updates
and snapshots; voice 2,213 + 97; OTA 152 + 84. Component host suites for board,
display, fonts, filesystem, network, UI and apps also pass. Native sanitizers
cover the new allocation and diagnostic tests.

Root pytest: 396 passed, including nine version regressions. The artifact tests
used temporary matching throwaway signatures over this built app and exact font
pack; both test-signed files and their private key were removed afterwards. They
do not validate the lead's existing development signing key. Hub/adapters/MCP:
792 passed, two real-agent tests excluded. The exact pytest invocations used the
existing hub interpreter plus a temporary dependency layer (Pillow 12.0.0,
PyYAML 6.0.3, standardwebhooks 1.1.0), without changing that interpreter's packages:

```sh
orb_test_python="$HOME/.venvs/orb-hub/bin/python"
orb_test_deps=/private/tmp/orb-mem-py313/lib/python3.13/site-packages
PYTHONPATH="$orb_test_deps" ORB_PYTHON="$orb_test_python" \
  "$orb_test_python" -m pytest -q
PYTHONPATH="hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src:$orb_test_deps" \
  "$orb_test_python" -m pytest -q -m 'not live' \
  hub/tests hub/packages/orb_agents/tests hub/packages/orb_mcp/tests
```

The temporary layer must be recreated if absent; an isolated test venv with the
project's test extras and those dependencies is also suitable. The 396-test run
included temporary matching signed artifact fixtures as described above.

The Linux-target link build also passes, followed by **65 successful e2e checks**
against a loopback-only fake hub with temporary certificates:

```sh
scripts/idf.sh -C "$PWD/firmware/components/orb_link/tests/e2e" \
  --preview set-target linux build
"$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/python" \
  firmware/components/orb_link/tests/e2e/hub_stub.py \
  firmware/components/orb_link/tests/e2e/build/orb_link_e2e.elf
```

Ruff (including ASYNC), formatting,
shellcheck and whitespace checks pass. The full firmware build and exhaustive
network timezone test needed execution outside the process-restricting sandbox.
Raw logs are `/tmp/orb-mem-*.log`. Nine representative CTest regression PNGs are:
`sim/build/shell-{face,agents,approval}-{default,ax3,ax3-bold-rt}.png`. These verify
existing surfaces; no UI/layout changes were made, so the design gate is not
applicable to this fix. E2e logs containing throwaway tokens were not retained.
