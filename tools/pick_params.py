#!/usr/bin/env python3
"""params.json + the full-page list parameters (tools/picklist.h's pl_params_json, for pages whose params.json is
written by hand).   pick_params.py <params.json> <key>=<title>... > out.json"""
import json
import sys

d = json.load(open(sys.argv[1]))
ps = d["params"] if isinstance(d, dict) else d
lists = [a.split("=", 1) for a in sys.argv[2:]]
ps += [{"key": k + "__list", "name": t + " List", "options": ["Closed", "Open"]} for k, t in lists]
ps += [{"key": "pick_%d" % (r + 1), "name": "List %d" % (r + 1), "options": ["Off", "On"], "display": "string"} for r in range(24)]
ps += [{"key": "pick_" + k, "name": "List " + n, "options": ["Off", "On"], "momentary": True}
       for k, n in (("prev", "<"), ("next", ">"), ("close", "Close"))]
ps.append({"key": "pick_title", "name": "List Title", "options": ["Off", "On"], "display": "string"})
json.dump(d, sys.stdout, indent=2)
print()
