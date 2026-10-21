#!/bin/sh
# Checks your own copy of the game and prepares what the recompiler reads:
#   ext/game_work/game                  the dump's `game`, patched with the 52
#                                       binary patches (vr/kit/patch_game.sh)
#   ext/game_work/symbols_original.txt  the function map (from this repo)
#   ext/TeknoParrot/ElfLdr2/libs/       the PhysX 2.8 i386 runtime (4 files)
#   ext/game_dir.txt                    where your game folder is
# Every input is checked against its SHA-256: only this exact game version
# (Jurassic Park Arcade 1.08, Raw Thrills Linux PC, TeknoParrot dump) works.
#
#   sh tools/setup/prepare_game.sh "<game folder>" ["<folder with the PhysX libs>"]
# The PhysX folder defaults to ext/TeknoParrot/ElfLdr2/libs (TeknoParrot
# installed into ext/TeknoParrot).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
EXT="$ROOT/ext"
[ $# -ge 1 ] || { sed -n 2,13p "$0"; exit 2; }
GAME=$(cd "$1" && pwd)
TPL=$EXT/TeknoParrot/ElfLdr2/libs
SRC_TPL=${2:-$TPL}
sha() { sha256sum "$1" | cut -d' ' -f1; }
fail() { echo "ERROR: $*"; exit 1; }

echo "game folder: $GAME"
[ -f "$GAME/game" ] || fail "no 'game' file in $GAME (the folder of the TeknoParrot dump)"
[ "$(sha "$GAME/game")" = c18a9329e72a363937155d245440694e58b5a1e27984016ae7648af3413b4269 ] ||
    fail "'game' is not the expected v1.08 TeknoParrot binary (sha256 $(sha "$GAME/game"))"
for d in g5/dino/data g5/shaders g5/g5engine pmuser programs_dec g5/dino/data/programs_enc; do
    [ -d "$GAME/$d" ] || fail "missing folder $d in the game folder"
done
echo "  game binary and folders: ok"

# PhysX 2.8 runtime (NVIDIA; ships with TeknoParrot's ElfLdr2)
mkdir -p "$TPL"
while read -r f sum; do
    [ -f "$SRC_TPL/$f" ] || fail "PhysX library $f not found in $SRC_TPL"
    [ "$(sha "$SRC_TPL/$f")" = "$sum" ] || fail "$f has an unexpected sha256 ($(sha "$SRC_TPL/$f"))"
    [ "$SRC_TPL" = "$TPL" ] || cp "$SRC_TPL/$f" "$TPL/$f"
done <<PHYSX
libPhysXLoader.so.1 18eaf2d3efa5f0830cb7efa4beb72bec194c6ec7b97a8a24bd9accc21f80dfd0
libPhysXCore.so e7abebbd2c8203e7172e0e3a905abb94b034dd7cbb6a12e4666a8b9409815d42
libNxCooking.so 0e09448b4b08e98905f2de136f0fbaf08c647080c5bde812e64c60dc6e29ffff
libNxCharacter.so.1 81a52480329e90fc64cd60c2f9dd50bde2c0f97ea96940fe11bda75e9685ae59
PHYSX
echo "  PhysX runtime: ok ($TPL)"

# the patched working copy the recompiler lifts
W="$EXT/game_work"
mkdir -p "$W"
cp "$GAME/game" "$W/game.tmp"
sh "$ROOT/vr/kit/patch_game.sh" "$W/game.tmp"
[ "$(sha "$W/game.tmp")" = 795862124cb4bdf0e1064ab145cd0cd7ff9f030078c2aa74839fb700ae4f7d4a ] ||
    fail "patched binary hash mismatch ($(sha "$W/game.tmp"))"
mv "$W/game.tmp" "$W/game"
cp "$ROOT/tools/recomp/symbols_original.txt" "$W/symbols_original.txt"
echo "  patched game: ok ($W/game)"

printf '%s\n' "$GAME" > "$EXT/game_dir.txt"
echo "ready. Next: sh tools/build_all.sh"
