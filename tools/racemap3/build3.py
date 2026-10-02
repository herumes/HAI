"""Builds the whole Cosmic Odyssey world and writes the build script.

    python build3.py [zone ...]          zones: station candy city tower skya dunes atlantis jungle keep glacier skyb ribbon
    python build3.py --render            also write top-down PNGs into ./out
    python build3.py --no-emit           build only (checks)
"""
import importlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import common3  # noqa: E402,F401  (sets the import path of the shared modules)
from common3 import nearest_s, find_straight  # noqa: E402
import layout3  # noqa: E402
import render  # noqa: E402
from core import World, emit, kenc, load_known_names  # noqa: E402
from path import raster_band  # noqa: E402

NAME = "Universe Grand Prix - Cosmic Odyssey v3"
OUT_NAME = "universe_grand_prix_cosmic_odyssey_v3.json"
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
KNOWN = os.path.join(ROOT, "tools", "ai_prefab_blocks.txt")
OUT_DIR = os.path.join(HERE, "out")
ZONES = ["station", "candy", "city", "tower", "skya", "dunes", "atlantis", "jungle", "keep", "glacier", "skyb", "ribbon", "core"]

# assumed top speeds used to size the lap (blocks per second)
SPEED = {"kart": 45.0, "plane": 75.0, "sub": 38.0}


class Ctx:
    pass


# --------------------------------------------------------------------------- road height profiles
def set_profile(P, keys, win=14.0):
    """Linear height profile through the (s, T) keys, smoothed by a moving average of `win` blocks, rounded to whole blocks."""
    ks = np.array([k[0] for k in keys], float)
    ts = np.array([k[1] for k in keys], float)
    T = np.interp(P.s, ks, ts)
    n = max(1, int(win / 0.5))
    if n > 1:
        pad = np.pad(T, n, mode="reflect", reflect_type="odd")
        ker = np.ones(2 * n + 1) / (2 * n + 1)
        T = np.convolve(pad, ker, mode="same")[n:-n]
    P.T = np.rint(T).astype(np.int64)


def set_heights(ctx):
    A, B, C, SA, SB = ctx.A, ctx.B, ctx.C, ctx.SA, ctx.SB
    m = A.marks
    # ---- A: the stadium is flush with the ground (-1); candy hills; the city is flat with one overpass; the tower spiral climbs
    sp0 = nearest_s(A, layout3.TOWER[0], layout3.TOWER[1] - layout3.TOWER_R0, m["tower"])
    pad0 = m["lip"] - 90.0
    ctx.marks["spiral0"], ctx.marks["pad0"] = sp0, pad0
    c0 = m["candy"]
    keys = [(0, -1), (545, -1), (600, 0), (c0 + 70, 0), (c0 + 180, 6), (c0 + 290, 11), (c0 + 380, 5), (c0 + 470, 1), (c0 + 570, 7),
            (c0 + 680, 12), (c0 + 790, 14), (c0 + 900, 9), (c0 + 1000, 3), (c0 + 1100, 0), (m["city"] - 20, 0)]
    # the city overpass: the second pass over the crossing climbs to 12
    cr = layout3.CITY_CROSS
    s_c2 = nearest_s(A, cr[0], cr[1], m["city"] + 400, m["tower"])
    ctx.marks["cross2"] = s_c2
    s_c1 = nearest_s(A, cr[0], cr[1], m["city"], m["city"] + 400)
    ctx.marks["cross1"] = s_c1
    keys += [(m["city"], 0), (s_c2 - 100, 0), (s_c2 - 22, 12), (s_c2 + 22, 12), (s_c2 + 110, 0), (sp0 - 10, 0), (sp0, 0),
             (pad0, 100), (m["lip"] - 18, 100), (m["lip"], 103)]
    keys = sorted(set(keys))
    A.keys = keys
    set_profile(A, keys, win=10.0)
    # ---- B
    b = B.marks
    s_pyr = find_straight(B, b["dunes"] + 200, b["dunes"] + 640, half=75.0)
    ctx.marks["pyramid"] = s_pyr
    dkeys = [(b["dunes"] + 180, 5), (b["dunes"] + 300, 2), (b["dunes"] + 420, 8), (b["dunes"] + 550, 3), (b["dunes"] + 660, 9),
             (b["dunes"] + 760, 4), (b["dunes"] + 860, 1)]
    if s_pyr is not None:
        dkeys = [k for k in dkeys if abs(k[0] - s_pyr) > 190] + [(s_pyr - 100, 3), (s_pyr + 100, 3)]
    keys = [(0, -1), (b["dunes"] - 20, -1), (b["dunes"] + 60, 0)] + dkeys + [
            (b["oasis"] - 70, 0),
            (b["oasis"] + 20, 14), (b["dive"] - 5, 14), (b["dive"] + 115, 0), (b["surface"] - 110, 0), (b["surface"], 13),
            (b["surface"] + 40, 13), (b["jungle"] + 40, 0),
            (b["jungle"] + 130, 3), (b["jungle"] + 260, 12), (b["jungle"] + 390, 22), (b["jungle"] + 500, 26), (b["keep"] - 20, 24),
            (b["keep"] + 100, 24), (b["keep"] + 190, 16), (b["keep"] + 290, 8), (b["keep"] + 420, 8), (b["keep"] + 540, 16),
            (b["keep"] + 660, 10), (b["keep"] + 760, 6), (b["glacier"] - 100, 6), (b["summit"], 90), (b["pad"] - 18, 90), (b["pad"], 93)]
    set_profile(B, sorted(set(keys)), win=12.0)
    # ---- C: landing strip, swooping ribbon, the star slide
    cc = C.marks
    keys = [(0, 72), (330, 72), (470, 82), (620, 94), (770, 86), (900, 76), (1010, 86), (1100, 72), (C.length, -1)]
    set_profile(C, keys, win=30.0)
    # ---- flights
    sa = SA.marks
    layout3.set_altitude(SA, [(0, 106), (180, 150), (320, 157), (480, 157), (640, 170), (900, 164), (1250, 150), (1500, 140), (1760, 118), (1904, 46), (2030, 9), (SA.length, 3)])
    layout3.set_altitude(SB, [(0, 98), (300, 150), (580, 188), (904, 214), (1238, 228), (1492, 222), (1746, 208), (1950, 156), (2113, 102), (SB.length, 75)])


