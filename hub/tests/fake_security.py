"""A stand-in for /usr/bin/security covering what orb_hub.signing uses, so tests never touch a real Keychain.

Items live in $FAKE_KEYCHAIN_DIR/items.json; every call's argv and stdin go to calls.jsonl. Like the real tool,
find-generic-password -w prints a value with non-printable bytes as hex, and `security -i` reads commands one
line of at most 4095 bytes at a time.
"""

import json
import os
import shlex
import sys
from pathlib import Path

LINE_MAX = 4095
NOT_FOUND = 44

root = Path(os.environ["FAKE_KEYCHAIN_DIR"])
items_path = root / "items.json"
items = json.loads(items_path.read_text()) if items_path.exists() else {}


def options(args: list[str]) -> tuple[dict, list[str]]:
    opts, rest, i = {}, [], 0
    while i < len(args):
        if args[i].startswith("-") and len(args[i]) == 2:
            opts[args[i]] = args[i + 1] if i + 1 < len(args) else ""
            i += 2
        else:
            rest.append(args[i])
            i += 1
    return opts, rest


def add(args: list[str]) -> int:
    opts, keychain = options(args)
    service = opts["-s"]
    if service in items:
        print("add-generic-password: returned -25299", file=sys.stderr)
        return 1
    value = opts["-w"] if "-w" in opts else bytes.fromhex(opts["-X"]).decode()
    items[service] = {"account": opts.get("-a"), "label": opts.get("-l"), "value": value, "keychain": keychain}
    items_path.write_text(json.dumps(items))
    return 0


def find(args: list[str]) -> int:
    opts, _ = options(args)
    item = items.get(opts["-s"])
    if item is None:
        print("security: SecKeychainSearchCopyNext: The specified item could not be found.", file=sys.stderr)
        return NOT_FOUND
    value = item["value"]
    print(value.encode().hex() if not value.isprintable() else value)
    return 0


stdin = sys.stdin.read() if sys.argv[1:2] == ["-i"] else ""
with open(root / "calls.jsonl", "a") as log:
    log.write(json.dumps({"argv": sys.argv[1:], "stdin": stdin}) + "\n")

if sys.argv[1] == "-i":
    status = 0
    for line in stdin.splitlines():
        if len(line) > LINE_MAX:
            print("security: line too long", file=sys.stderr)
            status = 1
            continue
        parts = shlex.split(line)
        status |= add(parts[1:]) if parts[0] == "add-generic-password" else 1
    sys.exit(status)
if sys.argv[1] == "find-generic-password":
    sys.exit(find(sys.argv[2:]))
sys.exit(1)
