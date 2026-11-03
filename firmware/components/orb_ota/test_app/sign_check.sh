#!/usr/bin/env bash
# Host-side proof of the signing pipeline, on the test_app image (build test_app first):
#
#   firmware/components/orb_ota/test_app/sign_check.sh
#
# Uses two TEST-ONLY RSA-3072 keys generated here in a private temp directory and deleted
# on exit; the real key and the Keychain are never touched (a stand-in `security` plays the
# Keychain). "Device accepts" below is ESP-IDF's rule in esp_secure_boot_verify_sbv2_signature_block
# with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT: the key in the new image's first signature
# block must be the key in the running image's first block, and its signature must verify.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SIGN="$(cd "$HERE/../../../.." && pwd)/scripts/sign-app.sh"
ESPSECURE="${ESPSECURE:-$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/espsecure}"
PYTHON="${PYTHON:-$(dirname "$ESPSECURE")/python}"
APP="$HERE/build/orb_ota_test.bin"
[[ -f "$APP" ]] || {
    echo "no $APP: build test_app first" >&2
    exit 1
}

umask 077
work="$(mktemp -d "${TMPDIR:-/tmp}/orb-sign-check.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir "$work/tmp" "$work/bin"

pass=0 fail=0
check() {
    local name="$1"
    shift
    if "$@" >>"$work/log" 2>&1; then
        pass=$((pass + 1))
        echo "ok    $name"
    else
        fail=$((fail + 1))
        echo "FAIL  $name"
    fi
}
fails() { ! "$@"; }
verify() { "$ESPSECURE" verify-signature --version 2 --keyfile "$1" "$2"; }
block0_key() { "$ESPSECURE" signature-info-v2 "$1" | sed -n 's/^Public key digest for block 0: //p' | tr -d ' '; }
verify_first() {
    "$PYTHON" - "$2" "$work/first-only.bin" <<'PYBLOCK'
from pathlib import Path
import sys

image = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_bytes(image[:-2880] + b"\xff" * 2880)
PYBLOCK
    verify "$1" "$work/first-only.bin"
}
device_accepts() {   # running image, new image, the key the new image should verify with
    local running new
    running="$(block0_key "$1")"
    new="$(block0_key "$2")"
    [[ -n "$running" && "$running" == "$new" ]] && verify_first "$3" "$2"
}
sign() { TMPDIR="$work/tmp" ORB_SIGNING_KEY="$1" "$SIGN" "${@:2}"; }
same_file() { cmp -s "$1" "$2"; }
size() { stat -f %z "$1"; }

key_a="$work/TEST_ONLY_key_a.pem" key_b="$work/TEST_ONLY_key_b.pem"
"$ESPSECURE" generate-signing-key --version 2 --scheme rsa3072 "$key_a" >/dev/null
"$ESPSECURE" generate-signing-key --version 2 --scheme rsa3072 "$key_b" >/dev/null
for f in a b c k p e; do cp "$APP" "$work/$f.bin"; done

echo "Signing with sign-app.sh:"
check "the unsigned build doesn't verify" fails verify "$key_a" "$work/a.bin"
check "signs with key A" sign "$key_a" "$work/a.bin"
check "verifies with key A" verify "$key_a" "$work/a.bin"
check "appends one 4 KB signature sector" test $(($(size "$work/a.bin") - $(size "$APP"))) -eq 4096
cp "$work/a.bin" "$work/a.before"
check "signing again is a no-op" sign "$key_a" "$work/a.bin"
check "  and leaves the image byte for byte" same_file "$work/a.bin" "$work/a.before"
check "-o signs a copy" sign "$key_a" "$work/c.bin" -o "$work/c.signed"
check "  leaving the input unsigned" same_file "$work/c.bin" "$APP"
check "  and the copy verifies" verify "$key_a" "$work/c.signed"

echo "Another key is rejected:"
check "signs a copy with key B" sign "$key_b" "$work/b.bin"
check "key A rejects the B-signed image" fails verify "$key_a" "$work/b.bin"
check "key B rejects the A-signed image" fails verify "$key_b" "$work/a.bin"
check "sign-app.sh won't sign a B-signed image with key A" fails sign "$key_a" "$work/b.bin"
cp "$work/a.bin" "$work/t.bin"
printf '\x00' | dd of="$work/t.bin" bs=1 seek=4096 conv=notrunc 2>/dev/null
check "a byte changed after signing is rejected" fails verify "$key_a" "$work/t.bin"

