# orb_link

The device's one connection to the hub, as specified in [docs/protocol.md](../../../docs/protocol.md) ("Identity and TLS", "Pairing", "Link"). It finds the hub with `orb_net`, pairs once (showing the 6-digit code), then keeps a pinned-TLS WebSocket open. Other components register handlers for the methods and events they own. The API is [include/orb_link.h](include/orb_link.h).

## How it works

A single task (`orb_link`, core 0, priority 4, 8 KB stack) owns discovery, the certificate fetch, the WebSocket, the request table, NVS and every callback. Other tasks only post to its queue. The WebSocket client's own task (core 0, priority 5, 6 KB stack) only forwards connection changes and whole text messages.

Connection state lives outside the bounded payload queue. WebSocket callbacks never wait on that queue: a full queue cannot lose a disconnect. A normal close drains already accepted text first (including `pair.done`); a lost valid-size frame from queue pressure or receive allocation failure instead forces reconnect, so the app cannot silently continue with incomplete state. Old connection callbacks cannot affect a new connection. Legacy oversized/deep messages retain their documented drop behavior. With negotiated cards, envelopes are capped at 4096 bytes; oversize receive frames close with 1009 and force a fresh snapshot.


```
OFFLINE ──orb_net CONNECTED──▶ SEARCHING ──paired──▶ CONNECTING ──hello ok──▶ ONLINE
                                  │                       │ unauthorized            │ drop
                                  │ unpaired              ▼                         ▼
                                  └──pair.begin ok──▶ PAIRING ◀── UNAUTHORIZED    SEARCHING (backoff)
```

- **Discovery.** Every attempt browses `_orb-hub._tcp` for 3 s. A paired device takes the hub whose TXT `fp` matches its pinned certificate, and otherwise the last address that worked. An unpaired device takes the first hub that answers.
- **Pairing.** The device GETs `http://<hub>:<pair>/v1/hub-cert` and checks that the PEM holds exactly one certificate and matches the advertised `fp`. It keeps that certificate alone, re-encoded, so what esp-tls trusts is exactly what the code hashes, and opens `wss://<hub>:<port>/v1/link` trusting only it. It picks 16 random bytes (`esp_fill_random`, with Wi-Fi on) and sends `pair.begin` with their SHA-256 (`commit_d`). Once the hub has answered with its nonce, it reveals them in `pair.reveal`, and when that's answered shows the code from [link_sas.c](link_sas.c) as `status.sas` in state `PAIRING`. The order matters: the hub picks its nonce before it learns the device's, so a man in the middle can't choose one that makes the codes agree. On `pair.done` it stores the pairing and reconnects with `hello`. On `pair.failed` "expired", or after 5 min 10 s with no answer, it starts over a second later with new nonces. "rejected" goes through the backoff.
- **TLS.** The pinned certificate is the only trust anchor; no certificate bundle is used and no verification is skipped. A PEM with more than one certificate is refused, while pairing and when loaded from NVS: esp-tls would trust them all, and the pairing code and `fp` cover only one. The certificate's name is checked against the hub's SRV host from mDNS, which the hub keeps at the `orb-hub-*.local` name in its certificate even after the Mac is renamed ([protocol](../../../docs/protocol.md), "Discovery"). That survives a new DHCP lease too, where the IPs in the certificate go stale. Only when the host name isn't known is the IP checked.
- **Link.** Text frames are JSON envelopes ([link_proto.c](link_proto.c)) of at most 16 KB in either direction. Larger outgoing messages are refused, and larger incoming ones are dropped. So are incoming ones nested more than 32 levels deep, before cJSON sees them: it recurses per level (80 bytes of stack each on the S3), and its own limit of 1000 levels would take about 80 KB. The client pings every 15 s and drops the link after 45 s without a pong. Reconnects wait 1, 2, 4 … 30 s plus up to 20 % jitter ([link_backoff.c](link_backoff.c)), starting again from 1 s after any connection the hub answered.
- **Unauthorized.** If `hello` gets `unauthorized`, the device forgets the token and certificate, shows `UNAUTHORIZED` for about a second, and pairs again.
- **state.report** goes out every 60 s while online. It also goes out within 5 s of a change in the power reading (polled every 5 s).