# --------------------------------------------------------------------------- post passes
ROAD_OK = {"k_asphalt", "k_asph_stair", "k_conc", "k_conc_stair", "k_curb_yel_b", "k_curb_red_b", "k_finish_line", "k_checker"}


def restore_roads(ctx):
    """Terrain, rocks and props sometimes land on a road cell; put the stamped surface back."""
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


def clear_corridor(ctx):
    """Keep head-room above every registered kart road; layer road+1 keeps only the Kart decals, stairs and neon (k_*, n_*)."""
    W = ctx.W
    n0 = len(W.d)
    for (P, i0, i1, hw, tmin) in ctx.corridors:
        cx, cz, si, uu = raster_band(P, i0, i1, -hw, hw)
        T = P.T[si]
        if tmin is not None:
            keep = T >= tmin
            cx, cz, T = cx[keep], cz[keep], T[keep]
        for h in (2, 3, 4):
            W.erase_arr(cx, T + h, cz)
        for x, y, z in zip(cx.tolist(), (T + 1).tolist(), cz.tolist()):
            sid = W.d.get(kenc(x, y, z))
            if sid is not None:
                nm = W.specs[sid][0]
                if not (nm.startswith("k_") or nm.startswith("n_")):
                    del W.d[kenc(x, y, z)]
    print("corridor clearing removed %d blocks" % (n0 - len(W.d)))


def make_ctx():
    ctx = Ctx()
    ctx.W = World()
    ctx.rng = np.random.default_rng(20260930)
    P = layout3.all_paths()
    ctx.A, ctx.SA, ctx.B, ctx.SB, ctx.C = P["A"], P["SA"], P["B"], P["SB"], P["C"]
    ctx.marks = {}
    ctx.corridors = []
    set_heights(ctx)
    ctx.known = load_known_names(KNOWN)
    keys = []
    for Pk in (ctx.A, ctx.B, ctx.C):
        cx, cz, si, uu = raster_band(Pk, 0, len(Pk) - 1, -15.0, 15.0)
        keys.append(((cx + 4096) | ((cz + 4096) << 14)).astype(np.int64))
    ctx.road_keys = np.unique(np.concatenate(keys))
    return ctx


def build_zones(zones, quiet=False):
    """Build the named zones (plus the post passes) and return the context."""
    ctx = make_ctx()
    for z in zones:
        mod = importlib.import_module("z_" + z)
        ctx.rng = np.random.default_rng(1000 + ZONES.index(z))
        with ctx.W.zone(z):
            mod.build(ctx)
    restore_roads(ctx)
    clear_corridor(ctx)
    return ctx


def main(argv):
    do_render = "--render" in argv
    names = [a for a in argv if not a.startswith("--")]
    zones = names or ZONES
    t0 = time.time()
    ctx = make_ctx()
    for z in zones:
        t1 = time.time()
        mod = importlib.import_module("z_" + z)
        ctx.rng = np.random.default_rng(1000 + ZONES.index(z))
        with ctx.W.zone(z):
            mod.build(ctx)
        print("zone %-9s %8d blocks  (%.1fs)" % (z, ctx.W.zcount.get(z, 0), time.time() - t1), flush=True)
        if "--ticks" in argv:
            for zz, lab, n in ctx.W.ticks:
                if zz == z:
                    print("      %-22s %8d" % (lab, n))
    restore_roads(ctx)
    clear_corridor(ctx)
    below = [k for k in ctx.W.d if (k >> 28) - __import__("core").OY < -1]
    for k in below:
        del ctx.W.d[k]
    total = len(ctx.W.d)
    print("TOTAL blocks: %d   build time %.1fs" % (total, time.time() - t0), flush=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    if "--no-emit" not in argv and not names:
        out = os.path.join(OUT_DIR, OUT_NAME)
        emit(ctx.W, NAME, ctx.known, out_path=out)
    if do_render:
        print("render", render.topdown(ctx.W, os.path.join(OUT_DIR, "top.png"), scale=0.5 if total > 2_000_000 else 1))
    return ctx


if __name__ == "__main__":
    main(sys.argv[1:])
