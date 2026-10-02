"""Zone 1: Emerald Meadows - hedged road through rolling hills, windmills, giant mushrooms, a covered bridge, balloons."""
import math

import numpy as np

from core import fbm, mat, smoothstep, stair_rot
from kart import boost_pad
from kartprops import billboards, corner_furniture, flood_posts
from kartroad import KartStyle, stamp_kart_road
from kit import (Frame, bunting, cloud, hoop, item_boxes, lbox, lsign, oak, pine, road_grid, zone_gate, blob)
from terrain import Grid, carve, centre_field, pick, skin, slope_of

mat("hedge", ["Plant_Leaves_Oak", "Plant_Leaves_Oak", "Plant_Leaves_Jungle", "Plant_Leaves_Maple"])
mat("m_grass", ["Soil_Grass_Full", "Soil_Grass_Full", "Soil_Grass_Sunny_Full"])
mat("m_dirt", ["Soil_Dirt", "Soil_Dirt", "Soil_Dirt_Dry"])
mat("m_rock", ["Rock_Stone_Cobble", "Rock_Stone_Cobble_Mossy", "Rock_Stone_Cobble_Mossy"])
mat("pond", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("pond_edge", ["Soil_Sand", "Soil_Gravel"])
mat("planks", ["Wood_Darkwood_Planks", "Wood_Hardwood_Planks"])
mat("wood_beam", "Wood_Darkwood_Beam")
mat("shingle", "Cloth_Roof_Red")
mat("shingle_stair", "Wood_Darkwood_Stairs")
mat("mush_stem", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Cloth_Block_Wool_White"])
mat("mush_cap_red", "Cloth_Block_Wool_Red")
mat("mush_cap_blue", "Cloth_Block_Wool_Blue")
mat("mush_cap_pink", "Cloth_Block_Wool_Pink")
mat("mush_cap_orange", "Cloth_Block_Wool_Orange")
mat("mush_spot", "Cloth_Block_Wool_White")
mat("mill_wall", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Rock_Chalk"])
mat("mill_roof", "Cloth_Block_Wool_Red")
mat("mill_sail", "Cloth_Block_Wool_White")
mat("mill_beam", "Wood_Darkwood_Beam")
mat("cottage_wall", ["Wood_Village_Wall_White_Full", "Wood_Village_Wall_White_Full", "Wood_Village_Wall_Yellow_Full"])
mat("cottage_wall2", "Wood_Village_Wall_Blue_Full")
mat("cottage_wall3", "Wood_Village_Wall_Pink_Full")
mat("balloon_a", "Cloth_Block_Wool_Red")
mat("balloon_b", "Cloth_Block_Wool_Yellow")
mat("balloon_c", "Cloth_Block_Wool_White")
mat("balloon_d", "Cloth_Block_Wool_Blue")
mat("fence", "Wood_Hardwood_Fence")
mat("castle_wall", ["Rock_Stone_Brick", "Rock_Stone_Brick", "Rock_Stone_Brick_Smooth", "Rock_Stone_Cobble_Mossy"])
mat("castle_trim", "Rock_Marble_Brick")
mat("castle_floor", ["Rock_Stone_Brick_Smooth", "Soil_Gravel"])
mat("castle_roof_red", "Cloth_Block_Wool_Red")
mat("castle_roof_blue", "Cloth_Block_Wool_Blue")
mat("castle_banner_a", "Cloth_Block_Wool_Red")
mat("castle_banner_b", "Cloth_Block_Wool_Yellow")
mat("castle_bars", "Metal_Iron_Fence")
mat("castle_stair", "Rock_Stone_Brick_Stairs")
mat("rainbow_r", "Cloth_Block_Wool_Red")
mat("rainbow_o", "Cloth_Block_Wool_Orange")
mat("rainbow_y", "Cloth_Block_Wool_Yellow")
mat("rainbow_g", "Cloth_Block_Wool_Green")
mat("rainbow_c", "Cloth_Block_Wool_Cyan")
mat("rainbow_b", "Cloth_Block_Wool_Blue")
mat("rainbow_p", "Cloth_Block_Wool_Purple")
RB = ["rainbow_r", "rainbow_o", "rainbow_y", "rainbow_g", "rainbow_c", "rainbow_b", "rainbow_p"]


def rainbow_arch(W, F, r_out, band=2, thick=2):
    """Half-ring of colour bands over the road; F is the frame at the arch (axis aligned looks best)."""
    T = F.T
    for bi, m in enumerate(RB):
        r0 = r_out - (bi + 1) * band
        r1 = r_out - bi * band
        for k in range(0, 181):
            a = math.radians(k)
            for rr in np.arange(r0, r1, 0.5):
                u = rr * math.cos(a)
                h = rr * math.sin(a)
                for v in range(thick):
                    x, z = F.cell(int(round(u)), v)
                    W.put(x, T + int(round(h)), z, m)


def windmill(W, x, z, face, h=26):
    # tapered tower
    for y in range(0, h):
        r = 6.4 - 2.6 * y / h
        W.cyl(x, z, y, y, r, "mill_wall", mode="fill" if y == 0 else "wall")
    # door + windows
    dx, dz = face
    for yy in (1, 2):
        W.put(x + dx * 6, yy, z + dz * 6, "planks")
    for yy in (10, 17):
        W.put(x + dx * 5 + 0, yy, z + dz * 5, "lit_yellowlight")
    # roof cone
    for i in range(7):
        rr = 4.6 - i * 0.7
        if rr < 0.5:
            break
        W.cyl(x, z, h + i, h + i, rr, "mill_roof", mode="fill")
    W.put(x, h + 7, z, "lit_orange")
    # hub + sails (vertical plane perpendicular to face)
    hx, hy, hz = x + dx * 4, h - 3, z + dz * 4
    ax = np.array([-dz, 0, dx], float)    # horizontal axis of the sail plane
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 8
        d = np.array([ax[0] * math.cos(a), math.sin(a), ax[2] * math.cos(a)])
        for t in range(2, 22):
            p = np.array([hx, hy, hz]) + d * t
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "mill_beam")
            if t > 5:
                n = np.cross(d, np.array([dx, 0, dz], float))
                n /= max(1e-6, np.linalg.norm(n))
                for w in range(1, 4):
                    q = p + n * w * (1 if k % 2 == 0 else 1)
                    W.put(int(round(q[0])), int(round(q[1])), int(round(q[2])), "mill_sail")
    W.ellipsoid(hx, hy, hz, 2, 2, 2, "mill_beam")


def mushroom(W, rng, x, y, z, h, r, cap):
    for k in range(h):
        W.cyl(x, z, y + k, y + k, max(1.0, r * 0.32 + (0.6 if k < 2 else 0)), "mush_stem")
    W.ellipsoid(x, y + h, z, r, r * 0.62, r, cap, part="upper")
    # underside rim
    W.cyl(x, z, y + h - 1, y + h - 1, r - 0.8, cap, mode="wall")
    # spots
    for _ in range(int(r * 1.5)):
        a = rng.uniform(0, 2 * math.pi)
        rr = rng.uniform(0.1, 0.85) * r
        px, pz = x + rr * math.cos(a), z + rr * math.sin(a)
        ht = r * 0.62 * math.sqrt(max(0.0, 1 - (rr / (r + 0.5)) ** 2))
        W.put(int(round(px)), int(y + h + round(ht)), int(round(pz)), "mush_spot")


def castle(W, rng, cx, cz, yb):
    """Emerald Castle: curtain wall with battlements, four round towers, a central keep, a gate and banners."""
    H = 11
    a = 20
    # courtyard floor
    W.box((cx - a, yb - 1, cz - a), (cx + a, yb - 1, cz + a), "castle_floor")
    # curtain walls (3 thick) with gate gap on the south side
    for t in range(3):
        W.box((cx - a + t, yb, cz - a + t), (cx + a - t, yb + H - 1, cz + a - t), "castle_wall", mode="walls")
    for x in range(cx - a, cx + a + 1):
        for z in (cz - a, cz + a):
            if (x + z) % 2 == 0:
                W.put(x, yb + H, z, "castle_wall")
    for z in range(cz - a, cz + a + 1):
        for x in (cx - a, cx + a):
            if (x + z) % 2 == 0:
                W.put(x, yb + H, z, "castle_wall")
    # gate
    for x in range(cx - 3, cx + 4):
        for y in range(yb, yb + 8):
            for z in range(cz + a - 2, cz + a + 1):
                W.erase(x, y, z)
    for x in range(cx - 3, cx + 4):
        W.put(x, yb + 8, cz + a, "castle_trim")
        if x in (cx - 2, cx, cx + 2):
            W.put(x, yb + 7, cz + a, "castle_bars")
    for y in range(yb, yb + 7):
        W.put(cx - 4, y, cz + a, "castle_trim")
        W.put(cx + 4, y, cz + a, "castle_trim")
    # ramp of stairs up to the gate
    for k in range(0, yb + 1):
        for x in range(cx - 4, cx + 5):
            W.put(x, yb - 1 - k, cz + a + 1 + k, "castle_stair", rot=stair_rot("n"))
            for yy in range(0, yb - 1 - k):
                W.put(x, yy, cz + a + 1 + k, "castle_floor")
    # corner towers with cone roofs and flags
    for sx in (-1, 1):
        for sz in (-1, 1):
            tx, tz = cx + sx * a, cz + sz * a
            W.cyl(tx, tz, yb, yb + 23, 6, "castle_wall", mode="wall")
            W.cyl(tx, tz, yb + 24, yb + 24, 7, "castle_trim", mode="fill")
            for k in range(10):
                r = 7 - k * 0.72
                if r < 0.6:
                    break
                W.cyl(tx, tz, yb + 25 + k, yb + 25 + k, r, "castle_roof_red" if sx * sz > 0 else "castle_roof_blue", mode="wall" if r > 2 else "fill")
            for y in range(yb + 35, yb + 41):
                W.put(tx, y, tz, "fence")
            for dy in range(3):
                for dxx in range(1, 5):
                    W.put(tx + dxx, yb + 40 - dy, tz, "castle_banner_a" if sx > 0 else "castle_banner_b")
            for k in range(0, 24, 5):
                W.put(tx, yb + 6 + k, tz + 6 * sz, "lit_yellowlight")
    # keep
    k0, kr = 7, 28
    W.box((cx - k0, yb, cz - k0), (cx + k0, yb + kr, cz + k0), "castle_wall", mode="walls")
    W.box((cx - k0 - 1, yb + kr + 1, cz - k0 - 1), (cx + k0 + 1, yb + kr + 1, cz + k0 + 1), "castle_trim", mode="fill")
    for i in range(10):
        r = k0 + 1 - i
        if r < 0:
            break
        W.box((cx - r, yb + kr + 2 + i, cz - r), (cx + r, yb + kr + 2 + i, cz + r), "castle_roof_blue", mode="walls" if r > 0 else "fill")
    for y in range(yb + kr + 12, yb + kr + 20):
        W.put(cx, y, cz, "fence")
    for dy in range(4):
        for dxx in range(1, 7):
            W.put(cx + dxx, yb + kr + 19 - dy, cz, "castle_banner_a")
    for y in range(yb + 4, yb + kr, 6):
        for dxx in (-3, 0, 3):
            W.put(cx + dxx, y, cz + k0, "lit_yellowlight")
            W.put(cx + dxx, y + 1, cz + k0, "lit_yellowlight")
    for y in range(yb + 8, yb + 20):
        for dxx in (-1, 0, 1):
            W.put(cx + dxx, y, cz + k0 + 1, "castle_banner_a" if y < yb + 16 else "castle_banner_b")
    # keep door
    for y in range(yb, yb + 5):
        for dxx in (-1, 0, 1):
            W.erase(cx + dxx, y, cz + k0)
    # courtyard trees
    for (dx, dz) in ((-13, 12), (13, 12), (-13, -12), (13, -12)):
        oak(W, rng, cx + dx, yb, cz + dz, h=5)


def cottage(W, x, z, face, wall, w=9, d=7, h=5):
    W.box((x, 0, z), (x + w, h - 1, z + d), wall, mode="walls")
    W.box((x, -1, z), (x + w, -1, z + d), "planks")
    W.box((x, 0, z), (x + w, 0, z + d), "planks", mode="floor")
    # corner beams
    for cx in (x, x + w):
        for cz in (z, z + d):
            W.box((cx, 0, cz), (cx, h - 1, cz), "wood_beam")
    # gable roof along x
    half = d // 2 + 1
    for i in range(half + 1):
        for xx in range(x - 1, x + w + 2):
            W.put(xx, h + i, z - 1 + i, "shingle_stair", rot=stair_rot("s"))
            W.put(xx, h + i, z + d + 1 - i, "shingle_stair", rot=stair_rot("n"))
    # door + windows on the front (south, +z side)
    W.put(x + w // 2, 0, z + d, "planks")
    W.put(x + 2, 2, z + d, "lit_yellowlight")
    W.put(x + w - 2, 2, z + d, "lit_yellowlight")
    # chimney
    W.box((x + w - 2, h, z + 1), (x + w - 2, h + 4, z + 1), "m_rock")


def balloon(W, x, y, z, r, cols):
    for yy in range(-r, r + 1):
        rad = math.sqrt(max(0.0, (r + 0.5) ** 2 - yy * yy))
        if yy < -r * 0.3:
            rad *= 1.0 - (-yy - r * 0.3) / (r * 1.6)
        for seg in range(0, 12):
            pass
        ring = int(rad * 7 + 6)
        for k in range(ring):
            a = 2 * math.pi * k / ring
            m = cols[int((a / (2 * math.pi)) * 8) % len(cols)]
            W.put(int(round(x + rad * math.cos(a))), y + yy, int(round(z + rad * math.sin(a))), m)
    # basket + ropes
    by = y - r - 6
    W.box((x - 1, by, z - 1), (x + 1, by + 1, z + 1), "planks")
    for dx, dz in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        for yy in range(by + 2, y - r + 2):
            W.put(x + dx, yy, z + dz, "fence")
    W.put(x, by + 2, z, "lit_orange")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["meadows"], m["canyon"]
    i0, i1 = A.i(s0), A.i(s1)

    # ------------------------------------------------------------------ road
    st = KartStyle(hw=7.0, verge="k_grass", wall=("hedge",), wall_h=2, wall_period=1000, curbs=("k_curb_red",), curb_len=4.0,
                   curb_rmax=160.0, lane_dash=(3.0, 3.0))
    stamp_kart_road(W, A, i0, i1, st)

    # ------------------------------------------------------------------ terrain
    xs, zs = A.p[i0:i1, 0], A.p[i0:i1, 1]
    g = Grid(xs.min() - 70, xs.max() + 70, zs.min() - 70, zs.max() + 70)
    d, Tn, Sn = centre_field(g, A, i0, i1, 56, hw=9.0)
    amp = smoothstep(s0, s0 + 50, Sn) * (1 - smoothstep(s1 - 50, s1, Sn))
    n1 = fbm(g.X, g.Z, 95.0, 4, 11)
    hills = 22.0 * smoothstep(0.42, 0.78, n1) * amp
    ridge = 7.0 * smoothstep(0.35, 0.8, fbm(g.X + 500, g.Z - 300, 40.0, 3, 5)) * amp
    H = hills + ridge - 1.0
    shoulder = np.where(Tn > 0, Tn - 1, -1)
    Hc = carve(H, d, shoulder, 4.0, 20.0)
    Ht = np.rint(Hc).astype(np.int64)
    Ht[d > 52] = -1
    Ht = np.maximum(Ht, -1)
    mask = d < 52
    # a hill for the castle, inside the big northern bend
    CX, CZ, CR, CH = 395, 55, 38, 9
    rr_ = np.hypot(g.X - CX, g.Z - CZ)
    mound = np.where(rr_ < 27, CH, CH * (1 - smoothstep(27, CR, rr_)))
    Ht = np.where(rr_ < CR + 2, np.maximum(Ht, np.rint(mound).astype(np.int64)), Ht)
    Ht = np.where(rr_ < 27, CH, Ht)
    mask = mask | (rr_ < CR + 2)
    rb = road_grid(ctx, g)
    mask = mask & ~(rb & (d > 12))
    slope = slope_of(Hc)
    mats = ["m_grass", "m_dirt", "m_rock"]
    top_ids = pick([(slope > 1.4, 2)], 0)
    side_ids = pick([(slope > 1.1, 2)], 1)
    skin(W, g, Ht, top_ids, side_ids, mats, mask=mask)

    # ------------------------------------------------------------------ decoration on the grass
    castle(W, rng, CX, CZ, CH + 1)
    def candidates(n, dmin, dmax, slope_max=0.9, need_grass=True):
        ok = (d >= dmin) & (d <= dmax) & (Ht >= -1) & (slope < slope_max) & mask & ~rb
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), int(Ht[ix[k], iz[k]]), g.z0 + int(iz[k])) for k in sel]

    # flowers and tall grass
    ix, iz = np.nonzero((d >= 2.5) & (d <= 46) & (slope < 0.8) & mask & (Ht >= -1) & ~rb)
    pick_f = rng.random(ix.size) < 0.06
    xf, zf = g.x0 + ix[pick_f], g.z0 + iz[pick_f]
    yf = Ht[ix[pick_f], iz[pick_f]] + 1
    W.put_arr(xf, yf, zf, "flowers")

    for (x, y, z) in candidates(46, 14, 50):
        oak(W, rng, x, y + 1, z)
    for (x, y, z) in candidates(16, 26, 60, 1.0):
        mushroom(W, rng, x, y + 1, z, int(rng.integers(9, 18)), int(rng.integers(6, 11)),
                 ["mush_cap_red", "mush_cap_blue", "mush_cap_pink", "mush_cap_orange"][int(rng.integers(0, 4))])
    # cottage village in the big inside curve
    vill = candidates(80, 30, 52, 0.4)
    placed = []
    for (x, y, z) in vill:
        if y != -1 and y > 2:
            continue
        if any(abs(x - px) < 16 and abs(z - pz) < 14 for px, pz in placed):
            continue
        if len(placed) >= 6:
            break
        placed.append((x, z))
        cottage(W, x, z, (0, 1), ["cottage_wall", "cottage_wall2", "cottage_wall3"][len(placed) % 3])
    # windmills at the outer bends
    for frac in (0.18, 0.55, 0.86):
        sw = s0 + frac * (s1 - s0)
        F = Frame(A, sw)
        x, z = F.cell(-34 if frac != 0.55 else 36, 0)
        ixy = g.idx(np.array([x]), np.array([z]))
        windmill(W, x, z, (0, 1) if frac != 0.55 else (0, -1))
    # hot air balloons
    cols = [["balloon_a", "balloon_c"], ["balloon_b", "balloon_d"], ["balloon_d", "balloon_c"], ["balloon_a", "balloon_b"]]
    for k, frac in enumerate((0.1, 0.33, 0.62, 0.9)):
        F = Frame(A, s0 + frac * (s1 - s0))
        x, z = F.cell(rng.uniform(-45, 45), rng.uniform(-30, 30))
        balloon(W, x, 62 + int(rng.integers(-6, 10)), z, 9, cols[k])
    for k in range(6):
        F = Frame(A, s0 + rng.uniform(0.02, 0.98) * (s1 - s0))
        x, z = F.cell(rng.uniform(-60, 60), rng.uniform(-20, 20))
        cloud(W, rng, x, int(rng.integers(74, 96)), z, int(rng.integers(12, 22)), int(rng.integers(4, 7)), int(rng.integers(8, 14)))

    # ------------------------------------------------------------------ set pieces on the road
    F0 = Frame(A, s0 + 18)
    rainbow_arch(W, F0, 30)
    zone_gate(W, A, s0 + 6, "MEADOWS", scale=2, text_mat="n_yellow", board_mat="rainbow_b", frame_mat="marble", trim_mat="n_white",
              tower_u=46, board_half=46, tower_h=36)
    # boosts + item boxes at the start of every bend
    L = s1 - s0
    for frac in (0.12, 0.3, 0.46, 0.64, 0.8, 0.93):
        boost_pad(W, A, s0 + frac * L, 14, 4, "n_orange", count=3, spacing=4)
    for frac in (0.08, 0.26, 0.42, 0.6, 0.76, 0.9):
        item_boxes(W, A, s0 + frac * L, 7.0, n=4)

    # covered bridge over a stream
    sb = s0 + 0.5 * L
    Fb = Frame(A, sb)
    stream(W, A, sb, Fb)
    # track furniture: corner signs + hay bales, floodlight posts, billboards and flags
    corner_furniture(W, A, i0, i1, 7.0, rmax=110.0, hay=True, stacks=False, wall_w=1.0, skid=True)
    flood_posts(W, A, i0, i1, 72, 10.5, 6)
    billboards(W, A, i0 + 20, i1, 140, 12.0, start=0)


