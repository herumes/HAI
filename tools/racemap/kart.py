"""racemap kart: the race server's Kart_* blocks (and the NoCube neon blocks) as materials, plus the orientation rules
and small pieces of track furniture built from them.

Orientation facts (from the server's block list, and what buildscript.cpp's `road` op does):
  * thin pieces that lie ALONG the road (Armco, Barrier_Metal, Jersey, Catch_Fence, Curbs): rot 0 when the road runs
    east-west, rot 1 when it runs north-south.
  * Kart_Painted_Line / Painted_Yellow / Zebra: the line runs north-south at rot 0, so the opposite rule.
  * Armco_Corner / Barrier_Metal_Corner: rails go east+south at rot 0, north+east 1, north+west 2, south+west 3.
  * Kart_Arrow_Straight points north at rot 0 (1 west, 2 south, 3 east).
  * Rotatesign(_Left), Billboard: picture faces north at rot 0 (1 west, 2 south, 3 east) - i.e. rot for a driver heading
    south/east/north/west is 0/1/2/3.
  * Kart_Floodlight: the lamp head leans south at rot 0 (1 east, 2 north, 3 west).
  * Flag: cloth streams east at rot 0 (1 north, 2 west, 3 south).
  * Red_Workwall: front faces north at rot 0 (3 wide, centred).
"""
import math

import numpy as np

from core import mat, stair_rot

# --------------------------------------------------------------------------- materials
mat("k_asphalt", "Kart_Asphalt")
mat("k_asph_slab", "Kart_Asphalt_Slab")
mat("k_asph_quarter", "Kart_Asphalt_Quarter")
mat("k_asph_stair", "Kart_Asphalt_Stairs")
mat("k_conc", "Kart_Concrete")
mat("k_conc_stair", "Kart_Concrete_Stairs")
mat("k_grass", "Kart_Grass")
mat("k_andesite", "Andesite_Smooth")
mat("k_andesite_slab", "Andesite_Smooth_Slab")

mat("k_curb_red", "Kart_Curb_Red")
mat("k_curb_yel", "Kart_Curb_Yellow")
mat("k_curb_red_b", "Kart_Curb_Red_Block")
mat("k_curb_yel_b", "Kart_Curb_Yellow_Block")
mat("k_bump", "Kart_Speed_Bump")
mat("k_chevron", "Kart_Chevron_Block")

mat("k_finish_line", "Finish_Line")
mat("k_checker", "Kart_Finish_Checker")
mat("k_line", "Kart_Painted_Line")
mat("k_yline", "Kart_Painted_Yellow")
mat("k_arrow", "Kart_Arrow_Straight")
mat("k_zebra", "Kart_Zebra_Crossing")
mat("k_pitbox", "Kart_Pit_Box")
mat("k_skid", "Kart_Skidmarks")
mat("k_skid_straight", "Kart_Skidmarks_Straight")
mat("k_skid_curve", "Kart_Skidmarks_Curve")
mat("k_skid_cross", "Kart_Skidmarks_Cross")
mat("k_skid_end", "Kart_Skidmarks_End")
GRID = ["Kart_Start_Grid", "Kart_Start_Grid_2", "Kart_Start_Grid_3", "Kart_Start_Grid_4", "Kart_Start_Grid_5",
        "Kart_Start_Grid_6", "Kart_Start_Grid_7", "Kart_Start_Grid_8", "Kart_Start_Grid_9", "Kart_Start_Grid_0"]
for _i, _n in enumerate(GRID):
    mat("k_grid%d" % (_i + 1), _n)

mat("k_armco", "Kart_Armco")
mat("k_armco_c", "Kart_Armco_Corner")
mat("k_mesh", "Kart_Barrier_Metal")
mat("k_mesh_c", "Kart_Barrier_Metal_Corner")
mat("k_catch", "Kart_Catch_Fence")
mat("k_jersey", "Kart_Jersey")
mat("k_tyre", "Kart_Tire_Wall")
mat("k_stack15", "Stack1_5x1_5")
mat("k_stack12", "Stack1x2")
mat("k_hay", "Kart_Hay_Bale")

mat("k_cone", "Kart_Cone")
mat("k_cone_s", "Kart_Cone_Striped")
mat("k_flood", "Kart_Floodlight")
mat("k_trophy", "Kart_Trophy")
mat("k_pod1", "Kart_Podium_1")
mat("k_pod2", "Kart_Podium_2")
mat("k_pod3", "Kart_Podium_3")
mat("k_flag", "Flag")
mat("k_billboard", "Kart_Billboard")
mat("k_rsign", "Rotatesign")
mat("k_rsign_l", "Rotatesign_Left")
mat("k_redblock", "Red_Block")
mat("k_work", "Work")
mat("k_workwall", "Red_Workwall")
mat("k_roof_slate", "Kart_Slate_Roof_Shallow")
mat("k_roof_calcite", "Kart_Calcite_Roof_Shallow")