## API notes

Protocol v1.1 is built into the link owner. Initialize `orb_model` before
starting the link. `hello` advertises cards and, when the voice worker has
successfully started, voice. A missing/empty capability intersection keeps v1
pairing, OTA and methods working. The link explicitly re-subscribes after each
hello. `link_v11.c` validates byte limits, UTF-8, enum values, timestamps,
duplicate identities, approval scope and snapshot ordering before publication.
Chunked sessions replace state atomically; live cards replay after completion.
Malformed model events or capacity loss reconnect instead of leaving a
plausibly current partial state. Pending answers survive resync.

Call `orb_link_memory_init()` once at the start of `app_main`, before starting
OTA, voice or link workers. It installs immutable process-wide cJSON hooks:
nodes, keys, values and printed envelopes use `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`
and fail on PSRAM exhaustion rather than borrowing internal RAM. The audited
cJSON users are link, OTA, voice and main's link callbacks, all ordinary task
context; none runs from an ISR or while flash/cache is disabled. Hooks must never
be replaced while any tree or printed buffer exists. Free trees with
`cJSON_Delete` and printed buffers with `cJSON_free` as before.

Incoming text uses fixed startup-allocated PSRAM slots: sixteen 4,097-byte
slots and two 16,385-byte legacy/Home-classification slots. Fragment growth
within a slot and migration to a large slot allocate nothing. Saturation marks
a lost frame and reconnects rather than silently dropping state. The decoder
uses a separate 192 KiB PSRAM arena for envelopes up to 4,096 bytes. cJSON hooks
select it only on the link owner while decoding; callbacks and outgoing builders
retain independent ownership. A parsed tree stays valid through synchronous
dispatch and is discarded before the next parse. Arena exhaustion fails closed.
The 294,930-byte retained receive/parse workspace, encoded TX envelopes, queued
audio and private pairing PEM buffers share the 384 KiB `network` owner budget.
No path falls back to internal RAM. Audio frames remain at most 1,031 bytes;
the shared queue has 32 entries, so all individual maxima cannot accumulate.

The public `orb_link_hub_https().cert_pem` copy keeps its documented caller
`free()` boundary and is excluded from tracked ownership. The managed
WebSocket client's public API has no
buffer allocator/capability option: its 4096-byte RX and TX allocations exceed
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`, so IDF prefers PSRAM but can fall back
to internal RAM. No managed component patch or global malloc policy change is
needed. Those 8,192 payload bytes, managed-client metadata and mbedTLS are
included in heap residual rather than charged twice to app buffers. mbedTLS
uses strict PSRAM through `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.
RTOS task stacks, queue metadata, display DMA and audio DMA keep their existing
internal requirements.

Snapshot processing is incremental by wire chunk. A focus snapshot contains
at most eight sessions; only final `more:false` publishes the complete model.
The older local/demo model retains its 24-session compatibility. Typed scratch
for session snapshots remains separately owned; `agent.stream` and
`usage.snapshot` mapping and retained logs allocate nothing per message.
Host allocation counters cover receive-slot saturation, fragment migration,
dense 4 KiB JSON, and repeated arena parsing without allocator calls. Separate
fault injection still verifies the legacy allocating parser's PSRAM routing.
These tests establish allocation behavior, not a board memory or timing result.

Cards subscribers receive live session replacements. The mapper validates all
bounded fields, UTF-8, enum values and sequences before publishing. Replays are
idempotent. A sequence gap requests `sessions.subscribe` in-band; a subsequent
snapshot establishes fresh baselines without cancelling voice capture. The
`usage` capability is advertised independently and gates usage snapshots.

The model pump runs at most every 50 ms, claims one immutable answer at a time,
and retries timeout/busy/internal results after one second. The original card
ID and content stay fixed until the hub returns its recorded first answer or
a terminal rejection. The recorded decision can differ from the local tap;
`snapshot.last_answer` reports both acceptance and provider transport delivery.
`delivered:false` is final and does not reissue a decision. Ordinary messages
are sent once and retain completion/error text in `snapshot.last_message`;
ambiguous message timeouts never automatically start a second turn. Confirmed
stream messages carry their source generation and fail locally after reconnect.

