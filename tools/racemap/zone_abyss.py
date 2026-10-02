"""Zone 4: Coral Abyss - a walled basin (YOU fill it with water to y=WATER_Y), seabed, reef, wreck, whale bones, temple, hoops."""
import math

import numpy as np

from core import fbm, mat, smoothstep, stair_rot
from kart import road_arrows
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, hoop, lbox, zone_gate, lcells
from layout import WATER_Y
from path import raster_band
from terrain import Grid, centre_field, pick, skin, slope_of

WALL_TOP = WATER_Y + 3
INNER = 18.0

mat("ab_road", ["Rock_Sandstone_White_Brick_Smooth", "Rock_Sandstone_White_Brick_Smooth", "Rock_Marble_Brick_Smooth"])
mat("ab_stair", "Rock_Marble_Brick_Stairs")
mat("ab_sand", ["Soil_Sand_White", "Soil_Sand_White", "Soil_Sand", "Soil_Gravel_Sand"])
mat("ab_rock", ["Rock_Stone_Cobble_Mossy", "Rock_Shale_Cobble", "Rock_Calcite", "Rock_Stone_Cobble"])
mat("ab_rock_top", ["Rock_Stone_Cobble_Mossy", "Soil_Grass_Full", "Rock_Stone_Cobble"])
mat("ab_rock_side", ["Rock_Stone_Brick", "Rock_Shale_Cobble", "Rock_Stone_Cobble_Mossy", "Rock_Calcite"])
mat("kelp", ["Plant_Seaweed_Grass_Tall", "Plant_Seaweed_Grass_Green", "Plant_Seaweed_Grass_Stack", "Plant_Seaweed_Grass_Bulbs"])
mat("kelp_stack", "Plant_Seaweed_Grass_Stack")
for c in ["Pink", "Orange", "Purple", "Cyan", "Yellow", "Red", "Blue", "Green", "Violet", "Lime", "White"]:
    mat("coral_" + c.lower(), "Plant_Coral_Block_" + c)
    mat("cbush_" + c.lower(), "Plant_Coral_Bush_" + c)
    mat("cmodel_" + c.lower(), "Plant_Coral_Model_" + c)
mat("coral_warm", ["Plant_Coral_Block_Pink", "Plant_Coral_Block_Orange", "Plant_Coral_Block_Red", "Plant_Coral_Block_Yellow"])
mat("coral_cool", ["Plant_Coral_Block_Cyan", "Plant_Coral_Block_Blue", "Plant_Coral_Block_Purple", "Plant_Coral_Block_Violet", "Plant_Coral_Block_Green"])
mat("coral_any", ["Plant_Coral_Block_Pink", "Plant_Coral_Block_Orange", "Plant_Coral_Block_Cyan", "Plant_Coral_Block_Purple",
                  "Plant_Coral_Block_Yellow", "Plant_Coral_Block_Lime", "Plant_Coral_Block_Violet", "Plant_Coral_Block_Blue"])
mat("cbush_any", ["Plant_Coral_Bush_Pink", "Plant_Coral_Bush_Orange", "Plant_Coral_Bush_Cyan", "Plant_Coral_Bush_Purple", "Plant_Coral_Bush_Yellow",
                  "Plant_Coral_Bush_Lime", "Plant_Coral_Bush_Violet", "Plant_Coral_Bush_Blue"])
mat("cmodel_any", ["Plant_Coral_Model_Pink", "Plant_Coral_Model_Orange", "Plant_Coral_Model_Cyan", "Plant_Coral_Model_Purple",
                   "Plant_Coral_Model_Yellow", "Plant_Coral_Model_Lime", "Plant_Coral_Model_Violet", "Plant_Coral_Model_Blue"])
mat("ship_hull", ["Wood_Darkwood_Planks", "Wood_Darkwood_Planks", "Wood_Blackwood_Planks"])
mat("ship_deck", "Wood_Hardwood_Planks")
mat("ship_beam", "Wood_Darkwood_Beam")
mat("ship_sail", "Cloth_Block_Wool_Gray_Light")
mat("whale", ["Rock_Chalk", "Rock_Chalk", "Rock_Calcite"])
mat("treasure", "Deco_Treasure_Pile_Large")
mat("starfish", ["Deco_Starfish", "Deco_Coral_Shell_Swirly", "Deco_Coral_Shell"])
mat("pearl", "Rock_Gem_Diamond")
mat("jelly_a", "Build_Lightsource_Pink")
mat("jelly_b", "Build_Lightsource_Cyan")
mat("jelly_c", "Build_Lightsource_White")
mat("temple", ["Rock_Marble_Brick", "Rock_Marble_Brick", "Rock_Marble_Brick_Smooth", "Rock_Sandstone_White_Brick"])
mat("temple_pillar", "Rock_Marble_Brick_Pillar_Middle")
mat("temple_base", "Rock_Marble_Brick_Pillar_Base")


