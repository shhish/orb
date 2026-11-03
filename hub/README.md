# Orb hub

The Mac mini side of [docs/protocol.md](../docs/protocol.md): pairs Orbs, keeps a pinned-TLS WebSocket link to each, signs and serves firmware, and gives new devices the Wi-Fi over Bluetooth. Python 3.13, asyncio (aiohttp, zeroconf, cryptography, esptool's espsecure, bleak).

## Install

```bash
/opt/homebrew/bin/python3.13 -m venv ~/.venvs/orb-hub
~/.venvs/orb-hub/bin/pip install -e 'hub[dev]'      # from the repository root
~/.venvs/orb-hub/bin/orb-hub --help
```

`pyproject.toml` is the source of truth for dependencies.

The `agents` extra declares `orb-agents`; `dot` and `mcp` declare `orb-mcp`.
These are local packages, so supply them in the same install command rather
than depending on their availability in a package index. Voice is an optional
Apple Silicon extra and uses existing cached weights only:

```bash
~/.venvs/orb-hub/bin/pip install -e hub/packages/orb_agents -e hub/packages/orb_mcp -e 'hub[agents,dot]'
~/.venvs/orb-hub/bin/pip install -e 'hub[voice]'
# Owner action when ready to run these integrations:
orb-hub serve --port 8444 --agents --dot
orb-hub agents status
orb-hub agents hooks-snippet
orb-hub dot status
orb-hub demo-cards orb-020000abcdef
```

Do not restart an existing hub to try the examples during parallel development.
`--agents` adds an authenticated Claude hooks listener on `127.0.0.1:8447`
(`--hooks-port` overrides). The generated snippet contains a placeholder;
the owner privately substitutes `agents/hook.token` from the hub data directory
before merging it into their settings. `--dot` adds MCP on `127.0.0.1:8445`
(`--mcp-port` overrides), with separate client tokens under `mcp/tokens/`.
`orb-hub dot stdio …` and `orb-hub dot tunnel …` forward to the MCP package's
commands; tunnel setup generates artifacts and never loads launchd.
See [agents](../docs/agents.md) and [Dot](../docs/dot.md) for capabilities/setup.

Devices opt in with `hello.caps:["cards","voice"]`. The hub returns supported
caps, sends chunked session snapshots and replays live cards after reconnect.
Each device defaults to all sources enabled and a 10-minute recent-state window.
The hub pushes at most eight focus sessions: waiting first, then busy, then
recent state changes. `sessions.summary` counts the collapsed remainder and
sessions hidden by disabled sources. `sessions.list` pages the collapsed
remainder on demand without subscribing to its updates. Per-device preferences
persist in `sessions.db`; disabling a source cancels its displayed cards on
that device without answering pending approvals or changing another device.
Legacy devices continue pairing, reporting state and receiving OTA as before.
The coordinator enforces 24 sessions, 16 pending cards across providers and
64 delivery items per device. Decisions persist in private `sessions.db`
before acknowledgement; duplicate answers never repeat a provider action.
`delivered:false` records provider failure or lost ownership and requires
inspection in the original agent/MCP client. Stale approvals/questions are
cancelled on hub restart, so ownership is never silently reclaimed.

`orb-hub agents start claude|codex --cwd PATH --prompt TEXT [--model MODEL]`
creates an explicitly hub-owned session. Discovered native sessions retain
their original identity and truthful read-only/message capabilities.

Voice accepts at most four 60-second recordings, serializes transcription,
and emits link-private results for confirmation with `message.send`.
`ORB_WHISPER_MODEL` can select a local large-v3-turbo directory; otherwise the
Hugging Face cache is searched. Missing weights/runtime remove `voice` from
negotiation, without initiating a download. Results are limited to 280 UTF-8
bytes so they fit the confirmation/send contract. Cancellation suppresses
native results; shutdown waits for already-running native code to return.

## HomeRelay

Enable Home explicitly with `orb-hub serve --home` alongside any existing serve
options. The owner privately places a HomeRelay control token in
`<hub-data>/home/homerelay.token` with mode `0600`, inside a private `home/`
directory. The hub never prints this token or sends it to a device. No command
here changes launchd or restarts the owner's running hub.

`HOMERELAY_URL` selects the API base URL. Without it, the bridge discovers
Bonjour `_homerelay._tcp` (TXT `api=/api/v1`), then falls back to
`http://127.0.0.1:18796/api/v1`. One shared SSE stream follows changes;
devices use their existing pinned TLS link and opt in with `hello.caps:["home"]`
(add `"cards"` for doorbell attention cards). Legacy clients receive no Home
events. See [the Home wire contract](../docs/protocol.md) for bounds and commands.

The status and favourites commands emit JSON through the running hub's control
socket. Favourites persist separately for each paired device, including while
that device is offline. Use the short accessory or scene aliases shown by the
bridge; adding an accessory can pin it from Admin or Server Room. Bridges,
cameras and generated Trigger scenes remain hidden.

```bash
orb-hub home status
orb-hub home favourites list orb-020000abcdef
orb-hub home favourites add orb-020000abcdef ACCESSORY_ALIAS
orb-hub home favourites remove orb-020000abcdef ACCESSORY_ALIAS
```

## Files and ports

| | |
| --- | --- |
| `~/.local/share/orb-hub/` (mode 700; `ORB_HUB_DATA` overrides) | `devices.db` (sqlite, mode 600: devices, token SHA-256s, last state), `tls/hub.key` (600) and `tls/hub.pem`, `firmware/<sha256>.bin` and `firmware/<channel>.json`, `control.sock` (600) |
| `~/Library/Logs/orb-hub/` (`ORB_HUB_LOGS` overrides) | `hub.log` (`serve --log-file`, rotated at 5 MB with 3 old files), `launchd.log` (what launchd catches: crashes before logging starts), `devices/<device_id>.log` (each device's `log.batch` lines and the hub's notes on it, rotated at 5 MB) |
| TCP 8443, TLS | `/v1/link` (WebSocket) and `/v1/firmware/<sha256>.bin` |
| TCP 8080, plain HTTP | `/v1/hub-cert` only |
| mDNS | `_orb-hub._tcp`, instance = this Mac's name, TXT `v=1 pair=8080 fp=<16 hex>` |

The certificate is made on first run (ECDSA P-256, CN `orb-hub`, ten years, SAN = this Mac's `.local` names and LAN addresses at that moment) and never replaced: paired devices pin it. Deleting `tls/` means pairing every device again.

mDNS names its own host, `orb-hub-<LocalHostName>.local`, with A records for the LAN addresses, rather than announcing the Mac's own name: mDNSResponder owns that, and a second announcer with a different address set can make macOS rename the Mac. The host name is the one in the certificate, which devices check it against, so it stays put when the Mac is renamed later (after a Bonjour conflict, say). The addresses are checked every 30 s and re-announced when they change, so a hub that starts before Wi-Fi joins, or gets a new DHCP lease, stays reachable.

## Commands

| | |
| --- | --- |
| `orb-hub serve [--bind 0.0.0.0] [--port 8443] [--pair-port 8080] [--name NAME] [--no-mdns] [--log-file PATH]` | Runs the hub, logging to stderr or to a file it rotates. It raises its open-file limit to 4096 (macOS starts processes at 256). |
| `orb-hub pair 123456` | Confirms the pairing request whose device shows that code. |
| `orb-hub unpair <device_id>` | Forgets a device; its next `hello` is refused and it pairs again. Works with the hub stopped. |
| `orb-hub devices [--json]` | Paired devices (online, firmware, battery, signal) and pending requests (never their codes). Works with the hub stopped. |
| `orb-hub identify <device_id>` / `reboot <device_id>` | `device.identify` / `device.reboot`. |
| `orb-hub config <device_id> [--name N] [--channel dev\|stable] [--brightness 0-100]` | Name and channel live on the hub (`hello` hands them out), so they change even while the device is away; brightness needs it online. |
| `orb-hub prefs show <device_id> [--json]` | Shows source switches, recent-state window and resolved temperature unit/source through the running hub. The device may be offline. |
| `orb-hub prefs set <device_id> [--claude on\|off] [--codex on\|off] [--dot on\|off] [--idle-minutes 0-1440] [--temperature auto\|F\|C] [--json]` | Saves only supplied settings and refreshes connected devices. Zero disables the recent-state grace period. Temperature defaults to the hub's macOS temperature/region setting; `auto` clears a device override. Automatic units refresh every 60 seconds and on SIGHUP. Home wire temperatures always remain Celsius. |
| `orb-hub home status` | Home bridge connectivity and accessory/scene aliases, without credentials. Requires `serve --home`. |
| `orb-hub home favourites list <device_id>` | Saved Home favourites for a paired device; returns JSON. |
| `orb-hub home favourites add\|remove <device_id> <id>` | Changes that device's favourite accessory or scene alias and refreshes its Home snapshot; returns JSON. |
| `orb-hub release <bin> --channel dev\|stable [--notes "…"]` | Signs, stores and publishes an image, then offers it to online devices on that channel. |
| `orb-hub sign <bin> [-o out]` | Signs an image for flashing over USB (default `<name>-signed.bin`). |
| `orb-hub keys init [--pub-out PATH]` | Creates the signing key in the login Keychain (kept if it exists) and writes the public key to `firmware/keys/orb_signing_pub.pem`. |
| `orb-hub setup [--name Orb-XXXX] [--ssid SSID]` | Gives an unprovisioned Orb the Wi-Fi over Bluetooth. |

Device and integration status commands reach the running hub through `control.sock`: one JSON request per connection, `{"cmd": "...", "args": {...}}`, answered by `{"ok": true, "result": ...}` or `{"ok": false, "error": "..."}`. Snippet generation, signing and delegated MCP artifact commands run locally.

## Pairing

1. The Orb fetches the certificate from port 8080, opens `wss://…:8443/v1/link` trusting only it, and sends `pair.begin` with `commit_d`, the SHA-256 of its nonce. The hub answers with its own nonce; only then does the Orb send its nonce in `pair.reveal`, which must hash to `commit_d`.
2. It shows a 6-digit code: the hub derives the same one from its certificate's fingerprint and both nonces. `orb-hub devices` lists the request; the code itself is only on the Orb's screen.
3. `orb-hub pair <code>` creates the token (32 random bytes; only its SHA-256 is stored), sends `pair.done` and closes. The Orb reconnects with `hello`.

The commitment means neither side picks its nonce knowing the other's, so a man in the middle gets one chance in a million per run to make the codes agree; the hub takes at most 10 `pair.begin` a minute, so it can't rerun pairing fast enough to search either. Requests expire after 5 minutes (`pair.failed` `expired`); a device that starts again replaces its older request (`pair.failed` `rejected`). At most 8 requests wait at once; more, or more than 10 attempts a minute, get `busy`. A wrong code changes nothing.

## The link

- First message `pair.begin` (then `pair.reveal`) or `hello`; anything else gets `unauthorized` (as a response when it was a request) and the connection closes. Either has to be done within 10 s, pings or not. While 4 connections from one address, or 32 in all, are at that stage, more get 503 instead of the upgrade. An HTTP connection that sends no request for 10 s, or stalls in the TLS handshake that long, is closed too.
- `hello` answers with the hub's name and version, the time, this Mac's IANA zone (from `/etc/localtime`) and the device's name and channel, then offers firmware if the device's channel has a release it doesn't run.
- Device requests: `ota.check`. Device events: `state.report` (kept as the device's last state), `log.batch` (to its log file, colour codes stripped), `ota.report`. Unknown events are ignored, unknown methods get `unknown_method`.
- Messages over 16 KB close the link (1009); so does 45 s of silence, pings included (1001). Close codes: 1000 after `pair.done`/`pair.failed`, 1001 idle / too slow to pair or say hello / replaced by a newer connection of the same device / hub stopping, 1008 unauthorized, unpaired or a malformed `pair.begin` or `pair.reveal`, 1009 too big, 1013 too many pending or recent pairing requests.
- Hub requests time out after 10 s.

## Firmware

Build stable applications from the repository root with
`scripts/idf.sh --channel stable -j4 build`, then publish
`firmware/build-stable/orb.bin` with `orb-hub release … --channel stable`.
This uses the separate stable sdkconfig overlay and compiles out devtools;
the default development build remains `scripts/idf.sh -j4 build`.
`release` signs/publishes an existing image and does not rebuild it for the
selected channel. See the [build instructions](../README.md#build-flash-monitor)
for configuration isolation and the stable configuration guard.

`orb-hub release` reads the app descriptor (offset 0x20; it must be an ESP32-S3 app image), signs it with `espsecure sign-data --version 2` unless its first signature block already carries the hub's key, checks it with `espsecure verify-signature --version 2`, stores it as `firmware/<sha256>.bin` and writes `firmware/<channel>.json`. Only the first block counts because that is the only one ESP-IDF checks on the device (`verify-signature` accepts any): an image whose first block holds another key, or isn't valid while a later one is, is refused.

- **Versions.** An offer's `version` is the descriptor's version, whole (`PROJECT_VER`, or IDF's `git describe` output), which is what the firmware's `hello` reports as `fw` and what it checks the downloaded image against; `build` is the first 7 hex digits of the descriptor's `app_elf_sha256`, as `hello` reports it. A device whose `fw` equals the release's version counts as up to date, as it does on the device, so give every release its own version: `release` warns when one reuses the channel's previous version with a different image.
- **Install policy.** `dev` → `now`, `stable` → `idle`. A version that already failed on that device (rolled back, or `ota.report` `failed`) is offered with `ask`, so a broken dev build doesn't install-reboot-roll back forever.
- **Rolling back** is releasing the older image again.
- **Downloads** need `Authorization: Bearer <token>` (401 otherwise); only names that are exactly 64 lowercase hex digits plus `.bin` are looked up.

### The signing key

`orb-hub keys init` makes an RSA-3072 key and stores it with `security -i` (fed on stdin, so the key never appears in process arguments) as generic password `orb-hub-firmware-signing` in the login Keychain. The value is the PEM, base64-encoded: `security -i` reads one line of at most 4 KB per command, which neither a PEM's line breaks nor its hex form fit. To get the PEM back by hand: `security find-generic-password -s orb-hub-firmware-signing -w | base64 -d`. Signing passes the key to espsecure through a pipe, never a file. `ORB_SIGNING_KEY=<pem path>` replaces the Keychain (tests use throwaway keys this way).

The key is never replaced by `keys init`: devices trust only the key their running image was signed with. ESP-IDF (RSA Secure Boot v2 signatures without hardware Secure Boot) takes the trusted keys from the running app's own signature block, so the image flashed over USB must be signed too (`orb-hub sign firmware/build/orb.bin`), or every update is rejected.

## Wi-Fi setup

`orb-hub setup` scans for `Orb-XXXX`, asks for the 8-digit proof of possession and the network, reads the password with `getpass`, and runs espressif/network_provisioning 1.4's protocol over BLE: protocomm security 1 (X25519, PoP, AES-CTR), `prov-config` set/apply, then polls until the Orb has joined or says why it couldn't. The protobuf messages are encoded in `orb_hub/provision/` directly (checked against ESP-IDF's generated modules), so neither protoc output nor `esp_prov` is needed. The password is never printed, logged or stored.

The terminal running it needs Bluetooth permission (System Settings → Privacy & Security → Bluetooth).

## Running under launchd

[`launchd/orb-hub.plist.template`](launchd/orb-hub.plist.template) preserves
port 8444, Agents, Dot, keep-alive and rotating hub logging. The renderer derives
paths from this checkout, the selected Python and your home directory. It only
writes the requested plist; it never installs or loads a service.
`ORB_LAUNCHD_PREFIX` defaults to `org.orb`; set it to your existing label prefix
when replacing a private installation. Keep the generated plist in an ignored
location because it contains local paths. Install the hub and its local provider
packages in the selected Python environment first.

```bash
# From the repository root, using the hub's Python environment:
~/.venvs/orb-hub/bin/python scripts/render_launchd.py --output .cache/orb-hub.plist
# Optional: ORB_LAUNCHD_PREFIX=com.example before the renderer above.
# Owner installation steps (use the same prefix if you override it):
label="${ORB_LAUNCHD_PREFIX:-org.orb}.orb-hub"
mkdir -p ~/Library/Logs/orb-hub ~/Library/LaunchAgents
cp .cache/orb-hub.plist "$HOME/Library/LaunchAgents/$label.plist"
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/$label.plist"
launchctl kickstart -k "gui/$(id -u)/$label"     # restart
```

If devices can't find the hub over mDNS, check that Python is allowed under Privacy & Security → Local Network.

## Development

```bash
# From the repository root; install hub[dev] and the local package dev/test extras.
PYTHONPATH=hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src python -m pytest -c hub/pyproject.toml hub/tests -q
PYTHONPATH=hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src python -m pytest hub/packages/orb_agents/tests -q
PYTHONPATH=hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src python -m pytest hub/packages/orb_mcp/tests -q
ruff check hub
ruff format --check hub
```

The tests run the hub in-process on 127.0.0.1 with ephemeral ports and temporary data directories, and drive it with a Python fake device over real TLS (`tests/fake_device.py`). Signing uses the real espsecure with RSA-3072 keys generated per run; the Keychain is replaced by `tests/fake_security.py` (`ORB_HUB_SECURITY`), and provisioning runs against a simulated device. Nothing listens beyond loopback and nothing touches `~/.local/share/orb-hub` or the real Keychain.

`tests/test_architecture_boundaries.py` enforces the service import direction,
public ports, Link state ownership and supervised task creation. Independent
provider packages accept lifecycle ports without importing the hub.

The clean installation matrix is skipped in the default test run. To run it,
provide an existing offline wheelhouse containing the dependencies pinned in
`capability-lock.txt` plus the three projects' PEP 517 build dependencies:

```bash
nice ~/.venvs/orb-hub/bin/python hub/tools/check_capabilities.py --wheelhouse /path/to/wheels
# Or include it in pytest:
ORB_HUB_CAPABILITY_WHEELHOUSE=/path/to/wheels PYTHONPATH=hub:hub/packages/orb_agents/src:hub/packages/orb_mcp/src \
  ~/.venvs/orb-hub/bin/python -m pytest -c hub/pyproject.toml hub/tests/test_capability_packaging.py -q
```

The checker copies the local sources into a temporary directory, builds their
wheels and creates separate venvs for core, Agents, Dot, MCP and combined
capabilities. It installs locked runtime dependencies, imports the capability
entry points with `python -I` and runs `pip check`. It removes source-path
injection, disables pip indexes and persistent caches, and never downloads.
The runtime constraints capture the macOS Python 3.13 test environment; refresh
them deliberately when changing supported dependency versions.

## Wireless developer tools

A paired dev-channel board built with `CONFIG_ORB_DEVTOOLS=y` advertises the
`devtools` capability. Commands use only its existing pinned-TLS link. The local
CLI reaches the running hub through its private Unix control socket; no HTTP or
MCP developer endpoint is added. Changing channel revokes access until reconnect.

```sh
orb-hub dev orb-020000abcdef shot /tmp/orb-face.png
orb-hub dev orb-020000abcdef tap 233 400
orb-hub dev orb-020000abcdef hold 233 233 1000
orb-hub dev orb-020000abcdef swipe 233 380 233 120 300
orb-hub dev orb-020000abcdef bench 16000
orb-hub dev orb-020000abcdef bench_open apps
orb-hub dev orb-020000abcdef bench_scroll agents 16000
orb-hub dev orb-020000abcdef bench_game marble 16000
orb-hub dev orb-020000abcdef bench_nav back
orb-hub dev orb-020000abcdef heap
orb-hub dev orb-020000abcdef top
orb-hub dev orb-020000abcdef power
```

`bench_nav` aliases the existing `bench_open` production navigation benchmark.
Receipts retain the USB format. One command runs per device and at most four
globally, with a 90-second completion deadline and bounded output. Commands are
never retried automatically. Screenshots require complete ordered base64 chunks,
466×466 RGB565 dimensions and a matching CRC before the CLI atomically replaces
the PNG; failed transfers preserve any prior file. PNG pixels outside the round
panel are transparent. USB tooling remains available.
