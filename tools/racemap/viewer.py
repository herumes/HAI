"""python viewer.py <name> x0 x1 z0 z1 [az el scale y0 y1] after building zones -> out/<name>.png
(runs the zones given in env ZONES, default all built zones)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build, render

def run(zones, shots):
    ctx = build.main(zones)
    for name, bbox, az, el, scale in shots:
        out = os.path.join(build.OUT_DIR, name + ".png")
        print(name, render.iso(ctx.W, out, bbox, az, el, scale))
    return ctx
