# orb_ota

Over-the-air updates from the hub, as specified in [docs/protocol.md](../../../docs/protocol.md) ("Firmware downloads", "Releases", `ota.check` / `ota.available` / `ota.report`, hello's `last_ota`). API: [include/orb_ota.h](include/orb_ota.h).

1. An offer arrives as `ota.available`, or as the answer to `ota.check`. It is acted on by its install policy:
   - `now`: install at once.
   - `idle`: wait for `orb_ota_set_idle(true)`.
   - `ask`: wait for `orb_ota_install()`.
2. The image streams over pinned HTTPS (`esp_https_ota`, using the certificate and `Authorization` header from `orb_link_hub_https`) into the inactive slot. The certificate's names aren't checked: the pin alone identifies the hub, and the URL holds the hub's current IP, which a new DHCP lease can take out of the certificate's SAN. esp-tls logs a warning about the skipped check on every download.
3. The SHA-256 of what landed in flash is checked against the offer.
4. `esp_https_ota_finish` → `esp_ota_end` has ESP-IDF check the signature. Only then does the boot slot switch, and the device restarts.
5. The new image boots on probation and has 60 s to be confirmed with `orb_ota_mark_healthy()`. Otherwise it is marked invalid and the previous slot boots.
6. The outcome goes to the hub in the next hello's `last_ota`.

## Using it

```c
void app_main(void)
{
    nvs_flash_init();                   /* erase-and-retry on NO_FREE_PAGES / NEW_VERSION_FOUND, as usual */
    ESP_ERROR_CHECK(orb_ota_boot_check());   /* before anything that can hang */
    /* ... board, display, UI and app link handlers ... */
    ESP_ERROR_CHECK(orb_ota_start(on_ota_status, NULL));
    /* ... orb_net_start, orb_link_start ... */
}

/* In the orb_link state callback, the first time it reports ORB_LINK_ONLINE with the UI up: */
orb_ota_mark_healthy();

/* From the power service, on every change: */
orb_ota_set_power(battery_pct, charging);
orb_ota_set_idle(charging && untouched_for_5_min);
```

- **`orb_ota_boot_check()`** needs NVS initialised. On probation it starts the 60 s deadline. It also works out what became of the last install and loads the report for hello. NVS trouble is logged and skipped, because the deadline must never depend on it.
- **`orb_ota_mark_healthy()`** is called once per boot, after the hub accepted a hello (and the UI is up), whether or not the image is on probation. Off probation it does not touch the image. In both cases it is also how orb_ota learns that the hello which carried `last_ota` got through, and so stops sending it.
  - If the image can't be confirmed, the deadline keeps running and it rolls back like an unhealthy one.
  - Rolling back checks the previous slot's signature. If ESP-IDF won't vouch for it, orb_ota simply restarts, and the bootloader aborts the unconfirmed image by itself.
- **`orb_ota_start()`** registers the `ota.available` event and the hello hook with orb_link. Call it before `orb_link_start()`; registration is supported before the link task starts. Otherwise an early hello or OTA offer can arrive before its handler exists.
- **HTTPS ownership.** `orb_link_hub_https()` supplies one copied snapshot: `base_url`, `authorization` and an owned `cert_pem`. The install keeps the snapshot through `esp_https_ota_finish()` or `_abort()` and frees the PEM on every exit, so unpairing or re-pairing during a download cannot invalidate the HTTP client's pointers. The auth callback reads this snapshot from HTTP `user_data`, not shared globals.
- **Callbacks.** The status callback runs on whichever task changed the state: orb_link's task (offers), orb_ota's install task (progress, verify, ready, failed), or the caller of `orb_ota_install` / `orb_ota_set_idle` / `orb_ota_set_power` / `orb_ota_mark_healthy`. It must not block, and takes `lv_lock()` for the UI. Progress changes call it too, at most once per percent.
- **`READY` restarts the device** about 1.5 s later, so the `ready` report can go out and the UI can say so.
- **`orb_ota_set_power()`** holds `now` and `idle` offers while the battery is below 30 % and not charging. A tap still installs. Until it is called, or with a negative `battery_pct` (no battery), nothing is held. A held offer installs once the battery recovers or charging starts.

### Policy details

| Situation | What happens |
| --- | --- |
| Offer for the version already running | Ignored |
| Download, verify or restart in progress | New offers ignored; the hub re-offers after the next hello |
| On probation | Offers wait: ESP-IDF refuses OTA until the image is confirmed. `orb_ota_mark_healthy()` then acts on the waiting offer |
| Image that failed its checks (SHA-256, signature, descriptor, size) or rolled back | Its SHA-256 is remembered, along with the image it failed under. `now` and `idle` offers of it wait for a tap instead of looping on fetch, fail, reconnect, re-offer. Once another image runs (a USB flash, say), it's forgotten, since that image may have been the cause |
| Rolled back because power was lost or the device was reset from outside (power-on, brownout, reset pin, USB, JTAG) on probation | Reported as `rolled_back`, but not held against the image |
| Battery below 30 % and not charging | `now` and `idle` offers wait; a tap installs |
| Running image unsigned | Nothing is downloaded: ESP-IDF would reject every update. Fails with `running image is unsigned`, not held against the image |
| Network failure | Not held against the image. A re-offer after reconnecting, or a tap, retries |
| Download stops moving for 30 s with the connection up | Fails with `download stalled`, a network failure. ESP-IDF reports a read timeout as "still in progress", so without this a hung hub would block every later offer |
| `{"available":false}` | A waiting offer is dropped |

`ota.report` goes out at 0 %, every 10 %, then at `verifying`, `ready` or `failed` (with `error`). Downloads always go to the paired hub's address (`orb_link_hub_https`) plus the path of the offer's `url`, so the token never goes to another host.

The downloaded image's app descriptor must name the running project and the offered version. `Content-Length` must equal the offer's `size`.

### NVS (namespace `orb_ota`)

| Key | Holds |
| --- | --- |
| `try_ver`, `try_sha`, `try_slot`, `try_err` | The install the next boot reports on, written just before `esp_https_ota_finish` |
| `last_ver`, `last_res`, `last_err` | The outcome waiting for an accepted hello |
| `bad_sha`, `bad_on` | The last image that failed its checks or rolled back, and the first 16 hex digits of the ELF SHA-256 of the image it failed under |

At boot, the `try_*` record becomes a report only if the boot slot really switched to it:

| Boot finds | Report |
| --- | --- |
| The attempt's slot running, on probation | Nothing yet: `orb_ota_mark_healthy()` or the deadline decides. Any older report is dropped |
| The attempt's slot running and confirmed, same version | `ok` |
| The attempt's slot marked invalid | `rolled_back` |
| otadata selects the attempt's slot, but the old one runs | `failed: did not start` (the bootloader couldn't load it) |
| otadata never pointed at it, or its slot now holds another version | Nothing: a restart before `esp_https_ota_finish` switched the slot, or a USB flash since. The record is dropped, and an older report still waiting stays |

