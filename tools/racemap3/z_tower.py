"""Zone 3: Skyport Tower - the road spirals inward around a needle tower (450 degrees, 0 -> 100 blocks up) and ends on a straight
launch pad over the city.  Kart -> plane transform gate on the pad."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
import layout3
from z_city import GLASS, windows_face  # noqa: F401  (materials of the city)

mat("tw_body", ["Metal_Zinc_Smooth", "Metal_Zinc_Smooth", "Metal_Iron_Smooth"])
mat("tw_dark", "Build_Black_Cube")
mat("tw_white", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("tw_deck", "Rock_Slate_Brick")
mat("tw_pier", "Rock_Slate_Brick_Pillar_Middle")
mat("tw_trim", "Metal_Iron_Smooth")
mat("tw_gold", "Rock_Gold_Brick_Smooth")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    cx, cz = layout3.TOWER
    cx, cz = int(cx), int(cz)
    sp0, pad0, lip = ctx.marks["spiral0"], ctx.marks["pad0"], m["lip"]
    s1 = m["tower"]
    tick = W.tick

    # ------------------------------------------------------------------ roads: the entry street, the spiral (chunked), the pad
    edge = lambda s, u: ["n_cyan", "n_magenta", "n_yellow", "n_magenta"][int(s // 4) % 4]
    base = dict(hw=8.0, verge="k_conc", edge=edge, lanes=(0.0,), lane_dash=(4.0, 4.0), lane_mat="k_yline", curbs=(), rail="mesh")
    stamp(ctx, A, s1, sp0 + 2, KartStyle(**base))
    piers = dict(mat="tw_pier", every=10, size=2, inset=1.7, cap="tw_deck", bottom=0)
    stamp(ctx, A, sp0, pad0 + 3, KartStyle(deck="tw_deck", piers=piers, **base), chunk=140)
    # the pad: no automatic piers (it passes over the spiral's own outer turn); open at the very end
    pad_style = KartStyle(deck="tw_deck", **base)
    stamp(ctx, A, pad0, lip - 14, pad_style)
    stamp(ctx, A, lip - 16, lip, KartStyle(deck="tw_deck", rail=None, **{k: v for k, v in base.items() if k != "rail"}))
    tick("roads")
    # pad piers placed by hand, clear of the outer turn
    Fp = Frame(A, pad0 + 8)
    for v in (8, 26, 78):
        Fq = Frame(A, pad0 + v)
        for side in (-1, 1):
            x, z = Fq.cell(side * 6, 0)
            W.box((x, 0, z), (x + 1, int(Fq.T) - 2, z + 1), "tw_pier")
    for side in (-1, 1):
        Fq = Frame(A, lip - 6)
        x, z = Fq.cell(side * 6, 0)
        for y in range(0, int(Fq.T) - 1):
            pass

    # ------------------------------------------------------------------ the terminal ring at the foot
    r_out, r_in = 54, 36
    for y in range(0, 12):
        W.cyl(cx, cz, y, y, r_out, "tw_body", mode="wall")
        W.cyl(cx, cz, y, y, r_in, "tw_body", mode="wall")
    for y in range(2, 10):
        for a in range(0, 360, 4):
            if (a // 4) % 3:
                W.put(int(round(cx + r_out * math.cos(math.radians(a)))), y, int(round(cz + r_out * math.sin(math.radians(a)))),
                      "cy_glass_c" if (a // 12) % 2 else "cy_glass_w")
    W.cyl(cx, cz, 12, 12, r_out + 2, "tw_deck")
    W.cyl(cx, cz, 13, 13, r_out + 2, "tw_trim", mode="wall")
    W.cyl(cx, cz, 14, 14, r_out + 2, "cy_glass_m", mode="wall")
    W.cyl(cx, cz, -1, -1, 66, "cy_pave", mode="fill", keep=True)
    for k in range(0, 360, 20):
        for rr in (60, 64):
            W.put(int(round(cx + rr * math.cos(math.radians(k)))), -1, int(round(cz + rr * math.sin(math.radians(k)))), "cy_glass_c" if k % 40 else "cy_glass_y")
    # a garden roof between the ring and the shaft
    W.cyl(cx, cz, 12, 12, r_out - 2, "k_grass", mode="fill")
    tick("terminal")

    # ------------------------------------------------------------------ the needle tower
    H = 128
    for y in range(13, H):
        r = 29.0 - 5.0 * (y / H) + 1.4 * math.sin(y * 0.18)
        W.cyl(cx, cz, y, y, r, "tw_body" if y % 5 else "tw_trim", mode="wall")
    # window bands (neon) following the profile
    for y in range(16, H, 3):
        r = 29.0 - 5.0 * (y / H) + 1.4 * math.sin(y * 0.18)
        for a in range(0, 360, 5):
            if (a * 7 + y * 13) % 11 < 7:
                W.put(int(round(cx + (r + 0.0) * math.cos(math.radians(a)))), y, int(round(cz + r * math.sin(math.radians(a)))), GLASS[(a // 45 + y // 16) % 8])
    # spiral neon stripes up the shaft
    for k in range(3):
        for t in range(0, 360 * 3, 3):
            y = 14 + int(t / (360 * 3.0) * (H - 16))
            a = math.radians(t + k * 120)
            r = 29.0 - 5.0 * (y / H) + 1.4 * math.sin(y * 0.18) + 0.9
            W.put(int(round(cx + r * math.cos(a))), y, int(round(cz + r * math.sin(a))), ["cy_glass_c", "cy_glass_m", "cy_glass_y"][k])
    # ring platforms
    for y, r in ((34, 40), (62, 38), (88, 44)):
        W.cyl(cx, cz, y, y, r, "tw_deck", mode="fill")
        W.cyl(cx, cz, y + 1, y + 1, r, "cy_glass_c", mode="wall")
        W.cyl(cx, cz, y - 1, y - 1, r - 1, "cy_glass_m", mode="wall")
    # the top: observation deck (under the pad level), dome, mast
    W.cyl(cx, cz, 96, 97, 56, "tw_deck", mode="fill")
    W.cyl(cx, cz, 98, 98, 56, "cy_glass_y", mode="wall")
    W.cyl(cx, cz, 95, 95, 55, "cy_glass_c", mode="wall")
    for a in range(0, 360, 15):
        for rr in range(30, 56):
            if rr % 6 == 0:
                W.put(int(round(cx + rr * math.cos(math.radians(a)))), 98, int(round(cz + rr * math.sin(math.radians(a)))), "tw_trim")
    W.ellipsoid(cx, 118, cz, 22, 16, 22, "tw_body", mode="shell", part="upper")
    W.ellipsoid(cx, 118, cz, 22.5, 16.5, 22.5, "cy_glass_w", mode="shell", part="upper") if False else None
    W.cyl(cx, cz, 128, 129, 24, "tw_trim", mode="fill")
    for y in range(130, 240):
        W.put(cx, y, cz, "tw_trim")
        W.put(cx + 1, y, cz, "tw_trim")
    for y, r in ((148, 14), (168, 11), (188, 8), (208, 5)):
        hoop(W, (cx, y, cz), (0, 1, 0), r, "cy_glass_c" if (y // 20) % 2 else "cy_glass_m", thick=2)
    W.put(cx, 240, cz, "cy_beacon")
    W.put(cx + 1, 240, cz, "cy_beacon")
    W.put(cx, 241, cz, "lit_white")
    # SKYPORT lettering on the south and north faces (readable from the spiral)
    tick("tower")

    # ------------------------------------------------------------------ spiral dressing
    L = pad0 - sp0
    for frac in np.arange(0.06, 0.95, 0.09):
        boost_pad(W, A, sp0 + frac * L, 14, 5, "n_cyan" if int(frac * 100) % 2 else "n_magenta", count=3, spacing=4)
    for frac in np.arange(0.1, 0.95, 0.14):
        item_boxes(W, A, sp0 + frac * L, 8.0, n=5)
    flood_posts(W, A, A.i(sp0), A.i(pad0), 40, 10.5, 6, glow="kp_glow_cyan", need_support=False)
    corner_furniture(W, A, A.i(s1), A.i(pad0), 8.0, rmax=60.0, stacks=False, wall_w=0.0, skid=False, need_support=False)

    # ------------------------------------------------------------------ launch pad: runway lights, ring tunnel, the transform gate
    for v in range(4, 76, 6):
        Fq = Frame(A, pad0 + v)
        for side in (-1, 1):
            x, z = Fq.cell(side * 9.5, 0)
            W.put(x, Fq.T + 1, z, "lit_white" if (v // 6) % 2 else "lit_cyan")
        road_arrows(W, A, pad0 + v, n=1, u=-4.0)
        road_arrows(W, A, pad0 + v, n=1, u=4.0)
    for k in range(7):
        Fk = Frame(A, pad0 + 6 + k * 10)
        cxx, czz = Fk.cell(0, 0)
        hoop(W, (cxx, Fk.T + 8, czz), (Fk.t[0], 0, Fk.t[1]), 12.5 - k * 0.5, ["n_yellow", "n_orange"][k % 2], thick=1, ymin=Fk.T + 1)
    for k, r in enumerate((14, 16.5, 19)):
        Fk = Frame(A, lip - 36 + k * 8)
        cxx, czz = Fk.cell(0, 0)
        hoop(W, (cxx, Fk.T + 9, czz), (Fk.t[0], 0, Fk.t[1]), r, ["n_white", "n_cyan", "n_magenta"][k], thick=2, ymin=Fk.T + 1)
    FL = Frame(A, lip - 1)
    for u in range(-9, 10):
        x, z = FL.cell(u, 0)
        W.put(x, FL.T + 1, z, "n_white" if u % 2 else "n_yellow")
    tick("pad")