def coral_tree(W, rng, x, y, z, h, cols):
    """A branching coral made of blocks."""
    col = cols
    trunk_h = int(h * 0.5)
    for k in range(trunk_h):
        W.put(x, y + k, z, col)
    for b in range(int(rng.integers(3, 6))):
        a = rng.uniform(0, 2 * math.pi)
        ln = int(rng.integers(3, max(4, h)))
        sx, sy, sz = x, y + trunk_h - int(rng.integers(0, 3)), z
        for t in range(1, ln + 1):
            px = int(round(sx + math.cos(a) * t * 0.7))
            pz = int(round(sz + math.sin(a) * t * 0.7))
            py = sy + int(t * 0.9)
            W.put(px, py, pz, col)
            if t == ln:
                W.ellipsoid(px, py, pz, 1.2, 1.2, 1.2, col)


def hull_cells(L, beam, depth):
    """Local cells (v along, u across, h up) for a wrecked ship hull."""
    cells = []
    for v in range(0, L + 1):
        t = v / float(L)
        w = beam * (math.sin(math.pi * (0.06 + 0.88 * t)) ** 0.7)
        if t > 0.85:
            w *= 1.0 + (t - 0.85) * 0.8
        d = depth * (0.35 + 0.65 * math.sin(math.pi * (0.04 + 0.92 * t)) ** 0.5)
        for u in range(-int(w) - 1, int(w) + 2):
            ru = abs(u) / max(1.0, w)
            if ru > 1.0:
                continue
            hb = -d * math.sqrt(max(0.0, 1 - ru * ru)) + d
            for h in range(int(hb), int(depth) + 1):
                edge = ru > 0.86 or h == int(hb)
                if edge or h == int(depth):
                    cells.append((v, u, h, "hull" if h < int(depth) else "deck"))
    return cells


def ship(W, rng, F, u0, v0, L=32, beam=7, depth=8, y0=0):
    for (v, u, h, kind) in hull_cells(L, beam, depth):
        if rng.random() < 0.06:
            continue               # rotten gaps
        x, z = F.cell(u0 + u, v0 + v)
        W.put(x, y0 + h, z, "ship_hull" if kind == "hull" else "ship_deck")
    # masts (one snapped, one leaning) and a torn sail
    for (vm, hm, lean) in ((int(L * 0.3), 18, 0.25), (int(L * 0.62), 11, -0.4)):
        for k in range(hm):
            x, z = F.cell(u0 + lean * k * 0.6, v0 + vm)
            W.put(x, y0 + depth + k, z, "ship_beam")
        if lean > 0:
            for k in range(3, hm - 2):
                for w in range(-3, 4):
                    if rng.random() < 0.8:
                        x, z = F.cell(u0 + w + lean * k * 0.6, v0 + vm + 1)
                        W.put(x, y0 + depth + k, z, "ship_sail")
    # stern cabin
    for v in range(L - 7, L - 1):
        for h in range(depth + 1, depth + 5):
            for u in (-4, 4):
                x, z = F.cell(u0 + u, v0 + v)
                W.put(x, y0 + h, z, "ship_hull")
    x, z = F.cell(u0, v0 + L - 4)
    W.put(x, y0 + depth + 1, z, "treasure")


def whale(W, F, v0, half=14, n=10, spacing=4):
    T = F.T
    for k in range(n):
        Fk = Frame(F.path, F.s + v0 + k * spacing)
        n_ = int(math.pi * half * 1.6)
        amp = 1.0 - abs(k - n / 2.0) / (n * 0.9)
        for j in range(n_ + 1):
            a = math.pi * j / n_
            u = half * math.cos(a)
            y = 1 + (half * 0.78 * amp + 2) * math.sin(a)
            for dd in (0, 1):
                x, z = Fk.cell(int(round(u + dd * math.cos(a))), 0)
                W.put(x, int(round(y + dd * math.sin(a))), z, "whale")
    # spine runs along the seabed beside the lane, and the skull rests to one side
    for k in range(n * spacing):
        Fk = Frame(F.path, F.s + v0 + k)
        x, z = Fk.cell(13, 0)
        W.put(x, 1 + int(abs(math.sin(k * 0.3))), z, "whale")
    Fs = Frame(F.path, F.s + v0 + n * spacing + 6)
    cx, cz = Fs.cell(11, 0)
    W.ellipsoid(cx, 7, cz, 7, 5, 8, "whale", mode="shell")
    for sgn in (-1, 1):
        x, z = Fs.cell(11 + sgn * 5, -6)
        W.ellipsoid(x, 7, z, 1.5, 1.5, 1.5, "jelly_b")


