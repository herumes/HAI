"""Zone 7: Ancient Jungle - a road that climbs into the canopy on wooden piers between giant trees: tiki gates, a ruined temple
colonnade with a giant stone head, waterfalls into a plunge pool, hanging vines, tree houses, fireflies."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403

mat("jg_top", ["Soil_Grass_Lush_Full" if False else "Soil_Grass_Full", "Soil_Grass_Full", "Soil_Grass_Deep_Full", "Soil_Leaves_Full" if False else "Soil_Grass_Full"])
mat("jg_dirt", ["Soil_Dirt_Lush", "Soil_Dirt_Lush", "Soil_Dirt_Wet", "Soil_Mud"])
mat("jg_rock", ["Rock_Stone_Cobble_Mossy", "Rock_Stone_Cobble_Mossy", "Rock_Stone_Cobble", "Rock_Stone_Brick_Mossy"])
mat("jg_rock2", ["Rock_Stone_Brick_Mossy", "Rock_Stone_Brick", "Rock_Stone_Cobble_Mossy"])
mat("jg_brick", ["Rock_Stone_Brick_Mossy", "Rock_Stone_Brick_Mossy", "Rock_Stone_Brick", "Rock_Stone_Brick_Smooth"])
mat("jg_orn", "Rock_Stone_Brick_Ornate")
mat("jg_pillar", "Rock_Stone_Brick_Pillar_Middle")
mat("jg_pillar_b", "Rock_Stone_Brick_Pillar_Base")
mat("jg_deck", ["Wood_Hardwood_Planks", "Wood_Darkwood_Planks", "Wood_Hardwood_Planks"])
mat("jg_pier", "Wood_Darkwood_Beam")
mat("jg_trunk", ["Wood_Jungle_Trunk", "Wood_Jungle_Trunk", "Wood_Banyan_Trunk"])
mat("jg_root", "Wood_Banyan_Roots")
mat("jg_leaf", ["Plant_Leaves_Jungle", "Plant_Leaves_Jungle", "Plant_Leaves_Jungle", "Plant_Leaves_Banyan", "Plant_Leaves_Palm"])
mat("jg_leaf2", ["Plant_Leaves_Jungle", "Plant_Leaves_Bamboo", "Plant_Leaves_Jungle"])
mat("jg_vine", ["Plant_Vine_Hanging", "Plant_Vine_Green_Hanging", "Plant_Vine_Liana"])
mat("jg_fern", ["Plant_Fern_Jungle", "Plant_Bush_Jungle", "Plant_Grass_Jungle_Tall", "Plant_Fern_Giant", "Plant_Grass_Jungle", "Plant_Fern_Wet"])
mat("jg_flower", ["Plant_Flower_Orchid_Pink", "Plant_Flower_Orchid_Yellow", "Plant_Flower_Orchid_Purple", "Plant_Flower_Tall_Red",
                  "Plant_Flower_Orchid_Cyan", "Plant_Flower_Bushy_Orange"])
mat("jg_bamboo", ["Wood_Bamboo_Trunk", "Wood_Bamboo_Trunk_Full"])
mat("jg_fall", ["Build_Lightsource_Cyan", "Cloth_Block_Wool_Cyan_Light", "Cloth_Block_Wool_White"])
mat("jg_pool", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue", "Soil_Clay_Smooth_Cyan"])
mat("jg_gold", "Rock_Gold_Brick_Smooth")
mat("jg_eye", "NoCube_Neon_Block_Lime")
mat("jg_glow", "Build_Lightsource_Green")
mat("jg_glow2", "Build_Lightsource_YellowLight")
mat("jg_planks", "Wood_Softwood_Planks")
mat("jg_roof", "Wood_Softwood_Roof")
mat("jg_rope", "Wood_Hardwood_Fence")
mat("jg_totem_a", "Soil_Clay_Smooth_Red")
mat("jg_totem_b", "Soil_Clay_Smooth_Yellow")
mat("jg_totem_c", "Soil_Clay_Smooth_Blue")
mat("jg_totem_d", "Soil_Clay_Smooth_Green")


def giant_tree(W, rng, x, y, z, h, r, canopy=1.0):
    """A giant jungle tree: flared hollow trunk, buttress roots, two canopy shells, hanging vines."""
    for k in range(h):
        rr = r + (2.2 * max(0.0, 1.0 - k / 10.0)) + 0.8 * math.sin(k * 0.2)
        W.cyl(x, z, y + k, y + k, rr, "jg_trunk", mode="wall" if rr > 2.3 else "fill")
    for a in range(6):
        ang = a * math.pi / 3 + rng.uniform(-0.3, 0.3)
        for t in range(0, 11):
            px = x + (r + 1 + t) * math.cos(ang)
            pz = z + (r + 1 + t) * math.sin(ang)
            for yy in range(y, y + max(1, 6 - t // 2)):
                W.put(int(round(px)), yy, int(round(pz)), "jg_root")
    top = y + h
    cr = (10 + h * 0.12) * canopy
    W.ellipsoid(x, top, z, cr, cr * 0.42, cr, "jg_leaf", mode="shell")
    W.ellipsoid(x + int(rng.integers(-5, 6)), top + 3, z + int(rng.integers(-5, 6)), cr * 0.7, cr * 0.34, cr * 0.7, "jg_leaf2", mode="shell")
    for _ in range(int(cr * 1.1)):
        a = rng.uniform(0, 2 * math.pi)
        rr = rng.uniform(0.2, 0.95) * cr
        vx, vz = int(x + rr * math.cos(a)), int(z + rr * math.sin(a))
        vy = int(top - cr * 0.42 * math.sqrt(max(0.0, 1 - (rr / cr) ** 2)))
        for k in range(1, int(rng.integers(3, 12))):
            W.put(vx, vy - k, vz, "jg_vine")


def tiki(W, x, y, z, h, face, cols):
    """A totem pole of stacked carved faces; face = unit (dx, dz) the faces look toward."""
    fx, fz = face
    px, pz = -fz, fx
    for k in range(h):
        col = cols[(k // 3) % len(cols)]
        for a in (-1, 0, 1):
            for b in (-1, 0, 1):
                W.put(x + a * px + b * fx, y + k, z + a * pz + b * fz, col)
    for k in range(1, h - 1, 3):
        for s_ in (-1, 1):
            W.put(x + s_ * px + 2 * fx, y + k + 1, z + s_ * pz + 2 * fz, "jg_eye")
        W.put(x + 2 * fx, y + k, z + 2 * fz, "b_black")
        for s_ in (-1, 0, 1):
            W.put(x + s_ * px + 2 * fx, y + k - 1, z + s_ * pz + 2 * fz, "jg_gold" if k % 2 else "jg_orn")
    for s_ in (-2, 2):
        for dy in range(0, 3):
            W.put(x + s_ * px, y + h + dy, z + s_ * pz, "jg_totem_a")
    W.ellipsoid(x, y + h + 1, z, 2, 1.4, 2, "jg_gold")


def stone_head(W, cx, cy, cz, fwd, s=1.0):
    """A colossal mossy stone head (Olmec style) facing `fwd`, neon eyes."""
    F = {"n": (0, -1), "s": (0, 1), "e": (1, 0), "w": (-1, 0)}[fwd]
    px, pz = -F[1], F[0]

    def P(a, b, y):
        return int(round(cx + F[0] * a + px * b)), int(round(cy + y)), int(round(cz + F[1] * a + pz * b))

    def ell(a, b, y, ra, rb, ry, m, mode="fill", part="all"):
        x, yy, z = P(a, b, y)
        if F[0] != 0:
            W.ellipsoid(x, yy, z, ra, ry, rb, m, mode=mode, part=part)
        else:
            W.ellipsoid(x, yy, z, rb, ry, ra, m, mode=mode, part=part)
    ell(0, 0, 11 * s, 9 * s, 10 * s, 11 * s, "jg_brick", mode="shell")
    ell(8 * s, 0, 7 * s, 4 * s, 7 * s, 5 * s, "jg_brick", mode="shell")                        # broad nose / cheeks
    for b in (-1, 1):
        x, yy, z = P(9 * s, b * 5 * s, 13 * s)
        W.ellipsoid(x, yy, z, 2.4 * s, 2.0 * s, 2.4 * s, "jg_eye")
        for dy in range(-1, 2):                                                           # brow ridge
            for t in range(-3, 4):
                xx, y2, zz = P(8 * s, b * 5 * s + t, 15 * s + dy)
                W.put(xx, y2, zz, "jg_orn")
    for t in range(-6, 7):                                                                  # mouth and teeth
        xx, y2, zz = P(10 * s, t * s, 4 * s)
        W.put(xx, y2, zz, "b_black")
        if t % 2 == 0:
            xx, y2, zz = P(10 * s, t * s, 5 * s)
            W.put(xx, y2, zz, "jg_totem_d" if False else "b_white")
    x, yy, z = P(0, 0, 22 * s)
    W.ellipsoid(x, yy, z, 6 * s, 3 * s, 6 * s, "jg_leaf", mode="shell", part="upper")        # moss hair
    for k in range(int(10 * s)):
        xx, y2, zz = P(-2 * s, (k - 5 * s), 2 * s)
        W.put(xx, y2, zz, "jg_vine")


def treehouse(W, rng, x, y, z, w=9):
    W.box((x, y, z), (x + w, y, z + w), "jg_planks")
    for dx, dz in ((0, 0), (w, 0), (0, w), (w, w)):
        for k in range(1, 5):
            W.put(x + dx, y + k, z + dz, "jg_pier")
    W.box((x + 1, y + 1, z + 1), (x + w - 1, y + 4, z + w - 1), "jg_planks", mode="walls")
    for dx in (3, 6):
        W.put(x + dx, y + 3, z + w - 1, "jg_glow2")
    W.erase(x + w // 2, y + 1, z + 1)
    W.erase(x + w // 2, y + 2, z + 1)
    for i in range(w // 2 + 2):
        for xx in range(x - 1, x + w + 2):
            W.put(xx, y + 5 + i, z + i - 1, "jg_roof", rot=stair_rot("s"))
            W.put(xx, y + 5 + i, z + w + 1 - i, "jg_roof", rot=stair_rot("n"))
    for k in range(1, 14):
        W.put(x + 1, y - k, z - 1, "jg_rope")


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s0, s1 = m["jungle"], m["keep"]
    L = s1 - s0
    i0, i1 = B.i(s0), B.i(s1)
    tick = W.tick

    # ------------------------------------------------------------------ road: lime / yellow neon edge, wooden piers + planks deck when it is high
    edge = lambda s, u: "n_lime" if int(s // 3) % 2 == 0 else "n_yellow"
    piers = dict(mat="jg_pier", every=10, size=2, inset=1.5, cap="jg_deck", bottom=0, beam="jg_pier")
    st_low = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail="armco", lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=("k_curb_red",), curb_len=4.0, curb_rmax=110.0)
    st_high = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail_fn=lambda s, side: "mesh" if B.Tat(s) >= 8 else "armco", lanes=(0.0,), lane_dash=(4.0, 4.0),
                        deck="jg_deck", piers=piers, curbs=())
    s_hi = s0 + 150.0
    stamp(ctx, B, s0, s_hi, st_low)
    stamp(ctx, B, s_hi - 2, s1, st_high)
    tick("road")

    # ------------------------------------------------------------------ terrain: jungle hills, the ground stays under the raised road
    land = Land(ctx, B, i0, i1, margin=80, radius=62, hw=9.0)
    g, d, Tn, Sn = land.g, land.d, land.Tn, land.Sn
    amp = smoothstep(s0, s0 + 30, Sn) * (1 - smoothstep(s1 - 40, s1, Sn))
    n1 = fbm(g.X, g.Z, 70.0, 4, 111)
    n2 = fbm(g.X + 90, g.Z - 40, 26.0, 3, 113)
    Hn = -1.0 + amp * (16.0 * smoothstep(0.38, 0.8, n1) + 5.0 * n2)
    low = Tn <= 8
    # near the road: low sections follow the road, high sections stay at least 7 below it
    shoulder = np.where(low, np.where(Tn > 0, Tn - 1, -1), np.minimum(Hn, Tn - 8))
    w = 1.0 - smoothstep(4.0, 18.0, d)
    Hc = Hn * (1 - w) + shoulder * w
    taper = 1.0 - smoothstep(40.0, 58.0, d)
    Hc = Hc * taper + (-1.0) * (1 - taper)
    Ht = np.maximum(np.rint(Hc).astype(np.int64), -1)
    mask = (d < 60) & ~(land.rb & (d > 14))
    slope = slope_of(Hc)
    land.Hc, land.Ht, land.mask, land.slope = Hc, Ht, mask, slope
    # plunge pool + waterfall cliff
    mats = ["jg_top", "jg_dirt", "jg_rock", "jg_rock2", "jg_pool"]
    pat = fbm(g.X, g.Z, 18.0, 2, 117)
    top_ids = pick([(slope > 1.0, 2), (pat > 0.66, 1)], 0)

    def side_fn(y, x, z):
        return np.where(y < 3, 1, np.where((y // 4) % 2 == 0, 2, 3))

    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask)
    tick("terrain")

    def ground(x, z):
        ix, iz = x - g.x0, z - g.z0
        if 0 <= ix < g.nx and 0 <= iz < g.nz and mask[ix, iz]:
            return int(Ht[ix, iz]) + 1
        return 0

    placed = []

    def spot(s, u, r, ymax=None):
        F = Frame(B, s)
        x, z = F.cell(u, 0)
        if blocked_by_road(ctx, x, z) and abs(u) < 15:
            return None
        if any((x - a) ** 2 + (z - b) ** 2 < (r + rr) ** 2 for (a, b, rr) in placed):
            return None
        placed.append((x, z, r))
        return x, ground(x, z), z

    # ------------------------------------------------------------------ gate at the start: two stone pillars and a vine-draped lintel
    s_g = axis_s(B, s0 + 210, 120) or (s0 + 210)
    F0 = Frame(B, s_g)
    T0 = F0.T
    for side in (-1, 1):
        x, z = F0.cell(side * 25, 0)
        for k in range(34):
            rr = 4.2 - 1.2 * k / 34.0
            W.box((int(x - rr), T0 + k, int(z - rr)), (int(x + rr), T0 + k, int(z + rr)), "jg_brick" if k % 5 else "jg_orn", mode="walls")
        for k in range(2, 30, 5):
            xe, ze = F0.cell(side * 25, -5)
            W.put(xe, T0 + k, ze, "jg_eye")
    lbox(W, F0, -25, 25, -3, 3, T0 + 25, T0 + 33, "jg_brick")
    lsign(W, F0, "JUNGLE", 0, T0 + 31, -4, 1, "n_lime", backing="b_black", pad=1)
    for u in list(range(-24, -9, 2)) + list(range(10, 25, 2)):
        x, z = F0.cell(u, 1)
        for k in range(int(rng.integers(3, 9))):
            W.put(x, T0 + 24 - k, z, "jg_vine")
    gxc, gzc = F0.cell(0, 0)
    placed.append((gxc, gzc, 40))                      # no tree may grow into the arch or in front of its lettering
    tick("gate")

    # ------------------------------------------------------------------ giant trees beside the road, small trees and the undergrowth
    for k in range(34):
        s = s0 + rng.uniform(40, L - 30)
        side = 1 if rng.random() < 0.5 else -1
        u = side * rng.uniform(15, 44)
        q = spot(s, u, 11)
        if q:
            giant_tree(W, rng, q[0], q[1], q[2], int(rng.integers(34, 62)), float(rng.integers(3, 6)), canopy=rng.uniform(0.85, 1.15))
    for k in range(90):
        q = spot(s0 + rng.uniform(20, L - 20), (1 if rng.random() < 0.5 else -1) * rng.uniform(12, 55), 3)
        if q:
            oak(W, rng, q[0], q[1], q[2], h=int(rng.integers(4, 8)), leaf="jg_leaf", trunk="jg_trunk")
    for k in range(40):                                                                           # bamboo clumps
        q = spot(s0 + rng.uniform(20, L - 20), (1 if rng.random() < 0.5 else -1) * rng.uniform(13, 50), 4)
        if q:
            for _ in range(int(rng.integers(4, 9))):
                bx, bz = q[0] + int(rng.integers(-3, 4)), q[2] + int(rng.integers(-3, 4))
                for yy in range(q[1], q[1] + int(rng.integers(8, 19))):
                    W.put(bx, yy, bz, "jg_bamboo")
    ix, iz = np.nonzero((d >= 4.0) & (d <= 56) & mask & (Ht >= -1) & ~land.rb & (slope < 1.1))
    pick_f = rng.random(ix.size) < 0.16
    W.put_arr(g.x0 + ix[pick_f], Ht[ix[pick_f], iz[pick_f]] + 1, g.z0 + iz[pick_f], "jg_fern")
    pick_g = rng.random(ix.size) < 0.012
    W.put_arr(g.x0 + ix[pick_g], Ht[ix[pick_g], iz[pick_g]] + 1, g.z0 + iz[pick_g], "jg_flower")
    tick("trees")

    # ------------------------------------------------------------------ tree houses, totems, vines under the deck
    for frac in (0.35, 0.62, 0.86):
        s = s0 + frac * L
        F = Frame(B, s)
        q = spot(s, 24 * (1 if frac < 0.5 else -1), 9)
        if q:
            ty = max(q[1], F.T - 3)
            for k in range(q[1], ty):
                W.put(q[0] + 4, k, q[2] + 4, "jg_trunk")
                W.put(q[0] + 5, k, q[2] + 4, "jg_trunk")
            treehouse(W, rng, q[0], ty, q[2])
    for k, s in enumerate(np.arange(s0 + 60, s1 - 40, 26.0)):
        side = 1 if k % 2 == 0 else -1
        F = Frame(B, s)
        x, z = F.cell(side * 11.2, 0)
        if blocked_by_road(ctx, x, z) and False:
            continue
        gy = F.T
        tiki(W, x, gy + 1, z, int(rng.integers(5, 8)), (-side * (1 if abs(F.t[1]) > 0.7 else 0), -side * (1 if abs(F.t[0]) > 0.7 else 0)) if False else (0, 1), ["jg_totem_a", "jg_totem_b", "jg_totem_c", "jg_totem_d"])
    for i in range(i0 + 20, i1 - 20, 7):
        T = int(B.T[i])
        if T < 8:
            continue
        for side in (-1, 1):
            x, z = side_cell(B, i, side * 7.6)
            for k in range(int(rng.integers(2, 9))):
                W.put(x, T - 2 - k, z, "jg_vine")
    tick("details")

    # ------------------------------------------------------------------ the ruined temple on the canopy road: arches, columns, the stone head
    s_t = s0 + 0.58 * L
    for k, s in enumerate((s_t - 60, s_t, s_t + 60)):
        F = Frame(B, s)
        T = F.T
        for side in (-1, 1):
            for y in range(max(-1, T - 1), T + 14):
                x, z = F.cell(side * 11.4, 0)
                W.put(x, y, z, "jg_pillar" if y % 7 else "jg_orn")
        for u in range(-11, 12):
            for y in (T + 14, T + 15):
                x, z = F.cell(u, 0)
                if rng.random() < 0.9:
                    W.put(x, y, z, "jg_brick")
        for u in range(-11, 12, 3):
            x, z = F.cell(u, 0)
            for kk in range(int(rng.integers(3, 9))):
                W.put(x, T + 13 - kk, z, "jg_vine")
    for s in np.arange(s_t - 52, s_t + 54, 9.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 13.6, 0)
            h = int(rng.integers(4, 11))
            for y in range(max(-1, F.T - 6), F.T + h):
                W.put(x, y, z, "jg_pillar")
            W.put(x, F.T + h, z, "jg_orn")
    F = Frame(B, s_t)
    face = {"e": "w", "w": "e", "n": "s", "s": "n"}[heading(F.t[0], F.t[1])]
    q = spot(s_t + 8, 32, 14)
    if q:
        stone_head(W, q[0], max(q[1], F.T - 6), q[2], "w" if abs(F.t[0]) < 0.5 else "n", s=1.5)
    tick("temple")

    # ------------------------------------------------------------------ waterfall + plunge pool on the first hillside
    sw = s0 + 0.2 * L
    Fw = Frame(B, sw)
    wx, wz = Fw.cell(-26, 0)
    base = ground(wx, wz)
    for y in range(base, base + 22):
        for dv in (-1, 0, 1):
            x, z = Fw.cell(-26, dv)
            W.put(x, y, z, "jg_fall")
    for dv in range(-8, 9):
        for du in range(-30, -14):
            x, z = Fw.cell(du, dv)
            if math.hypot(du + 22, dv) <= 7.5:
                W.put(x, base - 1, z, "jg_pool")
    tick("waterfall")

    # ------------------------------------------------------------------ fireflies, road dressing
    for k in range(260):
        s = s0 + rng.uniform(0, L)
        F = Frame(B, s)
        x, z = F.cell(rng.uniform(-40, 40), 0)
        y = int(F.T + rng.uniform(4, 30))
        if not W.has(x, y, z):
            W.put(x, y, z, "jg_glow2" if k % 3 else "jg_glow")
    for frac in (0.07, 0.17, 0.28, 0.4, 0.5, 0.62, 0.74, 0.86, 0.94):
        boost_pad(W, B, s0 + frac * L, 14, 4, "n_lime" if int(frac * 10) % 2 else "n_yellow", count=3, spacing=4)
    for frac in (0.1, 0.22, 0.34, 0.46, 0.58, 0.7, 0.82, 0.92):
        item_boxes(W, B, s0 + frac * L, 7.0, n=4)
    corner_furniture(W, B, i0, i1, 7.0, rmax=100.0, hay=False, stacks=True, wall_w=0.0, skid=True, need_support=False)
    flood_posts(W, B, i0, i1, 70, 10.5, 6, glow="kp_glow_warm", need_support=False)
    billboards(W, B, i0 + 30, i1 - 30, 160, 12.0, start=1, need_support=False)
    tick("dressing")