## Signing: what ESP-IDF actually checks

Researched in ESP-IDF v6.1 (`bootloader_support/src/secure_boot_v2/secure_boot_signatures_app.c`, `esp_image_format.c`, `app_update/esp_ota_ops.c`):

- **The trusted key is the running image's own.** With `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` (no hardware Secure Boot, so no eFuse key digests), `get_secure_boot_key_digests()` reads the signature blocks of the *running* app. `esp_secure_boot_verify_sbv2_signature_block()` then accepts a new image only if its first signature block carries the same public key as the running image's first block, and its RSA-PSS signature verifies. In this mode only block 0 counts on both sides.
- **There is no verification-key setting for this scheme.** `CONFIG_SECURE_BOOT_VERIFICATION_KEY` exists only for the Secure Boot v1 ECDSA scheme, so nothing about the key is compiled into the firmware.
- **So the image flashed over USB must be signed too, with the same key.** An unsigned running image has no key to trust: ESP-IDF would fail every OTA with "No signatures were found for the running app" (`ESP_ERR_OTA_VALIDATE_FAILED`), and the device can only be updated over USB. orb_ota checks for this before downloading (`esp_secure_boot_get_signature_blocks_for_running_app`) and fails with `running image is unsigned`, without holding it against the release. Today's devices, running unsigned images, need one USB flash of a signed image before OTA works.
- **The bootloader doesn't check signatures in this mode.** `SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` is only offered for the v1 ECDSA scheme, so an unsigned USB image still boots; it just can't take updates. USB recovery always works.
- **Key rotation needs USB.** An OTA image must be signed with the running key in block 0 and keeps trusting that key afterwards. A different key can only arrive by USB.
- **Rollback verifies too.** `esp_ota_mark_app_invalid_rollback()` first checks that the previous slot still validates, signature included, against the *new* image's key. With one key that always holds. If it doesn't, orb_ota restarts and the bootloader's own abort handles it.

