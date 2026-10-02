"""Fill the Abyss basin with 'water' (flood fill) up to WATER_Y and report leaks / volume."""
import os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import build, core
from core import kenc, MAT
from layout import WATER_Y

ctx = build.main(["abyss"])
W, A = ctx.W, ctx.A
m = A.marks
s_seed = m["abyss"] + 200
i = A.i(s_seed)
q = A.p[i] + A.r[i] * 12
seed = (int(np.floor(q[0] + .5)), 6, int(np.floor(q[1] + .5)))
solid = set()
passable = ("Plant_", "Deco_")
for k, sid in W.d.items():
    name = MAT[W.specs[sid][0]][0]
    if (not name.startswith(passable)) or name.startswith("Plant_Coral_Block"):
        solid.add(k)
x0, x1, z0, z1 = -60, 520, 580, 780
seen = {seed}
stack = [seed]
leak = None
while stack:
    x, y, z = stack.pop()
    for dx, dy, dz in ((1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)):
        n = (x+dx, y+dy, z+dz)
        if n in seen: continue
        if n[1] < 0 or n[1] > WATER_Y: continue
        if not (x0 <= n[0] <= x1 and z0 <= n[2] <= z1):
            leak = n; continue
        if kenc(*n) in solid: continue
        seen.add(n); stack.append(n)
print("water volume %d blocks, bbox of fill x[%d..%d] y[%d..%d] z[%d..%d]" % (len(seen), min(p[0] for p in seen), max(p[0] for p in seen), min(p[1] for p in seen), max(p[1] for p in seen), min(p[2] for p in seen), max(p[2] for p in seen)))
print("LEAK at", leak)
# is the fill confined to y<=WATER_Y? cells at top layer that touch air above and open sideways?

# where does the water go?  bucket the filled cells by x
xs = np.array([p[0] for p in seen]); zs = np.array([p[2] for p in seen]); ys = np.array([p[1] for p in seen])
for lo in range(-60, 520, 40):
    sel = (xs >= lo) & (xs < lo + 40)
    if sel.any():
        print("x %4d..%4d: %7d cells, z %d..%d, y %d..%d" % (lo, lo + 39, sel.sum(), zs[sel].min(), zs[sel].max(), ys[sel].min(), ys[sel].max()))
print("channel ends: entry plug s=%.0f exit plug s=%.0f" % (m["abyss"] + 58, m["abyss"] + 495), "p_entry", A.p[A.i(m["abyss"]+58)], "p_exit", A.p[A.i(m["abyss"]+495)])
