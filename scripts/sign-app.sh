#!/usr/bin/env bash
# Sign an Orb app image (Secure Boot v2 signature block, RSA-3072) for USB flashing and OTA.
#
#   scripts/sign-app.sh [--assets-envelope] <app.bin> [-o <signed.bin>]
#
# Signs in place unless -o is given. The device checks an update against the key in the
# first signature block of the image it is running, so every image it runs, USB-flashed
# ones included, must be signed with the same key. The key is the PEM in the login
# Keychain's generic password "orb-hub-firmware-signing" (`orb-hub keys init` stores it
# base64-encoded), or the file named by ORB_SIGNING_KEY (tests use throwaway keys). It is
# never printed.
#
# An image already signed with this key is left alone, so signing twice is harmless; one
# signed with another key is refused, since the device only ever looks at the first block.
set -euo pipefail

KEYCHAIN_ITEM="orb-hub-firmware-signing"
ESPSECURE="${ESPSECURE:-$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/espsecure}"

usage() {
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}
die() {
    echo "sign-app: $*" >&2
    exit 1
}

in="" out="" assets=0
while (($#)); do
    case "$1" in
    --assets-envelope)
        assets=1
        shift
        ;;
    -o)
        [[ $# -ge 2 ]] || usage
        out="$2"
        shift 2
        ;;
    -*) usage ;;
    *)
        [[ -z "$in" ]] || usage
        in="$1"
        shift
        ;;
    esac
done
[[ -n "$in" ]] || usage
out="${out:-$in}"

[[ -f "$in" ]] || die "no such file: $in"
if ((assets)); then
    [[ "$(head -c8 "$in")" == "ORBASSET" ]] || die "$in is not an Orb assets envelope"
else
    [[ "$(head -c1 "$in" | xxd -p)" == "e9" ]] || die "$in is not an ESP app image"
fi
[[ -x "$ESPSECURE" ]] || die "espsecure not found at $ESPSECURE"

# Everything secret lives in a private directory that goes away however this exits.
umask 077
tmp="$(mktemp -d "${TMPDIR:-/tmp}/orb-sign.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' HUP INT TERM

if [[ -n "${ORB_SIGNING_KEY:-}" ]]; then
    key="$ORB_SIGNING_KEY"
    [[ -r "$key" ]] || die "can't read ORB_SIGNING_KEY ($key)"
else
    key="$tmp/key.pem"
    security find-generic-password -s "$KEYCHAIN_ITEM" -w >"$tmp/key.out" 2>/dev/null ||
        die "no Keychain item \"$KEYCHAIN_ITEM\" (orb-hub keys init creates it), or the Keychain is locked"
    # The hub's rule (orb_hub.signing._decode_item): a PEM stored as is; otherwise hex, which
    # security prints for a secret with line breaks; otherwise base64, as orb-hub keys init
    # stores it (security -i reads one line of at most 4 KB).
    if grep -q -e '-----BEGIN' "$tmp/key.out"; then
        mv "$tmp/key.out" "$key"
    else
        tr -d ' \t\r\n' <"$tmp/key.out" >"$tmp/key.txt"
        if ! { grep -Eqx '([0-9a-fA-F]{2})+' "$tmp/key.txt" && xxd -r -p "$tmp/key.txt" >"$key" &&
            grep -q -e '-----BEGIN' "$key"; }; then
            base64 --decode <"$tmp/key.txt" >"$key" 2>/dev/null || true
        fi
    fi
    grep -q -e '-----BEGIN' "$key" || die "Keychain item \"$KEYCHAIN_ITEM\" is not a PEM key"
fi

"$ESPSECURE" digest-sbv2-public-key --keyfile "$key" --output "$tmp/key.digest" >/dev/null
ours="$(xxd -p -c 64 "$tmp/key.digest")"

# Digest of the key in an image's first signature block; "" when unsigned.
block0_key() {
    local info
    info="$("$ESPSECURE" signature-info-v2 "$1" 2>&1)" ||
        die "$1 has a signature block that doesn't match it (changed after signing?)"
    sed -n 's/^Public key digest for block 0: //p' <<<"$info" | tr -d ' '
}

# espsecure accepts any valid block, while the device verifies only block zero.
signed_by_us() {
    [[ "$(block0_key "$1")" == "$ours" ]] || return 1
    local size
    size="$(wc -c <"$1")"
    # Keep the original image intact; erase later signature blocks only in this verification copy.
    head -c "$((size - 4096 + 1216))" "$1" >"$tmp/block-zero.bin"
    dd if=/dev/zero bs=2880 count=1 2>/dev/null | LC_ALL=C tr '\000' '\377' >>"$tmp/block-zero.bin"
    "$ESPSECURE" verify-signature --version 2 --keyfile "$key" "$tmp/block-zero.bin" >/dev/null 2>&1
}

publish() {
    [[ "$1" -ef "$out" ]] || install -m 644 "$1" "$out"
}

theirs="$(block0_key "$in")"
if [[ -n "$theirs" ]]; then
    [[ "$theirs" == "$ours" ]] || die "$in is signed with another key (${theirs:0:16}…); sign the unsigned build"
    signed_by_us "$in" || die "$in has a signature from this key that doesn't verify"
    publish "$in"
    echo "$out: already signed (key ${ours:0:16}…)"
    exit 0
fi

"$ESPSECURE" sign-data --version 2 --keyfile "$key" --output "$tmp/signed.bin" "$in" >/dev/null
signed_by_us "$tmp/signed.bin" || die "the signed image doesn't verify"
publish "$tmp/signed.bin"
echo "$out: signed (key ${ours:0:16}…)"