### The pipeline

1. `scripts/idf.sh build` builds an **unsigned** image (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` off), padded with `--secure-pad-v2` so a signature sector can be appended. IDF prints "App built but not signed".
2. `scripts/sign-app.sh build/orb.bin` signs it in place with the Keychain key:
   - generic password `orb-hub-firmware-signing`, which `orb-hub keys init` stores base64-encoded. It is decoded by the hub's own rule: PEM as is, else hex (how `security` prints a secret with line breaks), else base64. Or the file in `ORB_SIGNING_KEY`, for tests;
   - the key is written only to a mode-600 file in a private temp directory, removed on exit, and is never printed;
   - re-signing is a no-op, and an image signed with another key is refused.
3. `scripts/remote.sh flash` must flash the signed image. It doesn't yet: that is integration step 4 below. Until it lands, a USB-flashed device runs an unsigned image and every install fails with `running image is unsigned`.
4. `orb-hub release build/orb.bin` finds it already signed with the release key ("unless it's already signed with that key" in the protocol) and publishes it as is.

Since `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME` sets esptool's `--flash-size keep`, flashing never rewrites the app header, so the signature stays valid.

## Required sdkconfig

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y          # already in firmware/sdkconfig.defaults
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set
```

None of these burn eFuses. The partition table needs two OTA slots and `otadata` (it has them). `orb_ota.c` refuses to compile without signature checks on update (`CONFIG_SECURE_SIGNED_ON_UPDATE`), the RSA scheme and app rollback, so a configuration that would accept unsigned images can't ship by accident.

## Integration

1. **`firmware/sdkconfig.defaults`**: add the four signing lines above. An existing `firmware/sdkconfig` (untracked, generated) keeps its own values, and one generated before this change has `# CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT is not set`, which wins over the defaults. Delete it (`rm firmware/sdkconfig`, then build) or set the options in `idf.py menuconfig`. Until then `orb_ota.c` stops the build with an `#error`.
2. **`firmware/main/CMakeLists.txt`**: add `orb_ota nvs_flash` to `PRIV_REQUIRES`. In `main.c`, call `nvs_flash_init()` and `orb_ota_boot_check()` first in `app_main`, `orb_ota_start()` before `orb_link_start()`, and `orb_ota_mark_healthy()` once the UI is up and orb_link reports `ORB_LINK_ONLINE`, on every boot. The power service calls `orb_ota_set_power()` on every change and `orb_ota_set_idle()` (charging and untouched for 5 minutes).
3. **Merge orb/link first, or in the same merge.** orb_ota has `PRIV_REQUIRES orb_link`, and at `8c6df88` `components/orb_link` has only its header. ESP-IDF builds every component it finds, so orb_ota alone breaks `scripts/idf.sh build` with "Failed to resolve component orb_link".
4. **`scripts/remote.sh flash`**: sign before copying, so USB images carry the key OTA images will be checked against:
   ```bash
   flash)
       [[ -f build/flash_args ]] || { echo "No build/flash_args; run scripts/idf.sh build first" >&2; exit 1; }
       # The device trusts updates signed with the key in its running image, so USB images are signed too.
       ../scripts/sign-app.sh "build/$(awk '$1 == "0x20000" {print $2}' build/flash_args)"
       ssh "$HOST" "mkdir -p $DEST"
       ...
   ```
   remote.sh has already changed into `firmware/`, hence `../scripts`. The awk picks the app at the `ota_0` offset out of `flash_args` (`orb.bin`). `scripts/dev-flash-monitor.sh` goes through `remote.sh flash` and needs no change. `restore` (the factory image) is unaffected.
5. **Hub**:
   - `orb-hub keys init` stores the PEM base64-encoded through `security -i` (orb/hub, `signing.py`), so `/usr/bin/security` is the app the item trusts and sign-app.sh reads it without a prompt. sign-app.sh decodes it by the same rule as the hub's `_decode_item`.
   - `orb-hub release` should check block 0's key digest (`espsecure signature-info-v2`), as sign-app.sh does. `espsecure verify-signature` on its own passes if *any* of three blocks verifies.

## Tests

- **Host** (pure logic in `orb_ota_logic.c`: offer parsing and validation, install decisions with the battery floor, boot outcome and what counts against an image, the stall watch, image checks, SHA and UTF-8 helpers, JSON): `make -C tests/host`. This uses macOS clang with ASan and UBSan, and cJSON from `test_app/managed_components` (build test_app once) or `CJSON_DIR`.
- **test_app**: builds orb_ota with the signing config against a stub orb_link (`test_app/components/orb_link`, which wins over the real one). From `test_app/`, run `idf.py set-target esp32s3 build` with the IDF environment sourced. Its `app_main` is an on-device smoke test of offers, policies (the battery floor too), hello and a failing install; it prints `orb_ota test: N passed, 0 failed`.
- **Signing**: `test_app/sign_check.sh` (after building test_app) generates two TEST-ONLY RSA-3072 keys in a temp directory and shows:
  - a test_app image signed by `sign-app.sh` verifies;
  - an image signed with the other key, an unsigned one and a tampered one are rejected;
  - the device's block-0 rule accepts and rejects the right images;
  - the Keychain path works with a stand-in `security`, for the base64 item `orb-hub keys init` writes, hex and plain PEM, and refuses an item that isn't a key;
  - no key material reaches the output and no temp files are left.

## Independent signed assets channel

`orb-hub release <versioned-assets.bin> --assets --channel dev` uses the same
release key and authenticated download route, but publishes `dev-assets.json`.
Application releases, application probation and otadata remain independent.
The full header and wire contract are in `docs/protocol.md`, “Signed assets
updates”. A compatible device advertises its verified assets version in hello;
older devices are never offered data images as application updates.

The assets writer uses one slot, entering rescue-font mode before erase. It
checks the whole-download SHA-256, signed envelope and offered content version
before reporting ready and rebooting. Failures invalidate the first sector and
retain a bad-image SHA in NVS to prevent unattended retry loops. Existing loaded
font pointers stay valid; font loads during the transaction use rescue faces.
USB migration must precede use of the larger app slots and shifted assets slot.

`make -C firmware/components/orb_ota/tests/host` runs the ordinary offer/rollback
regressions plus the production single-slot transaction under ASan/UBSan.
Injected faults cover begin/erase/read/write, timeout, truncated/oversized data,
SHA rejection, signature rejection, version rejection, header invalidation and
balanced cleanup. Physical brownout/OTA/heap tests remain lead-owned.
