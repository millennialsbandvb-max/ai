#!/usr/bin/env python3
"""Read and rebuild MPC OS update images in the device-tree (FIT) format, e.g. MPC-3.9.1-Gen1-update.img.

    mpcfit.py info    <image.img>
    mpcfit.py extract <image.img> <rootfs.xz>             the rootfs partition exactly as stored (xz-compressed)
    mpcfit.py build   <image.img> <rootfs.xz> <out.img>   the same image with a new rootfs partition and its hash

An image is a flattened device tree: root properties (description, compatible, inmusic,devices, inmusic,version,
timestamp) and /images/rootfs with `data` (the xz rootfs), `compression`, and /images/rootfs/hash (`algo` = sha1,
`value` = SHA-1 of `data` as stored). `build` keeps every other byte of the tree as it was, so building from an
unmodified extract gives back the identical file (checked by `selftest`):

    mpcfit.py selftest <image.img>
"""
import hashlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


def align4(n):
    return (n + 3) & ~3


class Fdt:
    def __init__(self, blob):
        (magic, self.totalsize, off_struct, off_strings, off_rsvmap, self.version, self.last_comp,
         self.boot_cpu, size_strings, size_struct) = struct.unpack(">10I", blob[:40])
        if magic != FDT_MAGIC:
            raise SystemExit("not a device-tree image (bad magic)")
        if self.version != 17:
            raise SystemExit("unsupported device-tree version %d" % self.version)
        if self.totalsize != len(blob):
            raise SystemExit("image is truncated or has trailing data (%d bytes, header says %d)" % (len(blob), self.totalsize))
        self.blob = blob
        self.off_struct, self.size_struct = off_struct, size_struct
        self.off_strings, self.size_strings = off_strings, size_strings
        self.off_rsvmap = off_rsvmap
        self.props = {}   # "/path:name" -> (offset of the value in blob, length)
        self._walk()

    def _string(self, off):
        s = self.blob[self.off_strings + off:self.blob.index(b"\0", self.off_strings + off)]
        return s.decode()

    def _walk(self):
        p, end, path = self.off_struct, self.off_struct + self.size_struct, []
        while p < end:
            tag, = struct.unpack(">I", self.blob[p:p + 4])
            p += 4
            if tag == BEGIN_NODE:
                e = self.blob.index(b"\0", p)
                path.append(self.blob[p:e].decode())
                p = align4(e + 1)
            elif tag == END_NODE:
                path.pop()
            elif tag == PROP:
                ln, nameoff = struct.unpack(">II", self.blob[p:p + 8])
                p += 8
                key = ("/".join(path) or "/") + ":" + self._string(nameoff)
                self.props[key.replace("//", "/")] = (p, ln)
                p = align4(p + ln)
            elif tag == NOP:
                pass
            elif tag == END:
                break
            else:
                raise SystemExit("bad device-tree tag %d" % tag)

    def get(self, key):
        off, ln = self.props[key]
        return self.blob[off:off + ln]

    def text(self, key):
        return " ".join(s.decode() for s in self.get(key).split(b"\0") if s)


DATA, HASH, ALGO = "/images/rootfs:data", "/images/rootfs/hash:value", "/images/rootfs/hash:algo"


def load(path):
    f = Fdt(open(path, "rb").read())
    for k in (DATA, HASH, ALGO):
        if k not in f.props:
            raise SystemExit("%s: no %s: not an MPC update image in the expected layout" % (path, k))
    if f.text(ALGO) != "sha1":
        raise SystemExit("unexpected hash algorithm %r" % f.text(ALGO))
    if f.text("/images/rootfs:compression") != "xz":
        raise SystemExit("unexpected compression %r" % f.text("/images/rootfs:compression"))
    if hashlib.sha1(f.get(DATA)).digest() != f.get(HASH):
        raise SystemExit("%s: the rootfs hash doesn't match: the file is damaged" % path)
    return f


def info(path):
    f = load(path)
    for k in ("/:description", "/:inmusic,version", "/:compatible", "/images/rootfs:description",
              "/images/rootfs:compression"):
        if k in f.props:
            print("%-28s %s" % (k, f.text(k)))
    print("%-28s %s" % ("/:inmusic,devices", " ".join("%08x" % d for d in struct.unpack(">%dI" % (len(f.get("/:inmusic,devices")) // 4), f.get("/:inmusic,devices")))))
    print("%-28s %d bytes" % ("rootfs data", len(f.get(DATA))))
    print("%-28s %s (verified)" % ("rootfs sha1", f.get(HASH).hex()))
    print("%-28s %s" % ("properties", " ".join(sorted(f.props))))


def build(path, data_path, out):
    f = load(path)
    data = open(data_path, "rb").read()
    doff, dlen = f.props[DATA]
    hoff, hlen = f.props[HASH]
    if hoff < doff:
        raise SystemExit("unexpected layout: hash before data")
    b = f.blob
    # the data property: its length word sits 8 bytes before the value; the tree after it moves by the size change
    delta = align4(len(data)) - align4(dlen)
    out_blob = bytearray()
    out_blob += b[:doff - 8]
    out_blob += struct.pack(">I", len(data)) + b[doff - 4:doff]
    out_blob += data + b"\0" * (align4(len(data)) - len(data))
    out_blob += b[doff + align4(dlen):]
    # the new hash
    nh = hoff + delta
    out_blob[nh:nh + hlen] = hashlib.sha1(data).digest()
    # header: total size, and the offsets/sizes of blocks after the data
    if not (f.off_rsvmap < f.off_struct < f.off_strings):
        raise SystemExit("unexpected block order")
    struct.pack_into(">I", out_blob, 4, f.totalsize + delta)
    struct.pack_into(">I", out_blob, 12, f.off_strings + delta)
    struct.pack_into(">I", out_blob, 36, f.size_struct + delta)
    open(out, "wb").write(out_blob)
    load(out)   # re-parse and re-verify what we wrote
    print("wrote %s (%d bytes), rootfs sha1 %s" % (out, len(out_blob), hashlib.sha1(data).hexdigest()))


def main(a):
    if len(a) == 3 and a[1] == "info":
        info(a[2])
    elif len(a) == 4 and a[1] == "extract":
        open(a[3], "wb").write(load(a[2]).get(DATA))
    elif len(a) == 5 and a[1] == "build":
        build(a[2], a[3], a[4])
    elif len(a) == 3 and a[1] == "selftest":
        import os, tempfile
        d = tempfile.mkdtemp()
        open(os.path.join(d, "r.xz"), "wb").write(load(a[2]).get(DATA))
        build(a[2], os.path.join(d, "r.xz"), os.path.join(d, "o.img"))
        same = open(os.path.join(d, "o.img"), "rb").read() == open(a[2], "rb").read()
        for n in ("r.xz", "o.img"):
            os.remove(os.path.join(d, n))
        os.rmdir(d)
        print("selftest:", "identical rebuild" if same else "REBUILD DIFFERS")
        sys.exit(0 if same else 1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
