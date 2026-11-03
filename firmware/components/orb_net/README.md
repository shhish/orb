# orb_net

Wi-Fi for Orb: BLE provisioning when there is no stored network, then a station that keeps itself connected, mDNS (finds hubs advertising `_orb-hub._tcp` on demand), SNTP, and IANA time zones. The contract is [`include/orb_net.h`](include/orb_net.h) and [docs/protocol.md](../../../docs/protocol.md) ("Discovery", "Wi-Fi setup").

## How it behaves

One task (`net`, core 0, priority 4, 4 KB internal stack) owns all state. ESP-IDF's event handlers only queue what happened, so state changes, reconnect timing and the app's callback run in order on that task. The callback gets a snapshot and must not block; take `lv_lock()` before touching the UI. The decisions themselves are plain logic in `orb_net_sm.c` (events in, new state and what to do out), tested on the Mac; `orb_net.c` feeds it ESP-IDF's events and carries out what it asks for.

| State | When | Fields |
| --- | --- | --- |
| `UNPROVISIONED` | No network stored and BLE provisioning isn't built in (sdkconfig below) or couldn't start; or just before a restart into provisioning | |
| `PROVISIONING` | Advertising over BLE, waiting for the hub's `orb-hub setup` | `service_name` (`Orb-CDEF`), `pop` (8 digits to show) |
| `CONNECTING` | Joining or rejoining (also while provisioning, once credentials arrived) | `ssid` |
| `CONNECTED` | Has an IPv4 address | `ssid`, `ip`, `rssi` |
| `FAILED` | Credentials refused: wrong password or no such network | `ssid`; while provisioning also `service_name` and `pop`, and new credentials can be sent at once |

