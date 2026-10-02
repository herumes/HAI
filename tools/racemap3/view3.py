"""Quick looks at a zone: builds the named zones and writes top-down / oblique / section PNGs into out/.

    python view3.py station                      top-down of the whole zone + one oblique
    python view3.py station iso x0 x1 z0 z1 [az el scale name]
    python view3.py station top x0 x1 z0 z1 [scale name]
    python view3.py station sec A s [umax y0 y1]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build3  # noqa: E402
import render  # noqa: E402

OUT = os.path.join(HERE, "out")


def main(argv):
    zones = argv[0].split(",")
    mode = argv[1] if len(argv) > 1 else "top"
    args = argv[2:]
    ctx = build3.make_ctx()
    import importlib
    import numpy as np
    if zones == ["cached"]:
        import pickle
        for z in build3.ZONES:
            importlib.import_module("z_" + z)
        with open(os.path.join(OUT, "world.pkl"), "rb") as f:
            ctx.W.d, ctx.W.specs, ctx.W.spec_ids, ctx.W.surf, ctx.W.zcount, ctx.marks = pickle.load(f)
    else:
        for z in zones:
            mod = importlib.import_module("z_" + z)
            ctx.rng = np.random.default_rng(1000 + build3.ZONES.index(z))
            with ctx.W.zone(z):
                mod.build(ctx)
            print("zone", z, ctx.W.zcount.get(z))
        build3.restore_roads(ctx)
        build3.clear_corridor(ctx)
    W = ctx.W
    if mode == "top":
        if args:
            x0, x1, z0, z1 = [int(a) for a in args[:4]]
            sc = float(args[4]) if len(args) > 4 else 1
            name = args[5] if len(args) > 5 else "view_top.png"
            print(render.topdown(W, os.path.join(OUT, name), bbox=(x0, x1, z0, z1), scale=sc))
        else:
            print(render.topdown(W, os.path.join(OUT, "view_top.png"), scale=1))
    elif mode == "iso":
        x0, x1, z0, z1 = [int(a) for a in args[:4]]
        az = float(args[4]) if len(args) > 4 else 35
        el = float(args[5]) if len(args) > 5 else 35
        sc = float(args[6]) if len(args) > 6 else 2
        name = args[7] if len(args) > 7 else "view_iso.png"
        y0 = int(args[8]) if len(args) > 8 else -5
        y1 = int(args[9]) if len(args) > 9 else 400
        print(render.iso(W, os.path.join(OUT, name), (x0, x1, z0, z1, y0, y1), az=az, el=el, scale=sc))
    elif mode == "sec":
        P = getattr(ctx, args[0])
        s = float(args[1])
        umax = int(args[2]) if len(args) > 2 else 40
        y0 = int(args[3]) if len(args) > 3 else -3
        y1 = int(args[4]) if len(args) > 4 else 40
        print(render.section(W, P, s, os.path.join(OUT, "view_sec.png"), umax=umax, y0=y0, y1=y1, scale=8))
    return ctx


if __name__ == "__main__":
    main(sys.argv[1:])
