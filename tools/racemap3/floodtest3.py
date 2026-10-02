"""Fill the Atlantis channel with 'water' (flood fill) up to WATER_Y: reports volume and any leak.

    python floodtest3.py [--sections]
"""
import os
import sys
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402

import build3  # noqa: E402
from core import MAT, kenc  # noqa: E402
from layout3 import WATER_Y  # noqa: E402


def run(ctx=None, zones=("atlantis",)):
    if ctx is None:
        ctx = build3.build_zones(list(zones))
    W, B = ctx.W, ctx.B
    ch0, ch1 = ctx.marks["ch0"], ctx.marks["ch1"]
    s_seed = 0.5 * (ch0 + ch1)
    i = B.i(s_seed)
    seed = (int(np.floor(B.p[i, 0] + 0.5)), 6, int(np.floor(B.p[i, 1] + 0.5)))
    passable = ("Plant_", "Deco_")
    solid = set()
    for k, sid in W.d.items():
        name = MAT[W.specs[sid][0]][0]
        if (not name.startswith(passable)) or name.startswith("Plant_Coral_Block"):
            solid.add(k)
    # bounding box of the channel path with margin
    seg = B.p[B.i(ch0):B.i(ch1)]
    x0, x1 = int(seg[:, 0].min()) - 60, int(seg[:, 0].max()) + 60
    z0, z1 = int(seg[:, 1].min()) - 60, int(seg[:, 1].max()) + 60
    if seed_in_solid := (kenc(*seed) in solid):
        print("seed is solid at", seed)
    seen = {seed}
    stack = [seed]
    leak = None
    while stack:
        x, y, z = stack.pop()
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            n = (x + dx, y + dy, z + dz)
            if n in seen:
                continue
            if n[1] < 0 or n[1] > WATER_Y:
                continue
            if not (x0 <= n[0] <= x1 and z0 <= n[2] <= z1):
                leak = n
                continue
            if kenc(*n) in solid:
                continue
            seen.add(n)
            stack.append(n)
    xs = np.array([p[0] for p in seen])
    zs = np.array([p[2] for p in seen])
    print("water volume %d blocks, fill bbox x[%d..%d] z[%d..%d]" % (len(seen), xs.min(), xs.max(), zs.min(), zs.max()))
    print("LEAK at", leak)
    # expected volume: cells between the dams of the channel
    return len(seen), leak


if __name__ == "__main__":
    run()


def analyse():
    ctx = build3.build_zones(["atlantis"])
    W, B = ctx.W, ctx.B
    ch0, ch1 = ctx.marks["ch0"], ctx.marks["ch1"]
    s_seed = 0.5 * (ch0 + ch1)
    i = B.i(s_seed)
    seed = (int(np.floor(B.p[i, 0] + 0.5)), 6, int(np.floor(B.p[i, 1] + 0.5)))
    passable = ("Plant_", "Deco_")
    solid = set()
    for k, sid in W.d.items():
        name = MAT[W.specs[sid][0]][0]
        if (not name.startswith(passable)) or name.startswith("Plant_Coral_Block"):
            solid.add(k)
    seg = B.p[B.i(ch0):B.i(ch1)]
    x0, x1 = int(seg[:, 0].min()) - 60, int(seg[:, 0].max()) + 60
    z0, z1 = int(seg[:, 1].min()) - 60, int(seg[:, 1].max()) + 60
    seen = {seed}
    order = [seed]
    stack = [seed]
    while stack:
        x, y, z = stack.pop()
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            n = (x + dx, y + dy, z + dz)
            if n in seen or n[1] < 0 or n[1] > WATER_Y or not (x0 <= n[0] <= x1 and z0 <= n[2] <= z1) or kenc(*n) in solid:
                continue
            seen.add(n)
            stack.append(n)
    P = np.array(list(seen))
    # distance to centre line over the channel samples
    pts = B.p[B.i(ch0 - 40):B.i(ch1 + 40)]
    ss = B.s[B.i(ch0 - 40):B.i(ch1 + 40)]
    out = []
    for (x, y, z) in P[::7]:
        d2 = (pts[:, 0] - x) ** 2 + (pts[:, 1] - z) ** 2
        j = int(np.argmin(d2))
        out.append((float(np.sqrt(d2[j])), float(ss[j]), x, y, z))
    out = np.array(out)
    far = out[out[:, 0] > 26]
    print("filled sample cells", len(out), " far (>26 from the lane):", len(far))
    if len(far):
        hist = collections.Counter(int(v) // 40 * 40 for v in far[:, 1])
        print("by path position:", sorted(hist.items()))
        print("some far cells:", far[:8].tolist())


if __name__ == "__main__" and "--analyse" in sys.argv:
    analyse()
