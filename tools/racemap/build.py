"""Builds the whole world tour and writes the build script.

    python build.py [zone ...]            zones: start meadows canyon crystal abyss frost sky volcano rainbow
    python build.py --render              also write top-down / side PNGs into ./out
"""
import importlib
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import core
from core import World, emit, kenc, load_known_names
import layout
import render

NAME = "Galaxy Grand Prix - World Tour v2"
OUT_NAME = "galaxy_world_tour_v2.json"
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
KNOWN = os.path.join(ROOT, "tools", "ai_prefab_blocks.txt")
OUT_DIR = os.path.join(HERE, "out")
ZONES = ["start", "meadows", "canyon", "crystal", "abyss", "frost", "sky", "volcano", "rainbow"]

# assumed top speeds used to size the lap (blocks per second)
SPEED = {"kart": 45.0, "plane": 75.0, "sub": 38.0}


class Ctx:
    pass


def set_heights(A, B, S):
    m = A.marks

    def zone(a, b, keys):
        z0, z1 = m[a], m[b]
        return [(z0 + f * (z1 - z0), t) for f, t in keys]

    # the stadium road is flush with the ground (y = -1); one step up to 0 just before the Meadows terrain starts
    keys = [(0, -1), (270, -1), (318, 0), (m["meadows"], 0)]
    keys += zone("meadows", "canyon", [(0.06, 0), (0.22, 3), (0.34, 1), (0.48, 5), (0.6, 2), (0.72, 0), (0.84, 2), (1.0, 0)])[0:]
    keys += zone("canyon", "crystal", [(0.05, 0), (0.29, 14), (0.71, 14), (0.95, 0), (1.0, 0)])
    keys += zone("crystal", "abyss", [(0.15, 0), (0.45, 6), (0.75, 11), (1.0, 14)])
    keys += zone("abyss", "frost", [(0.1, 14), (0.28, 0), (0.78, 0), (0.98, 13), (1.0, 13)])
    keys += zone("frost", "end", [(0.4, 38), (0.75, 63), (0.93, 75), (1.0, 82)])
    # drop duplicate s and keep ascending
    clean = []
    for s, t in keys:
        if clean and s <= clean[-1][0] + 1e-6:
            continue
        clean.append((s, t))
    A.set_heights(clean)

    mb = B.marks
    B.set_heights([(0, 6), (mb["volcano"], 6), (mb["rainbow"], 50), (860, 56), (1000, 63), (1180, 46), (1290, 26), (B.length, -1)])

    ms = S.marks
    layout.set_altitude(S, [(0, 92), (ms["islands"], 104), (ms["helix"], 122), (ms["helix"] + 2 * np.pi * 60, 180), (ms["tunnel"] + 60, 172),
                            (S.length - 330, 110), (S.length - 120, 28), (S.length, 7)])


def clear_corridor(ctx):
    """Keep 3 blocks of head-room above every kart road (the sub lane and the runway apron aside)."""
    from path import raster_band
    W, A, B = ctx.W, ctx.A, ctx.B
    m = A.marks
    ch0, ch1 = m["abyss"] + 58, m["abyss"] + 495
    jobs = [(A, 0, A.i(ch0), 7.5, -1), (A, A.i(ch0), A.i(ch1), 7.5, 2), (A, A.i(ch1), len(A) - 1, 7.5, -1),
            (B, B.i(215), len(B) - 1, 7.5, -1), (B, 0, B.i(215), 11.5, -1)]
    n0 = len(W.d)
    for P, i0, i1, hw, tmin in jobs:
        cx, cz, si, uu = raster_band(P, i0, i1, -hw, hw)
        T = P.T[si]
        if tmin > 0:                       # the sub lane keeps its reef; only the climbing ramps are cleared
            keep = T >= tmin
            cx, cz, T = cx[keep], cz[keep], T[keep]
        for h in (2, 3, 4):
            W.erase_arr(cx, T + h, cz)
        # layer road+1 keeps only the Kart decals, stairs and props (k_*, n_*): anything else in the lane is an obstacle
        for x, y, z in zip(cx.tolist(), (T + 1).tolist(), cz.tolist()):
            sid = W.d.get(kenc(x, y, z))
            if sid is not None:
                nm = W.specs[sid][0]
                if not (nm.startswith("k_") or nm.startswith("n_")):
                    del W.d[kenc(x, y, z)]
    print("corridor clearing removed %d blocks" % (n0 - len(W.d)))


ROAD_OK = {"k_asphalt", "k_asph_stair", "k_conc", "k_conc_stair", "k_curb_yel_b", "k_curb_red_b", "k_finish_line", "k_checker"}


def restore_roads(ctx):
    """Terrain, rocks and crystals sometimes land on a road cell; put the stamped surface back."""
    W = ctx.W
    n = 0
    for k, sid in W.surf.items():
        cur = W.d.get(k)
        if cur == sid:
            continue
        if cur is not None:
            name = W.specs[cur][0]
            if name in ROAD_OK or name.startswith("n_"):
                continue
        W.d[k] = sid
        n += 1
    print("road surface restored on %d cells" % n)


def main(argv):
    do_render = "--render" in argv
    names = [a for a in argv if not a.startswith("--")]
    zones = names or ZONES
    t0 = time.time()
    ctx = Ctx()
    ctx.W = World()
    ctx.rng = np.random.default_rng(20260930)
    ctx.A, ctx.FC = layout.build_path_a()
    ctx.B, ctx.VC = layout.build_path_b()
    ctx.S, ctx.HC = layout.build_sky()
    set_heights(ctx.A, ctx.B, ctx.S)
    ctx.known = load_known_names(KNOWN)
    from path import raster_band
    keys = []
    for P in (ctx.A, ctx.B):
        cx, cz, si, uu = raster_band(P, 0, len(P) - 1, -15.0, 15.0)
        keys.append(((cx + 4096) | ((cz + 4096) << 14)).astype(np.int64))
    ctx.road_keys = np.unique(np.concatenate(keys))
    for z in zones:
        t1 = time.time()
        mod = importlib.import_module("zone_" + z)
        rng_seed = 1000 + ZONES.index(z)
        ctx.rng = np.random.default_rng(rng_seed)
        with ctx.W.zone(z):
            mod.build(ctx)
        print("zone %-9s %8d blocks  (%.1fs)" % (z, ctx.W.zcount.get(z, 0), time.time() - t1))
        if "--ticks" in argv:
            for zz, lab, n in ctx.W.ticks:
                if zz == z:
                    print("      %-22s %8d" % (lab, n))
    restore_roads(ctx)
    clear_corridor(ctx)
    # nothing below the ground surface (y = -1) - the world already has it
    below = [k for k in ctx.W.d if (k >> 28) - core.OY < -1]
    for k in below:
        del ctx.W.d[k]
    total = len(ctx.W.d)
    print("TOTAL blocks: %d   build time %.1fs" % (total, time.time() - t0))
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, OUT_NAME)
    emit(ctx.W, NAME, ctx.known, out_path=out)
    if do_render:
        print("render", render.topdown(ctx.W, os.path.join(OUT_DIR, "top.png"), scale=1))
    return ctx


if __name__ == "__main__":
    main(sys.argv[1:])
