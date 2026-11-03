# Orb ↔ hub protocol, v1.1

The contract between the device firmware (`firmware/components/orb_net`, `orb_link`, `orb_ota`) and the Mac mini hub (`hub/`). Both sides implement exactly this; change it here first.

## Discovery

The hub advertises over mDNS:

| | |
| --- | --- |
| Service | `_orb-hub._tcp.local.` |
| Instance name | the hub's display name, e.g. `Mac mini` |
| Port | `8443` (TLS: WebSocket link and firmware downloads) |
| TXT | `v=1`, `pair=8080` (plain-HTTP pairing port), `fp=<first 16 hex chars of the hub certificate's SHA-256>` |
| Host (SRV target) | `orb-hub-<LocalHostName>.local`, the name in the certificate's SAN; it stays that even if the Mac is renamed later. Its A records are the hub's current LAN addresses. |

The device browses for `_orb-hub._tcp` once Wi-Fi is up and on every reconnect, and checks the link's certificate against the host name. A paired device prefers the hub whose `fp` matches its pinned certificate; if mDNS fails it retries the last known IP and port.

## Identity and TLS

- The hub has one self-signed certificate (ECDSA P-256, CN `orb-hub`, SAN = its `.local` hostname and LAN IPs, 10-year validity), created on first run in `~/.local/share/orb-hub/tls/` (`hub.key` mode 600, `hub.pem`).
- `cert_fp` = SHA-256 of the certificate in DER form (32 bytes). It's shown lowercase hex where printed.
- A paired device keeps the hub's PEM in NVS and gives it to esp-tls as the only trusted certificate (a self-signed certificate is its own CA), so every TLS connection is pinned. No certificate bundle is used, and the certificate is always verified. Firmware downloads go to the hub's last known IP and don't check the certificate's names: the pin alone identifies the hub, and the IPs the certificate names go stale when the hub's DHCP lease changes.
- `device_id` = `orb-` + the 12 lowercase hex digits of the Wi-Fi station MAC, e.g. `orb-020000abcdef`.

## Pairing

Pairing gives the device a token and pins the hub's certificate, with a short code you compare to defeat a man in the middle. The PEM the hub serves holds exactly that one certificate, and the device refuses any other: esp-tls trusts every certificate it's given, while the code covers one.

1. **Fetch the certificate.** The device GETs `http://<hub>:8080/v1/hub-cert`, which returns the PEM (`Content-Type: application/x-pem-file`). This port serves nothing else.
2. **Open the link with it.** The device connects to `wss://<hub>:8443/v1/link`, trusting only that PEM, picks a fresh random `nonce_d` (16 bytes) and sends `pair.begin` (below) as its first message with `commit_d` = SHA-256(`nonce_d`) (hex), not the nonce itself.
3. **Hub answers** with its own random `nonce_h` (16 bytes, hex).
4. **The device reveals** `nonce_d` with `pair.reveal`. The hub checks that its SHA-256 is `commit_d` (if not: `bad_request`, and the connection closes), answers `{}`, and holds the request as pending for 5 minutes.
5. **Both sides compute the code:**
   `sas = (big-endian uint32 of the first 4 bytes of SHA-256("orb-pair-v1" ‖ cert_fp ‖ nonce_d ‖ nonce_h)) mod 1000000`, shown as 6 digits with leading zeros. `cert_fp`, `nonce_d` and `nonce_h` are hashed as raw bytes; the device uses the certificate it actually connected with. The device shows the code full-screen.
6. **You confirm on the Mac:** `orb-hub pair 123456`. The hub finds the pending request with that code (none: error, nothing changes), creates a 32-byte random token, stores its SHA-256 with the device record, and sends the device the `pair.done` event over the same connection.
7. **The device stores** `device_id`, `token`, the hub PEM and the hub's last address in NVS namespace `orb_link`, closes, and reconnects normally.

Test vector (both sides unit-test against it):
- `cert_fp` = SHA-256("test-cert") = `ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2`
- `nonce_d` = `000102030405060708090a0b0c0d0e0f`
- `commit_d` = SHA-256(`nonce_d`) = `be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991`
- `nonce_h` = `101112131415161718191a1b1c1d1e1f`
- hash = `acbc0186c13758df7488335a8e95f3eba38e7818a36dad14e3be0940ebc3f554`
- `sas` = `002310`

A man in the middle presenting its own certificate makes the device's code differ from the hub's, so the confirmation fails. The commitment is what keeps it from fixing that: whoever answers `pair.begin` has to pick `nonce_h` before it learns `nonce_d`, so a man in the middle can't search for a `nonce_h` that gives the device the hub's code; each run gives it one chance in a million. To stop it simply rerunning pairing with the hub until the codes agree, the hub accepts at most 10 `pair.begin` a minute (more get `busy`). Unconfirmed requests expire; a device can retry with new nonces. `orb-hub unpair <device_id>` deletes the record; that device's next `hello` gets `unauthorized` and it returns to step 1.

## Link

`wss://<hub>:8443/v1/link`. Text frames carry JSON envelopes; binary frames carry audio only when the `voice` capability is negotiated (v1.1 below). Developer screenshots use bounded base64 text events below.

| Envelope | Shape |
| --- | --- |
| Request | `{"t":"req","id":<int>,"m":"<method>","p":{…}}` |
| Success | `{"t":"res","id":<int>,"ok":true,"r":{…}}` |
| Failure | `{"t":"res","id":<int>,"ok":false,"e":{"code":"<code>","msg":"<text>"}}` |
| Event | `{"t":"evt","m":"<name>","p":{…}}` |

- Either side may send requests and events. Each side numbers its own requests from 1; a response carries the id of the request it answers.
- A request with no response within its timeout (default 10 s) fails locally with `timeout`.
- Error codes: `bad_request`, `unauthorized`, `not_found`, `unknown_method`, `busy`, `internal`, `timeout`.
- Unknown events are ignored. Unknown methods get `unknown_method`.
- Keepalive: the device sends a WebSocket ping every 15 s; the hub closes a connection silent for 45 s. Negotiated `power_hint` peers may use the background cadence below. The device reconnects with backoff 1, 2, 4 … 30 s, plus up to 20 % jitter while interactive; background retries may grow to 300 s. Deliberate viewing intent admits one prompt retry per 30 s.
- Messages are at most 16 KB and nest at most 32 levels of arrays and objects, the envelope included. The device drops others unread.

### The first message

An unpaired device's first message is `pair.begin`, and its second `pair.reveal`; a paired device's first is `hello`. Anything else in their place gets `unauthorized` and the connection closes. A connection that hasn't sent `hello` or `pair.reveal` within 10 s is closed (1001), pings or not, and the hub refuses the WebSocket upgrade (503) while 4 connections from the same address, or 32 in all, are at that stage.

**`pair.begin`** (device → hub)
```json
{"device_id":"orb-020000abcdef","name":"Orb","model":"waveshare-s3-amoled-1.75c","fw":"0.2.0","commit_d":"<64 hex>"}
```
Result: `{"nonce_h":"<32 hex>","hub_name":"Mac mini"}`, or `busy` when 8 requests are pending or 10 were made in the last minute.

**`pair.reveal`** (device → hub), sent once `pair.begin` has answered
```json
{"nonce_d":"<32 hex>"}
```
Result: `{}`; the device shows the code. Later, event **`pair.done`** `{"token":"<64 hex>"}` or **`pair.failed`** `{"reason":"expired"|"rejected"}`.

**`hello`** (device → hub)
```json
{
  "device_id": "orb-020000abcdef",
  "token": "<64 hex>",
  "fw": "0.2.0", "build": "d7b73f7", "idf": "v6.1",
  "boot_slot": "ota_0", "reset_reason": "poweron", "uptime_s": 12,
  "last_ota": {"version": "0.2.0", "result": "ok"|"rolled_back"|"failed", "error": "…"},
  "power": {"battery_pct": 100, "battery_mv": 4020, "charging": false, "usb": true, "display_state": "active"},
  "rssi": -54
}
```
`fw` is the app descriptor's `version` and `build` the first 7 hex digits of its `app_elf_sha256`, so the hub reads both from a release image the same way. `battery_pct` is -1 without a battery and `battery_mv` is -1 when the device cannot read the cell voltage; `display_state` is one of `active`, `dimmed`, `always_on` or `sleep` and describes the panel at sampling time. `power` is left out when the device can't read it. These fields are additive: older hubs ignore the new ones. They ride the existing report and never cause one, so the reporting cadence below is unchanged. `last_ota` is present only on the first `hello` after an update attempt (strictly: until a `hello` carrying it is accepted). A new image on probation reports `"result":"ok"` in the `hello` that completes its health check, since that acceptance is what makes it healthy. Result:
```json
{"hub_name":"Mac mini","hub_version":"0.1.0","time_ms":1793050000000,"tz":"America/New_York","device":{"name":"Orb","channel":"dev"}}
```
`tz` is an IANA name; the device maps it to a POSIX TZ string with a small built-in table, falling back to UTC.
The result may also contain `tz_posix` (string, ≤63 ASCII characters), the POSIX TZ rule for `tz` derived by the hub from its tzdata/zoneinfo footer. Orb resolves time zones by checking its built-in table first, then `tz_posix` (if valid printable ASCII under 64 chars), and falls back to UTC. Orb persists `tz_posix` in NVS across boots.
The result may also contain `hour24` (boolean), the Mac owner's preferred hour
cycle. The hub resolves it from the current user's date formatter and caches
it for its process lifetime; a non-macOS hub falls back to its process locale.
Older peers may omit/ignore it. Orb adopts it as the Time default, persists it
for offline use, and preserves an explicit on-device 12/24-hour selection
across reconnects. This field does not promise translated UI strings or fonts;
the device's supported C locale supplies AM/PM and weekday names.

