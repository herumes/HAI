"""Zone 8: Rainbow Road - a floating neon ribbon through space: planets, nebulae, comets, an orbital ring, warp tunnel.
Ends in the long Star Slide that lands on the start straight."""
import math

import numpy as np

from core import fbm, mat
from font import text_cells
from kart import boost_pad, road_arrows
from kartprops import corner_furniture
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, cloud, hoop, item_boxes, lbox, lsign, zone_gate
from path import raster_band

BANDS = ["clay_red", "clay_orange", "clay_yellow", "clay_lime", "clay_cyan", "clay_blue", "clay_purple", "clay_pink"]
mat("rb_star", "Build_Lightsource_White")
mat("rb_rail_a", "Build_Lightsource_Cyan")
mat("rb_rail_b", "Build_Lightsource_Pink")
mat("rb_deck", "Build_Black_Cube")
mat("neb_pink", ["Cloth_Block_Wool_Pink", "Cloth_Block_Wool_Pink_Light", "Cloth_Block_Wool_Purple_Light"])
mat("neb_blue", ["Cloth_Block_Wool_Blue", "Cloth_Block_Wool_Blue_Light", "Cloth_Block_Wool_Purple"])
mat("neb_teal", ["Cloth_Block_Wool_Cyan", "Cloth_Block_Wool_Cyan_Light", "Cloth_Block_Wool_Green_Light"])
mat("rock_ast", ["Rock_Basalt", "Rock_Volcanic", "Rock_Slate", "Rock_Basalt_Cobble"])
mat("ring_dust", ["Soil_Clay_Smooth_Yellow2", "Soil_Clay_Smooth_Orange", "Soil_Clay_Smooth_White"])
mat("station", "Metal_Iron_Smooth")
mat("station_dark", "Metal_Zinc_Smooth")
PLANETS = [
    ["clay_orange", "clay_yellow2", "clay_orange", "clay_red2", "clay_orange", "clay_yellow"],
    ["clay_cyan", "clay_blue", "clay_white", "clay_cyan"],
    ["clay_green", "clay_greendark", "clay_blue", "clay_white", "clay_green"],
    ["clay_purple", "clay_pink", "clay_purple", "clay_white"],
    ["clay_red", "clay_red2", "clay_orange"],
    ["clay_grey", "clay_grey2", "clay_white", "clay_grey"],
    ["clay_yellow", "clay_orange", "clay_yellow2"],
]