def temple(W, rng, F, u0, v0):
    """A sunken temple that stays between the lane edge (|u| 8) and the basin wall (|u| 18): platform 9 wide."""
    # platform
    for v in range(-10, 11):
        for u in range(-4, 5):
            x, z = F.cell(u0 + u, v0 + v)
            W.put(x, 0, z, "temple")
    for v in range(-8, 9, 4):
        for u in (-3, 3):
            h = int(rng.integers(5, 11))
            x, z = F.cell(u0 + u, v0 + v)
            W.put(x, 1, z, "temple_base")
            for y in range(2, h):
                W.put(x, y, z, "temple_pillar")
    # broken lintel + statue + treasure
    for u in range(-3, 4):
        if rng.random() < 0.6:
            x, z = F.cell(u0 + u, v0 - 8)
            W.put(x, 11, z, "temple")
    x, z = F.cell(u0, v0)
    for y in range(1, 6):
        W.put(x, y, z, "temple_pillar" if y < 4 else "temple")
    W.put(x, 6, z, "pearl")
    for du, dv in ((2, 2), (-2, 1), (1, -3)):
        x, z = F.cell(u0 + du, v0 + dv)
        W.put(x, 1, z, "treasure")


def jelly(W, rng, x, y, z):
    col = ["jelly_a", "jelly_b", "jelly_c"][int(rng.integers(0, 3))]
    W.ellipsoid(x, y, z, 2, 1.3, 2, col, part="upper")
    for t in range(int(rng.integers(3, 7))):
        dx, dz = int(rng.integers(-1, 2)), int(rng.integers(-1, 2))
        for k in range(1, int(rng.integers(3, 7))):
            W.put(x + dx, y - k, z + dz, col)


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["abyss"], m["frost"]
    L = s1 - s0
    i0, i1 = A.i(s0), A.i(s1)
    ch0, ch1 = s0 + 58, s0 + 495          # the water channel runs between the two dams

    st = KartStyle(hw=7.0, verge="k_conc", rail_fn=lambda s, side: "jersey" if A.Tat(s) >= 6 else None, edge=lambda s, u: "n_cyan" if int(s // 3) % 2 == 0 else "n_white", lane_dash=(4.0, 4.0))
    stamp_kart_road(W, A, i0, i1, st)

    # ------------------------------------------------------------------ the basin
    xs, zs = A.p[i0:i1, 0], A.p[i0:i1, 1]
    g = Grid(xs.min() - 50, xs.max() + 50, zs.min() - 50, zs.max() + 50)
    d, Tn, Sn = centre_field(g, A, i0, i1, 44, hw=0.0)
    inch = (Sn >= ch0) & (Sn <= ch1)
    H = np.where(d < INNER, np.where(inch, -1.0, WALL_TOP), np.where(d < INNER + 3, WALL_TOP, WALL_TOP - 0.85 * (d - INNER - 3)))
    H = np.where(d > 41, -1.0, H)
    road = d <= 9.8
    H = np.where(road, np.where(Tn > 0, Tn - 1, -1), H)
    # the dams hug the ramp: at the plateau the road is flush with the wall tops
    Ht = np.maximum(np.rint(H).astype(np.int64), -1)
    mask = d < 44
    inside = (d < INNER) & inch
    mats = ["ab_sand", "ab_rock_top", "ab_rock_side", "ab_rock"]
    top_ids = pick([((Ht >= WALL_TOP - 1) | (Ht >= 0), 1), (inside & (Ht < 0), 0)], 1)
    top_ids = np.where(Ht < 0, 0, top_ids)
    side_ids = pick([(d < INNER + 4, 2)], 3)
    skin(W, g, Ht, top_ids, side_ids, mats, mask=mask)

    # solid plugs across both ends of the channel (a skin is hollow underneath, so the dams need real blocks)
    for (sa, sb) in ((ch0 - 5, ch0 + 0.5), (ch1 - 0.5, ch1 + 5)):
        cx, cz, si, uu = raster_band(A, A.i(sa), A.i(sb), -(INNER + 4.0), INNER + 4.0)
        Tq = A.T[si]
        for y in range(0, WALL_TOP):
            top_ok = np.where(np.abs(uu) <= 9.6, Tq - 1, WALL_TOP - 1)
            sel = y <= top_ok
            if sel.any():
                W.put_arr(cx[sel], np.full(int(sel.sum()), y), cz[sel], "ab_rock")

    # water level markers and coral on the inner faces
    wall_inner = (d >= INNER - 0.6) & (d < INNER + 1.0) & inch
    ix, iz = np.nonzero(wall_inner)
    for k in range(ix.size):
        x, z = g.x0 + int(ix[k]), g.z0 + int(iz[k])
        if ((x // 3 + z // 3) % 4) == 0:
            W.put(x, WATER_Y, z, "n_cyan")
        if rng.random() < 0.09:
            y = int(rng.integers(0, WATER_Y))
            W.put(x, y, z, "coral_any")
            if rng.random() < 0.5:
                W.put(x, min(WATER_Y, y + 1), z, "coral_any")

    # ------------------------------------------------------------------ seabed life
    def seabed(n, dmin, dmax):
        ok = inside & (d >= dmin) & (d <= dmax)
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), g.z0 + int(iz[k])) for k in sel]

    for (x, z) in seabed(2400, 9.6, INNER - 1):
        W.put(x, 0, z, "kelp")
        if rng.random() < 0.25:
            for k in range(1, int(rng.integers(2, 6))):
                W.put(x, k, z, "kelp_stack")
    for (x, z) in seabed(160, 9.6, INNER - 2):
        W.put(x, 0, z, "cbush_any")
    for (x, z) in seabed(70, 9.6, INNER - 2):
        W.put(x, 0, z, "cmodel_any")
    for (x, z) in seabed(45, 11, INNER - 2):
        coral_tree(W, rng, x, 0, z, int(rng.integers(6, 12)), ["coral_warm", "coral_cool", "coral_any"][int(rng.integers(0, 3))])
    for (x, z) in seabed(80, 9.6, INNER - 1):
        W.put(x, 0, z, "starfish")
    # glowing jellies in the water column
    for (x, z) in seabed(46, 7.5, INNER - 1):
        jelly(W, rng, x, int(rng.integers(4, 10)), z)
    # lanterns of light embedded in the wall bases
    for (x, z) in seabed(60, INNER - 1.6, INNER - 0.4):
        W.put(x, 1, z, "lit_cyan")

    # ------------------------------------------------------------------ hoops: the submarine line (every 26 blocks)
    n_h = 0
    for sh in np.arange(ch0 + 26, ch1 - 10, 26.0):
        F = Frame(A, sh)
        cx, cz = F.cell(0, 0)
        wobble = 4.6 + 1.6 * math.sin(sh * 0.05)
        cy = int(round(wobble))
        hoop(W, (cx, cy, cz), (F.t[0], 0, F.t[1]), 6.6, "n_cyan" if n_h % 2 == 0 else "n_magenta", thick=2)
        if n_h % 3 == 0:
            hoop(W, (cx, cy, cz), (F.t[0], 0, F.t[1]), 8.4, "coral_any", thick=2)
        n_h += 1

    # ------------------------------------------------------------------ set pieces
    Fw = Frame(A, s0 + 175)
    ship(W, rng, Fw, 12.5, 0, L=34, beam=4, depth=8)
    Fb = Frame(A, s0 + 232)
    whale(W, Fb, 0, half=13, n=10, spacing=4)
    Ft = Frame(A, s0 + 345)
    temple(W, rng, Ft, -12.5, 0)
    Ft2 = Frame(A, s0 + 420)
    temple(W, rng, Ft2, 12.5, 0)
    # the reef tunnel: rings of coral the sub threads through
    for k in range(18):
        Fk = Frame(A, s0 + 290 + k * 2.4)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, 5, cz), (Fk.t[0], 0, Fk.t[1]), 8.5, "coral_warm" if k % 2 else "coral_cool", thick=2)
    # gates
    zone_gate(W, A, s0 + 25, "ABYSS", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="coral_cool", trim_mat="n_magenta",
              tower_u=44, board_half=44, tower_h=34)
    Fd = Frame(A, ch0 + 6)
    for k in range(3):
        hoop(W, (Fd.cell(0, 0)[0], Fd.T - 3 + 2, Fd.cell(0, 0)[1]), (Fd.t[0], 0, Fd.t[1]), 10 + k * 1.3, ["n_cyan", "n_magenta", "n_white"][k], thick=2)
    zone_gate(W, A, ch1 + 30, "SURFACE", scale=2, text_mat="n_yellow", board_mat="b_black", frame_mat="coral_warm", trim_mat="n_orange",
              tower_u=44, board_half=44, tower_h=34)
