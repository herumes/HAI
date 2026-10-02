"""Writes out/big_test.js: the cached v3 world in the compact format the C++ side writes for bigview.html (first block name of a material list)."""
import base64, importlib, json, os, pickle, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import numpy as np
import build3
from core import MAT, OX, OY, OZ
for z in build3.ZONES: importlib.import_module("z_" + z)
with open(os.path.join(HERE, "out", "world.pkl"), "rb") as f:
    d, specs, spec_ids, surf, zc, marks = pickle.load(f)
keys = np.fromiter(d.keys(), dtype=np.int64, count=len(d)); vals = np.fromiter(d.values(), dtype=np.int32, count=len(d))
X = (keys & 0x3FFF) - OX; Z = ((keys >> 14) & 0x3FFF) - OZ; Y = (keys >> 28) - OY
names = []; nid_of_spec = {}; rot_of_spec = {}
for si, (m, rot) in enumerate(specs):
    nm = MAT[m][0]
    if nm not in names: names.append(nm)
    nid_of_spec[si] = names.index(nm); rot_of_spec[si] = 255 if rot is None else rot
nid = np.array([nid_of_spec[i] for i in range(len(specs))], dtype=np.uint16)[vals]
rot = np.array([rot_of_spec[i] for i in range(len(specs))], dtype=np.uint8)[vals]
rec = np.zeros((len(X), 9), dtype=np.uint8)
for c, arr in enumerate((X, Y, Z)):
    a = arr.astype("<i2").view(np.uint8).reshape(-1, 2); rec[:, c * 2:c * 2 + 2] = a
rec[:, 6:8] = nid.astype("<u2").view(np.uint8).reshape(-1, 2); rec[:, 8] = rot
b64 = base64.b64encode(rec.tobytes()).decode()
open(os.path.join(HERE, "out", "big_test.js"), "w").write('window.HYBIG={"name":"v3 test","n":%d,"names":%s,"data":"%s"};\n' % (len(X), json.dumps(names), b64))
print(len(X), "blocks", len(names), "names", len(b64) // 1000000, "MB base64")
