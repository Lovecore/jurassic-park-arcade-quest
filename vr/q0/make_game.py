#!/usr/bin/env python3
"""Build the runnable game binary from the pristine TP dump.

    make_game.py <pristine game> <output>

Verifies the input is the known TP dump, applies every byte patch listed in
game_patches.json (each record carries old/new bytes and the reason), and
verifies the output hash. Fails loudly on any mismatch so a wrong input can
never produce a half-patched binary.
"""
import hashlib
import json
import os
import sys

PRISTINE_SHA256 = "c18a9329e72a363937155d245440694e58b5a1e27984016ae7648af3413b4269"
OUTPUT_SHA256 = "795862124cb4bdf0e1064ab145cd0cd7ff9f030078c2aa74839fb700ae4f7d4a"
PATCHES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "game_patches.json")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    if sha256(data) != PRISTINE_SHA256:
        sys.exit(f"{src}: not the pristine TP dump (sha256 {sha256(data)})")
    patches = json.load(open(PATCHES))
    for p in patches:
        old, new = bytes.fromhex(p["old"]), bytes.fromhex(p["new"])
        off = p["off"]
        if data[off:off + len(old)] != old:
            sys.exit(f"patch at {off:#x} ({p['group']}): unexpected bytes")
        data[off:off + len(new)] = new
    if sha256(data) != OUTPUT_SHA256:
        sys.exit(f"output hash mismatch: {sha256(data)}")
    with open(dst, "wb") as f:
        f.write(data)
    os.chmod(dst, 0o755)
    groups = {}
    for p in patches:
        groups[p["group"]] = groups.get(p["group"], 0) + 1
    print(f"{dst}: {len(patches)} patches {groups}, sha256 {OUTPUT_SHA256[:16]}...")


if __name__ == "__main__":
    main()
