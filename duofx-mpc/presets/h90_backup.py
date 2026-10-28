#!/usr/bin/env python3
"""Pull the programs out of an H90 backup file (Eventide Control's "Backup", .h90):
   h90_backup.py <backup.h90> > h90_backup.json
The backup is a FlatBuffers file (no schema published; layout worked out from one backup): a list of programs, each
with a header (name, author, its two presets' names and algorithm ids) and per slot either named parameters or the
H9-family algorithms' settings as base64 JSON. Only the facts we use are kept, for Eventide's factory programs only: names, algorithms, numbers.
The routing isn't decoded with certainty: the header's byte at field 5 is 3 on the one program described as parallel
("Floating in Space"), so 3 is read as parallel."""
import json, struct, sys
d = open(sys.argv[1], "rb").read()
u32 = lambda p: struct.unpack_from("<I", d, p)[0]
i32 = lambda p: struct.unpack_from("<i", d, p)[0]
u16 = lambda p: struct.unpack_from("<H", d, p)[0]
f32 = lambda p: struct.unpack_from("<f", d, p)[0]
def fields(t):
    vt = t - i32(t); vl = u16(vt)
    return [u16(vt + 4 + 2 * i) for i in range((vl - 4) // 2)]
def fld(t, i):
    o = fields(t); return t + o[i] if i < len(o) and o[i] else None
def ref(p): return p + u32(p)
def string(t, i):
    p = fld(t, i)
    if p is None: return None
    s = ref(p); n = u32(s); return d[s + 4:s + 4 + n].decode("utf-8", "replace")
def vec(t, i):
    p = fld(t, i)
    if p is None: return []
    v = ref(p); return [v + 4 + 4 * k for k in range(u32(v))]
def tab(t, i):
    p = fld(t, i); return ref(p) if p is not None else None
def value(t):  # {1: type, 2: table{0: value}}
    typ = d[fld(t, 1)] if fld(t, 1) is not None else 0
    vt = tab(t, 2)
    if vt is None: return None
    p = fld(vt, 0)
    if p is None: return 0
    return round(f32(p), 5) if typ == 1 else i32(p)
root = ref(0)
progs = []
for pp in vec(root, 0):
    pt = ref(pp)
    h = tab(pt, 0)
    name = string(h, 1)
    if name == "INIT Program": continue
    meta = tab(h, 11)
    o5 = fld(h, 5)
    prog = {"name": name, "author": string(meta, 1) if meta else None, "route": d[o5] if o5 is not None else None}
    pa = tab(pt, 1)
    prog["params"] = {}
    for q in vec(pa, 0):
        e = ref(q); idx = i32(fld(e, 0)) if fld(e, 0) is not None else 0
        prog["params"][str(idx)] = value(e)
    for slot, hk, bk in (("A", 3, 2), ("B", 4, 3)):
        ph = tab(h, hk); b = tab(pt, bk)
        pr = {"name": string(ph, 1), "algo": string(ph, 3)[:8]}
        # field 0: float vector (H9-style knobs), field 5: named params
        if fld(b, 0) is not None:
            import base64
            try:
                pr["h9"] = json.loads(base64.b64decode(string(b, 0)))
            except Exception as ex:
                pr["h9err"] = str(ex)
        pr["named"] = {}
        for q in vec(b, 5):
            e = ref(q); nm = string(e, 0)
            vals = vec(e, 1)
            pr["named"][nm.replace("-obj", "")] = value(ref(vals[0])) if vals else None
        pr["slots"] = {}
        for q in vec(b, 1):
            e = ref(q); idx = i32(fld(e, 0)) if fld(e, 0) is not None else 0
            pr["slots"][idx] = value(e)
        prog[slot] = pr
    progs.append(prog)
ALGS = {"2375b086": "SP2016 Reverb", "ab92390e": "WeedWacker", "106962ad": "Even-Vibe", "9f826985": "Polyphony",
        "31f22e07": None}
SKIP = ("in1_sens", "in2_sens", "out1_sens", "out2_sens", "killdry", "slow_mode", "bypt_normal", "expression_pedal",
        "version", "product_id", "preset_name", "algorithm_name", "x_switch", "y_switch", "z_switch")
keep = []
for p in progs:
    if p["author"] != "Eventide":   # (factory programs only: the owner's own and others' stay private)
        continue
    slots = []
    for s in "AB":
        x = p[s]
        algo = x.get("h9", {}).get("algorithm_name") or ALGS.get(x["algo"], "?" + x["algo"])
        if algo is None:
            slots.append(None); continue
        par = {k: v for k, v in x.get("h9", {}).items()
               if k not in SKIP and not k.endswith(("_exp", "_hot_switch")) and not isinstance(v, str)}
        par.update({k.split(":")[0]: v for k, v in x["named"].items()})
        slots.append({"preset": x["name"], "algorithm": algo, "params": par})
    keep.append({"name": p["name"], "factory": p["author"] == "Eventide", "parallel": p.get("route") == 3,
                 "tempo": p["params"].get("23"), "A": slots[0], "B": slots[1]})
json.dump(keep, sys.stdout, indent=1)
print()
