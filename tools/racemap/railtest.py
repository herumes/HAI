import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from core import World
import kart
from kartroad import KartStyle, stamp_kart_road
from path import Path, arc, concat
W = World()
pts = concat([(0.0, 0.0), (20.0, 0.0)], arc((20.0, 25.0), 25.0, -90, 0, 0.25)[1:], [(45.0, 25.0), (45.0, 45.0)], arc((20.0, 45.0), 25.0, 0, 90, 0.25)[1:], [(20.0, 70.0), (0.0, 70.0)])
P = Path(pts)
P.T[:] = 0
st = KartStyle(hw=3.5, rail="armco", lanes=(0.0,), curbs=("k_curb_red",), curb_rmax=100.0)
stamp_kart_road(W, P, 0, len(P) - 1, st)
xs = [x for (x, y, z) in (__import__('core').kdec(k) for k in W.d)]
zs = [z for (x, y, z) in (__import__('core').kdec(k) for k in W.d)]
x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
grid = {}
from core import kdec
for k, sid in W.d.items():
    x, y, z = kdec(k)
    m, rot = W.specs[sid]
    if m in ("k_armco", "k_armco_c"):
        grid[(x, z)] = ("-" if rot == 0 else "|") if m == "k_armco" else "ABCD"[rot]
    elif m == "k_curb_red" and (x, z) not in grid:
        grid[(x, z)] = "c"
    elif m == "k_asphalt" and (x, z) not in grid:
        grid[(x, z)] = "."
    elif m == "k_conc" and (x, z) not in grid:
        grid[(x, z)] = ","
for z in range(z0, z1 + 1):
    print("".join(grid.get((x, z), " ") for x in range(x0, x1 + 1)))
