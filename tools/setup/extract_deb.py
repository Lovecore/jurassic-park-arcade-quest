#!/usr/bin/env python3
"""Unpack a .deb's data archive into a directory (no `ar` or dpkg needed).

  python tools/setup/extract_deb.py <package.deb> <out dir>

A .deb is an `ar` archive holding debian-binary, control.tar.* and
data.tar.* (gz, xz or bz2); the file tree is in data.tar.*.
"""
import io
import os
import sys
import tarfile


def ar_members(blob):
    if blob[:8] != b"!<arch>\n":
        raise SystemExit("not an ar archive")
    off = 8
    while off + 60 <= len(blob):
        hdr = blob[off:off + 60]
        name = hdr[:16].decode().strip().rstrip("/")
        size = int(hdr[48:58].decode().strip())
        yield name, blob[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)


def main():
    deb, out = sys.argv[1], sys.argv[2]
    blob = open(deb, "rb").read()
    for name, data in ar_members(blob):
        if name.startswith("data.tar"):
            os.makedirs(out, exist_ok=True)
            links = []
            with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as t:
                for m in t.getmembers():
                    p = os.path.normpath(m.name)          # no absolute paths or ..
                    if p.startswith("..") or os.path.isabs(p):
                        raise SystemExit(f"unsafe path in {deb}: {m.name}")
                    if m.issym() or m.islnk():
                        links.append(m)
                    else:
                        try:
                            t.extract(m, out, filter="tar")     # Python 3.12+: no metadata surprises
                        except TypeError:
                            t.extract(m, out)
            # links (libc.so.6 -> libc-2.19.so ...) become copies: Windows
            # needs no symlink rights that way
            for m in links:
                dst = os.path.join(out, os.path.normpath(m.name))
                if m.issym():
                    src = os.path.normpath(os.path.join(os.path.dirname(dst), m.linkname))
                else:
                    src = os.path.join(out, os.path.normpath(m.linkname))
                if os.path.isfile(src) and not os.path.exists(dst):
                    with open(src, "rb") as fi, open(dst, "wb") as fo:
                        fo.write(fi.read())
            return
    raise SystemExit(f"no data.tar in {deb}")


if __name__ == "__main__":
    main()
