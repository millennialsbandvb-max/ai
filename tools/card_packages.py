#!/usr/bin/env python3
"""Card versions of our plugins, for MPC OS 3.10.1+ External Plugin Support (Akai's MPC plugin SDK format:
<drive>/VST2 Plugins/<Manufacturer> - VST - <Name>/ with plugin.json, version.xml, VST/<slug>-arm32.so, Plugin Skins/).

  card_packages.py <firmware work/extra> <NAM-MPC.so> <NAM page folder> <out dir>

Reads the same MPC.settings entries the firmware uses (entries/*.xml: name, manufacturer, uid, inputs, outputs,
instrument), so the card folder names match the strings the binaries report (the MPC finds a plugin's pages by
them). Writes <out>/VST2 Plugins/... and <out>/Other-Things-card-plugins.zip. The plugins' MIDI from the firmware's
add-on (mpc-buttons.so, the weak symbol mpc_ext_midi_read) still works for them wherever they load from."""
import json, os, re, shutil, sys, zipfile
import xml.etree.ElementTree as ET

extra, nam_so, nam_page, out = sys.argv[1:5]
ROOT = os.path.join(out, "VST2 Plugins")
shutil.rmtree(ROOT, ignore_errors=True)
os.makedirs(ROOT)
BAD = set('"#@,;:<>*^|?\\/')   # the characters the MPC drops when it builds the folder name


def clean(s):
    return "".join(c for c in s if c not in BAD)


def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")


def package(entry_xml, so, page):
    p = ET.fromstring(entry_xml).attrib
    name, maker = p["name"], p["manufacturer"]
    folder = "%s - VST - %s" % (clean(maker), clean(name))
    if os.path.basename(page) != folder:
        sys.exit("page folder %r is not %r" % (os.path.basename(page), folder))
    d = os.path.join(ROOT, folder)
    os.makedirs(os.path.join(d, "VST"))
    binary = "VST/%s-arm32.so" % slug(name)
    shutil.copy2(so, os.path.join(d, binary))
    for f in os.listdir(page):   # Plugin Skins/ and version.xml
        src = os.path.join(page, f)
        (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, os.path.join(d, f))
    inst = p.get("isInstrument") == "1"
    meta = {"name": name, "manufacturer": maker, "uid": "0x" + p["uid"].lower(), "format": "vst2",
            "file32": binary, "kind": "instrument" if inst else "effect", "numInputs": int(p["numInputs"]),
            "numOutputs": int(p["numOutputs"]), "version": p.get("version", "1.0.0"),
            "category": p.get("category", "Synth" if inst else "Effect")}
    assert meta["numOutputs"] > 0 and len(meta["uid"]) <= 10
    with open(os.path.join(d, "plugin.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    return folder


made = []
nam_entry = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "nam-mpc", "firmware",
                              "entries-nam-mpc.xml")).read()
made.append(package(nam_entry, nam_so, nam_page))
for x in sorted(os.listdir(os.path.join(extra, "entries"))):
    so = os.path.join(extra, "plugins", x[:-4] + ".so")
    entry = open(os.path.join(extra, "entries", x)).read()
    page = os.path.join(extra, "skins", "%s - VST - %s" % (clean(ET.fromstring(entry).attrib["manufacturer"]),
                                                          clean(ET.fromstring(entry).attrib["name"])))
    made.append(package(entry, so, page))

z = os.path.join(out, "Other-Things-card-plugins.zip")
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    for base, _, files in os.walk(ROOT):
        for f in sorted(files):
            full = os.path.join(base, f)
            zf.write(full, os.path.relpath(full, out))
print("card plugins (%d): %s" % (len(made), ", ".join(m.split(" - VST - ")[1] for m in made)))
print("wrote %s (%d bytes)" % (z, os.path.getsize(z)))
