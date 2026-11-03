# Built artifact/device verifier regression

From the worktree root, after `scripts/idf.sh build`:

```sh
.venv/bin/python -m pytest -q tests/test_storage_artifacts.py
```

The test requires the lead's existing `firmware/build/orb.signed.bin` and
`orb_assets-signed.bin`, the current unsigned `orb.bin`, installed ESP-IDF 6.1,
`espsecure`, Clang and OpenSSL development headers/libraries. Environment overrides:
`IDF_PATH`, `ESPSECURE`, `CC`, `OPENSSL_PREFIX`. The two real signed artifacts are
only read. Test signatures use an ephemeral RSA-3072 key in pytest's temporary
directory; the development key and Keychain are never accessed.

The harness compiles the device's `orb_assets_esp.c`, installed IDF's
`secure_boot_signatures_app.c` and `secure_boot_rsa_signature.c` unchanged. It also
compiles the flash-hashing functions and 24-bit mmap range guard extracted directly
from installed IDF source. OpenSSL implements host PSA/SHA primitives and ASN.1
primitives; memory-backed flash preserves the real app/assets partition addresses.
The metadata adapter parses ESP image segment/checksum/hash lengths; Python also
checks the actual image's checksum, appended SHA-256, signature header, CRC, RSA
modulus size and shared signing-key digest. No stub grants a signature success.
The harness consumes the actual `build/config/sdkconfig.h`; its signing/cache
configuration and binary partition table geometry are asserted, and the signed
payload must equal the freshly generated `orb_fonts.pack` byte for byte. Both the
real app and temporary-key rebuilt app must pass IDF's app signature verifier.

The real signed pair succeeds through the fixed device verification path. The old
generic `esp_secure_boot_verify_signature` path fails using IDF's actual mmap range
guard: assets at `0xc20000` extend past the 24-bit mapping limit `0x1000000`. This
build has no 32-bit cache address mode. The guard is reproduced on the host without
changing the board's bootloader or runtime configuration.

The current rebuilt unsigned app and exact real assets envelope are additionally
signed with the temporary key. Both must verify, while wrong trust, altered payload,
signature damage with a repaired CRC, malformed header, truncation and a correctly
signed envelope containing an incorrect payload digest must fail. ASan/UBSan cover
the executable. These tests establish verification behavior, not physical heap or
power-loss measurements.
