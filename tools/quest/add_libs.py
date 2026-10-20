"""add_libs.py base.apk stage_dir out.apk — copy base.apk (from aapt2) and add
stage_dir/lib/** (native libraries) to it."""
import os
import sys
import zipfile

base, stage, out = sys.argv[1:4]
with zipfile.ZipFile(base) as zi, zipfile.ZipFile(out, 'w') as zo:
    for item in zi.infolist():
        zo.writestr(item, zi.read(item.filename))
    for dirpath, _, files in os.walk(os.path.join(stage, 'lib')):
        for f in sorted(files):
            full = os.path.join(dirpath, f)
            arc = os.path.relpath(full, stage).replace(os.sep, '/')
            zo.write(full, arc, compress_type=zipfile.ZIP_DEFLATED)
print('wrote', out)