### Methods and events

| Name | Kind | Direction | Params → result |
| --- | --- | --- | --- |
| `state.report` | event | device → hub | `{"power":{…},"rssi":-54,"heap_internal":78000,"heap_psram":7900000,"uptime_s":…}`, every 60 s and on change |
| `log.batch` | event | device → hub | `{"lines":["I (1234) main: …", …]}`, up to 50 lines, at most once a second |
| `ota.check` | request | device → hub | `{}` → `{"available":false}` or an offer (below) |
| `ota.available` | event | hub → device | an offer, sent after `hello` and on release |
| `ota.report` | event | device → hub | `{"version":"0.2.1","state":"downloading"\|"verifying"\|"ready"\|"failed","progress":0-100,"error":"…"}` |
| `device.ping` | request | hub → device | `{}` → `{"uptime_s":…}` |
| `device.identify` | request | hub → device | `{}` → `{}`: the device shows its name and code for 10 s |
| `device.reboot` | request | hub → device | `{}` → `{}`, then reboots after the response is sent |
| `device.config` | request | hub → device | any of `{"name":"…","channel":"dev"\|"stable","brightness":0-100}` → `{}` |

`state.report`'s `power` object has the same shape as `hello`'s, including the additive `battery_mv` and `display_state`. `battery_mv` is the cell voltage in millivolts (-1 when unknown) and `display_state` is the panel state at sampling time. Neither field changes when a report is sent: a report still goes out every 60 s and on the existing `battery_pct`, `charging` or `usb` change, never solely because the voltage or panel state moved.

After a panic reset, the first successfully sent `state.report` of that boot also
carries `crash`: `{"task":"sound_prefs","pc":1077551104,"backtrace":[1077551104,1077560000],"corrupted":false}`.
`task` is at most 16 bytes; `pc` and each of at most 16 `backtrace` entries are
unsigned 32-bit instruction addresses. `corrupted` means unwinding stopped at an
invalid frame; a bounded partial backtrace is still useful. If no valid saved
dump can be read, `crash` instead contains `{"error":"ESP_ERR_NOT_FOUND"}` (or
the actual ESP-IDF error name), never fabricated task/address data. The device
keeps this report pending across reconnects until transport send succeeds, leaves
the dump intact for developer retrieval, and does not repeat it on later reports
or non-panic boots. The hub appends the compact summary to the device log before
later state samples replace the live state. This is additive; older peers ignore
the field and reporting cadence is unchanged.

**OTA offer**
```json
{"available":true,"version":"0.2.1","build":"e1f2a3b","size":823456,
 "sha256":"<64 hex of the whole .bin>","url":"https://<hub>:8443/v1/firmware/<sha256>.bin",
 "install":"now"|"idle"|"ask","notes":"…"}
```
`version` and `build` are what `hello`'s `fw` and `build` would say running that image: the app descriptor's `version`, whole (the device refuses an image whose descriptor `version` isn't exactly the offer's), and the first 7 hex digits of its `app_elf_sha256`. The hub offers an app only when both versions contain a valid numeric `major.minor.patch` release number and the channel release number is strictly greater than `hello`'s. Compare the three integers, never strings, build IDs, timestamps, or suffixes. A different `-git`, `-bench` or `+build` suffix on the same release number is not newer and is not offered; unknown/non-orderable versions also fail closed. This deliberately preserves bench builds and prevents feed downgrades. Automatic distribution therefore requires a higher numeric release number. Publishing an older or equal-numbered artifact does not authorize replacing a device; rollback/recovery requires a separate explicit owner workflow. `build` remains descriptive for app ordering. `install` comes from the channel: `dev` → `now`, `stable` → `idle` (install while charging and untouched for 5 minutes, or when tapped). A version that already failed on that device (`last_ota` not `ok`, or `ota.report` `failed`) is offered with `ask` instead, so a bad build can't loop.

## Wireless developer tools (opt-in v1.1 capability)

Firmware built with `CONFIG_ORB_DEVTOOLS` advertises `devtools` in `hello.caps`
on its first connection, including when its channel is not yet known. This
advertises compiled support, not permission: the accepted hello supplies the
authoritative channel without requiring a second TLS handshake. Stable release images compile
with `CONFIG_ORB_DEVTOOLS=n`, excluding both USB and wireless developer tools.
The authenticated hub negotiates it only for a paired device whose saved channel
is `dev`. The device also checks its accepted hello channel and negotiated
capability before every command and output. A stable-channel device or a build
without that option cannot execute wireless developer commands. Changing channel
revokes access immediately; a fresh hello is needed to enable it again.
These commands and their output travel **only on the existing paired pinned-TLS
WebSocket**. There is no device listener, HTTP route, MCP tool, tunnel export or
unauthenticated fallback. The owner CLI uses the hub's existing private Unix
control socket. USB commands retain their existing format.

All developer pointer input (USB and Wi-Fi tap, hold, swipe and arc) is marked
synthetic at the LVGL input device. Synthetic input may navigate, scroll and
run benchmarks, but must never answer a card (including Allow, Deny, questions
and dismissal options), send a voice message, set/toggle a Home accessory,
execute a scene, confirm an OTA install or confirm another consequential
action. The activating input is checked at each action boundary, including
hold/timer paths; refusal leaves the pending action unchanged and shows only
a brief `Answer on Orb` hint. Physical touch and hardware buttons retain their
normal confirmation behaviour. This is a device interaction boundary; the hub
cannot infer physical provenance from `card.answer` or other wire requests.

Every hub-to-device request below has exactly `{stream,args}`: `stream` is an
integer 1–2147483647, strictly increasing on a connection, and `args` is the positional
argument array below. No booleans, extra fields or coercions are accepted.
The response is `{accepted:true}` within the normal 10 s request deadline;
acceptance means queued, not completed. One command at a time owns the shared
USB/Wi-Fi executor; contention returns `busy` without injecting input.

| Request | `args` |
| --- | --- |
| `devtools.shot` | `[]` |
| `devtools.tap` | `[x,y]` |
| `devtools.hold` | `[x,y,ms]` |
| `devtools.swipe` | `[x1,y1,x2,y2,ms]` |
| `devtools.arc` | `[radius,a0,a1,ms]`; one continuous touch around (233,233), 0° at 12 o'clock, clockwise positive |
| `devtools.tilt` | `[x,y,ms]`; finite specific-force x/y in g, each −1..1 with x²+y² ≤ 1, integer duration 1..5000 ms; game-input override only |
| `devtools.bench` | `[ms]` |
| `devtools.bench_open` | `[surface]`, `play\|dot\|agents\|apps\|back` |
| `devtools.bench_scroll` | `[surface,ms]`, `agents\|rooms\|settings\|notifications\|favourites` |
| `devtools.bench_game` | `[game,ms]`, `marble\|beads\|orbit\|ring-snake\|pop` |
| `devtools.bench_nav` | `[surface]`, same production navigation benchmark as `bench_open` |
| `devtools.heap`, `devtools.top`, `devtools.power` | `[]` |
| `devtools.lightsleep` | `["usb","on"\|"off"]`; RAM-only USB bench override, default off after reboot; does not wake, reveal or inject input |
| `devtools.buttons` | `[]`; last 16 raw/debounced button edges with monotonic millisecond timestamps |
| `devtools.wake` | `[]`; wake the display before synthetic navigation |
| `devtools.goto` | `[surface]`, `face\|stack\|apps\|agents\|sessions\|usage\|dot\|home\|time\|play\|notifications\|control\|settings\|sound\|battery\|updates\|onboarding`; navigate through the development input helper |
| `devtools.press` | `[button,gesture]`, `pwr\|boot`, `short\|double\|hold`; synthetic navigation/feedback only |
| `devtools.soundplay` | `[palette_id]`, integer 0..18; sampled palette order in `orb_sound.h` |
| `devtools.soundvolume` | `[volume]`, integer 0..100; persistent local volume |
| `devtools.soundsilent`, `devtools.soundalwayson` | `[enabled]`, integer 0 or 1; transient sound policy |
| `devtools.soundstop`, `devtools.soundstatus` | `[]`; stop palette loops, or print readiness, memory and call-to-codec-submission latency (not acoustic latency) |
| `devtools.senses` | `[]`; latest main-owned gyro/accel, guarded tap/twist counts, and truthful battery-detail fields; never injects motion |
| `devtools.micrecord` | `[]`; lease audio, capture exactly 5 s of raw dual-mic 16 kHz PCM16; bounded PSRAM allocation of 320,000 bytes |