`orb_link_request_on()` and `orb_link_send_audio_on()` require an explicit
connection generation. Admission captures online state and generation together;
old queued work cannot migrate onto a new connection. Audio copies use PSRAM,
at most 1031 bytes each within the shared 32-item FIFO. Queue saturation fails
the recording. Accepted PCM stays ahead of `voice.end`/`voice.cancel`. Request
deadlines include queue wait and bound the transport send wait as well.

- Callbacks (state, events, methods, replies, hello and power providers) run on the link task. They must not block or call LVGL; publish model/mailbox state for the UI task. cJSON passed to them is valid only during the call.
- **Methods.** `device.ping` and `device.reboot` are built in. `device.reboot` answers `{}`, closes the link cleanly so the answer is on the wire, then restarts. **`device.identify` and `device.config` belong to the app**: register them with `orb_link_on_method`, otherwise the hub gets `unknown_method`. Registering a name again replaces its handler, built-ins included. A handler's error maps to `bad_request` (`ESP_ERR_INVALID_ARG`), `not_found` (`ESP_ERR_NOT_FOUND`), `busy` (`ESP_ERR_INVALID_STATE`) or `internal` (anything else), with `esp_err_to_name()` as `msg`. Requests from the hub before `hello` is accepted get `busy`.
- **Events.** Several handlers may share an event name. Events with no handler are ignored. Hub events reach handlers only while `ONLINE`. `pair.done` and `pair.failed` are handled internally.
- **`orb_link_request`.** The reply callback gets `ESP_OK` with the result, or `ESP_ERR_TIMEOUT` (no answer in time, or the link dropped first). It gets `ESP_FAIL` with `{"code","msg"}` for an error from the hub, and also for a local `busy` (16 requests already waiting) or `internal` (the request is over 16 KB). `orb_link_request` and `orb_link_notify` never block. They return `ESP_ERR_INVALID_STATE` when not online and `ESP_ERR_NO_MEM` when the queue (32) is full, and they free `params` in both cases. A request or event that was queued on a connection which then drops is never sent on the next one.
- **Power.** `orb_link_set_power_source()` names a callback that fills `orb_link_power_t`. Its reading goes into `hello` and `state.report`, which leave `power` out while no source is set or when the callback fails.
- **`orb_link_hub_https()`** copies the base URL, the `Authorization` header and the pinned PEM (which the caller frees), all from one pairing, so they stay usable whatever happens to the pairing afterwards (`orb_link_unpair()`, an `unauthorized` hello). Give the PEM to esp-tls as the only trusted certificate and set `skip_cert_common_name_check`: the URL holds the hub's last IP, which the certificate may not name, and the pin alone identifies the hub.
- **`orb_link_unpair()`** works before or after start. If a connection is open, the pairing is forgotten when that connection closes.
- `orb_link_status()` and `orb_link_online()` can be called from any task.

## What `hello` and `pair.begin` say

| Field | Source |
| --- | --- |
| `device_id` | `orb_net_device_id()` |
| `name` (pair.begin) | `Orb`; the hub's name for the device arrives in `hello`'s result |
| `model` | `waveshare-s3-amoled-1.75c` |
| `fw` | `esp_app_desc_t.version` (git-derived by default; invocation-only override through `scripts/idf.sh`, see root README) |
| `build` | first 7 hex digits of the app's ELF SHA-256 (`esp_app_get_elf_sha256`), which the hub can read from the image's app descriptor too |
| `idf` | `esp_app_desc_t.idf_ver` |
| `boot_slot` | label of the running partition |
| `reset_reason` | `esp_reset_reason()` as `poweron`, `sw`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `usb`, `jtag`, … or `unknown` |
| `power`, `rssi` | the power source; `orb_net_status().rssi` |
| `last_ota` and the rest | `orb_link_on_hello` providers |

From `hello`'s result, `tz` goes to `orb_net_set_timezone()`. `time_ms` sets the clock if SNTP hasn't yet. `hub_name`, `device.name` and `device.channel` go into the status.

## NVS (namespace `orb_link`)

