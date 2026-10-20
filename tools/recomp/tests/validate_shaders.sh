#!/bin/sh
# Translate dumped game shaders (JPRT_SHADER_DUMP) to GLSL ES 3.20 and
# validate them with glslang. Usage: validate_shaders.sh <dump dir> <work dir>
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DUMP=$1
WORK=$2
CC="$(ls -d "$ROOT"/ext/llvm-mingw-*/bin | tail -1)/x86_64-w64-mingw32-clang"
G="$ROOT/ext/glslang/bin/glslang.exe"
mkdir -p "$WORK"
"$CC" -O1 -Wall -I"$HERE/rt" "$HERE/tests/glsl_es_test.c" "$HERE/rt/glsl_es.c" -o "$WORK/glsl_es_test.exe" || exit 1
ok=0; bad=0
for f in "$DUMP"/*.glsl; do
    b=$(basename "$f" .glsl)
    if grep -q "gl_Position" "$f"; then st=vert; else st=frag; fi
    "$WORK/glsl_es_test.exe" "$f" "$WORK/$b.$st"
    if "$G" "$WORK/$b.$st" > "$WORK/$b.$st.log" 2>&1; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
done
echo "ok=$ok bad=$bad"
cat "$WORK"/*.log | grep "ERROR: [^ ]*:[0-9]" | sed -E 's/ERROR: [^:]*:[0-9]+: //' | sort | uniq -c | sort -rn | head -20
