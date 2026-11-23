#!/usr/bin/env python3
"""Read and rebuild MPC OS 3.10.1+ update images (the "AZ01" container, format V2, as Akai's own az01-image reads it).

    az01img.py info    <image.img>                       header fields, the partition, its SHA-1 checked
    az01img.py extract <image.img> <rootfs.xz>           the rootfs partition as stored (xz)
    az01img.py build   <image.img> <rootfs.xz> <out.img> the same image with a new rootfs partition: every header
                                                         byte kept except the partition's length and SHA-1, and
                                                         the 20-byte end marker kept
    az01img.py selftest <image.img>                      extract + build gives back the identical file

Layout (little-endian u32s, strings as u32 length + text + NUL, padded to 4): "AZ01", 1, offset of the partition table
(0x108), the version string, the compatible strings (count + strings), the USB ids (count + u32s), the description,
then "PARTL\\0\\0\\0", the partition length (u64), its name ("rootfs"), compression ("xz"), hash count, per hash its
algorithm ("sha1") and value (u32 length + bytes); then the partition data; then the end marker (u32 0, "EOF\\0",
u32 16, 12 zero bytes in Akai's 3.10.1 image). Unsigned: az01-image --signing-keys lists none."""
import hashlib, struct, sys


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def rd_str(d, o):
    n = u32(d, o)
    s = d[o + 4:o + 4 + n].rstrip(b"\0").decode("latin-1")
    return s, o + 4 + ((n + 4) & ~3)   # (a NUL after the text, then padded to 4)


def parse(d):
    if d[:4] != b"AZ01":
        sys.exit("not an AZ01 image")
    h = {"version_word": u32(d, 4), "table": u32(d, 8)}
    o = 12
    h["version"], o = rd_str(d, o)
    n = u32(d, o); o += 4
    h["compatible"] = []
    for _ in range(n):
        s, o = rd_str(d, o)
        h["compatible"].append(s)
    n = u32(d, o); o += 4
    h["usb"] = ["%08x" % u32(d, o + 4 * i) for i in range(n)]
    o += 4 * n
    h["description"], o = rd_str(d, o)
    if o != h["table"] or d[o:o + 5] != b"PARTL":
        sys.exit("unexpected layout: partition table at %d, expected %d" % (o, h["table"]))
    o += 8
    h["len_at"] = o
    h["length"] = struct.unpack_from("<Q", d, o)[0]
    o += 8
    h["name"], o = rd_str(d, o)
    h["compression"], o = rd_str(d, o)
    nh = u32(d, o); o += 4
    if nh != 1:
        sys.exit("expected one hash, found %d" % nh)
    h["algo"], o = rd_str(d, o)
    hl = u32(d, o)
    h["hash_at"], h["hash_len"] = o + 4, hl
    h["hash"] = d[o + 4:o + 4 + hl].hex()
    o += 4 + ((hl + 3) & ~3)
    h["data_at"] = o
    h["tail"] = d[o + h["length"]:]
    if h["algo"] != "sha1" or hl != 20 or len(h["tail"]) != 20 or h["tail"][4:8] != b"EOF\0":
        sys.exit("unexpected hash or end marker")
    return h


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("info", "extract", "build", "selftest"):
        sys.exit(__doc__)
    d = open(sys.argv[2], "rb").read()
    h = parse(d)
    part = d[h["data_at"]:h["data_at"] + h["length"]]
    if cmd == "info":
        for k in ("version", "description", "compatible", "usb", "name", "compression", "length", "algo", "hash"):
            print("%-12s %s" % (k, h[k]))
        print("%-12s %s" % ("sha1 check", "ok" if hashlib.sha1(part).hexdigest() == h["hash"] else "BAD"))
        return
    if cmd == "extract":
        open(sys.argv[3], "wb").write(part)
        return
    new = open(sys.argv[3], "rb").read() if cmd == "build" else part
    out = bytearray(d[:h["data_at"]])
    struct.pack_into("<Q", out, h["len_at"], len(new))
    out[h["hash_at"]:h["hash_at"] + 20] = hashlib.sha1(new).digest()
    out += new + h["tail"]
    if cmd == "selftest":
        print("selftest:", "identical" if bytes(out) == d else "DIFFERENT")
        sys.exit(0 if bytes(out) == d else 1)
    open(sys.argv[4], "wb").write(out)
    print("wrote %s (%d bytes), %s sha1 %s" % (sys.argv[4], len(out), h["name"], hashlib.sha1(new).hexdigest()))


if __name__ == "__main__":
    main()
