"""Zone 5: Frostpeak - a switchback road up a snow mountain to the Skyport launch station."""
import math

import numpy as np

from core import fbm, mat, smoothstep, stair_rot
from kart import boost_pad, road_arrows
from kartprops import billboards, corner_furniture, flood_posts
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, hoop, item_boxes, lbox, lsign, pine, road_grid, zone_gate
from path import raster_band
from terrain import Grid, carve, centre_field, pick, ridge_field, skin, slope_of
from zone_canyon import rock_arch

mat("fr_road", ["Build_GreyLight_Cube", "Build_GreyLight_Cube", "Build_White_Cube", "Build_GreyLight_Cube"])
mat("fr_stair", "Build_GreyLight_Stairs")
mat("fr_line", "Build_Lightsource_Cyan")
mat("fr_curb_a", "Cloth_Block_Wool_Cyan_Light")
mat("fr_curb_b", "Cloth_Block_Wool_White")
mat("fr_wall_a", "Rock_Ice")
mat("fr_wall_b", "Soil_Snow_Brick")
mat("fr_snow", ["Soil_Snow", "Soil_Snow", "Soil_Snow", "Soil_Snow_Brick"])
mat("fr_ice", ["Rock_Ice", "Rock_Ice", "Rock_Ice_Permafrost"])
mat("fr_rock", ["Rock_Stone_Cobble", "Rock_Slate_Cobble", "Rock_Stone_Cobble_Mossy"])
mat("fr_rock2", ["Rock_Slate_Brick", "Rock_Stone_Brick", "Rock_Shale_Cobble"])
mat("fr_glacier", ["Rock_Ice", "Rock_Ice", "Soil_Snow", "Rock_Ice_Permafrost"])
mat("fr_lake", ["Rock_Ice", "Rock_Ice", "Rock_Ice_Permafrost"])
mat("igloo", ["Soil_Snow_Brick", "Soil_Snow_Brick", "Soil_Snow"])
mat("igloo_door", "Rock_Ice")
mat("fr_fir", ["Plant_Leaves_Fir", "Plant_Leaves_Fir", "Plant_Leaves_Fir_Snow", "Plant_Leaves_Fir_Snow"])
mat("fr_trunk", "Wood_Fir_Trunk")
mat("fr_snowcap", "Soil_Snow")
mat("snowflake", "Build_Lightsource_White")
mat("aurora_g", "Build_Lightsource_Green")
mat("aurora_c", "Build_Lightsource_Cyan")
mat("aurora_p", "Build_Lightsource_Pink")
mat("hangar_wall", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("hangar_roof", "Soil_Clay_Smooth_Cyan_Stairs")
mat("hangar_roof2", "Soil_Clay_Smooth_Cyan")
mat("hangar_trim", "Soil_Clay_Smooth_Blue")
mat("plane_body", "Soil_Clay_Smooth_White")
mat("plane_wing", "Soil_Clay_Smooth_Red")
mat("dish", "Metal_Iron_Smooth")


def snowflake(W, cx, cy, cz, r, axis="x"):
    """A 6-armed flat snowflake in a vertical plane (axis = plane normal, x or z)."""
    for k in range(6):
        a = k * math.pi / 3
        for t in range(-r, r + 1):
            du = t * math.cos(a)
            dh = t * math.sin(a)
            if axis == "x":
                W.put(cx, int(round(cy + dh)), int(round(cz + du)), "snowflake")
            else:
                W.put(int(round(cx + du)), int(round(cy + dh)), cz, "snowflake")
        for t in (r * 0.55, r * 0.8):
            for side in (-1, 1):
                for b in range(1, int(r * 0.3)):
                    a2 = a + side * math.pi / 3
                    du = t * math.cos(a) + b * math.cos(a2)
                    dh = t * math.sin(a) + b * math.sin(a2)
                    if axis == "x":
                        W.put(cx, int(round(cy + dh)), int(round(cz + du)), "snowflake")
                    else:
                        W.put(int(round(cx + du)), int(round(cy + dh)), cz, "snowflake")


def igloo(W, x, y, z, r, face):
    W.ellipsoid(x, y, z, r, r * 0.85, r, "igloo", mode="shell", part="upper")
    dx, dz = face
    for k in range(0, 3):
        for yy in range(y, y + 3 - (1 if k == 2 else 0)):
            W.erase(x + dx * (r - k), yy, z + dz * (r - k))
    W.put(x + dx * (r + 1), y - 0, z + dz * (r + 1), "igloo_door")
    W.put(x - dz * (r - 1), y + 1, z + dx * (r - 1), "lit_yellowlight")
    W.put(x + dz * (r - 1), y + 1, z - dx * (r - 1), "lit_yellowlight")


def aurora(W, rng, x0, x1, z, y0, n=3):
    cols = ["aurora_g", "aurora_c", "aurora_p"]
    for b in range(n):
        ph = rng.uniform(0, 6)
        for x in range(x0, x1):
            zz = z + int(14 * math.sin(x * 0.03 + ph + b) + 8 * math.sin(x * 0.11 + b * 2))
            top = y0 + int(10 * math.sin(x * 0.045 + ph)) + b * 9
            ln = int(12 + 10 * (0.5 + 0.5 * math.sin(x * 0.09 + b)))
            for k in range(ln):
                if rng.random() < 0.92:
                    W.put(x, top + k, zz, cols[(b + (k // 6)) % 3])


def skyport(W, rng, A, s_end):
    """The summit station: two hangars flanking the last straight, a transform gate, radar, flags, a plane statue."""
    F = Frame(A, s_end - 60)
    T = F.T
    # hangars
    for side in (-1, 1):
        for (v0, ln) in ((-34, 22), (8, 20)):
            u0 = side * 14
            u1 = side * 30
            ua, ub = sorted((u0, u1))
            lbox(W, F, ua, ub, v0, v0 + ln, T - 1, T - 1, "b_greydark")
            for y in range(T, T + 9):
                lbox(W, F, ua, ub, v0, v0, y, y, "hangar_wall")
                lbox(W, F, ua, ub, v0 + ln, v0 + ln, y, y, "hangar_wall")
                lbox(W, F, ua, ua, v0, v0 + ln, y, y, "hangar_wall")
                lbox(W, F, ub, ub, v0, v0 + ln, y, y, "hangar_wall")
            # barrel roof: stairs stepping in
            for k in range(0, 8):
                yy = T + 9 + k
                lo, hi = ua + k, ub - k
                if lo > hi:
                    break
                lbox(W, F, lo, lo, v0, v0 + ln, yy, yy, "hangar_roof2")
                lbox(W, F, hi, hi, v0, v0 + ln, yy, yy, "hangar_roof2")
            lbox(W, F, ua + 4, ub - 4, v0, v0 + ln, T + 16, T + 16, "hangar_roof2")
            for yy in range(T, T + 7):
                for v in range(v0 + 3, v0 + ln - 2):
                    x, z = F.cell(u0, v)
                    W.erase(x, yy, z)
            for v in range(v0 + 3, v0 + ln - 2, 4):
                x, z = F.cell(u0, v)
                W.put(x, T + 7, z, "lit_cyan")
            # stripes
            lbox(W, F, ua, ub, v0, v0 + ln, T + 9, T + 9, "hangar_trim")
    # radar dish on the right hangar
    cx, cz = F.cell(24, -24)
    for y in range(T + 17, T + 24):
        W.put(cx, y, cz, "dish")
    W.ellipsoid(cx, T + 26, cz, 6, 3, 6, "dish", mode="shell", part="upper")
    W.put(cx, T + 29, cz, "lit_red")
    # plane statue on a plinth on the left
    px, pz = F.cell(-22, 16)
    base_y = T + 10
    for y in range(T, base_y):
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                W.put(px + dx, y, pz + dz, "b_grey")
    W.ellipsoid(px, base_y + 3, pz, 2, 2, 9, "plane_body")
    for k in range(-2, 3):
        W.box((px - 9, base_y + 3, pz - 1 + k), (px + 9, base_y + 3, pz + k), "plane_wing")
    W.box((px - 4, base_y + 3, pz + 6), (px + 4, base_y + 3, pz + 7), "plane_wing")
    W.box((px, base_y + 4, pz + 6), (px, base_y + 7, pz + 8), "plane_wing")
    W.put(px, base_y + 4, pz - 8, "lit_cyan")
    # transform gate: three hoops over the last straight
    for k, r in enumerate((11, 12.5, 14)):
        Fk = Frame(A, s_end - 22 + k * 7)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 8, cz), (Fk.t[0], 0, Fk.t[1]), r, ["lit_yellow", "lit_white", "lit_cyan"][k], thick=2, ymin=Fk.T + 1)


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["frost"], m["end"]
    L = s1 - s0
    i0, i1 = A.i(s0), A.i(s1)

    st = KartStyle(hw=7.0, verge="k_conc", rail_fn=lambda s, side: "mesh" if A.Tat(s) >= 20 else "armco", wall=("fr_wall_a", "fr_wall_b"),
                   wall_period=5.0, wall_h=2, deck="fr_ice", curbs=("k_curb_red",), curb_len=4.0, curb_rmax=170.0, chevron_rmax=70.0)
    stamp_kart_road(W, A, i0, i1, st)
    W.tick("road")

    # ------------------------------------------------------------------ the mountain
    xs, zs = A.p[i0:i1, 0], A.p[i0:i1, 1]
    g = Grid(xs.min() - 120, xs.max() + 70, zs.min() - 200, zs.max() + 70)
    d, Tn, Sn = centre_field(g, A, i0, i1, 90, hw=9.0)
    ridge = ridge_field(g, A, i0, i1, 0.9, hw=9.0)
    dome = np.full(ridge.shape, -1.0)
    for (cx, cz, hh, rr) in ((-165, 540, 104, 112), (-270, 480, 76, 55), (-80, 480, 66, 50), (-215, 605, 70, 46)):
        q = 1.0 - ((g.X - cx) ** 2 + (g.Z - cz) ** 2) / float(rr * rr)
        dome = np.maximum(dome, hh * np.clip(q, 0, 1) ** 0.9 - 1)
    crag = 9.0 * (fbm(g.X, g.Z, 26.0, 4, 41) - 0.5) + 4.0 * (fbm(g.X, g.Z, 9.0, 2, 43) - 0.5)
    H = np.maximum(ridge, dome) + crag * smoothstep(8, 50, np.maximum(ridge, dome) + 10)
    H = np.where(np.maximum(ridge, dome) < 1.5, np.maximum(ridge, dome), H)
    shoulder = np.where(Tn > 0, Tn - 1, -1)
    Hc = carve(H, d, shoulder, 3.0, 12.0, noise=crag * 0.4)
    Ht = np.rint(Hc).astype(np.int64)
    Ht = np.maximum(Ht, -1)
    mask = (Ht >= 0) | (d < 60)
    rb = road_grid(ctx, g)
    mask = mask & ~(rb & (d > 12))
    slope = slope_of(Hc)
    # frozen lake at the foot
    lake = (((g.X + 135) / 62.0) ** 2 + ((g.Z - 752) / 26.0) ** 2) <= 1.0
    Ht = np.where(lake & (Ht < 2), -1, Ht)
    mats = ["fr_snow", "fr_ice", "fr_rock", "fr_rock2", "fr_glacier", "fr_lake"]
    top_ids = pick([(Ht > 40, 4), (slope > 1.2, 2), (slope > 0.9, 1), (lake & (Ht < 0), 5)], 0)

    def side_fn(y, x, z):
        n = (fbm(x.astype(float), z.astype(float), 14.0, 2, 47) > 0.55)
        out = np.where(y < 16, 2, np.where(y < 46, 3, 4))
        return np.where(n & (y > 16), 1, out)

    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask)
    W.tick("terrain")

    # ------------------------------------------------------------------ trees, igloos
    def candidates(n, dmin, dmax, slope_max, hmin, hmax):
        ok = (d >= dmin) & (d <= dmax) & (Ht >= hmin) & (Ht <= hmax) & (slope < slope_max) & mask & ~lake & ~rb
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), int(Ht[ix[k], iz[k]]), g.z0 + int(iz[k])) for k in sel]

    for (x, y, z) in candidates(125, 12, 80, 0.9, 0, 52):
        pine(W, rng, x, y + 1, z, h=int(rng.integers(9, 17)), leaf="fr_fir", trunk="fr_trunk", snow="fr_snowcap")
    placed = []
    for (x, y, z) in candidates(400, 18, 60, 0.3, -1, 26):
        if any(abs(x - a) < 14 and abs(z - b) < 14 for a, b in placed):
            continue
        if len(placed) >= 7:
            break
        placed.append((x, z))
        face = [(0, -1), (0, 1), (1, 0), (-1, 0)][len(placed) % 4]
        igloo(W, x, y + 1, z, int(rng.integers(4, 8)) if len(placed) > 2 else 9, face)
    W.tick("pines+igloos")
    # ice boulders
    for (x, y, z) in candidates(90, 10, 70, 1.0, 0, 50):
        r = int(rng.integers(1, 4))
        W.ellipsoid(x, y + 1, z, r + 1, r, r + 1, "fr_ice", part="upper")

    W.tick("boulders")
    # ------------------------------------------------------------------ road pieces
    zone_gate(W, A, s0 + 8, "FROST", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="fr_ice", trim_mat="n_white",
              tower_u=40, board_half=40, tower_h=34)
    for frac in (0.12, 0.44):
        Fa = Frame(A, s0 + frac * L)
        if Fa.axis_aligned:
            rock_arch(W, rng, Fa, 13, 14, 4, "fr_ice", base_down=int(Fa.T) - 1)
    for frac in (0.1, 0.2, 0.3, 0.46, 0.56, 0.68, 0.8):
        boost_pad(W, A, s0 + frac * L, 14, 4, "n_lightblue", count=3, spacing=4)
    for frac in (0.05, 0.16, 0.26, 0.4, 0.52, 0.64, 0.76):
        item_boxes(W, A, s0 + frac * L, 7.0, n=4)
    corner_furniture(W, A, i0, i1, 7.0, rmax=90.0, stacks=True, skid=True)
    flood_posts(W, A, i0, i1, 60, 10.5, 6, glow="kp_glow_cyan")
    billboards(W, A, i0 + 40, i1 - 160, 160, 12.0)
    for (x, y, z) in ((-120, 130, 700), (-150, 150, 640), (-40, 125, 640), (-260, 135, 560)):
        snowflake(W, x, y, z, 14, "x" if x % 2 == 0 else "z")
    aurora(W, rng, -330, 40, 520, 120, 3)

    W.tick("road pieces")
    # ------------------------------------------------------------------ summit station
    skyport(W, rng, A, s1)
    zone_gate(W, A, s1 - 100, "SKYPORT", scale=2, text_mat="n_yellow", board_mat="b_black", frame_mat="gold_brick", trim_mat="n_white",
              tower_u=46, board_half=46, tower_h=30)
