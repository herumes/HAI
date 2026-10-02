"""python shots_zone.py zone[,zone] name x0 x1 z0 z1 [scale] [iso az el isoscale x0 x1 z0 z1]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build, render
zones = sys.argv[1].split(",")
name = sys.argv[2]
bb = [int(v) for v in sys.argv[3:7]]
scale = float(sys.argv[7]) if len(sys.argv) > 7 else 3
ctx = build.main(zones)
print(render.topdown(ctx.W, os.path.join(build.OUT_DIR, name + "_top.png"), bbox=bb, scale=scale))
if len(sys.argv) > 12:
    az, el, sc = float(sys.argv[8]), float(sys.argv[9]), float(sys.argv[10])
    ib = [int(v) for v in sys.argv[11:15]]
    print(render.iso(ctx.W, os.path.join(build.OUT_DIR, name + "_iso.png"), ib, az, el, sc))
