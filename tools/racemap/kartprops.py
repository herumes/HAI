"""racemap kartprops: track furniture made of Kart_* blocks, placed beside a finished road (call AFTER the terrain, it checks
that there is ground under every piece)."""
import math

import numpy as np

from kart import (OPP, corners, facing_driver_rot, flag_rot, heading, lean_rot, outward, side_cell)
from core import mat

mat("kp_pole", "Build_GreyDark_Fence")
mat("kp_pole_metal", "Metal_Iron_Fence")
mat("kp_glow_warm", "Build_Lightsource_YellowLight")
mat("kp_glow_orange", "Build_Lightsource_Orange")
mat("kp_glow_cyan", "Build_Lightsource_Cyan")
mat("kp_glow_white", "Build_Lightsource_White")


def _gy(W, x, y, z, need=True, max_drop=4):
    """Height to put a piece standing on the ground near (x, z): starts at road level y and walks down until it rests on a block.
    None when the cell is already solid (inside a hill or wall) or nothing is below."""
    if not need:
        return y
    for d in range(max_drop + 1):
        yy = y - d
        if W.has(x, yy, z):
            return None
        if W.has(x, yy - 1, z):
            return yy
    return None


def asphalt_y(W, x, z, T):
    """y of the Kart_Asphalt surface at (x, z) near height T with nothing on top of it, else None."""
    for yy in (T, T - 1, T + 1):
        g = W.get(x, yy, z)
        if g is not None and g[0] == "k_asphalt" and not W.has(x, yy + 1, z):
            return yy
    return None


def drift_marks(W, path, c, hw, rng, n=None):
    """Skid-mark tiles on the outside half of the lane through a corner - where everyone drifts wide."""
    from kart import along_rot
    side = c["side"]
    a, b = c["i0"], c["i1"]
    n = n or int(min(14, max(4, (c["s1"] - c["s0"]) / 6)))
    kinds = ["k_skid", "k_skid_straight", "k_skid_curve", "k_skid"]
    for k in range(n):
        i = int(a + (b - a) * (0.25 + 0.7 * rng.random()))
        u = -side * rng.uniform(1.5, hw - 2.6)
        x, z = side_cell(path, i, u)
        y = asphalt_y(W, x, z, int(path.T[i]))
        if y is None:
            continue
        t = path.t[i]
        W.put(x, y + 1, z, kinds[int(rng.integers(0, len(kinds)))], along_rot(t[0], t[1]))


