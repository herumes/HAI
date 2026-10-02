"""Zone 5: Golden Dunes - the landing runway, a pharaoh gate, an obelisk avenue, a road through a great pyramid, a sphinx, a giant
scorpion, rock arches and an oasis market on the way to the Atlantis lagoon.  Terrain is sandstone / clay (no sand blocks)."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_canyon import cactus, palm, rock_arch  # noqa: F401  (also registers the cn_* materials)
from zone_meadows import balloon  # noqa: F401

mat("dn_sand_a", ["Rock_Sandstone", "Rock_Sandstone", "Soil_Clay_Smooth_Yellow2", "Rock_Sandstone_White"])
mat("dn_sand_b", ["Soil_Clay_Smooth_Yellow2", "Soil_Clay_Smooth_Orange", "Rock_Sandstone", "Soil_Clay_Smooth_Yellow2"])
mat("dn_sand_c", ["Rock_Sandstone_White", "Rock_Sandstone_White", "Rock_Sandstone"])
mat("dn_rock", ["Rock_Sandstone_Red", "Rock_Sandstone_Red", "Soil_Clay_Orange", "Rock_Sandstone"])
mat("dn_rock2", ["Rock_Sandstone_Brick", "Rock_Sandstone_Red_Brick", "Rock_Sandstone_Red"])
mat("dn_brick", ["Rock_Sandstone_Brick", "Rock_Sandstone_Brick", "Rock_Sandstone_Brick_Smooth"])
mat("dn_white", ["Rock_Sandstone_White_Brick_Smooth", "Rock_Sandstone_White_Brick", "Rock_Sandstone_White_Brick_Smooth"])
mat("dn_orn", "Rock_Sandstone_Brick_Ornate")
mat("dn_pillar", "Rock_Sandstone_Brick_Pillar_Middle")
mat("dn_pillar_b", "Rock_Sandstone_Brick_Pillar_Base")
mat("dn_gold", "Rock_Gold_Brick_Smooth")
mat("dn_gold_o", "Rock_Gold_Brick_Ornate")
mat("dn_lapis", "Soil_Clay_Smooth_Blue")
mat("dn_turq", "Soil_Clay_Smooth_Cyan")
mat("dn_terra", "Soil_Clay_Orange")
mat("dn_glyph", "NoCube_Neon_Block_Orange")
mat("dn_glyph2", "NoCube_Neon_Block_Cyan")
mat("dn_water", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("dn_rim", ["Soil_Grass_Sunny_Full", "Rock_Sandstone_White"])
mat("dn_reed", ["Plant_Reeds_Arid", "Plant_Reeds_Marsh"])
mat("dn_awn_r", "Cloth_Block_Wool_Red")
mat("dn_awn_y", "Cloth_Block_Wool_Yellow")
mat("dn_awn_b", "Cloth_Block_Wool_Blue")
mat("dn_awn_w", "Cloth_Block_Wool_White")
mat("dn_awn_o", "Cloth_Block_Wool_Orange")
mat("dn_post", "Wood_Hardwood_Fence")
mat("dn_scorp", ["Soil_Clay_Smooth_Red2", "Soil_Clay_Smooth_Red", "Soil_Clay_Smooth_Orange"])
mat("dn_scorp_eye", "Build_Lightsource_Red")
mat("dn_beam", "Build_Lightsource_YellowLight")
mat("dn_dish", "Metal_Zinc_Smooth")
AWN = ["dn_awn_r", "dn_awn_y", "dn_awn_b", "dn_awn_w", "dn_awn_o"]


# --------------------------------------------------------------------------- local frames
class LF:
    """Axis-aligned local frame: a along `fwd` (one of 'n','e','s','w'), b to the right of that, y up."""

    def __init__(self, ox, oy, oz, fwd):
        self.ox, self.oy, self.oz = ox, oy, oz
        self.f = {"n": (0, -1), "s": (0, 1), "e": (1, 0), "w": (-1, 0)}[fwd]
        self.r = (-self.f[1], self.f[0])

    def at(self, a, b, y):
        return (self.ox + self.f[0] * a + self.r[0] * b, self.oy + y, self.oz + self.f[1] * a + self.r[1] * b)

    def put(self, W, a, b, y, m, rot=None):
        x, yy, z = self.at(int(round(a)), int(round(b)), int(round(y)))
        W.put(int(x), int(yy), int(z), m, rot)

    def box(self, W, a0, a1, b0, b1, y0, y1, m, mode="fill"):
        p, q = self.at(a0, b0, y0), self.at(a1, b1, y1)
        W.box(p, q, m, mode=mode)

    def ell(self, W, a, b, y, ra, rb, ry, m, mode="fill", part="all"):
        x, yy, z = self.at(a, b, y)
        if self.f[0] != 0:
            W.ellipsoid(int(x), int(yy), int(z), ra, ry, rb, m, mode=mode, part=part)
        else:
            W.ellipsoid(int(x), int(yy), int(z), rb, ry, ra, m, mode=mode, part=part)


# --------------------------------------------------------------------------- set pieces
def obelisk(W, x, y, z, h, w=3):
    for k in range(h):
        r = max(1, int(round(w * (1.0 - 0.55 * k / float(h)))))
        W.box((x - r, y + k, z - r), (x + r, y + k, z + r), "dn_white" if k % 7 else "dn_orn", mode="walls")
    W.box((x - 1, y + h, z - 1), (x + 1, y + h, z + 1), "dn_gold", mode="fill")
    W.put(x, y + h + 1, z, "dn_gold")
    for k in range(3, h - 2, 3):
        W.put(x, y + k, z + w, "dn_glyph")
    W.box((x - w - 1, y, z - w - 1), (x + w + 1, y + 1, z + w + 1), "dn_brick")


def sphinx(W, cx, cy, cz, fwd, s=1.0):
    F = LF(cx, cy, cz, fwd)
    L = int(34 * s)
    F.box(W, -L // 2, L // 2 - 6, -6 * s, 6 * s, 0, 3, "dn_brick")                         # plinth
    F.ell(W, -2 * s, 0, 8 * s, 15 * s, 6.5 * s, 7 * s, "dn_sand_a", mode="shell")           # body
    F.ell(W, 8 * s, 0, 7 * s, 8 * s, 6 * s, 6 * s, "dn_sand_a", mode="shell")                # chest
    for b in (-4.2 * s, 4.2 * s):                                                           # paws
        F.box(W, 8 * s, 20 * s, b - 1.7 * s, b + 1.7 * s, 4, int(7 * s), "dn_sand_a")
        F.box(W, 18 * s, 21 * s, b - 1.9 * s, b + 1.9 * s, 4, int(5 * s), "dn_sand_c")
    F.ell(W, 15 * s, 0, 17 * s, 5.2 * s, 5.2 * s, 6 * s, "dn_sand_c", mode="shell")          # head
    for b in (-5.2 * s, 5.2 * s):                                                           # nemes lappets
        F.box(W, 11 * s, 16 * s, b - 1, b + 1, 10, int(18 * s), "dn_lapis")
    F.box(W, 10 * s, 20 * s, -5 * s, 5 * s, int(22 * s), int(23 * s), "dn_gold")            # headdress top
    for k, m_ in enumerate(("dn_gold", "dn_lapis", "dn_gold", "dn_lapis")):
        F.box(W, 11 * s + k * 2, 11 * s + k * 2 + 1, -5 * s, 5 * s, int(19 * s), int(21 * s), m_)
    for b in (-2.4 * s, 2.4 * s):
        F.put(W, 20 * s, b, 18 * s, "b_black")
    F.box(W, 20 * s, 21 * s, -1, 1, int(14 * s), int(16 * s), "dn_sand_c")                  # nose
    F.box(W, 20 * s, 21 * s, -2 * s, 2 * s, int(11 * s), int(11 * s), "dn_terra")           # mouth
    F.box(W, 19 * s, 20 * s, -1, 1, int(7 * s), int(10 * s), "dn_lapis")                    # beard
    for b in (-6 * s, 6 * s):                                                               # haunches + tail
        F.ell(W, -14 * s, b * 0.8, 5 * s, 6 * s, 3 * s, 4 * s, "dn_sand_a", mode="shell")
    for k in range(10):
        F.put(W, -17 * s - k * 0.7, 6 * s + k * 0.5, 3 + (k // 3), "dn_sand_a")
    F.put(W, 21 * s, 0, 24 * s, "dn_beam")


def scorpion(W, cx, cy, cz, fwd, s=1.0):
    F = LF(cx, cy, cz, fwd)
    F.ell(W, 0, 0, 4 * s, 9 * s, 5 * s, 3.5 * s, "dn_scorp", mode="shell")
    F.ell(W, 8 * s, 0, 4 * s, 4 * s, 3.5 * s, 3 * s, "dn_scorp", mode="shell")
    for b in (-1.6 * s, 1.6 * s):
        F.put(W, 11.5 * s, b, 5 * s, "dn_scorp_eye")
    for k, a in enumerate((-4, 0, 4, 8)):                                                     # legs
        for b in (-1, 1):
            for t in range(1, int(7 * s)):
                F.put(W, a * s + (0 if t < 3 else 1), b * (4 * s + t * 0.9), max(0, 3 * s - t * 0.5), "dn_scorp")
    for b in (-1, 1):                                                                       # claws
        for t in range(0, int(11 * s)):
            F.put(W, 10 * s + t * 0.9, b * (4 * s + t * 0.35), 4 * s, "dn_scorp")
        F.ell(W, 21 * s, b * 8 * s, 4 * s, 3.2 * s, 2.2 * s, 2.2 * s, "dn_scorp", mode="shell")
    for k in range(0, int(18 * s)):                                                         # curled tail
        t = k / float(18 * s)
        a, y = -9 * s - 8 * s * math.sin(t * 1.9), 6 * s + 16 * s * t - 7 * s * t * t
        F.ell(W, a, 0, y, 1.8 * s, 1.8 * s, 1.8 * s, "dn_scorp")
    F.ell(W, -9 * s - 8 * s * math.sin(1.9), 0, 17 * s, 2.4 * s, 2.0 * s, 2.4 * s, "dn_scorp_eye")


def pylon_gate(ctx, P, s, T_title="GOLDEN DUNES"):
    """Two tapered pylons 42 blocks either side of the road and a lintel between them wide enough for the lettering (71 wide)."""
    W = ctx.W
    s = axis_s(P, s, 200) or s
    F = Frame(P, s)
    T = F.T
    for side in (-1, 1):
        for k in range(36):
            hw_ = 5.2 - 1.2 * k / 36.0
            u0 = side * 44
            lbox(W, F, int(u0 - hw_), int(u0 + hw_), int(-4 + k * 0.05), int(4 - k * 0.05), T + k, T + k, "dn_brick" if k % 6 else "dn_orn")
        lbox(W, F, side * 44 - 2, side * 44 + 2, -2, 2, T + 36, T + 37, "dn_gold")
        x, z = F.cell(side * 44, 0)
        W.put(x, T + 38, z, "dn_beam")
        for k in range(4, 34, 4):
            x, z = F.cell(side * 44 + (-6 if side > 0 else 6), -4)
            W.put(x, T + k, z, "dn_glyph")
    lbox(W, F, -44, 44, -3, 3, T + 28, T + 36, "dn_brick")
    lbox(W, F, -44, 44, -4, -4, T + 36, T + 36, "dn_gold")
    lbox(W, F, -44, 44, -4, -4, T + 28, T + 28, "dn_gold")
    lbox(W, F, -44, 44, -4, -4, T + 29, T + 35, "b_black")
    lbox(W, F, -44, 44, -4, -4, T + 29, T + 29, "dn_lapis")
    lbox(W, F, -44, 44, -4, -4, T + 35, T + 35, "dn_lapis")
    lsign(W, F, T_title, 0, T + 34, -5, 1, "n_yellow")
    # winged sun disc above the lintel
    for a in range(0, 360, 6):
        for rr in np.arange(0.0, 6.0, 0.8):
            x, z = F.cell(int(round(rr * math.cos(math.radians(a)))), -3)
            W.put(x, T + 41 + int(round(rr * math.sin(math.radians(a)))), z, "dn_gold" if rr > 1.5 else "dn_glyph")
    for side in (-1, 1):
        for k in range(0, 15):
            x, z = F.cell(side * (7 + k), -3)
            for dy in range(0, 3):
                W.put(x, T + 41 + dy - (k // 5), z, "dn_lapis" if (k + dy) % 2 else "dn_gold")


def pyramid_hall(W, rng, cx, cz, T, axis_x=True, half=60):
    """A hollow stepped-smooth pyramid with the road running through a long hall.  axis_x: the road runs along x."""
    for i in range(half + 1):
        r = half - i
        y = i
        m_ = "dn_orn" if i % 8 == 7 else "dn_brick"
        W.box((cx - r, y, cz - r), (cx + r, y, cz + r), m_, mode="walls" if r > 0 else "fill")
    for k in range(5):
        W.box((cx - k, half + 1 + k, cz - k), (cx + k, half + 1 + k, cz + k), "dn_gold", mode="fill")
    for y in range(half + 6, half + 70):
        W.put(cx, y, cz, "dn_beam")
    # the hall: carve a corridor through the middle along the road, 12 wide x 12 tall
    for a in range(-half - 1, half + 2):
        for b in range(-7, 8):
            for y in range(T + 1, T + 12):
                x, z = (cx + a, cz + b) if axis_x else (cx + b, cz + a)
                W.erase(x, y, z)
        for b in range(-8, 9):
            x, z = (cx + a, cz + b) if axis_x else (cx + b, cz + a)
            W.put(x, T + 12, z, "dn_brick")                                                # roof slab over the hall (>= T+6 clear)
            W.put(x, T - 1, z, "dn_brick") if T > 0 else None
        for side in (-1, 1):
            for y in range(T, T + 12):
                x, z = (cx + a, cz + side * 8) if axis_x else (cx + side * 8, cz + a)
                W.put(x, y, z, "dn_brick" if y % 3 else "dn_orn")
    # pillars, glyph strips, statues, braziers
    for a in range(-half + 6, half - 3, 8):
        for side in (-1, 1):
            x, z = (cx + a, cz + side * 7) if axis_x else (cx + side * 7, cz + a)
            for y in range(T, T + 11):
                W.put(x, y, z, "dn_pillar")
            W.put(x, T, z, "dn_pillar_b")
            xg, zg = (cx + a + 3, cz + side * 7) if axis_x else (cx + side * 7, cz + a + 3)
            for y in range(T + 3, T + 9, 2):
                W.put(xg, y, zg, "dn_glyph" if (a // 8) % 2 else "dn_glyph2")
            xb, zb = (cx + a + 4, cz + side * 6) if axis_x else (cx + side * 6, cz + a + 4)
            W.put(xb, T, zb, "lit_orange")
    for a in (-half // 2, 0, half // 2):
        for side in (-1, 1):
            x, z = (cx + a, cz + side * 6) if axis_x else (cx + side * 6, cz + a)
            for y in range(T, T + 9):
                W.put(x, y, z, "dn_gold" if y < T + 6 else "dn_lapis")
            W.ellipsoid(x, T + 9, z, 1.8, 1.8, 1.8, "dn_gold")
    # a gold lintel arch at both entrances
    for e in (-half - 1, half + 1):
        for b in range(-9, 10):
            for y in (T + 12, T + 13):
                x, z = (cx + e, cz + b) if axis_x else (cx + b, cz + e)
                W.put(x, y, z, "dn_gold")
        for side in (-1, 1):
            for y in range(T, T + 13):
                x, z = (cx + e, cz + side * 9) if axis_x else (cx + side * 9, cz + e)
                W.put(x, y, z, "dn_gold" if y % 2 else "dn_lapis")


def mesa(W, rng, cx, y0, cz, rx, rz, h):
    """A banded sandstone mesa: shrinking elliptical rings (cliff) with a flat top and a few ledges."""
    for k in range(h):
        f = 1.0 - 0.18 * (k / float(h)) - (0.12 if k % 9 == 8 else 0.0)
        a, b = rx * f, rz * f
        n = int(2 * math.pi * max(a, b) / 0.6) + 8
        th = np.linspace(0, 2 * math.pi, n, endpoint=False)
        wob = 1.0 + 0.08 * np.sin(th * 5 + k * 0.3) + 0.05 * np.sin(th * 11 + k)
        X = np.floor(cx + a * wob * np.cos(th) + 0.5).astype(np.int64)
        Z = np.floor(cz + b * wob * np.sin(th) + 0.5).astype(np.int64)
        m_ = ["dn_rock", "dn_rock2", "dn_sand_b", "dn_rock"][(k // 4) % 4]
        W.put_arr(X, np.full(n, y0 + k), Z, m_)
    f = 1.0 - 0.18
    ox, oz = np.meshgrid(np.arange(-int(rx) - 2, int(rx) + 3), np.arange(-int(rz) - 2, int(rz) + 3), indexing="ij")
    sel = (ox / (rx * f)) ** 2 + (oz / (rz * f)) ** 2 <= 1.0
    W.put_arr(cx + ox[sel], np.full(sel.sum(), y0 + h), cz + oz[sel], "dn_sand_a")


def oasis(W, rng, cx, cz, rx, rz):
    n = int(max(rx, rz)) + 3
    for dx in range(-n, n + 1):
        for dz in range(-n, n + 1):
            q = (dx / float(rx)) ** 2 + (dz / float(rz)) ** 2
            if q <= 1.0:
                W.put(cx + dx, -1, cz + dz, "dn_water")
            elif q <= 1.25:
                W.put(cx + dx, -1, cz + dz, "dn_rim")
    for k in range(70):
        a = rng.uniform(0, 2 * math.pi)
        rr = 1.12 + rng.uniform(-0.05, 0.15)
        x, z = int(cx + rx * rr * math.cos(a)), int(cz + rz * rr * math.sin(a))
        if rng.random() < 0.28:
            palm(W, rng, x, 0, z)
        else:
            W.put(x, 0, z, "dn_reed")
    for k in range(5):
        a = k * 1.7
        x, z = int(cx + (rx * 0.5) * math.cos(a)), int(cz + (rz * 0.5) * math.sin(a))
        W.ellipsoid(x, -1, z, 3, 1, 3, "dn_rim")
        palm(W, rng, x, 0, z, h=int(rng.integers(7, 11)))


def market(W, rng, cx, cz, w=44, d=22):
    """A bazaar: a row of tented stalls with striped awnings, crates, lamps."""
    for k, x in enumerate(range(cx, cx + w, 8)):
        col = AWN[k % 5]
        for dz in (0, d):
            for dx in (0, 6):
                for y in range(0, 6):
                    W.put(x + dx, y, cz + dz, "dn_post")
        for xx in range(x - 1, x + 8):
            for zz in range(cz - 1, cz + 4):
                W.put(xx, 6, zz, col if (xx + zz) % 2 else "dn_awn_w")
        for zz in range(cz + 3, cz + 0, -1):
            pass
        W.box((x + 1, 0, cz), (x + 5, 1, cz + 1), "dn_terra")
        W.put(x + 3, 2, cz, "dn_orn")
        W.put(x + 3, 5, cz + 2, "lit_orange")


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    sr, sd, so = 0.0, m["dunes"], m["oasis"]
    i0, i1 = B.i(sd), B.i(so)
    L = so - sd
    tick = W.tick

    # ------------------------------------------------------------------ runway (wide, flush) and its taper
    def curb_block(s, u):
        return "k_curb_yel_b" if int(s // 4) % 2 == 0 else "k_curb_red_b"
    run = KartStyle(hw=10.0, verge="k_conc", rail=None, edge=curb_block, lanes=(-5.0, 0.0, 5.0), lane_dash=(8.0, 6.0), edge_line=True, curbs=())
    stamp(ctx, B, 0, sd - 36, run)
    for k, s in enumerate(np.arange(sd - 36, sd, 4.0)):
        hw_ = 10.0 - 3.0 * (k + 1) / 9.0
        stamp(ctx, B, s - 1.0, s + 5.0, KartStyle(hw=hw_, verge="k_conc", rail="armco", lanes=(), edge_line=True, curbs=()), steps=False)
    # runway dressing: threshold piano keys, touchdown marks, edge lights, windsocks
    for k in range(9):
        for ss in np.arange(240.0, 250.0, 1.0):
            i = B.i(ss)
            x, z = side_cell(B, i, -8.8 + 2.2 * k)
            t = B.t[i]
            W.put(x, int(B.T[i]) + 1, z, "k_line", line_rot(t[0], t[1]))
    for s in (40, 80, 120, 160):
        for u0 in (-7.5, 7.5):
            for ss in np.arange(s, s + 12.0, 1.0):
                i = B.i(ss)
                x, z = side_cell(B, i, u0)
                t = B.t[i]
                W.put(x, int(B.T[i]) + 1, z, "k_line", line_rot(t[0], t[1]))
    for s in (24, 32):
        skid_patch(W, B, s, -8.0, -6.0, "k_skid_straight")
        skid_patch(W, B, s, 6.0, 8.0, "k_skid_straight")
    for s in np.arange(16, sd - 40, 20.0):
        road_arrows(W, B, s, n=1, u=-2.5)
        road_arrows(W, B, s, n=1, u=2.5)
    for s in np.arange(8, sd - 20, 10.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 11.5, 0)
            h_ = heading(F.t[0], F.t[1])
            W.put(x, F.T + 1, z, "k_flood", lean_rot(OPP[outward(h_, side)]))
    for s in (60, 150, 230):
        F = Frame(B, s)
        x, z = F.cell(14, 0)
        for y in range(0, 14):
            W.put(x, y, z, "dn_post")
        for k in range(1, 7):
            W.put(x - k, 13 - k // 3, z, ["dn_awn_r", "dn_awn_w"][k % 2])
    boost_pad(W, B, 255, 14, 4, "n_orange", count=3, spacing=4)
    tick("runway")

    # ------------------------------------------------------------------ the dunes road
    edge = lambda s, u: "n_yellow" if int(s // 3) % 2 == 0 else "n_orange"
    st = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail="armco", lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=("k_curb_yel", "k_curb_red"),
                   curb_len=4.0, curb_rmax=130.0, chevron_rmax=70.0)
    stamp(ctx, B, sd - 2, so, st)
    tick("road")

    # ------------------------------------------------------------------ terrain: soft dunes, sandstone cliffs
    land = Land(ctx, B, i0, i1, margin=90, radius=70, hw=9.0)
    g, d, Tn, Sn = land.g, land.d, land.Tn, land.Sn
    amp = smoothstep(sd, sd + 30, Sn) * 1.0
    n1 = fbm(g.X, g.Z, 130.0, 4, 91)
    rip = np.sin(g.X * 0.045 + g.Z * 0.02 + 5.0 * fbm(g.X, g.Z, 70.0, 2, 93))
    H = -1.0 + amp * (24.0 * smoothstep(0.36, 0.78, n1) + 3.2 * (0.5 + 0.5 * rip) * smoothstep(0.2, 0.5, n1))
    taper = 1.0 - smoothstep(42.0, 64.0, d)
    H = H * taper + (-1.0) * (1 - taper)
    land.finish(H, d0=4.0, d1=22.0, mask_d=68)
    Ht, mask, slope = land.Ht, land.mask, land.slope
    pat = fbm(g.X, g.Z, 31.0, 2, 97)
    mats = ["dn_sand_a", "dn_sand_b", "dn_sand_c", "dn_rock", "dn_rock2"]
    top_ids = pick([(pat > 0.6, 1), (pat < 0.32, 2), (slope > 0.75, 3)], 0)

    def side_fn(y, x, z):
        return np.where((y // 3) % 2 == 0, 3, 4)

    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask)
    tick("terrain")

    def ground(x, z):
        ix, iz = x - g.x0, z - g.z0
        if 0 <= ix < g.nx and 0 <= iz < g.nz and mask[ix, iz]:
            return int(Ht[ix, iz]) + 1
        return 0

    placed = []

    def spot(s, u, r):
        F = Frame(B, s)
        x, z = F.cell(u, 0)
        if blocked_by_road(ctx, x, z) and abs(u) < 14:
            return None
        if any((x - a) ** 2 + (z - b) ** 2 < (r + rr) ** 2 for (a, b, rr) in placed):
            return None
        placed.append((x, z, r))
        return x, ground(x, z), z

    # ------------------------------------------------------------------ pharaoh gate + colonnade at the start
    pylon_gate(ctx, B, sd + 16)
    for s in np.arange(sd + 50, sd + 150, 9.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 12.5, 0)
            for y in range(max(-1, F.T - 2), F.T + 13):
                W.put(x, y, z, "dn_pillar")
            W.put(x, F.T + 13, z, "dn_orn")
            W.put(x, F.T - 1 if F.T >= 0 else -1, z, "dn_pillar_b")
    for side in (-1, 1):
        a = Frame(B, sd + 50)
        for s in np.arange(sd + 50, sd + 148, 1.0):
            F = Frame(B, s)
            x, z = F.cell(side * 12.5, 0)
            W.put(x, F.T + 14, z, "dn_brick")
    tick("gate")

    # ------------------------------------------------------------------ the pyramid hall on the first straight, the sphinx, obelisks
    bs = ctx.marks.get("pyramid")
    print("  straight for the pyramid:", bs)
    if bs is not None:
        Fp = Frame(B, bs)
        cxp, czp = Fp.cell(0, 0)
        axis_x = abs(Fp.t[0]) > abs(Fp.t[1])
        Tp = int(B.T[B.i(bs)])
        for i in range(B.i(bs - 80), B.i(bs + 80)):
            pass
        pyramid_hall(W, rng, cxp, czp, Tp, axis_x=axis_x, half=60)
        placed.append((cxp, czp, 85))
        ctx.marks["pyramid"] = bs
    tick("pyramid")

    # obelisk avenue + sphinxes along the road
    for k, s in enumerate(np.arange(sd + 170, so - 120, 34.0)):
        if bs is not None and abs(s - bs) < 90:
            continue
        for side in (-1, 1):
            q = spot(s, side * 14.5, 3)
            if q:
                obelisk(W, q[0], q[1], q[2], int(rng.integers(14, 24)), w=2)
    for frac, side in ((0.52, 1), (0.8, -1)):
        s = sd + frac * L
        F = Frame(B, s)
        x, z = F.cell(side * 34, 0)
        q = spot(s, side * 34, 22)
        if q:
            f = heading(F.t[0], F.t[1])
            face = {"e": "s", "w": "n", "s": "w", "n": "e"}[f]
            face = ("n" if side > 0 else "s") if f in ("e", "w") else ("w" if side > 0 else "e")
            sphinx(W, q[0], q[1], q[2], face, s=1.3 if frac < 0.6 else 1.0)
    # the great sphinx and the scorpion
    q = spot(sd + 0.67 * L, 46, 28)
    if q:
        scorpion(W, q[0], q[1], q[2], "n", s=1.6)
    tick("statues")

    # ------------------------------------------------------------------ mesas and buttes for the horizon
    for k in range(9):
        q = spot(sd + rng.uniform(60, L - 80), (1 if rng.random() < 0.5 else -1) * rng.uniform(52, 92), 30)
        if q:
            h = int(rng.integers(20, 40))
            mesa(W, rng, q[0], max(-1, q[1] - 2), q[2], float(rng.integers(12, 24)), float(rng.integers(10, 20)), h)
    tick("mesas")

    # ------------------------------------------------------------------ rock arches, cacti, palms, ruined columns, balloons
    for frac in (0.34, 0.62, 0.9):
        Fa = Frame(B, sd + frac * L)
        if Fa.axis_aligned and not (bs is not None and abs(sd + frac * L - bs) < 100):
            rock_arch(W, rng, Fa, 13, 15, 4, "dn_rock", base_down=int(Fa.T) - 1)
    for k in range(90):
        q = spot(sd + rng.uniform(20, L - 30), (1 if rng.random() < 0.5 else -1) * rng.uniform(13, 55), 4)
        if q:
            if rng.random() < 0.55:
                cactus(W, rng, q[0], q[1], q[2])
            elif rng.random() < 0.6:
                palm(W, rng, q[0], q[1], q[2])
            else:
                hh = int(rng.integers(2, 9))
                for y in range(q[1], q[1] + hh):
                    W.put(q[0], y, q[2], "dn_pillar")
                for dx, dz in ((1, 0), (-1, 1), (0, -1)):
                    W.put(q[0] + dx * 2, q[1], q[2] + dz * 2, "dn_brick")
    for k, frac in enumerate((0.15, 0.44, 0.74)):
        F = Frame(B, sd + frac * L)
        x, z = F.cell(rng.uniform(-60, 60), 0)
        balloon(W, x, 62 + int(rng.integers(-6, 10)), z, 8, [["balloon_a", "balloon_b"], ["balloon_d", "balloon_c"], ["balloon_b", "balloon_c"]][k])
    tick("decor")

    # ------------------------------------------------------------------ oasis market near the end
    sm = so - 150
    F = Frame(B, sm)
    q = spot(sm, 30, 30)
    if q:
        oasis(W, rng, q[0], q[2], 30, 20)
    q = spot(so - 60, -34, 24)
    if q:
        market(W, rng, q[0] - 22, q[2] - 10, w=44, d=14)
    tick("oasis")

    # ------------------------------------------------------------------ road dressing
    for frac in (0.08, 0.2, 0.32, 0.46, 0.58, 0.7, 0.8, 0.9):
        s = sd + frac * L
        if bs is not None and abs(s - bs) < 75:
            continue
        boost_pad(W, B, s, 14, 4, "n_yellow" if int(frac * 10) % 2 else "n_orange", count=3, spacing=4)
    for frac in (0.12, 0.26, 0.4, 0.54, 0.66, 0.76, 0.86):
        s = sd + frac * L
        if bs is not None and abs(s - bs) < 75:
            continue
        item_boxes(W, B, s, 7.0, n=4)
    corner_furniture(W, B, i0, i1, 7.0, rmax=110.0, hay=False, stacks=True, wall_w=0.0, skid=True)
    flood_posts(W, B, i0, i1, 70, 10.5, 6, glow="kp_glow_orange")
    billboards(W, B, i0 + 30, i1 - 30, 160, 12.0, start=1)
    tick("dressing")
