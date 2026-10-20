#!/bin/bash
# Decompile the VR-path functions of the patched game binary via REA+Ghidra
# into vr/ghidra/decomp/*.c (snapshot reuse makes follow-ups fast).
set -u
export GHIDRA_INSTALL_DIR="$HOME/tools/ghidra_12.1.4_PUBLIC"
export JAVA_HOME="$HOME/tools/jdk/jdk-21.0.12.1+1/Contents/Home"
REA="$HOME/Documents/rea/scripts/rea.mjs"
JP=$(cd "$(dirname "$0")/../.." && pwd)
GAME="${GAME:-${JP2015_GAME:-$JP/jp_game}/game.q0bak}"
SNAP="$JP/vr/ghidra/game.snapshot"
OUT="$JP/vr/ghidra/decomp"
mkdir -p "$OUT"

FUNCS="${FUNCS:-DongleInitialize DongleInit DongleOpen DongleFeatureLogin DongleRead DongleIsPresent DongleGetID DongleEncrypt DongleDecrypt DongleFeatureCheck DongleCheckMemoryInitialized DongleReadCabSerialNum DongleReadCountryCode DongleReadCabType DongleReadHLFeature DongleGetRand}"

for fn in $FUNCS; do
    tmp="$OUT/$fn.json"
    echo "=== $fn ==="
    node "$REA" function "$GAME" "$fn" --snapshot "$SNAP" --format json > "$tmp" 2>&1
    python3 - "$tmp" "$OUT/$fn.c" <<'PYEOF'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
try:
    data = json.load(open(src))
except Exception as ex:
    print(f"  JSON parse failed: {ex}")
    sys.exit(0)
v = data.get("raw_result", {}).get("value", {})
proc = v.get("procedure", {})
sig = proc.get("signature", "?")
pseudo = v.get("pseudocode")
if pseudo:
    open(dst, "w").write("// " + sig + "\n" + pseudo)
    print(f"  wrote {dst} ({len(pseudo)} chars)")
else:
    print(f"  NO pseudocode (keys: {list(v.keys())})")
PYEOF
done
echo "ALL DONE"
