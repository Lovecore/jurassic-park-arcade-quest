#!/usr/bin/env python3
"""Symbolize `host frame` lines of a jprt.log against jp2015.exe and the
lifted modules' function names.  python tools/recomp/symbolize.py [jprt.log]"""
import bisect, glob, json, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
exe = os.path.join(OUT, "process", "jp2015.exe")
nm = glob.glob(os.path.join(HERE, "..", "..", "ext", "llvm-mingw-*", "bin", "llvm-nm.exe"))[0]
symbolizer = nm.replace("llvm-nm.exe", "llvm-symbolizer.exe")
BASES = {"game": 0}
for fn in glob.glob(os.path.join(OUT, "*functions.json")):
    j = json.load(open(fn))
    BASES[j.get("module", "game")] = j.get("base", 0)

def guest_of(host_addr):
    """#line markers in the lifted C: file = module, line = offset."""
    r = subprocess.run([symbolizer, "--obj=" + exe, hex(host_addr)], capture_output=True, text=True).stdout
    for l in r.splitlines():
        m = re.match(r"(.+):(\d+):\d+$", l.strip())
        if m and os.path.basename(m.group(1)) in BASES:
            mod = os.path.basename(m.group(1))
            return f"{mod}+{int(m.group(2)):#x} = guest {BASES[mod] + int(m.group(2)):08x}"
    return ""
syms = []
for l in subprocess.run([nm, "-n", "--defined-only", exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 3:
        try:
            syms.append((int(p[0], 16), p[2]))
        except ValueError:
            pass
syms.sort()
A = [a for a, _ in syms]
names = {}
for fn in glob.glob(os.path.join(OUT, "*functions.json")):
    for f in json.load(open(fn))["functions"]:
        names[f["start"]] = f["name"]
log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT, "run", "jprt.log")
for line in open(log, errors="replace"):
    m = re.search(r"host frame\s+\d+: (\S+)\+(0x[0-9a-f]+)", line)
    if not m:
        continue
    if m.group(1) != "jp2015.exe":
        print(f"  {m.group(1)}+{m.group(2)}")
        continue
    a = 0x140000000 + int(m.group(2), 16)
    i = bisect.bisect_right(A, a) - 1
    s = syms[i][1]
    g = names.get(int(s[2:], 16), "") if s.startswith("f_") else ""
    print(f"  {s}+{a - syms[i][0]:#x}  {g}  {guest_of(a)}")