def planet(W, rng, cx, cy, cz, R, bands, ring=False, tilt=(0.3, 1.0, 0.2)):
    n = int(R) + 2
    ox, oy, oz = np.meshgrid(np.arange(-n, n + 1), np.arange(-n, n + 1), np.arange(-n, n + 1), indexing="ij")
    d2 = ox * ox + oy * oy + oz * oz
    sel = (d2 <= (R + 0.5) ** 2) & (d2 > (R - 1.0) ** 2)
    ox, oy, oz = ox[sel], oy[sel], oz[sel]
    wob = 2.0 * np.sin(ox * 0.25) + 1.5 * np.sin(oz * 0.31)
    idx = (((oy + R + wob) / (2 * R + 1)) * len(bands)).astype(int) % len(bands)
    for k in range(len(bands)):
        q = idx == k
        W.put_arr(cx + ox[q], cy + oy[q], cz + oz[q], bands[k])
    if ring:
        t = np.array(tilt, float)
        t /= np.linalg.norm(t)
        a = np.cross(t, np.array([0, 0, 1.0]))
        a /= np.linalg.norm(a)
        b = np.cross(t, a)
        for rr in np.arange(1.45 * R, 1.95 * R, 0.6):
            m_ = int(2 * math.pi * rr / 0.6)
            for th in np.linspace(0, 2 * math.pi, m_, endpoint=False):
                p = np.array([cx, cy, cz]) + rr * (math.cos(th) * a + math.sin(th) * b)
                W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "ring_dust")


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s0, s1 = m["rainbow"], m["end"]
    i0, i1 = B.i(s0), B.i(s1)
    L = s1 - s0
    tick = W.tick
    s_slide = s1 - 235.0

    # the driving surface is Kart_Asphalt; the outer three cells of each side are the colours of the rainbow in neon blocks
    LEFT = ["n_yellow", "n_orange", "n_red"]            # inner -> outer on the left of travel
    RIGHT = ["n_lime", "n_cyan", "n_purple"]             # inner -> outer on the right (with blue/indigo implied by cyan/purple)

    def surf(s, u):
        a = abs(u)
        if a >= 5.5:
            k = min(2, int(a - 5.5))
            if int(s * 3 + u * 7) % 53 == 0:
                return "n_white"
            return (LEFT if u < 0 else RIGHT)[k]
        return "k_asphalt"

    def wall(s, u, h):
        return "n_magenta" if int(s // 6) % 2 == 0 else "n_cyan"

    common = dict(hw=8.5, surface=surf, verge="n_black", rail="mesh", wall=wall, wall_h=1, deck="rb_deck", lanes=(-2.75, 2.75),
                  lane_dash=(4.0, 4.0), edge_line=False, curbs=())
    style_float = KartStyle(**common)
    style_slide = KartStyle(piers=dict(mat="b_grey", every=20, size=2, cap="rb_deck", inset=2.0, beam="b_grey"), **common)
    stamp_kart_road(W, B, i0, B.i(s_slide), style_float)
    stamp_kart_road(W, B, B.i(s_slide), i1, style_slide)
    tick("road")

    # ------------------------------------------------------------------ gates: warp rings at the start, sign, finish warp
    Fw = Frame(B, s0 + 10)
    cx, cz = Fw.cell(0, 0)
    for k in range(7):
        Fk = Frame(B, s0 + 10 + k * 2.6)
        hoop(W, (Fk.cell(0, 0)[0], Fk.T + 4, Fk.cell(0, 0)[1]), (Fk.t[0], 0, Fk.t[1]), 11 + k * 3.2,
             ["n_red", "n_orange", "n_yellow", "n_lime", "n_cyan", "n_blue", "n_magenta"][k], thick=2)
    F = Frame(B, s0 + 70)
    cols = ["n_red", "n_orange", "n_yellow", "n_lime", "n_cyan", "n_blue", "n_magenta"]
    for (txt, ytop) in (("RAINBOW", F.T + 74), ("ROAD", F.T + 50)):
        w, h, cells = text_cells(txt, 3)
        u0 = -w // 2
        for (c_, r_) in cells:
            col = cols[((c_ + u0 + 400) // 18) % 7]
            for v in (0, 1):
                x, z = F.cell(u0 + c_, v)
                W.put(x, ytop - r_, z, col)
    tick("signs")

    # ------------------------------------------------------------------ boosts and items all along
    for s in np.arange(s0 + 50, s_slide, 52.0):
        boost_pad(W, B, s, 14, 5, "n_white", count=3, spacing=4)
    for s in np.arange(s0 + 30, s_slide, 71.0):
        item_boxes(W, B, s, 8.0, n=5)
    for s in np.arange(s_slide + 5, s1 - 40, 44.0):
        boost_pad(W, B, s, 16, 5, "n_cyan", count=4, spacing=4)
    tick("boosts")

    # ------------------------------------------------------------------ the orbital ring the road passes through
    sr = s0 + 265.0
    Fr = Frame(B, sr)
    c = (Fr.cell(0, 0)[0], Fr.T + 6, Fr.cell(0, 0)[1])
    d = (Fr.t[0], 0, Fr.t[1])
    hoop(W, c, d, 26, "station", thick=4)
    hoop(W, c, d, 26, "lit_cyan", thick=1)
    hoop(W, c, d, 22, "station_dark", thick=2)
    hoop(W, c, d, 30, "rb_rail_b", thick=1)
    for k in range(12):
        a = k * math.pi / 6
        tx = np.array([-d[2], 0, d[0]])
        ty = np.array([0, 1.0, 0])
        for t in range(30, 40):
            p = np.array(c) + (t * math.cos(a)) * tx + (t * math.sin(a)) * ty
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "station")
        p = np.array(c) + (40 * math.cos(a)) * tx + (40 * math.sin(a)) * ty
        W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), 2, 2, 2, "lit_yellowlight")
    tick("station")

    # ------------------------------------------------------------------ planets, nebulae, asteroids, stars, comets
    pts = np.stack([B.p[::3, 0], B.T[::3].astype(float), B.p[::3, 1]], 1)

    def clear(x, y, z):
        return float(np.sqrt(((pts - np.array([x, y, z])) ** 2).sum(1)).min())

    placed = []

    def spot(R, margin, region, yr, tries=400):
        for _ in range(tries):
            x, z = rng.uniform(region[0], region[1]), rng.uniform(region[2], region[3])
            y = rng.uniform(*yr)
            if clear(x, y, z) < R + margin:
                continue
            if any(math.dist((x, y, z), (px, py, pz)) < R + pr + 18 for px, py, pz, pr in placed):
                continue
            placed.append((x, y, z, R))
            return int(x), int(y), int(z)
        return None

    reg = (-700, -260, -150, 235)
    sizes = [31, 22, 18, 26, 14, 12, 10]
    for k, R in enumerate(sizes):
        q = spot(R, 38, reg, (40, 135))
        if q:
            planet(W, rng, q[0], q[1], q[2], R, PLANETS[k % len(PLANETS)], ring=(k in (0, 3)))
    print("  rainbow planets:", len(placed))
    tick("planets")
    for k in range(5):
        R = int(rng.integers(18, 27))
        q = spot(R, 26, reg, (30, 120))
        if q:
            cloud(W, rng, q[0], q[1], q[2], R, int(R * 0.5), int(R * 0.8), n=2, m=["neb_pink", "neb_blue", "neb_teal"][k % 3])
    tick("nebulae")
    for _ in range(60):
        x, z = rng.uniform(reg[0], reg[1]), rng.uniform(reg[2], reg[3])
        y = rng.uniform(20, 120)
        if clear(x, y, z) > 14:
            r_ = int(rng.integers(1, 4))
            W.ellipsoid(int(x), int(y), int(z), r_ + 1, r_, r_ + 1, "rock_ast")
    # stars
    n_star = 2400
    sx = rng.uniform(reg[0] - 40, reg[1] + 200, n_star)
    sz = rng.uniform(reg[2] - 40, reg[3] + 80, n_star)
    sy = rng.uniform(25, 170, n_star)
    cnt = 0
    for x, y, z in zip(sx, sy, sz):
        if clear(x, y, z) < 12:
            continue
        r_ = rng.random()
        W.put(int(x), int(y), int(z), "rb_star" if r_ < 0.8 else "lit_yellowlight" if r_ < 0.9 else "lit_cyan" if r_ < 0.95 else "lit_pink", keep=True)
        cnt += 1
    print("  rainbow stars:", cnt)
    tick("stars")
    # comets
    for k in range(4):
        x, z = rng.uniform(reg[0], reg[1]), rng.uniform(reg[2], reg[3])
        y = rng.uniform(70, 150)
        if clear(x, y, z) < 25:
            continue
        ang = rng.uniform(0, 2 * math.pi)
        W.ellipsoid(int(x), int(y), int(z), 3, 3, 3, "lit_cyan")
        for t in range(1, 46):
            px, py, pz = x - t * math.cos(ang), y + t * 0.35, z - t * math.sin(ang)
            for _ in range(2):
                W.put(int(px + rng.normal(0, 0.7 + t * 0.03)), int(py + rng.normal(0, 0.7)), int(pz + rng.normal(0, 0.7 + t * 0.03)),
                      "lit_white" if t < 20 else "lit_cyan")
    # a giant five-point star and a spiral galaxy
    def star_shape(cx, cy, cz, R, r_in):
        pts_ = []
        for k in range(10):
            a = math.pi / 2 + k * math.pi / 5
            rr = R if k % 2 == 0 else r_in
            pts_.append((cx + rr * math.cos(a), cy + rr * math.sin(a), cz))
        for k in range(10):
            p, q = pts_[k], pts_[(k + 1) % 10]
            W.line(p, q, "lit_yellow", rad=1.6, keep=True)
            W.line(p, (cx, cy, cz), "lit_orange", rad=0.6, keep=True) if k % 2 == 0 else None
        W.ellipsoid(cx, cy, cz, 5, 5, 5, "lit_yellowlight", mode="shell")

    if clear(-610, 110, 70) > 50:
        star_shape(-610, 110, 70, 34, 14)
    gx, gy, gz = -500, 160, -110
    for arm in range(2):
        for t in np.linspace(0.15, 1.0, 220):
            a = arm * math.pi + 7.0 * t
            rr = 6 + 60 * t
            px, py, pz = gx + rr * math.cos(a), gy + 6 * math.sin(a * 0.5), gz + rr * math.sin(a) * 0.55
            if clear(px, py, pz) > 12:
                W.put(int(px + rng.normal(0, 1.6)), int(py + rng.normal(0, 1.2)), int(pz + rng.normal(0, 1.6)),
                      "lit_pink" if arm == 0 else "lit_cyan", keep=True)
    W.ellipsoid(gx, gy, gz, 5, 4, 5, "lit_white", mode="shell")
    # a big sun far to the west
    W.ellipsoid(-690, 190, 20, 18, 18, 18, "lit_yellowlight", mode="shell")
    for k in range(24):
        a = k * 2 * math.pi / 24
        for t in range(20, 34):
            W.put(int(-690 + t * math.cos(a)), 190 + int(t * math.sin(a)), 20, "lit_orange")
    tick("space")

    # ------------------------------------------------------------------ the Star Slide: warp tunnel and the finish warp
    n = 0
    for s in np.arange(s_slide + 10, s1 - 40, 7.0):
        Fk = Frame(B, s)
        hoop(W, (Fk.cell(0, 0)[0], Fk.T + 7, Fk.cell(0, 0)[1]), (Fk.t[0], 0, Fk.t[1]), 13.5,
             ["n_white", "n_cyan", "n_magenta", "n_yellow"][n % 4], thick=1)
        n += 1
    for k in range(7):
        Fk = Frame(B, s1 - 34 + k * 3.0)
        hoop(W, (Fk.cell(0, 0)[0], Fk.T + 6, Fk.cell(0, 0)[1]), (Fk.t[0], 0, Fk.t[1]), 10 + k * 1.6,
             ["n_white", "n_yellow", "n_cyan"][k % 3], thick=2)
    # 'WARP EXIT' floating text above the slide's middle
    F = Frame(B, s_slide + 70)
    w, h, cells = text_cells("WARP", 3)
    u0 = -w // 2
    for (c_, r_) in cells:
        for v in (0, 1):
            x, z = F.cell(u0 + c_, v)
            W.put(x, F.T + 46 - r_, z, "n_cyan" if (c_ // 4) % 2 else "n_white")
    tick("slide")