def stream(W, A, sb, Fb):
    """A cyan-clay stream across the land, and a covered bridge where the road crosses."""
    T = Fb.T
    for v in range(-7, 8):
        for u in range(-110, 111):
            x, z = Fb.cell(u, v)
            if abs(u) <= 12:
                continue
            W.put(x, -1, z, "pond")
    for v in (-9, 9):
        for u in range(-110, 111):
            if abs(u) <= 12:
                continue
            x, z = Fb.cell(u, v)
            W.put(x, -1, z, "pond_edge")
    # planked deck beside the asphalt over the crossing (the road itself stays Kart_Asphalt)
    for v in range(-8, 9):
        for u in range(-9, 10):
            if abs(u) < 7:
                continue
            x, z = Fb.cell(u, v)
            if not W.has(x, T, z):
                W.put(x, T, z, "planks")
    # covered frame
    for v in (-8, -4, 0, 4, 8):
        for u in (-9, 9):
            lbox(W, Fb, u, u, v, v, T + 1, T + 7, "wood_beam")
        lbox(W, Fb, -9, 9, v, v, T + 8, T + 8, "wood_beam")
    lbox(W, Fb, -10, 10, -8, 8, T + 9, T + 9, "shingle")
    lbox(W, Fb, -9, -9, -8, 8, T + 1, T + 1, "fence")
    lbox(W, Fb, 9, 9, -8, 8, T + 1, T + 1, "fence")
    for v in range(-8, 9, 4):
        x, z = Fb.cell(0, v)
        W.put(x, T + 7, z, "lantern")