# NoCube neon blocks: flat, unlit colours - glowing trims, signs, light strips, pixel art
NEON = {"black": "Black", "blue": "Blue", "crimson": "Crimson", "cyan": "Cyan", "darkgray": "Dark_Gray", "firered": "Fire_Red",
        "green": "Green", "lightblue": "Light_Blue", "lightgray": "Light_Gray", "lime": "Lime", "magenta": "Magenta",
        "orange": "Orange", "pink": "Pink", "purple": "Purple", "red": "Red", "white": "White", "yellow": "Yellow"}
for _k, _v in NEON.items():
    mat("n_" + _k, "NoCube_Neon_Block_" + _v)
RAINBOW = ["n_red", "n_orange", "n_yellow", "n_lime", "n_cyan", "n_blue", "n_purple", "n_magenta"]
mat("n_rainbow", ["NoCube_Neon_Block_" + NEON[k[2:]] for k in RAINBOW])
mat("n_cool", ["NoCube_Neon_Block_Cyan", "NoCube_Neon_Block_Blue", "NoCube_Neon_Block_Light_Blue", "NoCube_Neon_Block_Purple",
               "NoCube_Neon_Block_Magenta"])
mat("n_warm", ["NoCube_Neon_Block_Fire_Red", "NoCube_Neon_Block_Orange", "NoCube_Neon_Block_Yellow", "NoCube_Neon_Block_Crimson"])


# --------------------------------------------------------------------------- orientation helpers
def along_rot(tx, tz):
    """rot for thin pieces that run along the road (0 = east-west)."""
    return 0 if abs(tx) >= abs(tz) else 1


def line_rot(tx, tz):
    """rot for the painted line / zebra tiles (their line runs north-south at rot 0)."""
    return 1 - along_rot(tx, tz)


def heading(tx, tz):
    """Compass letter of the direction (tx, tz) = (east, south)."""
    if abs(tx) >= abs(tz):
        return "e" if tx > 0 else "w"
    return "s" if tz > 0 else "n"


def arrow_rot(h):
    return {"n": 0, "w": 1, "s": 2, "e": 3}[h]


def facing_driver_rot(h):
    """rot of a picture-faces-north-at-0 piece so it faces a driver heading h."""
    return {"s": 0, "e": 1, "n": 2, "w": 3}[h]


def lean_rot(to):
    """Kart_Floodlight: rot so the lamp head leans toward compass direction `to`."""
    return {"s": 0, "e": 1, "n": 2, "w": 3}[to]


def flag_rot(to):
    """Flag: rot so the cloth streams toward compass direction `to`."""
    return {"e": 0, "n": 1, "w": 2, "s": 3}[to]


def workwall_rot(face):
    """Red_Workwall: rot so the front faces compass direction `face`."""
    return {"n": 0, "w": 1, "s": 2, "e": 3}[face]


def outward(h, side):
    """Compass direction of the side of the road: side +1 = driver's right, -1 = left, for a driver heading h."""
    right = {"e": "s", "s": "w", "w": "n", "n": "e"}[h]
    left = {"e": "n", "n": "w", "w": "s", "s": "e"}[h]
    return right if side > 0 else left


OPP = {"n": "s", "s": "n", "e": "w", "w": "e"}
DIRV = {"e": (1, 0), "w": (-1, 0), "s": (0, 1), "n": (0, -1)}
CORNER_ROT = {frozenset("es"): 0, frozenset("ne"): 1, frozenset("nw"): 2, frozenset("sw"): 3}


def cell_of(q):
    return int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))


# --------------------------------------------------------------------------- curvature
def curvature(path, win=12):
    """Signed turning rate per sample in 1/blocks; positive = turning toward the driver's right.  Cached on the path."""
    k = getattr(path, "_kappa", None)
    if k is not None:
        return k
    n = len(path.s)
    t = path.t
    kap = np.zeros(n)
    for i in range(n):
        a, b = max(0, i - win), min(n - 1, i + win)
        ta, tb = t[a], t[b]
        ang = math.atan2(ta[0] * tb[1] - ta[1] * tb[0], ta[0] * tb[0] + ta[1] * tb[1])
        ds = max(1e-6, path.s[b] - path.s[a])
        # path.r is (-tz, tx) = right-hand side in (east, south): turning toward r means cross(ta, tb) > 0 here
        kap[i] = ang / ds
    path._kappa = kap
    return kap