Coordinates are integers 0–465. Arc radius is an integer 1–232 px; angles are
integers −3600–3600 degrees, interpolated without wrapping (350→370 crosses
12 o'clock clockwise; 10→−10 crosses counterclockwise). Injected gestures wake
the display first. Hold/swipe/arc durations are 1–5000 ms;
benchmark durations are 1000–60000 ms. Command strings are at most 95 bytes.
Tilt wakes the display first and replaces only the app motion input until its
monotonic deadline; it never synthesizes pickup, face-down, tap or twist events.
Its positive z is reconstructed as √(1−x²−y²), and the normal calibrated game
pipeline still applies (hold the neutral pose through calibration before testing
a tilt). Negative x moves downhill to the right; positive y moves down the panel.
The last tilt replaces any earlier override, expires even if an app is paused,
and is compiled only into development-tools firmware.
Unknown methods use `unknown_method`; malformed input uses `bad_request`;
unavailable/disabled capability uses `unsupported` and a revoked channel uses
`unauthorized`. No request executes shell commands or accepts arbitrary console
text. Benchmarks call the existing USB handlers and preserve their measurement
and restoration semantics; `bench_nav` is an explicit alias, not a new metric.

Output is ordered device-to-hub `devtools.output` events
`{stream,seq,text}`, with zero-based consecutive integer `seq` and nonempty UTF-8
`text` of at most 2048 bytes. Text fragments concatenate exactly; they need not
end on a line boundary. A complete envelope remains at most 4096 bytes.
`devtools.done {stream,ok}` terminates successful or failed execution; on failure
it also carries a generic `code` from the extension errors. No success may be
reported after output was lost. Each stream is limited to 2 MiB of decoded text,
8192 output events and 90 s from admission through completion. Send backpressure
has a bounded timeout; saturation fails the stream rather than dropping chunks.
Disconnect, channel revocation or a deadline discards output and pending work;
an already running handler may finish bounded cleanup but may never migrate its
output or touch sequence to a new connection. The hub never retries input or
benchmarks automatically. One stream per link and at most four globally.

Screenshot text reuses the USB framing verbatim:
`@@ORBSHOT BEGIN W H rgb565le 192 BYTES CRC32`, indexed `@@00000 BASE64`
lines (192 decoded bytes, except the final line), and `@@ORBSHOT END`.
The complete tightly packed little-endian RGB565 image must have dimensions
466×466, length 434312 bytes, all indexes exactly once in order, valid base64,
and the IEEE CRC32 matching the eight hexadecimal digits in the header (same
CRC as USB). The hub validates every bound and the final successful completion
before producing a PNG, masking pixels outside the round panel transparent.
Failed/truncated transfers do not replace the requested output file. Other
commands preserve the existing console receipts, including errors; the CLI
must report failed completion as failure, never benchmark success.

## Firmware downloads

`GET https://<hub>:8443/v1/firmware/<sha256>.bin` with header `Authorization: Bearer <token>`. The response is the signed app image (`application/octet-stream`, `Content-Length` set). Without a valid token: 401.

The device always fetches from the hub it is paired with (its pinned address), using the path of the offer's `url`, so the token never goes anywhere else. It streams the image into the inactive OTA slot, checks the SHA-256 against the offer, and lets ESP-IDF check the signature (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, RSA-3072, Secure Boot v2 signature block). Only then does it switch the boot slot.

In that mode ESP-IDF has no configured verification key. It trusts the public key in the first signature block of the image the device is *running*, and accepts a new image only if that image's first block carries the same key and its signature verifies. So every image a device runs must be signed with the release key, including images flashed over USB (`scripts/sign-app.sh`). A device running an unsigned image cannot take updates until it is USB-flashed with a signed one, and changing the key needs USB too. The bootloader doesn't check signatures in this mode, so USB recovery always works.

After rebooting into the new image (state `ESP_OTA_IMG_PENDING_VERIFY`), the app has 60 s to bring up the display and complete a `hello`. On success it marks itself valid; otherwise it marks itself invalid and reboots into the previous slot. The outcome goes in the next `hello`'s `last_ota`.

Offers with `install` `now` or `idle` wait while the battery is below 30 % and not charging (a tap still installs them): losing power on probation rolls a good image back.

## Signed assets updates (wave 2)

App OTA messages and missing `kind` continue to mean application updates. A device
with this implementation adds `"assets":"<verified assets version>"` to `hello`;
`"assets":""` means no verified signed assets image is available. A verified
image can still have an incompatible font-pack identity, in which case fonts use
rescue until matching assets are published. Older devices omit the field and are
never sent assets offers. `ota.available`, `ota.check` replies and
`ota.report` may carry `"kind":"assets"`; an explicit app kind is `"app"`.
`ota.check {"kind":"assets"}` asks only for the assets channel. All existing offer
fields and install policies apply, except `version` identifies the signed assets
header, `build` is the first seven hex digits of the payload SHA-256, and the size
is bounded by the 8 MiB assets partition. Assets reports are recorded separately
from app failures; a failed assets version waits for an explicit install request.

The 64-byte little-endian header is `magic[8]="ORBASSET"`, `format:u32=1|2`,
`header_size:u32=64`, `payload_size:u32`, `signed_size:u32`,
`content_version:u32` (nonzero; rendered as decimal in offer/hello `version`),
`kind:u32` (equal to `format`), `payload_sha256[32]`. Format/kind 1 carries a
font pack. Format/kind 2 carries a fonts-and-sounds bundle, so both formats
remain accepted. `signed_size` is
`align4096(64 + payload_size)`; padding bytes are `0xff`, then one 4096-byte
Secure Boot v2 signature sector. The total image is at most 8 MiB.

A format-2 payload starts with the 32-byte little-endian bundle header
`magic[8]="ORBBNDL2",schema:u32=2,font_offset:u32,font_size:u32,
sound_offset:u32,sound_size:u32,total_size:u32`. The font section starts at
byte 32, contains one complete `ORBFONTS` pack and is followed by alignment
padding. The sound section is 4-byte aligned, follows the font section, and
contains one complete `ORBSOUND` schema-1 palette through the end of the payload.
Offsets and lengths are relative to the payload; `total_size` equals its size.
All sections share the outer payload hash and signature.

The download uses the existing authenticated, pinned-hub `/v1/firmware/<sha>.bin`
route. An assets image is a versioned header plus its font or bundle payload, padded to
a 4 KiB boundary and signed with an RSA-3072 Secure Boot v2 signature sector. The
first signature block must use the running application's signing key. The SHA-256
of the entire signed download must match the offer, and the signed header's
version must match it. Unsigned, corrupt, incompatible or wrong-key assets never
become usable fonts.

There is one assets slot. Before its first erase, fonts atomically stop new reads
from that partition and use built-in rescue faces; already loaded font objects
remain valid. Download failure or power loss leaves the partition untrusted and
the rescue faces available. After a complete download, the device validates the
header, payload hash and signature from flash, reports ready and reboots to load
the new pack. It never changes otadata or the app boot slot for an assets update.
The next boot independently revalidates the image before trusting any pack bytes.

`orb-hub release <assets-image.bin> --assets --channel dev|stable [--notes "…"]`
signs/verifies this image with the existing release key, stores the immutable
signed blob, and updates `<channel>-assets.json` separately from the application
channel. App releases take precedence on connection. Assets are offered only when the
channel has an application manifest and the running application matches its
exact version **and** build ID. A missing application manifest/build or a
bench/older/newer application suppresses assets offers, including explicit
`ota.check` and release notifications, because the hub cannot establish font-pack
compatibility. Assets content versions are ordered as nonnegative decimal
integers and offered only when strictly greater; an empty advertised version
means no verified pack and permits any positive content version. Equal or older
assets are never re-offered automatically. No permanent key or eFuse is changed.

## Releases (hub side)

`orb-hub release <path/to/orb.bin> --channel dev|stable [--notes "…"]`:
1. reads `project_name`, `version` and the build time from the image's app descriptor (offset 0x20 of the first segment);
2. signs it with the RSA-3072 key from the Mac mini's Keychain (`espsecure sign-data --version 2`), unless its first signature block already carries that key (images signed with `scripts/sign-app.sh` for USB flashing do). It refuses an image whose first block holds another key: the device only checks the first block;
3. verifies the signature (`espsecure verify-signature --version 2`, which accepts any of the three blocks, so step 2's first-block check still matters);
4. stores it as `~/.local/share/orb-hub/firmware/<sha256>.bin` and updates `~/.local/share/orb-hub/firmware/<channel>.json`;
5. sends `ota.available` to every online device on that channel.

`orb-hub keys init` creates the signing key (RSA-3072) and stores it in the login Keychain as generic password `orb-hub-firmware-signing` (the PEM, base64-encoded so that `security -i` takes it on stdin, never in process arguments). The firmware build needs no key: `CONFIG_SECURE_BOOT_VERIFICATION_KEY` only exists for the Secure Boot v1 ECDSA scheme, and builds stay unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` off) until `scripts/sign-app.sh` signs them. The public key is also written to `firmware/keys/orb_signing_pub.pem`, so anyone can check an image without the private key (`espsecure verify-signature --version 2 --keyfile firmware/keys/orb_signing_pub.pem <bin>`). The private key never touches the repo.

## Wi-Fi setup

Not part of the link. An unprovisioned device runs ESP-IDF network provisioning over BLE: security 1, service name `Orb-XXXX` (last 2 MAC bytes, uppercase hex), proof of possession = 8 random digits shown on screen. `orb-hub setup` on the Mac (or any machine in BLE range) runs the provisioning client: it asks for the PoP, SSID and password in the terminal (the password via `getpass`, never echoed, logged or stored by the hub).

Security 1 with an 8-digit PoP keeps out passive listeners, not an active attacker in BLE range during setup. One that advertises as `Orb-XXXX` and gets the hub to connect to it learns the PoP offline in seconds from the hub's half of the handshake (10^8 candidates at one SHA-256 and one AES block each), finishes the session and receives the SSID and password. So setup is run with the device at hand, and `orb-hub setup` lets the user choose when it finds more than one `Orb-XXXX`; two with the same name mean one is an imitation. Security 2 (SRP6a, the device deriving salt and verifier from the on-screen PoP) would make each guess cost two 3072-bit exponentiations, too slow to finish within a session; that is a change to the device and the hub together.


## v1.1 — sessions, attention cards, voice and Home

This extension is opt-in. A paired device adds `"caps":["cards","voice"]`
to `hello`; the result contains `caps`, the intersection supported by the hub.
Missing caps means v1: no new events or binary processing. Unknown capabilities
are ignored. `voice` requires `cards` so that transcripts can be confirmed.
All existing pairing, OTA, authentication and request deadlines remain intact.
The independent `home` capability below is available when the hub is started
with `--home`; it does not require `cards` or `voice`.
The independent `usage` capability enables account quota snapshots below.
The independent `weather` capability enables owner-configured forecasts below.

### Radio power hints (additive capability)

The independent `power_hint` capability negotiates radio/traffic policy without
changing protocol version. A device may add `power_hint:"interactive"|"background"`
and `ping_interval_s:15` to `hello`. The hub echoes `power_hint` in result `caps`
when supported. Until that echo, the device keeps its 15-second WebSocket ping
and sends no new power events. Older hubs ignore these optional hello fields
and unknown capabilities; older devices omit them and retain existing behavior.
Unknown optional fields in `hello` and `state.report` are ignored (this does not
relax the strict validation of v1.1 action requests).

After negotiation, `state.report` also carries `power_hint` and
`ping_interval_s` (15 while interactive, 45 while background). Send a report
immediately on a hint change, as well as on the ordinary reporting cadence.
The hub accepts only these enum values and intervals, and allows 45 only with
`background`. It permits silence for three times the announced interval;
unnegotiated links retain the configured legacy timeout (45 seconds by default).
WebSocket control pings still receive immediate pongs; the hub does not add a
periodic server ping. Provisioning, voice capture/playback and OTA use the
interactive radio policy regardless of panel state. A hint never authorizes
an input action, reveals content or changes wake-contact consumption.

While background, the hub coalesces non-urgent session lists/progress, usage,
weather, Home and Now Playing updates. It sends at most one non-urgent
`power.batch` event per 60 seconds:
`{"events":[{"m":"usage.snapshot","p":{…}},…]}`. Children are existing hub events
and are applied in order through their ordinary handlers. The whole envelope
retains the 4096-byte extension bound; batches cannot contain nested batches,
requests or responses. Complete ordered snapshot groups are replaced as a
unit, never partially superseded. A group that cannot fit is retained until
interactive, when all pending state is flushed immediately using the ordinary
bounded events. Pending storage is bounded; saturation closes the link so
reconnect repairs state rather than silently losing updates. This means a
large Home/session snapshot may remain stale while dark, but waking flushes it
without waiting for the minute deadline.
Coalesced `agent.stream` snapshots receive consecutive per-link sequence
numbers at transmission, so deliberate suppression cannot resemble packet loss.

Attention/approval cards and their cancellation, alarms/reminders, voice,
OTA, request responses and developer commands remain immediate. A transition
to interactive flushes deferred state immediately. Only negotiated peers
receive `power.batch`; unknown events remain ignored by legacy firmware.
The initial complete `sessions.snapshot` after each hello is also immediate:
it establishes the model required to accept urgent cards. Subsequent complete
session snapshots follow the background coalescing policy. Because a complete
session snapshot clears the device's displayed cards, each replacement is
followed immediately by replay of still-live, currently authorized cards.
Cancelled or answered cards are never revived by this replay.

### Shared model and limits

All string limits below are **UTF-8 bytes**, not characters. IDs are nonempty:
session IDs ≤48, card IDs ≤64 and option IDs ≤32. Unix timestamps are finite,
nonnegative seconds. Unknown fields are rejected for requests in this extension.

A **Session** is `{id,agent,title,project,state,reason,summary,updated,can_message}`:
`agent` is `claude|codex|dot`; `state` is `busy|waiting|idle|done|error`;
`title` ≤64, `project` ≤32, `reason` ≤64, `summary` ≤160; `can_message` is a
boolean describing actual delivery support, never inferred from observation.
A **Card** is `{id,session?,kind,title,body,detail?,options,reply,priority,expires?,scope?}`:
`kind` is `approval|choice|message|attention`, `title` ≤64, `body` ≤280,
`detail` ≤200, `priority` is `low|normal|high`, and `reply` is boolean.
`options` contains at most four `{id,label,role}` entries with unique IDs,
`label` ≤20, `role` one of `default|primary|cancel|destructive`.
Approval cards may carry `scope:"operation"|"turn"`; only approval cards accept
this field. Provider-backed approvals always include their actual grant scope.
Operation-scoped grants say “Allow once”; turn-scoped grants say “Allow for turn”
and explicitly explain in `body` that subsequent operations in the current turn
are covered. These labels/body are mandatory display content even on clients
that ignore the additive `scope` field. Neither grant is a default/tinted action.
Unknown or broader scopes must not offer approval on the device: the hub shows
a read-only attention card directing the owner to the provider application.
Legacy/sample approval cards without `scope` convey no broader grant than the
single operation shown. A provider must never infer a broader scope from omission.

The hub admits at most 24 sessions and 16 live cards (including offline cards)
across providers. Additional work gets `busy` and can be retried; no approval
is automatically answered or evicted. Answer records survive reconnect/restart.
Extension JSON envelopes, including responses and events, are ≤4096 bytes.
Legacy frames retain their 16 KiB limit. Each snapshot is chunked to fit.
Except for the device-side Home rejection rule below, frames beyond the
negotiated connection ceiling close with 1009, including
grossly oversized binary frames; malformed audio within that ceiling gets
`voice.error` and terminates only the recording.
Session changes coalesce by ID, latest wins, at most four updates/second/ID.
Each device has one bounded delivery worker, at most 64 queued items; a slow
consumer is disconnected and receives a fresh snapshot/cards on reconnect.
No event is silently dropped while leaving apparently current state visible.

### Sessions and cards

| Name | Kind / direction | Params → result |
| --- | --- | --- |
| `sessions.subscribe` | req device → hub | `{}` → `{}`; sends a fresh snapshot and live cards after the response |
| `sessions.snapshot` | evt hub → device | `{sessions:[Session,…],reset:bool,more:bool}` |
| `sessions.summary` | evt hub → device | `{idle:n,hidden_by_source:{claude:n,codex:n,dot:n}}` |
| `sessions.list` | req device → hub | `{include:"idle",limit?:1..8,cursor?:string}` → `{sessions:[Session,…],cursor:string|null}` |
| `prefs.set` | req device → hub | `{sources?:{claude?:bool,codex?:bool,dot?:bool},idle_minutes?:int,units?:{temperature:"auto"|"F"|"C"}}` → `Prefs` |
| `prefs.update` | evt hub → device | `Prefs` (complete replacement) |
| `session.update` | evt hub → device | `{session:Session}` (upsert) |
| `session.remove` | evt hub → device | `{id}` |
| `card.show` | evt hub → device | `Card` (idempotent upsert by `id`) |
| `card.cancel` | evt hub → device | `{card}` |
| `card.answer` | req device → hub | `{card,option}` OR `{card,text}` → `{card,answer:{option}|{text},delivered:bool}` |
| `message.send` | req device → hub | `{target,text,stream?}` → `{accepted:true,status:"queued"\|"delivered",reason:string}`; failure includes `status:"failed"` and a safe reason in the error message |

The hub automatically subscribes after a cards-capable `hello` response.
A snapshot starts with `reset:true`; continuations use `reset:false`, and only
the last chunk has `more:false`. An empty snapshot still sends one chunk.
Chunks and subsequent updates are ordered; reconnect discards a partial snapshot.
Cards replay after the last snapshot chunk. A targeted MCP card is sent only to
its assigned paired device; agent cards are shared across paired cards devices.

### Per-device focus and preferences

Every cards-capable device uses focus mode, including existing v1.1 devices
that never send preferences. Cards- or Home-capable `hello` results include
`prefs:{sources:{claude:true,codex:true,dot:true},idle_minutes:10,units:{temperature:"F",source:"auto"}}`
by default on a US-region hub (otherwise the temperature default follows the
hub's settings below).
`Prefs` always has that complete shape. Preferences are persisted per paired
device by the hub; v1 devices receive no new fields/events. `prefs.set` is an
atomic partial patch: omitted settings keep their saved values, source keys
must be known booleans, and `idle_minutes` is an integer from 0 through 1440
(0 disables the recent-state grace period). Empty patches and unknown fields
get `bad_request`. The response contains the resulting complete preferences;
the hub refreshes subscribed state on all links for that device.

`units.temperature` in `Prefs` is always the resolved display unit, `"F"` or
`"C"`; `units.source` is `"auto"` or `"owner"`. The patch accepts only
`units:{temperature:"auto"|"F"|"C"}`: `"auto"` clears the saved override,
while `"F"`/`"C"` persist a per-device owner override. `source` is output-only.
An empty `units` object is invalid. Existing saved preferences without units
use `"auto"`. Partial patches preserve all omitted settings atomically.
Home-only and Weather-only clients may also use `prefs.set`; session source/focus settings have
no effect unless `cards` is negotiated.

Automatic units use macOS global `AppleTemperatureUnit` (`Fahrenheit` or
`Celsius`) when set, otherwise the region in `AppleLocale`: US, LR, MM, BS,
BZ, KY, PW, FM and MH use Fahrenheit; all other/missing regions use Celsius.
The hub reads these settings off the event loop at startup, on SIGHUP and
every 60 seconds. Owner overrides survive automatic changes and hub restarts.
When resolved preferences change (including via owner CLI), `prefs.update`
sends the complete `Prefs` to every connected cards/Home/Weather link for that device
through its bounded delivery worker. Legacy v1 links receive neither prefs nor
this event; older clients may safely ignore the additive units field/event.
Temperature units affect display only: Home `temp` and `target` remain Celsius
in snapshots, updates, write requests and accepted write responses. Devices
convert to the display unit and back to Celsius for temperature writes.

Snapshots and pushed upserts contain at most **8 focus sessions** from enabled
sources: first `waiting` ordered oldest state-entry first, then `busy` ordered
most recent activity (`updated`) first, then other sessions whose state changed
within `idle_minutes`, newest state-entry first. Equal timestamps use session
ID as a deterministic tie-breaker. Initial observation uses `min(updated,now)`
as state-entry time; subsequent state transitions use hub time. Changes to title,
summary or `updated` alone do not renew the grace period or reorder waiting
sessions. The hub expires recent sessions even without new provider activity.
Snapshot order is authoritative: membership/order changes send a fresh chunked
snapshot, while content-only changes use coalesced `session.update`. Removal
from focus also sends `session.remove`; it does not remove the provider session.

After every snapshot the hub sends `sessions.summary`, and sends it again when
counts change. `idle` counts all enabled sessions collapsed outside the focus
cap (including any overflow busy/waiting sessions); `hidden_by_source` counts
all sessions from each disabled source and is zero for enabled sources. Thus
focus + idle + hidden counts account for every admitted hub session.
`sessions.list` enumerates that collapsed enabled remainder on demand, ordered
by most recent `updated`, then ID. Limit defaults to 8. Pages may be smaller to
fit the 4096-byte envelope budget; `cursor:null` means exhausted. Omit cursor
for the first page. Cursors are opaque, at most 80 UTF-8 bytes, scoped to the
device, its preferences and the ordered remainder; stale/wrong-device cursors
get `conflict`, malformed cursors `bad_request`. Restart paging without a cursor
after a conflict. Paged sessions receive no pushes until they enter focus.

Disabling a source suppresses its sessions **and cards**, including cards without
a session reference. Cards carry hub-private source ownership, not a new Card
wire field. The hub sends `card.cancel` for its cards already presented/queued
on that device and suppresses stale writes before transport delivery. This is
presentation cancellation only: provider requests stay pending, no option is
selected, and other devices retain their cards. Answers/messages/new recordings
to disabled sources get `unsupported`; re-enabling replays still-live cards.
An already recorded answer still returns its durable result without redelivery.
The setting also applies to reconnect/replay and owner CLI updates. Existing
4096-byte, 64-item delivery queue and four-updates/second limits remain in force.

`card.answer` is first-answer-wins by card ID, including across devices and hub
restarts. Repeats return the recorded answer even if a different answer is
submitted; they never reissue a provider decision. The hub validates the first
option against the card, or accepts text (1–280 bytes) only when `reply:true`.
An explicit cancel/dismiss action is represented by a card option with role
`cancel`; it is never inferred from navigation or disconnect. Expired/cancelled
cards cannot be answered. The answer is durably recorded before acknowledgment.
`delivered:false` means the decision was recorded but provider delivery failed;
`delivered:true` is recorded only after the provider transport write completes
successfully, not merely after an internal decision future resolves. It confirms
transport delivery, not that the provider has executed the approved operation.
The hub never automatically replays approvals after restart. Provider hook
expiry/disconnect cancels its card and fails closed. A later answer cannot
override an approval already answered by another controller.
Each approval card binds the exact pending provider request instance and scope;
reusing a provider request ID cannot redirect an old card to a replacement grant.
Provider question/choice cards bind to a stable provider question ID held by the
hub. Their replies atomically claim that exact pending question; stale answers
record `delivered:false` and never answer a replacement question or start a turn.
Identities distinguish individual requests even when a provider reuses its RPC
IDs, and are re-checked after waiting for the transport write lock. Questions
without a stable identity remain read-only attention cards.

`target` is a session ID or the literal `dot`; text is 1–280 bytes and is
untrusted user content. Observed sessions with `can_message:false` return
`unsupported`; they are never resumed/forked/taken over. Dot text requires an
active matching `voice.transcribed` subscription; otherwise `not_listening`.
Dot acceptance commits a durable webhook outbox entry and returns `queued`,
not `delivered`: ChatGPT has not acknowledged receiving or acting on the text.
An absent Dot subscriber is an idle/disconnected session, never a permission
request or a reason to move Dot ahead of work that needs the owner.
Dot's `can_message` and disconnected reason are projected for the receiving
paired device, not inferred from another device's subscription. A direct Dot
view may discover its collapsed Session through `sessions.list`; that private
Session summary may contain its last answer sentence when no focus stream is
available. A newly queued Dot message enters idle; its actual reply enters
done, renewing the ordinary recent-state grace through a real state change.
An optional `stream` identifies the current link's completed recording; it must
match its target/text and is consumed only after accepted delivery. This is the
confirmation step: transcription alone never sends text or triggers actions.

### Agents 2.0: live progress and usage

Live session progress and confirmed talk are the primary Agents experience;
approvals remain reachable as waiting cards. No transcript archive or raw tool
input is sent to the device. Cards subscribers additionally receive
`agent.stream {session,seq,at,state,stale,activity,steps,assistant}` for **focus
sessions only**, respecting per-device source preferences and the eight-session
limit. `session` is a Session ID, `seq` a nonnegative per-link/session sequence,
`at` an observation timestamp in Unix seconds, `state` the Session state, and
`stale` a boolean (no progress observed for 15 minutes). `activity` is at most
64 UTF-8 bytes. `steps` contains the last three
`{id,kind,label,state}` entries: IDs ≤64 bytes, kinds `tool|reasoning`, labels
≤64 bytes, and states `running|done|failed`. Steps may include `added` and
`removed`, independently optional integer line counts in 0–2147483647. Omit
unknown counts; absence is never displayed as zero. The hub preserves these
only when its source provides them. `assistant` is
`{id,text,final}`: ID ≤64 bytes, last sentence ≤160 UTF-8 bytes, boolean final.
Empty assistant values are `{id:"",text:"",final:false}`.

Each event is a complete replacement, coalesced latest-wins at 250 ms (at most
four updates/second/session). Subscribe/re-subscribe and focus changes send a
fresh sessions snapshot followed by current `agent.stream` replacements;
these establish sequence baselines after a gap or hub restart. Clients that
detect a sequence gap request `sessions.subscribe {}` to repair it. The same
sequence may be replayed as a snapshot. Incomplete JSONL lines are held until
complete; truncation/replacement resets the tail and duplicate records are
ignored. Only readable reasoning **summaries** may be shown; thinking blocks,
encrypted content and raw reasoning never are. Labels omit tool bodies and
redact environment assignments, credential-like tokens and long hex strings.
Tool labels describe the operation using bounded command targets or basenames;
adjacent equivalent read/search bursts may collapse into a counted step. A
collapsed step stays running while any member runs, and is failed if any member
failed. Assistant text is the latest complete prose sentence with Markdown and
code removed; an unfinished next sentence does not replace it. Dot assistant
streams are scoped to the paired device that received the exchange, including
after reconnect, and never expose another device's conversation.

With `usage` negotiated the hub sends `usage.snapshot
{windows:[{provider,label,used_percent,resets_at,observed_at,stale,source,on_credits}],unknown}`.
`provider` is `claude|codex`; `label` comes from duration (300 minutes →
`"5 hours"`, 10080 → `"week"`, otherwise `"N minutes"` or `"N hours"`). A
Claude model-specific week window is instead labelled with the model name.
`source` is the observation channel: `statusline`, `usage`, `rollout`,
`app-server` or `unknown`. `used_percent` is finite 0–100, `resets_at` and
`observed_at` finite nonnegative Unix seconds, and `stale` is true after 15
minutes without a quota observation. `on_credits` is an optional boolean; for
Codex it is true when a window is at 100% and a positive paid credits balance
is available. There are at most ten windows (eight
Claude, including model-specific weeks, and two Codex). Nullable/absent windows
are omitted, never invented as zero usage. `unknown` lists providers without any
usable observation. A valid observation with no current windows is known, with
no windows for that provider. Expired windows are omitted. Usage is separate
from session token/context totals and does not depend on focus membership.
Snapshots are pushed on change at most once per 30 seconds per link, including
re-subscribe; a new connection receives the latest snapshot immediately.

Codex quota observations use rollout `event_msg/token_count.payload.rate_limits`
(`primary`, `secondary`, `credits`, `limit_name`), or an already hub-owned
app-server's `account/rateLimits/read`/`account/rateLimits/updated`. Window
minutes determine labels; credits are metadata, never treated as percentages.

Claude has two observation channels, and the freshest usable reading wins while
keeping its `observed_at` and `source`. The statusline channel is
`orb-hub statusline` stdin: `rate_limits.{five_hour,seven_day}.{used_percentage,resets_at}`
plus a hub observation timestamp in a private 0600 file in the hub data
directory. The primary `/usage` channel runs the owner's `claude` binary with
`-p /usage` (no shell and no model call) immediately at agents startup and then
no more often than every five minutes, with a 30-second timeout and exponential
backoff after repeated failures. The binary is resolved from `PATH`, then
`~/.local/bin/claude`. Output is parsed as session, week (all models) and
model-specific week windows; reset text uses its `(Area/City)` zone, including
year rollover. Missing, reworded or garbage lines are skipped without crashing;
a failed poll drops only that channel, so a newer statusline reading can still
be reported. A provider with no usable reading is in `unknown`. No raw provider
output, credentials, private session sockets or model probes are retained or
logged.

`can_message` is true only for an available owned transport or an explicitly
enabled Codex queue adapter. Owned Claude sends a stream-json stdin user
message; owned Codex starts a turn or steers with its exact `expectedTurnId`.
Opt-in existing Codex delivery invokes `codex queue --thread <native-id>
--message <text>` without a shell and acknowledges **queued**, never delivered.
Owned transport acknowledgement means **delivered**, not executed. Queue and
transport failures return **failed** with a safe reason; no failed recording
is consumed. Claude's private desktop inbox remains experimental and disabled
by default. Observation alone never resumes/forks/takes over a session.
Success `reason` is at most 160 UTF-8 bytes; failure uses `e.status:"failed"`
and the safe reason in `e.msg`, preserving the existing error code.
All Agents 2.0 events retain the 4096-byte envelope and bounded delivery worker.

### Voice

| Name | Kind / direction | Params → result |
| --- | --- | --- |
| `voice.begin` | req device → hub | `{stream,target}` → `{}` |
| `voice.end` | req device → hub | `{stream}` → `{}`; transcription runs asynchronously |
| `voice.cancel` | req device → hub | `{stream}` → `{}`; discards capture/result, suppresses later callbacks |
| `voice.result` | evt hub → device | `{stream,text}` (≤280 UTF-8 bytes; longer speech fails rather than silently truncating) |
| `voice.error` | evt hub → device | `{stream,code,msg}` |

`stream` is an integer 1–65535, unique for the life of one WebSocket connection.
`target` is a session ID, `dot`, or `time`. The `time` target is transcription
only: it does not require an agent or Dot conversation. Its result is reviewed
on Orb and an explicit physical Save creates a local reminder. Orb cancels the
capture/result after saving; it must never send `message.send` for `time`.
Local reminder parsing is deliberately bounded to explicit relative minutes or
hours, or a local clock time with AM/PM (24-hour values are also accepted).
Unrecognized or ambiguous instructions retain an error receipt and create
nothing. The hub never interprets, schedules or persists these local reminders.
One stream per device, at most four globally (capture or transcription), max
60 s of 16 kHz mono PCM16LE = 1,920,000 sample bytes. Capture also has a 60 s
wall-clock deadline. At most one transcriber runs at once, with a bounded global
admission count; it imports mlx-whisper lazily and uses a cached
`mlx-community/whisper-large-v3-turbo` model. No model download is initiated.
Native transcription has a 120 s result deadline. Cancellation suppresses its
result but retains its admission/serialization until native code returns;
shutdown joins that native work. A hung native runtime therefore requires
process supervision; there is no claim of a hard native termination deadline.

Binary frames have a **7-byte header** followed by little-endian signed PCM16:
byte 0 = `0x01` (audio); bytes 1–2 = stream ID, big endian; bytes 3–6 = sequence,
big endian, starting at zero and increasing by one. Payload is nonempty, even,
≤1024 bytes. 20–32 ms frames (640–1024 bytes) satisfy both the model's 20–60 ms
cadence and the 1 KiB cap; a 60 ms chunk must be split. No compression.
Bad type/stream/sequence/size terminates that recording with `voice.error`;
a v1 device's binary frames remain ignored. `voice.end` requires nonempty PCM.
Disconnect/cancel releases buffers and suppresses results, including when an
already running native transcriber cannot be interrupted. Results stay private
to the source link. Session/Dot results are confirmed with `message.send`;
Time results are confirmed locally as described above, never auto-delivered.
Refreshing `sessions.subscribe` preserves queued/in-flight voice events on that
link. Cancelling a stream invalidates queued results and re-checks their identity
after acquiring the transport send lock, immediately before writing. A result
whose write has already begun may precede the cancel response; no cancelled
result may begin a write after cancellation has succeeded.

### Extension errors

In addition to existing codes: `unsupported` (capability/provider unavailable),
`conflict` (ID reused with different content), `expired` (card/stream expired),
`not_listening` (Dot has no matching subscription), `too_large` (bounded content
or recording exceeds its budget). Malformed fields/frames use `bad_request`,
unknown IDs use `not_found`, saturation uses `busy`, transcription failures use
`internal`, and deadlines use `timeout`. Voice recordings with no speech activity
or no reliable transcription segments use `no_speech` (with a message inviting
the user to try again); this adds no fields or version negotiation to v1.1.
Errors never include provider payloads,
transcripts, credentials or local filesystem paths.
Device copy distinguishes an explicit rejected send (`not_found`, `expired`,
`not_listening`, `unsupported`, `busy`) from an ambiguous timeout/transport
failure. A failed send retains its transcript for review and is never retried
automatically. Transcription failures cannot appear as a sent message.

### Spoken replies (additive v1.1 device speech descriptor)

A device ready for playback adds `"speech":{"formats":["pcm_s16le"],"rates":[16000,24000],"prebuffer_ms":150}`
to hello. This descriptor is independent of microphone capture, and is honored
only when `voice` is negotiated. A hub must not send spoken replies to a device
without it. The hub generates the voice; the device never runs TTS. No wake word
is enabled. PCM16 avoids a decoder and per-chunk ADPCM state, has predictable
latency and uses only 32 or 48 kB/s on the existing LAN TLS link.

| Name | Kind/direction | Params |
| --- | --- | --- |
| `voice.speak` | evt hub → device | `{stream,rate,format:"pcm_s16le",text,voice?}`; rate 16000 or 24000, mono, text 1–1024 UTF-8 bytes for captions |
| `voice.speak.end` | evt hub → device | `{stream,chunks}`; unsigned count of accepted audio chunks, then drain buffered audio |
| `voice.speak.stop` | evt hub → device | `{stream}`; discard buffered audio immediately |
| `voice.speak.stopped` | evt device → hub | `{stream,reason}`; `complete`, `stop`, `double_tap`, `talk`, `silent`, `interrupted`, `underrun`, `overflow` or `bad_audio` |

Stream IDs are integers 1–65535, strictly increasing per connection in the
hub-to-device direction; they are independent of capture IDs. One reply plays
at once; a newer reply stops the previous one with `interrupted`. Reconnect
cancels all buffered audio and captions; stream numbers may restart then.
Caption text is untrusted display content and never an input action or command.
`voice`, when known by the Mac PCM producer, is its selected voice display name
(1–64 UTF-8 bytes, no control characters), for example `Ava`. It is optional,
never an inferred device default. The device hides the Voice settings row until
an accepted stream supplies it, clears it when an accepted stream omits it, and
clears it on disconnect. Older producers may omit it; older devices ignore it.
Silent and Always On keep captions available but suppress speech (`silent`).

Each complete binary WebSocket message is `0x02 | stream:u16be | seq:u32be |
pcm_s16le[]`. Sequence starts at zero and increments by one; PCM is nonempty,
even-sized and at most 1024 bytes. The 1031-byte message limit applies after
WebSocket fragment reassembly. Type 0x01 remains device capture only. A
duplicate/out-of-order/missing sequence, invalid size or format fails the active
reply closed; frames for old or unannounced streams are ignored. `chunks` must equal the next expected
sequence after all preceding binary messages. The hub sends chunks in real time
or with bounded lookahead (<500 ms); it never sends an entire answer as a burst.

The device prebuffers 150 ms, with a fixed 500 ms maximum at 24 kHz. An ended
short reply may start with less prebuffer. An underrun emits silence and
re-enters prebuffering; one second without progress stops with `underrun`.
System sounds are ducked during speech. Local Stop, a safe double-tap, or detected
speech over the speaker cancels audio locally before notifying the hub; there
is no dependency on a round trip. The fallback detector compares microphone
energy against a learned playback-envelope coupling and requires sustained
excess energy; this is talk-to-interrupt, not acoustic echo cancellation. Stopped
notifications belong to their source connection and must never migrate after
reconnect. The hub stops producing chunks on receipt, tolerates a notification
for a completed stream, and ignores unknown old stream IDs.

### Weather (additive v1.1)

The independent `weather` capability is offered in `hello.caps` and echoed by
the hub. It automatically subscribes the paired link to **`weather.state`**
events, complete replacements on hello, location/unit changes, successful
refreshes and staleness changes. Legacy peers receive no weather events.
Weather uses the existing bounded delivery worker and no new listening port.

```json
{"configured":true,"available":true,"place":"New York","unit":"F",
 "temperature":72.1,"condition":"clear","is_day":true,"label":"Sunny",
 "high":75.2,"low":58.0,
 "hourly":[{"hour":1793224800,"temperature":71.0,"condition":"clear","is_day":false}],
 "updated_at":1793221200,"stale":false}
```

`place` is at most 64 UTF-8 bytes, `label` at most 32, and `unit` is `F|C`,
resolved from this device's `Prefs.units.temperature`. All temperatures are
finite numbers in that unit, bounded to -250..350. `condition` is one of
`clear|partly_cloudy|cloudy|fog|drizzle|rain|snow|thunder`; `is_day` is boolean.
Clear daytime is labelled `Sunny`, clear night `Clear`; other short labels are
`Partly cloudy`, `Cloudy`, `Fog`, `Drizzle`, `Rain`, `Snow`, `Thunder`.
`hourly` contains the next six strictly future hourly forecasts (at most six),
in strictly increasing `hour` order. Each `hour` and `updated_at` is an integer
Unix second in 0..4294967295; UI formats hours with its normal local clock.
Hourly entries include their own `is_day` for day/night glyph selection.
The entire event envelope is at most 4096 bytes. Unknown additive keys may be
ignored; malformed, duplicate or oversized fields invalidate the whole snapshot.

`configured` means the owner set a location. With none (including `--weather
off`), send `configured:false,available:false,place:"",label:"Set a location in
orb-hub",temperature:null,condition:null,is_day:false,high:null,low:null,
hourly:[],updated_at:0,stale:false` plus the resolved `unit`. Before the first
successful fetch, configured locations use the same unavailable shape with
their place and label `Loading weather` (or `Weather unavailable` after a
failure); failure sets `stale:true`. Unavailable snapshots never contain
invented temperatures or conditions. `available:true` requires configured,
nonempty place, numeric readings, a condition and positive `updated_at`.
`updated_at` is Open-Meteo's current observation timestamp, retained on failure.
An available snapshot is stale on a fetch failure or when its observation is
at least 40 minutes old. A link loss retains its last good model snapshot and
marks it stale; reconnection replaces it. Clearing/changing location discards
the previous location's value. Unit changes never relabel old numbers.

The owner sets the hub-wide location with `orb-hub prefs --weather "<city>"`
(Open-Meteo geocoding, saving name/latitude/longitude),
`orb-hub prefs --weather-latlon 0,0`, or
`orb-hub prefs --weather off`. There is no IP location inference. The hub
fetches Open-Meteo over outbound HTTPS with a timeout every 20 minutes per
active unit, with failure retries after 60, 120, 240 ... 1200 seconds. It keeps
the last good observation during backoff. No API key or secret is stored.

### Home

Home is an independent opt-in v1.1 capability. Add `"home"` to `hello.caps`;
the hub returns it only when its Home bridge is enabled. A temporary HomeRelay
outage does not remove the negotiated capability. `home` alone imposes the
same **4096-byte complete-envelope ceiling**, depth limits, request deadlines
and bounded delivery rules as `cards`. No Home events or fields are sent to
links without this capability; existing v1 and cards/voice clients continue
unchanged. Unknown capabilities and unknown events remain safely ignored.

**Device failure isolation.** A malformed Home application message or an
over-limit Home model is rejected without closing the shared link. Count each
rejection and warn once per Home message type using a static reason, never
payload contents. Discard incomplete Home staging and disable Home writes
until a fresh complete snapshot arrives; cached state must not appear current.
There is no Home resync request in v1.1. The hub's next reset snapshot (or a
normal reconnect) repairs Home without disrupting sessions, cards or OTA.
Senders still obey the 4096-byte envelope ceiling. To isolate a violating
sender, the device may inspect complete text envelopes up to the legacy 16 KiB
transport bound and reject an identifiable oversized Home event or outstanding
Home reply locally. Invalid JSON, excessive nesting, unrouteable envelopes,
binary size violations and frames exceeding that transport bound still close
the link. This exception does not admit oversized Home data into the model.

The device uses only its existing pinned-TLS hub link. HomeRelay's URL, UUIDs,
services, characteristic metadata and bearer token stay on the hub. The token
is read from `<hub-data>/home/homerelay.token` (mode 0600), never logged, printed
or sent to the device. Relay discovery is `HOMERELAY_URL`, otherwise Bonjour
`_homerelay._tcp` with its `api` TXT path, otherwise
`http://127.0.0.1:18796/api/v1`. The hub owns one `/events` SSE stream for all
devices; comments count as keepalives, 45 seconds of silence triggers reconnect,
and reconnect uses bounded backoff and a fresh snapshot to repair missed events.

| Name | Kind / direction | Params → result |
| --- | --- | --- |
| `home.snapshot` | evt hub → device | `{rooms:[HomeRoom,…],accessories:[HomeAccessory,…],scenes:[HomeScene,…],available:bool,reset:bool,more:bool}` |
| `home.update` | evt hub → device | `{id,state?:HomeState,reachable?:bool}`; partial upsert of an existing accessory |
| `home.set` | req device → hub | `{id,values:{writableKey:value,…}}` → `{id,state:HomeState}` |
| `home.toggle` | req device → hub | `{id}` → `{id,state:HomeState}` |
| `home.scene` | req device → hub | `{id}` → `{id,accepted:true}` |

`hello` automatically subscribes to Home after its response. A snapshot starts
with `reset:true`; continuation chunks have `reset:false`, only the final chunk
has `more:false`, and **every chunk contains all three arrays** (possibly empty)
and the same `available`. Empty state still sends one chunk. Concatenate each
array across chunks and commit the complete replacement only after the last
chunk. Rooms precede accessories that reference them. Changes to membership,
metadata, favourites or availability send a fresh snapshot; accessory changes
between snapshots use `home.update`. Snapshot chunks and subsequent updates
are ordered through the device's single bounded delivery worker (64 queued
items across Home, sessions, cards and voice). Saturation disconnects the link
so reconnect repairs state; it never silently loses changes while showing
apparently current state. Disconnect discards an incomplete snapshot.

**Compact model.** All names are at most 32 UTF-8 bytes, clipped safely on a
character boundary; aliases are nonempty ASCII IDs of at most 16 bytes. The hub
assigns type-prefixed deterministic aliases from stable HomeKit IDs (`a…`
accessory, `r…` room, `s…` scene), stable across restarts and renames. A collision
fails closed rather than controlling another object. Room identity includes its
home so identically named rooms in different homes remain distinct.

- `HomeRoom` is `{id,name}`.
- `HomeScene` is `{id,name,favourite:bool}`.
- `HomeAccessory` is `{id,name,room,kind,reachable,state,writable,favourite}`;
  `room` is a room alias, `reachable` and `favourite` are booleans, `writable` is
  an array of supported HomeState keys that HomeRelay reports writable, and
  `kind` is `light|switch|outlet|fan|thermostat|lock|sensor|doorbell|button|cover|other`.
- `HomeState` contains only reported values from
  `{on,brightness,hue,sat,ct,temp,target,mode,humidity,motion,contact,locked,speed,position,battery,low_battery}`.
  Missing means unknown/unsupported, never an invented zero or false. `on`,
  `motion`, `contact`, `locked` and `low_battery` are booleans (`contact:true`
  means open). `brightness`, `sat`, `humidity`, `speed`, `position` and `battery`
  are numbers from 0 through 100; `hue` is 0 through 360; `ct` is colour
  temperature in mireds (50–1000); `temp` and `target` are Celsius (−100–100);
  `mode` is integer `0` off, `1` heat, `2` cool, `3` auto. Numbers must be finite;
  booleans are not accepted as numbers. An unknown or transitional lock state
  is omitted, never guessed from its target. Updatable writable fields are
  `on,brightness,hue,sat,ct,target,mode,locked,speed,position`; sensor readings
  are always read-only. HomeRelay's actual min/max/step normalization remains
  authoritative and its accepted response is what the hub returns.

The complete device model contains at most **64 accessories, 16 rooms and 32
scenes**. Favourites are selected first, then remaining objects in stable alias
order within these limits; rooms without selected accessories are omitted.
`home.update` merges partial state with existing state, latest value per field
wins. Updates coalesce to at most four events/second/accessory across state and
reachability. It is never a replace-all state operation. No custom/vendor values,
camera feed or video reach the device.

**Filtering and favourites.** Bridges and cameras are always hidden, as are
`custom.*` state values and scenes whose name matches `Trigger<digits>`.
Accessories in `Admin` and `Server Room` are excluded unless explicitly pinned
by that device's owner. Persist favourites separately for every paired device,
using stable IDs rather than display names. With no override, favourites are
lights and switches in Living Room, Bedroom, Kitchen and Office; every
thermostat, lock and fan outside excluded rooms; and scenes Good morning,
Evening, Good Night, I'm leaving and I'm home (case-insensitive names).
Owner CLI `orb-hub home favourites list|add|remove` adjusts a device's saved
preferences and refreshes all its links; no device favourite-setting request is
introduced in this round. Explicit pinning does not reveal always-hidden objects.

**Writes and errors.** Validate requests strictly: unknown fields, empty `values`,
unknown state keys, incorrect types or out-of-range values get `bad_request`.
Only aliases included in that device's current model may be controlled; unknown,
hidden or no-longer-visible IDs get `not_found`. A known read-only field or
unsupported toggle gets `unsupported`. An unreachable accessory or offline
relay gets `unsupported`, and transport deadlines get `timeout`. Relay auth
failure gets `unauthorized`, missing objects `not_found`, contention/rate limits
`busy`, rejected state conflicts `conflict`, and other provider failures
`internal`; all messages are generic, with no raw response, URL, path or token.
No relay writes are retried automatically, since a toggle or scene may already
have executed. A timeout/failure must never be reported as accepted.

`home.set` and `home.toggle` return the compact accepted state from the completed
HomeRelay write response, never merely echo a requested value or treat an SSE
notification as confirmation. Reply `state` is the complete reported accepted
state, including an empty object when no values are known; only `home.update`
is a partial merge. Device events received after a pending write remain
authoritative over that write's reply. The device may render an optimistic value only
for a reachable accessory while Home is available; on error it restores the last
confirmed value and displays a brief failure message. `home.scene` acknowledges
only after HomeRelay reports completion; `accepted:true` confirms that report,
not that every physical accessory reached its final state. Follow-up events
remain authoritative. Offline snapshots have `available:false`, retain known
objects with `reachable:false` (empty on cold start), and disable all writes.
Recovery sends a fresh available snapshot before updates resume.

Locks never support `home.toggle`: use explicit `home.set {id,values:{locked:true}}`
to lock. `locked:false` may be sent **only after a deliberate one-second hold**
in the lock detail screen, never from list toggles or scene capsules. That hold
is a device interaction requirement; the hub cannot prove a physical gesture
from the wire. HomeRelay's scene API does not expose scene actions, so the hub
cannot certify whether an arbitrary scene changes a lock. Never present a scene
as an unlock affordance; scene execution still requires an explicit owner tap.

**Doorbell.** A doorbell press creates an existing SessionHub `attention` Card,
title `Front Door`, body containing the local time, high priority, no reply,
and an explicit Dismiss option. The existing 16-card admission/expiry and
first-answer rules apply. Delivery requires both `home` and `cards`; it is
independent of Claude/Codex/Dot source preferences. It carries no video. The
device plays its doorbell chime when showing this Home attention card (identified
by its `home-doorbell-` card ID prefix), respecting quiet hours/face-down settings.
No new Card field, sound command or HomeRelay credential is added.

## Proposal: Reminders (`reminder.set`)

### Voice-set reminder contract (PROPOSAL)

This section is an unimplemented proposal, not a negotiated wire capability.
Neither the merged hub nor firmware dispatches `reminder.set` or
`reminder.status`; the implemented Time voice flow is described under Voice.

When the user asks by voice (e.g. “remind me at 6:30 to take the second Orb to the office”), the hub parses the intent, converts relative or civil time to an absolute UTC unix epoch timestamp, and pushes the reminder to the device:

| Name | Kind | Direction | Params → result |
| --- | --- | --- | --- |
| `reminder.set` | request | hub → device | `{"title":"…","due_time":<unix_epoch_seconds>}` → `{"accepted":true,"id":<uint32>}` |
| `reminder.status` | event | device → hub | `{"id":<uint32>,"state":"due"\|"done"\|"snoozed","due_time":<unix_epoch_seconds>}` |

- `title`: UTF-8 string, at most 64 bytes, clipped safely on character boundaries.
- `due_time`: positive integer unix epoch timestamp in seconds.
- On arrival, the device stores the reminder in its local reminder store (retaining up to 8 active reminders in memory).
- When the device wall clock reaches `due_time`, the reminder triggers: the device sounds a notification cue (respecting quiet hours and silent mode) and presents the Reminder Due screen (`Reminder.dc.html`).
- Actions on the device:
  - **In 1 hour** (Snooze): advances `due_time` by 3600 seconds, resets due status, and returns to the previous screen.
  - **Done**: marks the reminder as completed (`done = true`) and dismisses the due screen.
- If the device wall clock is not yet valid/set, `reminder.set` fails with `{"ok":false,"e":{"code":"not_ready","msg":"clock_not_set"}}`.

## Hub speech synthesis and Now Playing (additive v1.1)

The merged hub implements Mac speech synthesis and Now Playing observation/control
behind independent `--speech` and `--nowplaying` flags, both off by default.
Spoken replies use negotiated `voice` plus the device's top-level `speech`
descriptor above. Trunk firmware does not send a separate `speech` entry in
`hello.caps`; the hub also accepts/echoes that legacy entry when its Mac speech
service is available, but the entry alone never enables PCM playback. Now Playing
uses its own `nowplaying` capability, echoed only when the service is enabled
and available. Trunk firmware implements both device paths.

### 1. Spoken Replies (`speech`)

Spoken replies synthesise text from Dot or agent conversation replies using the Mac's built-in voices (`say`) and convert to raw mono PCM16 chunks (`afconvert`) fed to the device link.

#### Authoritative Wire Framing & Protocol
The device wire format, framing, and event contract are defined authoritatively in the **"Spoken replies (additive v1.1 device speech descriptor)"** section above:
- **Wire frames**: Binary WebSocket frames with type `0x02` (`0x02 | stream:u16be | seq:u32be | pcm_s16le[]`). Chunks are nonempty, even-sized, ≤1024 bytes, paced in real time.
- **Protocol events**: `voice.speak` (stream start with caption `text`), `voice.speak.end` (stream termination with accepted chunk count), `voice.speak.stop` (hub-initiated stop/cancel), and `voice.speak.stopped` (device-side termination report with reason).
- **Codecs & rates**: Linear signed 16-bit little-endian mono PCM (`pcm_s16le`) at 16000 Hz or 24000 Hz as negotiated by the device's hello descriptor. Alternate 8-byte-header binary framing and ADPCM codec proposals are retired in favour of this authoritative specification.

#### Hub Synthesis Contract
- **Text sanitization**: Reply text is limited to 400 characters, safely truncated on sentence or word boundaries. Fenced code blocks, inline code markers, markdown links, images, headers, formatting, blockquotes, and HTML tags are stripped before synthesis. Sanitized text is sent in the `voice.speak` event's `text` field for on-screen captions.
- **Voice selection**: Defaults to `'Ava (Premium)'` if installed; deterministic fallback to preferred natural English voices from `say -v ?` (Ava Enhanced/standard, Samantha Premium/Enhanced/standard, Allison, Siri, Daniel, Oliver, Alex, Victoria, Fred, or alphabetical English).
- **Subprocesses & cache**: Subprocesses (`say`, `afconvert`) run asynchronously via asyncio subprocesses without blocking the event loop. Synthesised PCM is cached by `(text_hash, voice, rate)` for 10 minutes in a bounded cache (max 64 entries).
- **Trigger policy**: Enabled only when started with `--speech` (default off). Spoken only when the device made an explicit voice request (`voice.result` flow / Dot ask) and the corresponding agent or Dot answer arrives. Spoken to that requesting device only, once. Never speaks unrequested agent output. Local stops (`voice.speak.stopped`) cancel streaming and drop remaining audio. A new voice request from the device preempts and stops any previous in-flight stream.

### 2. Now Playing (`nowplaying`)

The `nowplaying` capability enables glanceable media display and remote transport/volume control on Orb without installing third-party software on macOS.

#### Active Source Detection
Sources are checked in priority order using running application checks first, ensuring apps are never launched merely by polling. A playing source or a nonempty track title wins, including a paused track:
1. **Apple Music**: Checked via AppleScript `if application "Music" is running then …`
2. **Spotify**: Checked via AppleScript `if application "Spotify" is running then …`
3. **`nowplaying-cli`**: Invoked if present on macOS `PATH` when neither Music nor Spotify returns a playing track or nonempty title.
4. **Idle**: If no source provides a track, the running Music/Spotify state is retained when available; otherwise an idle snapshot with current master output volume is returned.

#### Subscriptions & Push Events
When a link negotiates `"nowplaying"` in `hello.caps`, it is automatically subscribed.
The hub polls playback state every 2.0 seconds while subscribers are active.
Whenever playback state, track metadata, or volume changes (and immediately upon subscription), the hub pushes:

**`nowplaying.snapshot`** (evt hub → device)
```json
{
  "title": "Song Title",
  "artist": "Artist Name",
  "album": "Album Name",
  "duration": 215.5,
  "position": 42.0,
  "playing": true,
  "state": "playing",
  "app": "Music",
  "volume": 65
}
```
- `title`, `artist`, `album`: Strings (empty if none loaded).
- `duration`, `position`: Float seconds, rounded to 2 decimal places.
- `playing`: Boolean (`true` if playing, `false` if paused or stopped).
- `state`: `"playing"` | `"paused"` | `"stopped"`.
- `app`: Source application name (`"Music"`, `"Spotify"`, `"NowPlaying"`, or `""`).
- `volume`: Current macOS master output volume (`0–100`).

#### Requests & Controls

| Name | Kind / direction | Params → result |
| --- | --- | --- |
| `nowplaying.snapshot` | req device → hub | `{}` → `NowPlayingSnapshot` object |
| `nowplaying.control` | req device → hub | `{action, value?}` → `{"accepted": true, "action": "<action>", "snapshot": NowPlayingSnapshot}`; volume also returns `value` |

**Actions for `nowplaying.control`**:
- `"play"`: Resume playback in the active source app.
- `"pause"`: Pause playback in the active source app.
- `"playpause"`: Toggle playback in the active source app.
- `"next"`: Skip to next track.
- `"previous"`: Return to previous track.
- `"volume"`: Set Mac master output volume to `value` (number clamped to integer `[0, 100]`).

#### Errors
- `bad_request`: Unknown action, missing action, or non-numeric volume value.
- `unsupported`: Neither `nowplaying` capability was negotiated, or no active media player is available to handle transport commands.
- `internal`: An uncaught subprocess/OS exception. Nonzero AppleScript exit
  codes are logged at debug level and treated as an empty observation; they do
  not necessarily fail a control request. `accepted:true` reports the hub's
  control attempt and accompanying snapshot, without a player execution receipt.

