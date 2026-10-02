"""Zone 2: Sunbaked Canyon - terraced red mesas, rock arches, pyramids, a rib-cage run and a long bridge."""
import math

import numpy as np

from core import fbm, mat, smoothstep, stair_rot
from kart import boost_pad
from kartprops import billboards, cone_gate, corner_furniture, flood_posts, speed_bumps
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, hoop, item_boxes, lbox, lsign, road_grid, zone_gate
from terrain import Grid, carve, centre_field, pick, skin, slope_of

mat("cn_road", ["Rock_Sandstone_Red_Brick_Smooth", "Rock_Sandstone_Red_Brick_Smooth", "Rock_Sandstone_Red_Brick_Smooth", "Rock_Sandstone_Brick_Smooth"])
mat("cn_stair", "Rock_Sandstone_Red_Brick_Stairs")
mat("cn_line", "Soil_Clay_Smooth_Yellow")
mat("cn_wall", "Rock_Sandstone_Red_Brick")
mat("cn_wall2", "Rock_Sandstone_Brick")
mat("cn_deck", "Rock_Sandstone_Red_Brick")
mat("cn_pier", "Rock_Sandstone_Red_Brick_Pillar_Middle")
mat("cn_cap", "Rock_Sandstone_Red_Brick_Ornate")
mat("cn_top", ["Soil_Sand_Red", "Soil_Sand_Red", "Soil_Sand", "Soil_Sand_Red"])
mat("cn_a", "Rock_Sandstone_Red")
mat("cn_b", "Soil_Clay_Orange")
mat("cn_c", "Rock_Sandstone")
mat("cn_d", "Soil_Clay_Red")
mat("cn_rock_top", ["Rock_Sandstone_Red", "Rock_Sandstone", "Soil_Clay_Orange"])
mat("pyr", ["Rock_Sandstone_Brick", "Rock_Sandstone_Brick", "Rock_Sandstone_Brick_Smooth"])
mat("pyr_orn", "Rock_Sandstone_Brick_Ornate")
mat("gold_cap", "Rock_Gold_Brick_Smooth")
mat("cactus", ["Soil_Clay_Smooth_GreenDark", "Soil_Clay_Smooth_Green"])
mat("bone", ["Rock_Chalk", "Rock_Chalk", "Rock_Calcite"])
mat("palm_l", "Plant_Leaves_Palm")
mat("palm_t", "Wood_Palm_Trunk")
mat("oasis", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("oasis_edge", ["Soil_Grass_Sunny_Full", "Soil_Sand_White"])
mat("sun", "Build_Lightsource_YellowLight")
mat("sun_ray", "Build_Lightsource_Orange")
mat("sand_frame", "Rock_Sandstone_Red_Brick_Ornate")
mat("sand_board", "Rock_Sandstone_Red_Brick")
mat("ruin", ["Rock_Sandstone_Brick", "Rock_Sandstone_Brick", "Rock_Sandstone_Brick_Smooth", "Rock_Sandstone_Brick_Half"])
mat("obelisk", "Rock_Sandstone_White_Brick_Smooth")
mat("hiero", "Build_Lightsource_Orange")


def rock_arch(W, rng, F, half_w, h, thick, m, base_down=0, noise=1.2):
    """A natural-looking arch over the road: lumpy half-ellipse ring plus legs down to base_down."""
    T = F.T
    n = int(math.pi * (half_w + h) * 1.4)
    for k in range(n + 1):
        a = math.pi * k / n
        u = half_w * math.cos(a)
        y = T + h * math.sin(a)
        r = thick / 2.0 + rng.uniform(-noise, noise) * 0.5 + 0.4
        for du in np.arange(-r, r + 0.01, 0.75):
            for dv in np.arange(-r, r + 0.01, 0.75):
                for dy in np.arange(-r, r + 0.01, 0.75):
                    if du * du + dv * dv + dy * dy <= r * r:
                        x, z = F.cell(int(round(u + du)), int(round(dv)))
                        W.put(x, int(round(y + dy)), z, m)
    for side in (-1, 1):
        for y in range(base_down, int(T + 2)):
            for du in range(-int(thick // 2), int(thick // 2) + 1):
                for dv in range(-int(thick // 2), int(thick // 2) + 1):
                    x, z = F.cell(int(side * half_w + du), dv)
                    W.put(x, y, z, m)


def rib_arch(W, F, half_w, h, m, thick=1.2):
    T = F.T
    n = int(math.pi * half_w * 2)
    for k in range(n + 1):
        a = math.pi * k / n
        u = (half_w + 1.5 * math.sin(a * 2)) * math.cos(a)
        y = T + 1 + h * math.sin(a) ** 0.8
        for du in (0, 1):
            x, z = F.cell(int(round(u + du * math.cos(a))), 0)
            W.put(x, int(round(y + du * math.sin(a))), z, m)


def pyramid(W, cx, y0, cz, base, m1, m2, cap):
    half = base // 2
    for i in range(half + 1):
        r = half - i
        if r < 0:
            break
        y = y0 + i
        mm = m2 if i % 6 == 5 else m1
        W.box((cx - r, y, cz - r), (cx + r, y, cz + r), mm, mode="walls" if r > 0 else "fill")
    W.put(cx, y0 + half + 1, cz, cap)
    W.put(cx, y0 + half + 2, cz, cap)
    # entrance on the south face
    for y in range(y0, y0 + 4):
        for x in range(cx - 1, cx + 2):
            W.erase(x, y, cz + half)
    W.box((cx - 2, y0 + 4, cz + half), (cx + 2, y0 + 4, cz + half), m2)
    W.put(cx, y0 + 5, cz + half, "lit_orange")


def cactus(W, rng, x, y, z):
    h = int(rng.integers(4, 10))
    for k in range(h):
        W.put(x, y + k, z, "cactus")
    for side in (-1, 1):
        if rng.random() < 0.7:
            ay = y + int(rng.integers(2, max(3, h - 1)))
            dx, dz = (side, 0) if rng.random() < 0.5 else (0, side)
            ln = int(rng.integers(2, 4))
            for k in range(1, ln + 1):
                W.put(x + dx * k, ay, z + dz * k, "cactus")
            for k in range(int(rng.integers(2, 4))):
                W.put(x + dx * ln, ay + 1 + k, z + dz * ln, "cactus")
    W.put(x, y + h, z, "Plant_Flower_Tall_Yellow" if False else "cactus")


def palm(W, rng, x, y, z, h=None):
    h = h or int(rng.integers(8, 14))
    lean = rng.uniform(-0.25, 0.25), rng.uniform(-0.25, 0.25)
    px, pz = x, z
    for k in range(h):
        px = x + lean[0] * k
        pz = z + lean[1] * k
        W.put(int(round(px)), y + k, int(round(pz)), "palm_t")
    top = (int(round(px)), y + h, int(round(pz)))
    for k in range(7):
        a = k * 2 * math.pi / 7 + rng.uniform(-0.2, 0.2)
        for t in range(1, 7):
            drop = int(t * t / 9)
            W.put(int(round(top[0] + t * math.cos(a))), top[1] - drop, int(round(top[2] + t * math.sin(a))), "palm_l")
    W.put(top[0], top[1], top[2], "palm_l")


def sun(W, cx, cy, cz, r, rays=14):
    W.ellipsoid(cx, cy, cz, r, r, r, "sun", mode="shell")
    for k in range(rays):
        a = 2 * math.pi * k / rays
        for t in range(r + 3, r + 14):
            W.put(int(cx + t * math.cos(a)), cy + int(t * math.sin(a)), cz, "sun_ray")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["canyon"], m["crystal"]
    L = s1 - s0
    i0, i1 = A.i(s0), A.i(s1)

    # ------------------------------------------------------------------ road + bridge
    b_lo, b_hi = s0 + 0.3 * L - 10, s0 + 0.7 * L + 10          # the long bridge
    st = KartStyle(hw=7.0, verge="k_conc", rail_fn=lambda s, side: "mesh" if b_lo < s < b_hi else "armco",
                   wall=("cn_wall", "cn_wall2"), wall_period=5.0, wall_h=2, deck="cn_deck",
                   piers=dict(mat="cn_pier", every=12, size=2, cap="cn_cap", inset=2.0),
                   curbs=("k_curb_yel",), curb_len=4.0, curb_rmax=150.0, chevron_rmax=75.0, lane_mat="k_yline")
    stamp_kart_road(W, A, i0, i1, st)

    # ------------------------------------------------------------------ terrain: terraced red mesas
    xs, zs = A.p[i0:i1, 0], A.p[i0:i1, 1]
    g = Grid(xs.min() - 70, xs.max() + 70, zs.min() - 70, zs.max() + 70)
    d, Tn, Sn = centre_field(g, A, i0, i1, 58, hw=9.0)
    amp = smoothstep(s0, s0 + 30, Sn) * (1 - smoothstep(s1 - 30, s1, Sn))
    n1 = fbm(g.X, g.Z, 70.0, 4, 21)
    edge = 1.0 - smoothstep(30, 54, d)
    raw = (4 + 46 * smoothstep(0.38, 0.72, n1)) * (0.25 + 0.75 * edge)
    mesa = 7 * np.floor(raw / 7.0)
    far = smoothstep(14, 34, d)           # low near the road, tall further out
    H = mesa * far * amp - 1
    dunes = 3.5 * smoothstep(0.35, 0.75, fbm(g.X, g.Z, 20.0, 3, 9)) * (1 - far)
    H = np.maximum(H, dunes - 1)
    crag = 7.0 * (fbm(g.X, g.Z, 14.0, 3, 31) - 0.5)
    shoulder = np.where(Tn > 0, Tn - 1, -1)
    # the chasm under the bridge: terrain stays at ground level there
    Hc = carve(H, d, shoulder, 3.0, 11.0, noise=crag)
    bridge = (Sn > s0 + 0.3 * L - 10) & (Sn < s0 + 0.7 * L + 10) & (d < 20)
    Hc = np.where(bridge & (d > 7), np.minimum(Hc, 2), Hc)
    Ht = np.rint(Hc).astype(np.int64)
    Ht[d > 54] = -1
    Ht = np.maximum(Ht, -1)
    mask = d < 54
    rb = road_grid(ctx, g)
    mask = mask & ~(rb & (d > 12))
    slope = slope_of(Hc)
    # pyramids: flatten a site on a tall mesa, build on it later (after the skin)
    sites = []
    ixs, izs = np.nonzero((Ht >= 14) & mask & (d > 24) & (d < 46))
    tries = 0
    while len(sites) < 4 and tries < 4000 and ixs.size:
        tries += 1
        k = int(rng.integers(0, ixs.size))
        cx, cz = g.x0 + int(ixs[k]), g.z0 + int(izs[k])
        base = [47, 35, 29, 23][len(sites)]
        r = base // 2 + 3
        sx0, sx1 = cx - r - g.x0, cx + r - g.x0
        sz0, sz1 = cz - r - g.z0, cz + r - g.z0
        if sx0 < 0 or sz0 < 0 or sx1 >= g.nx or sz1 >= g.nz:
            continue
        if mask[sx0:sx1 + 1, sz0:sz1 + 1].mean() < 0.8:
            continue
        if any(abs(cx - px) < 75 and abs(cz - pz) < 75 for px, pz, _, _ in sites):
            continue
        ylev = int(np.median(Ht[sx0:sx1 + 1, sz0:sz1 + 1]))
        Ht[sx0:sx1 + 1, sz0:sz1 + 1] = ylev
        mask[sx0:sx1 + 1, sz0:sz1 + 1] = True
        sites.append((cx, cz, base, ylev))
    mats = ["cn_top", "cn_a", "cn_b", "cn_c", "cn_d", "cn_rock_top"]
    top_ids = pick([(Ht > 12, 5), (slope > 1.5, 1)], 0)

    def strata(y, x, z):
        band = (y + (3 * np.sin(x * 0.045 + z * 0.03)).astype(np.int64)) // 3
        return 1 + (band % 4)

    skin(W, g, Ht, top_ids, strata, mats, mask=mask)

    # ------------------------------------------------------------------ props on the ground
    def candidates(n, dmin, dmax, need_flat=True, hmin=-1, hmax=4):
        ok = (d >= dmin) & (d <= dmax) & (Ht >= hmin) & (Ht <= hmax) & mask & ~rb
        if need_flat:
            ok &= slope < 0.6
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), int(Ht[ix[k], iz[k]]), g.z0 + int(iz[k])) for k in sel]

    for (x, y, z) in candidates(150, 11, 40):
        cactus(W, rng, x, y + 1, z)
    # small rubble / boulders
    for (x, y, z) in candidates(60, 11, 45):
        W.ellipsoid(x, y + 1, z, int(rng.integers(1, 3)), 1, int(rng.integers(1, 3)), "cn_rock_top")
    for (cx, cz, base, ylev) in sites:
        pyramid(W, cx, ylev + 1, cz, base, "pyr", "pyr_orn", "gold_cap")
    print("  canyon pyramids placed:", len(sites))
    # hoodoos: pillar rocks with a fat cap
    for (x, y, z) in candidates(45, 13, 40, need_flat=False, hmin=-1, hmax=6):
        hh = int(rng.integers(10, 26))
        rr = rng.uniform(1.6, 3.0)
        for k in range(hh):
            W.cyl(x, z, y + 1 + k, y + 1 + k, rr + 0.6 * math.sin(k * 0.7) + (1.6 if k > hh - 4 else 0), ["cn_a", "cn_b", "cn_c", "cn_d"][(k // 3) % 4])

    # oasis
    for (x, y, z) in candidates(1, 16, 30, hmin=-1, hmax=0):
        for dx in range(-9, 10):
            for dz in range(-7, 8):
                if (dx / 9.5) ** 2 + (dz / 7.5) ** 2 <= 1:
                    W.put(x + dx, -1, z + dz, "oasis")
                elif (dx / 12.0) ** 2 + (dz / 10.0) ** 2 <= 1:
                    W.put(x + dx, -1, z + dz, "oasis_edge")
        for k in range(7):
            a = k * 0.9
            palm(W, rng, int(x + 11 * math.cos(a)), 0, int(z + 9 * math.sin(a)))

    # ------------------------------------------------------------------ set pieces on the road
    zone_gate(W, A, s0 - 25, "CANYON", scale=2, text_mat="n_orange", board_mat="sand_board", frame_mat="sand_frame", trim_mat="n_yellow",
              tower_u=42, board_half=42, tower_h=34)
    # rock arches
    for frac in (0.36, 0.84):
        Fa = Frame(A, s0 + frac * L)
        rock_arch(W, rng, Fa, 13, 15, 5, "cn_a", base_down=int(Fa.T) - 1 if Fa.T > 2 else 0)
    # rib-cage run: bone arches
    sr = s0 + 0.58 * L
    for k in range(9):
        Fr = Frame(A, sr + k * 5)
        rib_arch(W, Fr, 11, 13 - abs(k - 4) * 0.7, "bone")
    F = Frame(A, sr + 20)
    for k in range(45):
        x, z = Frame(A, sr + k).cell(0, 0)
        W.put(x, Frame(A, sr + k).T + 15 - int(abs(k - 20) * 0.1), z, "bone")
    # obelisks at both bridge ends
    for frac in (0.26, 0.74):
        Fo = Frame(A, s0 + frac * L)
        for side in (-1, 1):
            cx, cz = Fo.cell(side * 13, 0)
            for y in range(0, Fo.T + 26):
                r = 1 if y < Fo.T + 20 else 0
                W.box((cx - r, y, cz - r), (cx + r, y, cz + r), "obelisk")
            W.put(cx, Fo.T + 26, cz, "gold_cap")
            for y in range(Fo.T + 4, Fo.T + 20, 3):
                W.put(cx + 2, y, cz, "hiero")
    for frac in (0.1, 0.24, 0.44, 0.62, 0.8, 0.93):
        boost_pad(W, A, s0 + frac * L, 14, 4, "n_orange", count=3, spacing=4)
    for frac in (0.06, 0.2, 0.4, 0.56, 0.76, 0.9):
        item_boxes(W, A, s0 + frac * L, 7.0, n=4)
    corner_furniture(W, A, i0, i1, 7.0, rmax=100.0, stacks=True, skid=True)
    flood_posts(W, A, i0, i1, 60, 10.5, 7, glow="kp_glow_orange", pole="cn_wall", skip=lambda i: b_lo < A.s[i] < b_hi)
    billboards(W, A, i0 + 30, i1, 150, 12.0, skip=lambda i: b_lo < A.s[i] < b_hi)
    for sb in (b_lo + 14, b_hi - 14):                            # rumble bumps + cones warning of the bridge ends
        speed_bumps(W, A, sb, 7.0, rows=2)
        cone_gate(W, A, sb - 8, 7.0)
    # twin suns + heat haze
    sun(W, 880, 120, 300, 14)
    sun(W, 930, 150, 140, 7)
