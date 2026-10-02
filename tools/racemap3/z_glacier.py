"""Zone 9: Aurora Glacier - the long climb to the summit launch station: frozen-lake drift basin, pine forest, igloo village, a
ski lift, frozen waterfalls, an ice tunnel with aurora lights, an ice-spire castle, aurora curtains, then the launch pad
(kart -> plane transform) on the roof of the world."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_frost import aurora, igloo, snowflake  # noqa: F401  (also registers the fr_* materials)

mat("gl_road", ["Build_GreyLight_Cube", "Build_GreyLight_Cube", "Build_White_Cube", "Build_GreyLight_Cube"])
mat("gl_ice", ["Rock_Ice", "Rock_Ice", "Rock_Ice_Permafrost"])
mat("gl_ice_b", ["Rock_Ice", "Soil_Snow_Brick", "Rock_Ice"])
mat("gl_icicle", ["Rock_Ice_Icicles", "Rock_Ice_Stalactite_Large"])
mat("gl_snow", ["Soil_Snow", "Soil_Snow", "Soil_Snow_Brick"])
mat("gl_cry", ["Rock_Crystal_White_Block", "Rock_Crystal_Cyan_Block", "Rock_Crystal_Blue_Block"])
mat("gl_cry_l", ["Rock_Crystal_White_Large", "Rock_Crystal_Cyan_Large", "Rock_Crystal_Blue_Large", "Rock_Crystal_Iridescent_Large"])
mat("gl_fall", ["Rock_Ice", "Cloth_Block_Wool_Cyan_Light", "Rock_Ice_Permafrost", "Build_Lightsource_Cyan"])
mat("gl_snowman", "WinterHoliday_Snowman")
mat("gl_cable", "Metal_Iron_Fence")
mat("gl_cab", "Soil_Clay_Smooth_Red")
mat("gl_cab2", "Soil_Clay_Smooth_Blue")
mat("gl_dish", "Metal_Iron_Smooth")
mat("gl_station", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("gl_station_r", "Soil_Clay_Smooth_Red")
mat("gl_glass", "Build_Lightsource_Cyan")
mat("gl_pylon", "Wood_Darkwood_Beam")
mat("gl_lake", ["Rock_Ice", "Rock_Ice", "Rock_Ice_Permafrost"])


def ice_spire(W, rng, x, y, z, h, r):
    for k in range(h):
        rr = r * (1.0 - k / float(h)) + 0.4
        W.cyl(x, z, y + k, y + k, rr, "gl_ice" if k % 4 else "gl_cry", mode="wall" if rr > 1.9 else "fill")
    W.put(x, y + h, z, "gl_cry_l")


def ski_lift(W, ctx, P, s_a, s_b, off, rng, ground):
    """Pylons along a slope beside the road with a cable and a few gondola cabins."""
    pts = []
    for s in np.arange(s_a, s_b, 34.0):
        F = Frame(P, s)
        x, z = F.cell(off, 0)
        gy = ground(x, z)
        top = max(gy, F.T) + 14
        for y in range(gy, top):
            W.put(x, y, z, "gl_pylon")
        W.box((x - 2, top, z), (x + 2, top, z), "gl_pylon")
        pts.append((x, top, z))
    for (a, b) in zip(pts[:-1], pts[1:]):
        n = int(max(abs(b[0] - a[0]), abs(b[2] - a[2])))
        for k in range(n + 1):
            t = k / float(max(1, n))
            x = int(round(a[0] + (b[0] - a[0]) * t))
            z = int(round(a[2] + (b[2] - a[2]) * t))
            y = int(round(a[1] + (b[1] - a[1]) * t - 2.0 * math.sin(math.pi * t)))
            W.put(x, y + 2, z, "gl_cable")
    for k, (a, b) in enumerate(zip(pts[:-1], pts[1:])):
        if k % 2:
            continue
        x, y, z = (a[0] + b[0]) // 2, (a[1] + b[1]) // 2 - 2, (a[2] + b[2]) // 2
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for dy in (-5, -4, -3):
                    W.put(x + dx, y + dy, z + dz, "gl_cab" if k % 4 else "gl_cab2")
        W.put(x, y - 4, z, "lit_yellowlight")
        for dy in range(-2, 1):
            W.put(x, y + dy, z, "gl_cable")


def launch_station(ctx, B, s_end):
    """Summit station around the pad: two hangars, a radio telescope, a control dome, plane statue, windsocks."""
    W = ctx.W
    F = Frame(B, s_end - 90)
    T = F.T
    for side in (-1, 1):
        for (v0, ln) in ((-20, 24), (14, 24)):
            u0 = side * 17
            ua, ub = sorted((u0, side * 36))
            lbox(W, F, ua, ub, v0, v0 + ln, T - 1, T - 1, "b_greydark")
            for y in range(T, T + 11):
                lbox(W, F, ua, ub, v0, v0, y, y, "gl_station")
                lbox(W, F, ua, ub, v0 + ln, v0 + ln, y, y, "gl_station")
                lbox(W, F, ua, ua, v0, v0 + ln, y, y, "gl_station")
                lbox(W, F, ub, ub, v0, v0 + ln, y, y, "gl_station")
            for k in range(0, 9):
                lo, hi = ua + k, ub - k
                if lo > hi:
                    break
                lbox(W, F, lo, lo, v0, v0 + ln, T + 11 + k, T + 11 + k, "gl_station_r")
                lbox(W, F, hi, hi, v0, v0 + ln, T + 11 + k, T + 11 + k, "gl_station_r")
            for yy in range(T, T + 8):
                for v in range(v0 + 3, v0 + ln - 2):
                    x, z = F.cell(side * 17, v)
                    W.erase(x, yy, z)
            for v in range(v0 + 3, v0 + ln - 2, 4):
                x, z = F.cell(side * 17, v)
                W.put(x, T + 8, z, "gl_glass")
    # radio telescope on the right hangar
    cx, cz = F.cell(30, -32)
    for y in range(T, T + 12):
        W.put(cx, y, cz, "gl_dish")
    W.ellipsoid(cx, T + 17, cz, 11, 5, 11, "gl_dish", mode="shell", part="upper")
    for y in range(T + 17, T + 24):
        W.put(cx, y, cz, "gl_dish")
    W.put(cx, T + 25, cz, "lit_red")
    # control dome
    cx, cz = F.cell(-30, -30)
    W.cyl(cx, cz, T, T + 6, 8, "gl_station", mode="wall")
    W.ellipsoid(cx, T + 6, cz, 8, 6, 8, "gl_station", mode="shell", part="upper")
    for a in range(0, 360, 40):
        W.put(int(round(cx + 8 * math.cos(math.radians(a)))), T + 3, int(round(cz + 8 * math.sin(math.radians(a)))), "gl_glass")
    # windsocks
    for v in (-10, 20, 60):
        for side in (-1, 1):
            x, z = F.cell(side * 11, v)
            for y in range(T, T + 9):
                W.put(x, y, z, "gl_dish")
            for k in range(1, 6):
                W.put(x + k * (1 if abs(F.t[0]) < 0.5 else 0), T + 8 - (k // 3), z + k * (1 if abs(F.t[0]) >= 0.5 else 0), ["cloth_r", "cloth_y"][k % 2]) if False else None


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s0, s1 = m["glacier"], m["summit"]
    s_pad = m["pad"]
    L = s1 - s0
    i0, i1 = B.i(s0), B.i(s_pad)
    tick = W.tick

    # ------------------------------------------------------------------ road: the lake basin is wide, the climb is walled in ice
    s_lake = s0 + 150.0
    edge = lambda s, u: "n_lightblue" if int(s // 3) % 2 == 0 else "n_white"
    wide = KartStyle(hw=11.0, verge="k_conc", edge=edge, surface=lambda s, u: "gl_road" if abs(u) > 7.0 else "k_asphalt", rail="jersey",
                     lanes=(-5.0, 0.0, 5.0), lane_dash=(4.0, 4.0), curbs=("k_curb_red",), curb_len=4.0, curb_rmax=140.0)
    stamp(ctx, B, s0, s0 + 30, KartStyle(hw=9.0, verge="k_conc", edge=edge, rail="jersey", lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=()), steps=False)
    stamp(ctx, B, s0 + 28, s_lake, wide)
    for k, s in enumerate(np.arange(s_lake, s_lake + 24, 4.0)):
        stamp(ctx, B, s - 1, s + 5, KartStyle(hw=11.0 - 4.0 * (k + 1) / 6.0, verge="k_conc", edge=edge, rail="jersey", lanes=(), curbs=(), edge_line=True), steps=False)
    main = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail_fn=lambda s, side: "mesh" if B.Tat(s) >= 18 else "armco", wall=("fr_wall_a", "fr_wall_b"),
                     wall_period=5.0, wall_h=2, deck="gl_ice", lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=("k_curb_red",), curb_len=4.0, curb_rmax=160.0,
                     chevron_rmax=70.0)
    stamp(ctx, B, s_lake + 20, s_pad - 126, main)
    stamp(ctx, B, s_pad - 128, s_pad, KartStyle(hw=9.0, verge="k_conc", edge=edge, rail="mesh", deck="gl_ice", lanes=(-4.5, 4.5), lane_dash=(5.0, 5.0), curbs=()))
    tick("road")

    # ------------------------------------------------------------------ the mountain
    xs, zs = B.p[i0:i1, 0], B.p[i0:i1, 1]
    g = Grid(xs.min() - 240, xs.max() + 110, zs.min() - 130, zs.max() + 80)
    d, Tn, Sn = centre_field(g, B, i0, i1, 80, hw=9.0)
    ridge = ridge_field(g, B, i0, i1, 0.82, hw=9.0)
    dome = np.full(ridge.shape, -1.0)
    for (cx, cz, hh, rr) in ((-700, 310, 74, 130), (-860, 450, 90, 110), (-640, 520, 70, 90), (-800, 190, 96, 120), (-930, 640, 60, 90)):
        q = 1.0 - ((g.X - cx) ** 2 + (g.Z - cz) ** 2) / float(rr * rr)
        dome = np.maximum(dome, hh * np.clip(q, 0, 1) ** 0.9 - 1)
    crag = 9.0 * (fbm(g.X, g.Z, 26.0, 4, 141) - 0.5) + 4.0 * (fbm(g.X, g.Z, 9.0, 2, 143) - 0.5)
    top = np.maximum(ridge, dome)
    H = top + crag * smoothstep(8, 50, top + 10)
    H = np.where(top < 1.5, top, H)
    shoulder = np.where(Tn > 0, Tn - 1, -1)
    w = 1.0 - smoothstep(3.0, 12.0, d + 0.4 * crag)
    Hc = H * (1 - w) + shoulder * w
    lake_c = (-790.0, 690.0)
    lake = (((g.X - lake_c[0]) / 70.0) ** 2 + ((g.Z - lake_c[1]) / 38.0) ** 2) <= 1.0
    Hc = np.where(lake & (d > 11.5), np.minimum(Hc, -1.0), Hc)
    Ht = np.maximum(np.rint(Hc).astype(np.int64), -1)
    land_rb = road_grid(ctx, g)
    mask = ((Ht >= 0) | (d < 62)) & (Ht >= -1) & ~(land_rb & (d > 14))
    slope = slope_of(Hc)
    mats = ["fr_snow", "fr_ice", "fr_rock", "fr_rock2", "fr_glacier", "gl_lake"]
    top_ids = pick([(Ht > 46, 4), (slope > 1.25, 2), (slope > 0.95, 1), (lake & (Ht < 0), 5)], 0)

    def side_fn(y, x, z):
        n = (fbm(x.astype(float), z.astype(float), 14.0, 2, 147) > 0.55)
        out = np.where(y < 18, 2, np.where(y < 52, 3, 4))
        return np.where(n & (y > 16), 1, out)

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
        if blocked_by_road(ctx, x, z) and abs(u) < 15:
            return None
        if any((x - a) ** 2 + (z - b) ** 2 < (r + rr) ** 2 for (a, b, rr) in placed):
            return None
        placed.append((x, z, r))
        return x, ground(x, z), z

    # ------------------------------------------------------------------ pines, igloos, ice boulders, ice spires
    def candidates(n, dmin, dmax, slope_max, hmin, hmax):
        ok = (d >= dmin) & (d <= dmax) & (Ht >= hmin) & (Ht <= hmax) & (slope < slope_max) & mask & ~lake & ~land_rb
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), int(Ht[ix[k], iz[k]]), g.z0 + int(iz[k])) for k in sel]

    for (x, y, z) in candidates(105, 12, 70, 0.9, 0, 58):
        pine(W, rng, x, y + 1, z, h=int(rng.integers(9, 17)), leaf="fr_fir", trunk="fr_trunk", snow="fr_snowcap")
    ig = []
    for (x, y, z) in candidates(500, 18, 64, 0.3, -1, 30):
        if any(abs(x - a) < 15 and abs(z - b) < 15 for a, b in ig):
            continue
        if len(ig) >= 8:
            break
        ig.append((x, z))
        igloo(W, x, y + 1, z, int(rng.integers(4, 8)) if len(ig) > 2 else 9, [(0, -1), (0, 1), (1, 0), (-1, 0)][len(ig) % 4])
        if len(ig) % 2 == 0:
            W.put(x + 3, y + 1, z + 9, "gl_snowman")
    for (x, y, z) in candidates(80, 10, 70, 1.0, 0, 60):
        r = int(rng.integers(1, 4))
        W.ellipsoid(x, y + 1, z, r + 1, r, r + 1, "fr_ice", part="upper")
    for (x, y, z) in candidates(26, 20, 66, 0.9, 6, 70):
        ice_spire(W, rng, x, y + 1, z, int(rng.integers(12, 30)), float(rng.integers(3, 6)))
    for (x, y, z) in candidates(40, 14, 56, 0.9, 0, 60):
        for _ in range(int(rng.integers(3, 6))):
            hh = int(rng.integers(3, 8))
            for k in range(hh):
                W.put(x + int(rng.integers(-2, 3)), y + 1 + k, z + int(rng.integers(-2, 3)), "gl_cry")
    tick("scatter")

    # ------------------------------------------------------------------ gate, the ice tunnel, the frozen waterfall, the ski lift
    gate(ctx, B, s0 + 6, "GLACIER", scale=2, text_mat="n_lightblue", board_mat="b_black", frame_mat="fr_ice", trim_mat="n_white", tower_u=36,
         board_half=38, tower_h=34)
    st0, st1 = s0 + 0.46 * (s1 - s0), s0 + 0.46 * (s1 - s0) + 120
    for s in np.arange(st0, st1, 3.0):
        F = Frame(B, s)
        T = F.T
        for k in range(0, 61):
            a = math.pi * k / 60
            u = 11.2 * math.cos(a)
            y = 11.5 * math.sin(a)
            for dv in range(2):
                x, z = F.cell(int(round(u)), dv)
                W.put(x, T + 2 + int(round(y)), z, "gl_ice" if (k + int(s)) % 5 else "gl_ice_b")
        if int(s) % 6 == 0:
            for side in (-1, 1):
                x, z = F.cell(side * 10.4, 0)
                W.put(x, T + 6, z, ["aurora_g", "aurora_c", "aurora_p"][int(s // 6) % 3])
        if int(s) % 9 == 0:
            for u in range(-8, 9, 4):
                x, z = F.cell(u + int(rng.integers(-1, 2)), 0)
                W.put(x, T + 13, z, "gl_icicle")
                if rng.random() < 0.5:
                    W.put(x, T + 12, z, "gl_icicle")
    for s in (st0 - 4, st1 + 2):
        F = Frame(B, s)
        T = F.T
        for k in range(0, 61):
            a = math.pi * k / 60
            for dv in range(3):
                x, z = F.cell(int(round(13.4 * math.cos(a))), dv)
                W.put(x, T + 2 + int(round(13 * math.sin(a))), z, "gl_cry")
    sw = s0 + 0.3 * (s1 - s0)
    q = spot(sw, 26, 14)
    if q:
        Fw = Frame(B, sw)
        for dv in range(-7, 8):
            for y in range(q[1], q[1] + 30):
                x, z = Fw.cell(26, dv)
                W.put(x, y, z, "gl_fall")
        for k in range(10):
            x, z = Fw.cell(26 + int(rng.integers(-4, 5)), int(rng.integers(-8, 9)))
            W.put(x, q[1] + 30, z, "gl_icicle")
    ski_lift(W, ctx, B, s0 + 180, s0 + 180 + 34 * 9, -30, rng, ground)
    tick("pieces")

    # ------------------------------------------------------------------ snowflakes, aurora curtains
    for (s, side) in ((s0 + 0.12 * (s1 - s0), 1), (s0 + 0.52 * (s1 - s0), -1), (s0 + 0.8 * (s1 - s0), 1)):
        F = Frame(B, s)
        x, z = F.cell(side * 50, 0)
        snowflake(W, x, F.T + 36, z, 14, "x" if abs(F.t[0]) < 0.5 else "z")
    aurora(W, rng, -1060, -520, 440, 150, 3)
    aurora(W, rng, -1060, -520, 240, 190, 3)
    tick("sky")

    # ------------------------------------------------------------------ the summit: launch station, hoops, gate, boosts
    launch_station(ctx, B, s_pad)
    for k, r in enumerate((14, 16.5, 19)):
        Fk = Frame(B, s_pad - 34 + k * 8)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 9, cz), (Fk.t[0], 0, Fk.t[1]), r, ["n_white", "n_cyan", "n_magenta"][k], thick=2, ymin=Fk.T + 1)
    gate(ctx, B, s_pad - 100, "AURORA PEAK", scale=2, text_mat="n_yellow", board_mat="b_black", frame_mat="gl_station", trim_mat="n_white", tower_u=36,
         board_half=38, tower_h=36)
    FL = Frame(B, s_pad - 1)
    for u in range(-8, 9):
        x, z = FL.cell(u, 0)
        W.put(x, FL.T + 1, z, "n_white" if u % 2 else "n_yellow")
    for frac in (0.04, 0.12, 0.2, 0.3, 0.4, 0.55, 0.66, 0.78, 0.9):
        boost_pad(W, B, s_lake + 30 + frac * (s1 - s_lake - 40), 14, 4, "n_lightblue", count=3, spacing=4)
    for frac in (0.02, 0.1, 0.18, 0.28, 0.38, 0.5, 0.62, 0.74, 0.86):
        item_boxes(W, B, s_lake + 20 + frac * (s1 - s_lake - 40), 7.0, n=4)
    corner_furniture(W, B, i0, B.i(s_pad - 130), 7.0, rmax=90.0, stacks=True, skid=True, need_support=False)
    flood_posts(W, B, i0, B.i(s_pad - 130), 60, 10.5, 6, glow="kp_glow_cyan", need_support=False)
    billboards(W, B, i0 + 40, B.i(s_pad - 130), 160, 12.0, need_support=False)
    tick("summit")