| Key | Type | |
| --- | --- | --- |
| `device_id` | str | stored with the pairing, as step 7 asks |
| `token` | str | 64 hex digits; never logged |
| `hub_pem` | blob | the pinned certificate alone, re-encoded as PEM, NUL-terminated |
| `hub_ip`, `hub_host`, `hub_port` | str, str, u16 | last address that worked |

Unpairing erases the namespace. A stored PEM that isn't exactly one certificate counts as unpaired.

## Required sdkconfig

None beyond the board defaults in `firmware/sdkconfig.defaults`. The parts it relies on are ESP-IDF defaults: mbedTLS with X.509 parsing and PEM reading and writing (`CONFIG_MBEDTLS_PEM_PARSE_C`, `CONFIG_MBEDTLS_PEM_WRITE_C`), PSA SHA-256 and ECDSA P-256, `esp_http_client`, and NVS. Recommended for the firmware:

```
# TLS buffers (~40 KB per connection) in PSRAM; internal RAM goes to the display bands
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
```

## Integration

1. `firmware/main/CMakeLists.txt`: add `orb_link` to `PRIV_REQUIRES`. cJSON comes with it, because `orb_link.h` includes `cJSON.h`.
2. In `app_main`, after `nvs_flash_init()`, `esp_event_loop_create_default()` and `orb_net_start()`:
   ```c
   static esp_err_t link_power(orb_link_power_t *out, void *ctx)
   {
       orb_power_t p;
       ESP_RETURN_ON_ERROR(orb_board_power(&p), "main", "power");
       *out = (orb_link_power_t){ .battery_pct = p.battery_pct, .battery_mv = -1,
                                  .display_state = ORB_LINK_DISPLAY_ACTIVE,
                                  .charging = p.charging, .usb = p.usb };
       return ESP_OK;
   }

   orb_link_set_power_source(link_power, NULL);
   orb_link_on_method("device.identify", on_identify, NULL);   /* show name and code for 10 s */
   orb_link_on_method("device.config", on_config, NULL);       /* name, channel, brightness */
   orb_link_start(on_link_state, NULL);                         /* UI: show status->sas while PAIRING */
   orb_ota_start(...);                                          /* after orb_link_start */
   ```
   Call `orb_ota_mark_healthy()` once the state callback reports `ORB_LINK_ONLINE` with the UI up.

   `orb_ota` takes its download settings from `orb_link_hub_https(orb_link_https_t *)`: `base_url` plus the offer's path for the URL, `cert_pem` with `skip_cert_common_name_check`, and `authorization` for the header. It frees `cert_pem` once `esp_https_ota_finish()` or `_abort()` has returned, since the HTTP client keeps the pointer until then.
3. The firmware's `dependencies.lock` gains `espressif/cjson` and `espressif/esp_websocket_client` on the first build.
4. `orb_link` needs the real `orb_net` component, so the firmware build only resolves once `orb_net` has its `CMakeLists.txt`.

## Tests

- Host tests cover the ESP-free parts: envelopes and message bodies against the spec's shapes, the nesting limit, the pairing commitment and code against the spec's test vector (SHA-256 from CommonCrypto), and the backoff. They need the cJSON source, which a test_app reconfigure or build downloads.
  ```bash
  make -C firmware/components/orb_link/tests/host
  ```