- **Stored network.** Joins at boot if the network has had an address before (see below). A lost connection is rejoined at once, then after 1, 2, 4, 8, 16, 30, 30 … s plus up to 20 % jitter while interactive. Background retries, or eight consecutive `NO_AP_FOUND` family failures, continue growing to five minutes including jitter. Three refusals in a row (wrong password, network not found) show `FAILED`; any other kind of failure goes back to `CONNECTING`. Retries continue in either state; a successful address resets failure and absent-network streaks. An interactive transition advances a pending retry, at most once per 30 seconds; its cooldown survives a successful join.
- **No stored network.** BLE provisioning starts at once with a fresh PoP. If the client's credentials fail, the device shows `FAILED`, keeps advertising, and accepts new credentials in the same session. When the client has read "connected" (or 30 s after joining), provisioning stops.
- **Only a network that worked counts.** network_provisioning writes credentials to flash before it tries them. So `orb_net` stores `orb_net`/`verified` in NVS on the first address, clears it when new credentials arrive, and at boot joins a stored network only when it is set. A device that lost power while trying a mistyped password provisions again instead of retrying it forever with Bluetooth gone. The untried network stays stored until new credentials replace it, and if provisioning can't start (no BLE in the build, or no memory for it), the device tries it anyway.
- **Drops right after provisioning.** network_provisioning doesn't reset its attempt count when a join succeeds, so a link that drops a few times before provisioning stops ends in `NETWORK_PROV_WIFI_CRED_FAIL` too. Credentials that have had an address are kept: `orb_net` rejoins them itself (`CONNECTING`) instead of resetting, which would erase them. If provisioning ends with nothing stored (the client sent `prov-ctrl` reset after a success), the device shows `UNPROVISIONED` and restarts into provisioning.
- **Bluetooth memory.** Bluetooth is only for provisioning, so its static memory (controller data and BSS, and the ROM's BLE data area) is handed to the heap: at boot when a network is stored, and when provisioning ends. It can't be started again after that, so **`orb_net_forget()` erases the network and restarts the device**, which comes back provisioning. The state goes to `UNPROVISIONED` 0.5 s before the restart, so the UI can say so.
- **Time.** SNTP (`pool.ntp.org`) starts on the first IP address; `time_synced` turns true on the first sync. `orb_net_set_timezone("America/New_York")` maps the hub's IANA name to a POSIX TZ string from a built-in table (124 zones and aliases: North America, Europe, India, East and South-East Asia, Australia, New Zealand, Latin America, the Gulf, Africa), falls back to UTC for anything else, and stores the name in NVS (`orb_net`/`tz`) so local time is right from the next boot. Morocco is not in the table (its Ramadan changes have no POSIX form).
- **Hubs.** `orb_net_find_hubs()` (only while `CONNECTED`) initialises mDNS only for the duration of the query and frees it immediately after (`mdns_free()`), serialised with a mutex so concurrent queries cannot race. Freeing mDNS between queries stops its 100 ms periodic timer so the CPU can stay in automatic light sleep. It runs one mDNS PTR query for the whole timeout, keeps answers whose TXT has `v=1`, a valid `pair` port and a 16-hex-digit `fp` (lowercased), keeps one entry per hub name (a hub usually answers over IPv4 and IPv6), and resolves the IPv4 address with an A query if the answer didn't carry it. TXT keys are case-insensitive; a repeated key keeps its first value.
- **Names.** From the Wi-Fi station MAC `02:00:00:ab:cd:ef`: `orb_net_device_id()` is `orb-020000abcdef`, the BLE name `Orb-CDEF`, the DHCP host name `orb-cdef`. `orb_net_device_id()` works before `orb_net_start()`.
- **Secrets.** The Wi-Fi password lives only in ESP-IDF's Wi-Fi NVS; orb_net copies the SSID out of the provisioning event and never logs the password or the PoP. ESP-IDF's own logs need two fixes, which `orb_net` applies before provisioning starts: protocomm's `security1` hex-dumps the session key, the AES-CTR nonce and both public keys at INFO (with a capture of the BLE traffic, that log gives away the password), so the tag goes down to WARN; `network_prov_mgr` logs the SSID and a partly masked password at DEBUG, so it is held at INFO. Both only ever lower a level. This needs per-tag log levels (`CONFIG_LOG_DYNAMIC_LEVEL_CONTROL`, on by default; the build stops with `#error` otherwise), and nothing may call `esp_log_level_set("*", …)` at INFO or above while provisioning, which would undo it.

## Radio power hint

The lead's service owner calls `orb_net_set_power_hint(background ? ORB_NET_POWER_BACKGROUND : ORB_NET_POWER_INTERACTIVE)` when panel/Always On policy changes. `background` must be false during voice capture/playback or OTA. These setters are non-blocking and safe from any task, including before start; the net task alone applies `esp_wifi_set_ps`. INTERACTIVE selects `WIFI_PS_MIN_MODEM`; BACKGROUND selects `WIFI_PS_MAX_MODEM`. The default is INTERACTIVE. The hint does not change input ownership, redaction, navigation or sleep admission.

`orb_net_set_work_active(ORB_NET_WORK_OTA, true/false)` and the independent `ORB_NET_WORK_VOICE` bit let transfer owners retain MIN_MODEM through overlapping work. The link tracks its voice and OTA paths with these overrides; the lead must still supply the actual work state when choosing the main hint. Provisioning always overrides BACKGROUND automatically, preserving BLE coexistence. `orb_net_get_power_hint()` returns this **effective** hint rather than the requested panel hint. A single link-owned `orb_net_on_power_hint` observer only wakes the link waiter; it runs outside locks on the changing task, must never block, and reads the authoritative getter after waking. Queue saturation cannot lose the effective hint. `orb_net_interactive_generation()` also retains every intervening interactive transition, so a quick BACKGROUND → INTERACTIVE → BACKGROUND while either task is asleep still admits one rate-limited prompt retry.

The station config explicitly sets `listen_interval=3` before each owned association, and the provisioning manager's synchronous `NETWORK_PROV_SET_WIFI_STA_CONFIG` callback sets it before the manager installs credentials and connects. A hint change does not reconnect an associated station.

Verified against this Mac's ESP-IDF 6.1 checkout (`scripts/idf.sh` defaults to `~/esp/esp-idf`):

- `components/esp_wifi/include/esp_wifi_types_generic.h:386-387,570`: MIN wakes every DTIM; MAX uses `wifi_sta_config_t.listen_interval`, in AP beacon intervals; zero defaults to three.
- `docs/en/api-guides/wifi-driver/wifi-performance-and-power-save.rst:1549-1561`: MIN follows DTIM, MAX follows listen interval; set the station config **before connecting**. Thus the association advertises the interval in advance, while switching MIN/MAX need not reconnect. The driver implementation is distributed in binary libraries, so the local source establishes the API contract and setup timing, not an inspected internal latch implementation.
- `firmware/managed_components/espressif__network_provisioning/src/manager.c:1547,1577,1596`: the synchronous config callback runs before `esp_wifi_set_config`, followed by a one-second delayed connect. No managed component changes are needed.
- `docs/en/api-guides/wifi-driver/station-scenarios.rst:355`: connect scans do not emit `SCAN_DONE`. `orb_net_get_counters()` therefore counts owned join scan **attempts** plus explicit scan-done events (e.g. provisioning scans), and successful new-address events while previously disconnected, after the first address. DHCP renewals reported while already CONNECTED do not increment reconnects. The scan counter is diagnostic, not a count of RF sweeps; provisioning-manager internal retry scans and blocked scans are not observable through those hooks. None of these counters measures current or battery life.

## Provisioning client (for the hub)

What `orb-hub setup` must do to provision an Orb.

| | |
| --- | --- |
| Library | ESP-IDF network provisioning: `espressif/network_provisioning` 1.4.0 on the device (`netprov-v1.2` in `proto-ver`), protocomm from ESP-IDF 6.1 |
| Transport | BLE GATT; the device is the peripheral, one connection at a time, no BLE pairing or bonding (the session is encrypted by protocomm) |
| Advertised name | `Orb-XXXX`: last 2 bytes of the Wi-Fi station MAC, uppercase hex (`Orb-CDEF` for this board), in the scan response |
| Service UUID | `1775244d-6b43-439b-877c-060f2d9bed07` (network_provisioning's default), complete 128-bit list in the advertising data |
| Endpoints | One characteristic each, UUID = the service UUID with bytes 12–13 set to the endpoint's 16-bit id, so `1775ff4f-…` `prov-ctrl`, `1775ff50-…` `prov-scan`, `1775ff51-…` `prov-session`, `1775ff52-…` `prov-config`, `1775ff53-…` `proto-ver`. Each has a 0x2901 user description holding its name; clients should map by that name |
| `proto-ver` | Plain JSON: `{"prov":{"ver":"netprov-v1.2","sec_ver":1,"sec_patch_ver":…,"cap":["wifi_prov","wifi_scan"]}}`; check `sec_ver` is 1 |
| Security | Version 1: X25519 key exchange, proof of possession, AES-256-CTR. PoP = the 8 digits on the device's screen (ASCII, e.g. `04718263`); a new one on every boot into provisioning. It keeps out passive listeners, not an active one posing as the device (protocol.md, "Wi-Fi setup") |
| Wi-Fi | `prov-scan` lists networks as the device sees them; `prov-config` set → apply → poll get-status. Status `connecting` while it tries (3 joins), then `connected` with the IP, or `disconnected` with `AuthError` / `WifiNetworkNotFound`. After a failure the device is ready for new credentials in the same session; `prov-ctrl` reset is accepted but not needed. A `disconnected` after `connected` means the link dropped, not that the password is wrong (the reason may be stale): the device keeps that network and rejoins, so stop at the first `connected` and don't send `prov-ctrl` reset after it |
| End | BLE stops about 1 s after the client reads `connected` (or 30 s after joining if no one asks); the device then finds the hub over mDNS and pairing starts (protocol.md, "Pairing") |

**Tool.** Use the `esp_prov` that ships inside network_provisioning 1.4.0, `tool/esp_prov/` (after any firmware or test_app build it is at `firmware/managed_components/espressif__network_provisioning/tool/esp_prov/`; otherwise from the component registry archive or idf-extra-components at `c492b279`). ESP-IDF 6.1 no longer has `tools/esp_prov`; the older one from IDF 5.x (for `wifi_provisioning`) isn't tested against this firmware. It needs:
- `IDF_PATH` pointing at an ESP-IDF 6.1 tree, for protocomm's generated protobuf modules (`$IDF_PATH/components/protocomm/python/*_pb2.py`);
- the component's own `python/` directory next to `tool/` (found by relative path, so copy the whole component, not just `tool/`);
- Python packages `bleak`, `protobuf` and `cryptography` (not installed in the ESP-IDF Python env on the Mac mini; the hub's venv needs them).

Equivalent command line, for a manual test:

```bash
IDF_PATH=~/esp/esp-idf python esp_prov.py --transport ble --service_name Orb-CDEF --sec_ver 1 --ssid "<ssid>"
```

It asks for the PoP and then the passphrase with `getpass`. Don't pass `--pop` or `--passphrase` on the command line: they show up in `ps`. The hub should import the tool's modules (`transport.Transport_BLE`, `security.Security1`, `prov.config_set_config_request` …) and feed the password from its own `getpass`, as protocol.md says.

## Required sdkconfig

The main firmware's `sdkconfig.defaults` needs these lines (they are also in `test_app/sdkconfig.defaults`). Without the first group the component still builds, but a device with no stored network stays `UNPROVISIONED` and logs why. Logging must keep per-tag levels, as ESP-IDF does by default (`CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=y` and a tag-level implementation other than `CONFIG_LOG_TAG_LEVEL_IMPL_NONE`); see "Secrets" above.

```
# Bluetooth LE, only for Wi-Fi provisioning
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y
# CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2 is not set
# Internal RAM: Bluetooth
CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set
# CONFIG_BT_NIMBLE_ROLE_OBSERVER is not set
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
# Internal RAM: Wi-Fi, lwIP, mbedTLS (Espressif's "memory saving" rank for S3 with PSRAM)
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
# CONFIG_ESP_WIFI_IRAM_OPT is not set
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6
CONFIG_ESP_WIFI_RX_BA_WIN=12
# CONFIG_ESP_WIFI_SOFTAP_SUPPORT is not set
# CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT is not set
# Internal RAM: mDNS
CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y
CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y
# CONFIG_MDNS_ENABLE_CONSOLE_CLI is not set
# CONFIG_MDNS_ENABLE_BROWSE is not set
# CONFIG_MDNS_PREDEF_NETIF_AP is not set
```

Why each group:
- **Security 1** is what protocol.md specifies; ESP-IDF 6.1 turns it off by default (it recommends security 2), so it must be enabled. Security 2 is turned off to save flash. `orb_net.c` stops the build with a clear `#error` if Bluetooth is on without security 1.
- **Bluetooth.** The controller's code moves from ROM/IRAM to flash (so to PSRAM, like the rest of the app): 19.4 KB less static internal RAM. Espressif warns BLE can stutter during flash erases in this mode, which only matters during the one setup session. NimBLE's own allocations go to PSRAM; only the peripheral role is built, with one connection.
- **Wi-Fi.** Wi-Fi and lwIP allocate from PSRAM first and mbedTLS (protocomm now, the hub link's TLS later) entirely from PSRAM. Wi-Fi code stays in flash/PSRAM instead of IRAM: 18.4 KB less static internal RAM. The DMA buffers that must be internal drop from 16 + 16 to 6 + 6 (about 32 KB less heap). Espressif measures this rank at over 40 Mbit/s TCP on the S3 (with larger TCP windows than the defaults used here), far more than JSON, OTA images and 16 kHz audio need. No SoftAP or WPA-Enterprise code (flash only).
- **mDNS.** Its allocations and its task stack go to PSRAM. A PSRAM stack is safe for it: the task never writes flash, and while another task's flash write has the cache off, other tasks don't run. No console commands, no continuous browsing or continuous hostname advertising (one-shot queries only), no SoftAP interface. Initialised transiently during `orb_net_find_hubs()` and freed immediately after with `mdns_free()`, avoiding the 100 ms periodic timer that wakes the CPU out of automatic light sleep.

## Memory

Measured with `idf.py size` / `size-components` on `test_app` (ESP-IDF 6.1, the sdkconfig above unless noted). "Internal" is the static part of DIRAM, the 341,760 bytes shared by IRAM code, data and BSS; whatever is left is the internal heap.

| Build | Static internal RAM | Flash image |
| --- | --- | --- |
| Empty app, same board config | 60,951 B | 0.16 MB |
| Main firmware today (display, UI; no networking) | 84,838 B | 0.78 MB |
| `test_app` with the sdkconfig above | 114,372 B (+53.4 KB over empty) | 1.19 MB of the 4 MB slot |
| … with Wi-Fi IRAM options back on | 132,932 B (+18.4 KB) | |
| … and the BT controller back in ROM/IRAM (IDF defaults) | 152,368 B (+19.4 KB) | |

The +53.4 KB is almost all ESP-IDF's binary Wi-Fi/PHY libraries (net80211 8.9 KB, phy 8.5 KB, pp 5.1 KB, lwIP 3.8 KB, mDNS 2.2 KB, wpa_supplicant 1.6 KB, BT controller 2.3 KB …). orb_net itself is 6.0 KB of code, 1.1 KB of constants (the zone table) and 260 bytes of RAM.

Heap at run time (internal, from the configuration; not measured on the board, which this stream doesn't flash): Wi-Fi DMA buffers 6 RX + 6 TX at 1.6 KB, the Wi-Fi, lwIP (3 KB) and `net` (4 KB) task stacks, plus the Bluetooth controller and NimBLE host task during provisioning only, all released when provisioning ends. `test_app` logs internal free and minimum free memory every 10 s, so one flash of it gives the real numbers.

**Integrated memory budget.** The app now uses one 466 × 84 × 2-byte internal
DMA band (78,288 bytes), retaining the glass-safe band height. Two such buffers
left BLE controller allocation failing with `ESP_ERR_NO_MEM`; the controller
requires INTERNAL|DMA memory, so moving generic malloc alone did not solve it.
The single-buffer setup passed a 60-second provisioning smoke with 45 KiB
internal free, 31 KiB largest block and 11 KiB minimum. The generic malloc
threshold is 1 KiB and packed-font inflate workspace prefers PSRAM. Measure
again during the owner's actual BLE connection and Wi-Fi association; these
initial numbers cover advertising and the setup UI.

## Integration

In `app_main`, after NVS, the event loop, board and display are up:

```c
ESP_ERROR_CHECK(nvs_flash_init());               /* with the usual erase-and-retry */
ESP_ERROR_CHECK(esp_event_loop_create_default());
esp_err_t err = orb_net_start(on_net, NULL);     /* on_net: show service_name/pop, ssid, state */
if (err != ESP_OK) {
    /* The face and touch still work offline; don't call it again. */
    ESP_LOGE(TAG, "no networking: %s", esp_err_to_name(err));
}
```

- Add the sdkconfig lines above to `firmware/sdkconfig.defaults` and `orb_net` to main's `PRIV_REQUIRES`.
- orb_link calls `orb_net_find_hubs()` once `CONNECTED` and on every reconnect, prefers the hub whose `fp` matches its pinned certificate, and calls `orb_net_set_timezone(hello.tz)`. `orb_net_status()` gives the live RSSI for `hello` and `state.report`.
- A "Forget network" setting calls `orb_net_forget()`; the device restarts into provisioning. It has to ship with networking: once a network has worked, it is the only way out when the router's password changes (the device shows `FAILED` and keeps retrying, and Bluetooth is gone until a restart).
- `firmware/dependencies.lock` gains `espressif/network_provisioning` 1.4.0, `espressif/mdns` 1.14.1 and `espressif/json_generator` 2.1.0 (committed separately on this branch; regenerate with `scripts/idf.sh reconfigure` if it conflicts).
- NVS: Wi-Fi credentials in ESP-IDF's own namespace; the time zone (`tz`) and whether the stored network has worked (`verified`) in `orb_net`. No partition changes.

## Tests

- `tests/host`: `make` builds the plain logic (`orb_net_util.c`, `orb_net_tz.c`, `orb_net_sm.c`) with the Mac's clang and runs it under ASan/UBSan:
  - names, PoP digits and rejection of biased draws, backoff bounds, TXT parsing (case, duplicates, malformed values, values without NUL), and the zone table: sorted, every entry equal to the footer of `/usr/share/zoneinfo/<zone>`, and the same UTC offset and DST flag as macOS's tzdata for every half hour of 2027–2030;
  - the state machine, with the event sequences ESP-IDF and network_provisioning 1.4.0 produce: boot with a stored network, three refusals, refusals mixed with other reasons, link loss and rejoin, a failed `esp_wifi_connect`, provisioning with wrong then right credentials, `CRED_FAIL` after a success, provisioning ending while disconnected or with nothing stored, provisioning that couldn't start, and the `verified` flag; then 20,000 random sequences against invariants (only one side joins at a time, reset only for credentials that never worked, every state change published, `FAILED` only after three refusals in a row outside provisioning).
- `test_app`: builds the component into a whole app for the compiler and `idf.py size`. Flashed, it joins (or provisions), lists hubs and logs the status, local time and free memory every 10 s. Nothing in this stream has run on the board yet.

```bash
cd firmware/components/orb_net/tests/host && make
# ESP-IDF environment as in scripts/idf.sh, then:
idf.py -C firmware/components/orb_net/test_app set-target esp32s3 build size
```