echo "A later signature cannot rescue a broken first RSA signature:"
"$ESPSECURE" sign-data --version 2 --append-signatures --keyfile "$key_a" \
    --output "$work/two.bin" "$work/a.bin" >/dev/null
check "valid multiple signatures are preserved" sign "$key_a" "$work/two.bin" -o "$work/two.copy"
check "  byte for byte" same_file "$work/two.bin" "$work/two.copy"
"$PYTHON" - "$work/two.bin" "$work/bad-first.bin" <<'PYBLOCK'
from pathlib import Path
import struct
import sys
import zlib

image = bytearray(Path(sys.argv[1]).read_bytes())
first = len(image) - 4096
image[first + 812] ^= 1
struct.pack_into("<I", image, first + 1196, zlib.crc32(image[first:first + 1196]))
Path(sys.argv[2]).write_bytes(image)
PYBLOCK
check "espsecure accepts the valid later block" verify "$key_a" "$work/bad-first.bin"
cp "$work/bad-first.bin" "$work/bad-first.before"
check "sign-app.sh rejects its invalid first RSA signature" fails sign "$key_a" "$work/bad-first.bin"
check "  leaving the image untouched" same_file "$work/bad-first.bin" "$work/bad-first.before"

echo "The device's rule (running image signed with key A):"
check "accepts an update signed with key A" device_accepts "$work/a.bin" "$work/c.signed" "$key_a"
check "rejects an update signed with key B" fails device_accepts "$work/a.bin" "$work/b.bin" "$key_b"
check "rejects an unsigned update" fails device_accepts "$work/a.bin" "$APP" "$key_a"
check "rejects a broken first RSA signature despite a valid later block" fails device_accepts "$work/a.bin" "$work/bad-first.bin" "$key_a"
check "accepts nothing while running an unsigned image" fails device_accepts "$APP" "$work/c.signed" "$key_a"

echo "Keychain path (stand-in security):"
# Answers only the lookup sign-app.sh makes, with TEST-ONLY key A, printed by $1.
stub() {
    cat >"$work/bin/security" <<STUB
#!/usr/bin/env bash
[[ "\$*" == "find-generic-password -s orb-hub-firmware-signing -w" ]] || exit 44
$1 "$key_a"
STUB
    chmod 700 "$work/bin/security"
}
keychain() { env -u ORB_SIGNING_KEY PATH="$work/bin:$PATH" TMPDIR="$work/tmp" "$SIGN" "$@"; }
stub "openssl base64 -A -in"   # how orb-hub keys init stores it
check "signs with the key from the Keychain (base64 form)" keychain "$work/e.bin"
check "  which is key A" verify "$key_a" "$work/e.bin"
stub "xxd -p -c 1000000"   # the real tool prints a secret with line breaks as hex
check "signs with the key from the Keychain (hex form)" keychain "$work/k.bin"
check "  which is key A" verify "$key_a" "$work/k.bin"
stub "cat"
check "signs with the key from the Keychain (PEM form)" keychain "$work/p.bin"
check "  which is key A" verify "$key_a" "$work/p.bin"
stub "echo bm90IGEga2V5Cg== #"   # base64 of something else
cp "$APP" "$work/n.bin"
check "refuses a Keychain item that isn't a PEM key" fails keychain "$work/n.bin"
printf '#!/usr/bin/env bash\nexit 44\n' >"$work/bin/security"
check "fails cleanly without the Keychain item" fails keychain "$work/n.bin"
check "  leaving the image untouched" same_file "$work/n.bin" "$APP"

echo "Hygiene:"
pem_line="$(sed -n 2p "$key_a")"
check "no key material in any output" fails grep -q -e "PRIVATE KEY" -e "${pem_line:0:40}" "$work/log"
check "no temp files left behind" test -z "$(ls -A "$work/tmp")"

echo "sign_check: $pass passed, $fail failed"
((fail == 0))