- `tests/e2e/` runs the real `orb_link.c` on the Mac (ESP-IDF's linux target) against `hub_stub.py`, a stand-in hub on 127.0.0.1 with a throwaway certificate. It covers:
  - pairing: a PEM with a second certificate (refused before connecting), an expired attempt, then a confirmed one, each with `commit_d` checked against `pair.reveal`, and the device's code matching the hub's;
  - `hello`, over a link whose certificate names the host but not the address the device uses (a new DHCP lease);
  - `orb_link_hub_https()`: the address, the token, and the pinned certificate alone;
  - built-in and app methods, the error mapping, and events;
  - 10 KB and fragmented messages, and dropping a message over 16 KB or nested 900 levels deep;
  - replies and request timeouts;
  - a dropped link, and a hub presenting another certificate (refused);
  - `unauthorized`, after which the device pairs again;
  - `device.reboot`.

  It needs `websockets` and `cryptography`, which ESP-IDF's Python env has. Test-only shims stand in for `orb_net`, for `esp_ota_ops` and for `esp_timer_get_time`, which have no linux port.
  ```bash
  idf.py -C firmware/components/orb_link/tests/e2e --preview set-target linux build
  $IDF_PYTHON_ENV_PATH/bin/python firmware/components/orb_link/tests/e2e/hub_stub.py \
      firmware/components/orb_link/tests/e2e/build/orb_link_e2e.elf
  ```
- `test_app/` builds orb_link against a test-only `orb_net` stub (`test_app/components/orb_net`, no Wi-Fi) and calls every public function:
  ```bash
  . ~/esp/esp-idf/export.sh   # with IDF_PYTHON_ENV_PATH as in scripts/idf.sh
  idf.py -C firmware/components/orb_link/test_app set-target esp32s3 build
  ```

### Focus and per-device preferences

The v1.1 mapper follows the session-focus contract: atomic ordered snapshots
contain at most eight sessions, content upserts preserve their positions,
`session.remove` leaves focus only, and `sessions.summary` retains collapsed and
source-hidden counts. A missing `hello.r.prefs` uses the compatibility defaults
(all sources enabled, ten recent minutes); present preferences must be complete.

The link drains single-flight partial `prefs.set` patches from the model and
applies the complete returned preferences only on acknowledgment. Errors retain
the last confirmed value and permit an explicit user retry. It requests idle
sessions only while that model view is open, always with `include:"idle"` and
`limit:8`. Pages are bounded to eight sessions and an 80-byte opaque cursor;
null ends browsing, conflict restarts without a cursor, and closed-view replies
are discarded by generation. Malformed responses fail closed through reconnect.
`card.cancel` remains a presentation cancellation and never fabricates an answer.

## Radio power policy

The lead's service loop must call
`orb_net_set_power_hint(background && !voice_active && !ota_active ? ORB_NET_POWER_BACKGROUND : ORB_NET_POWER_INTERACTIVE)`
when panel/work policy changes. `background` means panel dark or Always On.
Alternatively hold the independent `ORB_NET_WORK_OTA`/`ORB_NET_WORK_VOICE` bits
with `orb_net_set_work_active()` for each complete work lifetime; provisioning
already overrides the hint inside `orb_net`. This lane supplies these APIs and
the link observer, but does not edit `main` or the voice/OTA owners. Until the
lead wires that policy, the default remains interactive. Do not infer voice or
OTA lifetime from transient transport messages.

The observer wakes the link queue without waiting for capacity. The effective
hint and the admitted prompt retry remain outside the queue, so a busy queue
cannot lose an interactive wake. Interactive hub reconnects keep the existing
1–30 s backoff plus jitter; background retries grow to a five-minute ceiling
including jitter. One background-to-interactive change during backoff admits
a prompt retry, at most once per 30 s. A successful hello resets the exponential
attempt count, while the wake retry rate limit persists across connections.

`power_hint` is negotiated independently. New hello fields are additive, and
an older hub leaves the link on 15 s pings. A negotiated peer receives an
immediate `state.report` on effective hint changes. Only after that report is
successfully written may background pings become 45 s; allocation/encoding or
write failure closes the link with the original cadence retained. The managed
ESP WebSocket client exposes `esp_websocket_client_set_ping_interval_sec()`;
its pong deadline is measured after a ping, so the local 45 s pong timeout can
stay unchanged. The hub permits three announced ping intervals of silence.

A bounded `power.batch` contains only event children. The complete batch is
validated before ordinary event dispatch, nested batches and request/response
shapes are rejected, and existing application validation remains in force.
Urgent card/voice/OTA paths stay outside coalescing on the hub.

`orb_link_counters()` copies a since-boot counter snapshot for devtools:
complete inbound JSON/binary application messages, successful outbound
messages, payload bytes, WebSocket connect attempts and reconnect attempts
(excluding the first). Counts omit TLS/WebSocket framing, control pings and
TCP retransmissions. `orb_net_get_counters()` supplies Wi-Fi scan/join counters.
These counters support real-board traffic checks; host tests establish timing
and protocol behavior, not radio current or battery life.