def corners(path, i0, i1, rmax=90.0, min_len=8.0):
    """Runs of samples whose radius is below rmax: [dict(s0, s1, sm, i0, i1, side, radius)]."""
    kap = curvature(path)
    out = []
    cur = None
    for i in range(i0, i1):
        inside = abs(kap[i]) * rmax >= 1.0
        if inside and cur is None:
            cur = [i, i]
        elif inside:
            cur[1] = i
        elif cur is not None:
            out.append(cur)
            cur = None
    if cur is not None:
        out.append(cur)
    res = []
    for a, b in out:
        if (b - a) * 0.5 < min_len:
            continue
        seg = kap[a:b + 1]
        sgn = 1 if seg.sum() > 0 else -1
        res.append(dict(i0=a, i1=b, s0=float(path.s[a]), s1=float(path.s[b]), sm=float(path.s[(a + b) // 2]), side=sgn,
                        radius=float(1.0 / max(1e-9, np.abs(seg).max()))))
    return res


# --------------------------------------------------------------------------- pieces placed relative to the road
def side_cell(path, i, u):
    q = path.p[i] + path.r[i] * u
    return cell_of(q)


def supported(W, x, y, z):
    return W.has(x, y - 1, z)


def place_beside(W, path, s, u, m, rot=None, dy=0, need_support=True, clear=0):
    """Put block m at lateral offset u (right of travel) of path position s, at road height + dy.
    clear > 0 erases that many cells above first.  Returns True when placed."""
    i = path.i(s)
    x, z = side_cell(path, i, u)
    y = int(path.T[i]) + dy
    if need_support and not supported(W, x, y, z):
        return False
    for k in range(clear):
        W.erase(x, y + k, z)
    W.put(x, y, z, m, rot)
    return True


def cone_pair(W, path, s, u0, u1, step=1.0):
    """A small row of traffic cones across part of the road (a chicane gate)."""
    n = 0
    for u in np.arange(u0, u1 + 0.01, step):
        i = path.i(s)
        x, z = side_cell(path, i, u)
        y = int(path.T[i]) + 1
        if not W.has(x, y, z):
            W.put(x, y, z, "k_cone" if n % 2 == 0 else "k_cone_s")
            n += 1
    return n


def road_arrows(W, path, s, n=3, spacing=6.0, u=0.0):
    """Kart_Arrow_Straight decals down the centre line, pointing the way of travel."""
    for k in range(n):
        i = path.i(s + k * spacing)
        x, z = side_cell(path, i, u)
        t = path.t[i]
        if not W.has(x, int(path.T[i]) + 1, z):
            W.put(x, int(path.T[i]) + 1, z, "k_arrow", arrow_rot(heading(t[0], t[1])))


def zebra(W, path, s, hw, width=3):
    """A zebra crossing band across the road (stripes run along the road)."""
    i0, i1 = path.i(s), path.i(s + width)
    from path import raster_band
    cx, cz, si, uu = raster_band(path, i0, i1, -hw, hw)
    for x, z, sidx in zip(cx.tolist(), cz.tolist(), si.tolist()):
        t = path.t[sidx]
        y = int(path.T[sidx]) + 1
        if not W.has(x, y, z):
            W.put(x, y, z, "k_zebra", line_rot(t[0], t[1]))


def finish_band(W, path, s, hw, rows=2):
    """The start / finish line: Kart_Finish_Checker tiles, `rows` cells deep, across the whole road."""
    from path import raster_band
    i0, i1 = path.i(s), path.i(s + rows)
    cx, cz, si, uu = raster_band(path, i0, i1, -hw, hw)
    for x, z, sidx in zip(cx.tolist(), cz.tolist(), si.tolist()):
        W.put(x, int(path.T[sidx]) + 1, z, "k_checker")


def start_grid(W, path, s_line, hw, rows=5, row_gap=6.0, off=3.4):
    """Two columns of numbered start slots behind the line (slot 1 front-left ... 10 back-right), each with a box."""
    n = 0
    for r in range(rows):
        s = s_line - 6.0 - r * row_gap
        for side in (-1, 1):
            i = path.i(s)
            x, z = side_cell(path, i, side * off)
            T = int(path.T[i])
            n += 1
            W.put(x, T + 1, z, "k_grid%d" % n)
            # slot brackets: a short line either side
            for du in (-1.6, 1.6):
                x2, z2 = side_cell(path, i, side * off + du)
                t = path.t[i]
                W.put(x2, T + 1, z2, "k_line", line_rot(t[0], t[1]))
    return n


DECALS = ("k_line", "k_yline", "k_curb_red", "k_curb_yel")


def boost_pad(W, path, s0, length, hw_inner, m, count=3, spacing=4.0, pad=0.85):
    """Boost chevrons (neon blocks) laid INTO the road surface, pointing the way of travel; clears the decals on them."""
    from path import raster_band
    i0, i1 = path.i(s0), path.i(s0 + length)
    cx, cz, si, uu = raster_band(path, i0, i1, -hw_inner - 1, hw_inner + 1)
    n = 0
    for x, z, sidx, u in zip(cx.tolist(), cz.tolist(), si.tolist(), uu.tolist()):
        v = float(path.s[sidx]) - s0
        for k in range(count):
            v0 = k * spacing
            if abs(abs(u) - (hw_inner - (v - v0))) < pad and 0 <= v - v0 <= hw_inner + 0.5:
                y = int(path.T[sidx])
                W.put(x, y, z, m)
                g = W.get(x, y + 1, z)
                if g is not None and g[0] in DECALS:
                    W.erase(x, y + 1, z)
                n += 1
                break
    return n