def corner_furniture(W, path, i0, i1, hw, rmax=90.0, off=None, signs=True, stacks=True, hay=False, skid=False, need_support=True,
                     min_len=10.0, stack_len=16.0, wall_w=1.0, vw=1.0, dy=0):
    """Warning signs before and at every corner tighter than rmax, plus tyre piles (or hay) on the outside of the apex.
    Returns the list of corners found."""
    found = corners(path, i0, i1, rmax, min_len)
    if skid:
        rng = np.random.default_rng(int(path.length) + i0)
        for c in found:
            drift_marks(W, path, c, hw, rng)
    off = off if off is not None else hw + vw + wall_w + 1.6
    for c in found:
        side = c["side"]                                  # +1 = right-hand corner
        out_u = -side * off
        for s in (c["s0"] - 24.0, c["s0"] - 9.0, c["sm"]):
            if not signs:
                break
            i = path.i(max(0.0, s))
            t = path.t[i]
            x, z = side_cell(path, i, out_u)
            y = _gy(W, x, int(path.T[i]) + dy, z, need_support)
            if y is None or any(W.has(x, y + k, z) for k in range(3)):
                continue
            W.put(x, y, z, "k_rsign" if side > 0 else "k_rsign_l", facing_driver_rot(heading(t[0], t[1])))
        if stacks or hay:
            n = int(min(c["s1"] - c["s0"], stack_len * 3) // 4)
            sm = c["sm"]
            for k in range(-n // 2, n // 2 + 1):
                i = path.i(sm + k * 3.2)
                x, z = side_cell(path, i, -side * (hw + vw + wall_w * 0.0 + 0.6))
                y = _gy(W, x, int(path.T[i]) + dy, z, need_support)
                if y is None:
                    continue
                kind = "k_hay" if hay else ("k_stack15" if k % 2 == 0 else "k_stack12")
                W.put(x, y, z, kind)
                if hay:
                    W.put(x, y + 1, z, "k_hay")
    return found


def flood_posts(W, path, i0, i1, every, off, h, glow="kp_glow_warm", both=True, need_support=True, skip=None, pole="kp_pole"):
    """A pole with a glowing block and a Kart_Floodlight head that leans toward the road."""
    n = 0
    for i in range(i0 + every, i1, every):
        if skip and skip(i):
            continue
        T = int(path.T[i])
        t = path.t[i]
        h_ = heading(t[0], t[1])
        for side in ((-1, 1) if both else (1,)):
            x, z = side_cell(path, i, side * off)
            g = _gy(W, x, T, z, need_support)
            if g is None:
                continue
            for y in range(g, T + h - 1):
                W.put(x, y, z, pole)
            W.put(x, T + h - 1, z, glow)
            W.put(x, T + h, z, "k_flood", lean_rot(OPP[outward(h_, side)]))
            n += 1
    return n


def billboards(W, path, i0, i1, every, off, flags=True, need_support=True, start=0, skip=None):
    """Kart_Billboard banners (and Flag poles in between) along a road, facing the oncoming driver."""
    k = 0
    for i in range(i0 + every, i1, every):
        if skip and skip(i):
            continue
        T = int(path.T[i])
        t = path.t[i]
        h_ = heading(t[0], t[1])
        side = 1 if (k + start) % 2 == 0 else -1
        x, z = side_cell(path, i, side * off)
        g = _gy(W, x, T, z, need_support)
        if g is not None:
            W.put(x, g, z, "k_billboard", facing_driver_rot(h_))
        if flags:
            j = min(i1 - 1, i + every // 2)
            x, z = side_cell(path, j, -side * off)
            g = _gy(W, x, int(path.T[j]), z, need_support)
            if g is not None:
                tj = path.t[j]
                # cloth streams with the direction of travel
                W.put(x, g, z, "k_flag", flag_rot(heading(tj[0], tj[1])))
        k += 1


def cone_gate(W, path, s, hw, need=True):
    """Two lines of alternating cones either side of a short stretch - a chicane marker in the last lane."""
    placed = 0
    for side in (-1, 1):
        for k, u in enumerate(np.arange(side * (hw - 1.0), side * (hw - 4.5), -side * 1.0)):
            i = path.i(s + k * 0.0)
            x, z = side_cell(path, i, u)
            y = int(path.T[i]) + 1
            if not W.has(x, y, z):
                W.put(x, y, z, "k_cone" if k % 2 == 0 else "k_cone_s")
                placed += 1
    return placed


def speed_bumps(W, path, s, hw, rows=2):
    for r in range(rows):
        i = path.i(s + r * 2.0)
        for u in np.arange(-hw + 1, hw - 0.9, 1.0):
            x, z = side_cell(path, i, u)
            y = int(path.T[i]) + 1
            if not W.has(x, y, z):
                W.put(x, y, z, "k_bump")


def skid_patch(W, path, s, u0, u1, kind="k_skid_straight"):
    """A run of skid-mark tiles (flat decals on the road)."""
    from kart import along_rot
    for ss in np.arange(s, s + 10.0, 1.0):
        i = path.i(ss)
        for u in np.arange(u0, u1 + 0.1, 1.0):
            x, z = side_cell(path, i, u)
            y = int(path.T[i]) + 1
            if not W.has(x, y, z):
                t = path.t[i]
                W.put(x, y, z, kind, along_rot(t[0], t[1]))
