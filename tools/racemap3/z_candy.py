"""Zone 1: Candy Comet Hills - pastel clay hills, a giant donut gate, gumdrop slalom, lollipop forest, candy-cane tunnel,
a chocolate river with a wafer bridge, a layered cake, ice-cream cone towers, cupcakes and gummy bears."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403

for nm, key in [("Lime", "cd_lime"), ("Green", "cd_green"), ("Pink", "cd_pink"), ("White", "cd_white"), ("Yellow", "cd_yellow"),
                ("Cyan", "cd_cyan"), ("Purple", "cd_purple"), ("Orange", "cd_orange"), ("Red", "cd_red"), ("Blue", "cd_blue"),
                ("Yellow2", "cd_yellow2"), ("Red2", "cd_red2")]:
    mat(key, "Soil_Clay_Smooth_" + nm)
mat("cd_top", ["Soil_Clay_Smooth_Lime", "Soil_Clay_Smooth_Lime", "Soil_Clay_Smooth_Lime", "Soil_Clay_Smooth_Green"])
mat("cd_top_pink", ["Soil_Clay_Smooth_Pink", "Soil_Clay_Smooth_Pink", "Soil_Clay_Smooth_White"])
mat("cd_top_blue", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_White"])
mat("cd_side_a", ["Soil_Clay_Smooth_Pink", "Soil_Clay_Smooth_Pink", "Soil_Clay_Smooth_Red2"])
mat("cd_side_b", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Cloth_Block_Wool_White"])
mat("cd_side_c", ["Soil_Mud", "Soil_Mud", "Soil_Dirt_Wet"])
mat("choc", ["Soil_Mud", "Soil_Mud", "Soil_Mud", "Soil_Dirt_Wet"])
mat("choc_light", ["Soil_Dirt", "Soil_Dirt_Dry"])
mat("cd_wafer", ["Rock_Sandstone_Brick", "Rock_Sandstone_Brick", "Rock_Sandstone_Brick_Smooth"])
mat("cd_wafer_dark", ["Soil_Clay_Brick", "Soil_Clay_Brick", "Rock_Sandstone_Red_Brick"])
mat("cd_cone", ["Rock_Sandstone_Brick", "Soil_Clay_Brick", "Rock_Sandstone_Brick"])
mat("cd_cane_r", "Cloth_Block_Wool_Red")
mat("cd_cane_w", "Cloth_Block_Wool_White")
mat("cd_marsh", ["Cloth_Block_Wool_White", "Cloth_Block_Wool_Pink_Light"])
mat("cd_cotton_pink", ["Cloth_Block_Wool_Pink", "Cloth_Block_Wool_Pink_Light", "Cloth_Block_Wool_Purple_Light"])
mat("cd_cotton_blue", ["Cloth_Block_Wool_Cyan_Light", "Cloth_Block_Wool_Blue_Light", "Cloth_Block_Wool_White"])
mat("cd_sprinkle", ["NoCube_Neon_Block_Pink", "NoCube_Neon_Block_Yellow", "NoCube_Neon_Block_Cyan", "NoCube_Neon_Block_Lime",
                    "NoCube_Neon_Block_Magenta", "NoCube_Neon_Block_White", "NoCube_Neon_Block_Orange"])
mat("cd_cherry", "Cloth_Block_Wool_Red")
mat("cd_candle", "Soil_Clay_Smooth_Yellow")
mat("cd_flame", "Build_Lightsource_Orange")
mat("cd_fence", "Build_White_Fence")
mat("cd_jelly_a", "Soil_Clay_Smooth_Red")
mat("cd_jelly_b", "Soil_Clay_Smooth_Lime")
mat("cd_jelly_c", "Soil_Clay_Smooth_Orange")
mat("cd_jelly_d", "Soil_Clay_Smooth_Purple")
mat("cd_road_edge", ["NoCube_Neon_Block_Pink", "NoCube_Neon_Block_White"])
JELLY = ["cd_jelly_a", "cd_jelly_b", "cd_jelly_c", "cd_jelly_d", "cd_yellow", "cd_cyan"]
CANDY = ["cd_pink", "cd_cyan", "cd_yellow", "cd_purple", "cd_orange", "cd_lime", "cd_red", "cd_blue"]


# --------------------------------------------------------------------------- set pieces
def donut(W, rng, c, d, R=22.0, r=6.0):
    """A vertical ring around centre c (plane normal d): dough with pink frosting on the upper half and sprinkles."""
    d = np.array(d, float)
    d /= np.linalg.norm(d)
    up = np.array([0, 1.0, 0])
    a = np.cross(d, up)
    a /= np.linalg.norm(a)
    b = np.cross(d, a)                   # b ~ up in the ring plane
    n = int(2 * math.pi * R / 0.45)
    m_ = int(2 * math.pi * r / 0.45)
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    ph = np.linspace(0, 2 * math.pi, m_, endpoint=False)
    TH, PH = np.meshgrid(th, ph, indexing="ij")
    ring = (np.cos(TH)[..., None] * a + np.sin(TH)[..., None] * b)         # outward direction in the ring plane
    p = np.array(c, float) + (R + r * np.cos(PH))[..., None] * ring + (r * np.sin(PH))[..., None] * d
    pts = np.floor(p.reshape(-1, 3) + 0.5).astype(np.int64)
    nup = (np.cos(PH) * ring[..., 1]).reshape(-1)                  # how much the outward normal faces up
    frost = nup > 0.18
    key = (pts[:, 0] + OX) | ((pts[:, 2] + OZ) << 14) | ((pts[:, 1] + OY) << 28)
    _, first = np.unique(key, return_index=True)
    pts, frost = pts[first], frost[first]
    W.put_arr(pts[~frost, 0], pts[~frost, 1], pts[~frost, 2], "cd_orange")
    W.put_arr(pts[frost, 0], pts[frost, 1], pts[frost, 2], "cd_pink")
    fp = pts[frost]
    sel = rng.random(len(fp)) < 0.12
    for q in fp[sel]:
        W.put(int(q[0]), int(q[1]), int(q[2]), "cd_sprinkle")


def gumdrop(W, rng, x, y, z, r, col):
    mode = "shell" if r > 3.5 else "fill"
    W.ellipsoid(x, y, z, r, r * 0.82, r, col, mode=mode, part="upper")
    for _ in range(int(r * 1.5)):
        a, b = rng.uniform(0, 2 * math.pi), rng.uniform(0.25, 1.2)
        W.put(int(round(x + r * 0.92 * math.cos(b) * math.cos(a))), int(round(y + r * 0.8 * math.sin(b))),
              int(round(z + r * 0.92 * math.cos(b) * math.sin(a))), "cd_white")


def cane(W, x, y, z, h, hook_dir=(1, 0), hook_r=4):
    for k in range(h):
        W.put(x, y + k, z, "cd_cane_r" if (k // 2) % 2 == 0 else "cd_cane_w")
    for i in range(0, 13):
        a = math.pi * i / 12
        px = x + hook_dir[0] * (hook_r - hook_r * math.cos(a))
        pz = z + hook_dir[1] * (hook_r - hook_r * math.cos(a))
        py = y + h + hook_r * math.sin(a)
        W.put(int(round(px)), int(round(py)), int(round(pz)), "cd_cane_r" if (i // 2) % 2 == 0 else "cd_cane_w")


def lollipop(W, rng, x, y, z, h, r, plane, cols):
    """Stick plus a vertical spiral disc (plane 'x': disc in the y-z plane, 'z': in the x-y plane)."""
    for k in range(h):
        W.put(x, y + k, z, "cd_cane_w")
        if r > 6:
            W.put(x + 1, y + k, z, "cd_cane_w")
    cy = y + h + int(r * 0.7)
    n = int(r) + 2
    for u in range(-n, n + 1):
        for v in range(-n, n + 1):
            rr = math.hypot(u, v)
            if rr > r + 0.4:
                continue
            ang = math.atan2(v, u)
            band = int(math.floor(ang / math.pi * 4 + rr * 0.55)) % 2
            m = cols[band]
            for t in (0, 1):
                if plane == "x":
                    W.put(x + t, cy + v, z + u, m)
                else:
                    W.put(x + u, cy + v, z + t, m)
    W.put(x, cy, z, "cd_white")


def cupcake(W, rng, x, y, z, r, wrap, frost):
    for k in range(int(r * 0.9)):
        rr = r * 0.62 + k * 0.28
        W.cyl(x, z, y + k, y + k, rr, wrap, mode="wall" if rr > 2.3 else "fill")
    top = y + int(r * 0.9)
    W.cyl(x, z, top, top, r * 0.95, wrap)
    for k, f in enumerate((1.05, 0.82, 0.58, 0.34)):
        W.cyl(x, z, top + 1 + k, top + 1 + k, max(1.0, r * f), frost, mode="fill")
    W.ellipsoid(x, top + 6, z, 1.6, 1.6, 1.6, "cd_cherry")
    for _ in range(int(r)):
        a = rng.uniform(0, 2 * math.pi)
        rr = r * 0.8 * rng.random()
        W.put(int(x + rr * math.cos(a)), top + 2, int(z + rr * math.sin(a)), "cd_sprinkle")


def ice_cream(W, rng, x, y, z, h=30, r=8.0, scoops=("cd_pink", "cd_white", "choc")):
    for k in range(h):
        rr = r * (k + 1) / float(h)
        W.cyl(x, z, y + k, y + k, rr, "cd_cone", mode="wall" if rr > 1.6 else "fill")
    # waffle lines
    for k in range(0, h, 3):
        W.cyl(x, z, y + k, y + k, r * (k + 1) / float(h) + 0.45, "cd_wafer_dark", mode="wall")
    top = y + h
    W.cyl(x, z, top, top, r + 1.2, "cd_wafer_dark")
    W.ellipsoid(x, top + 4, z, r + 1.0, r * 0.9, r + 1.0, scoops[0], mode="shell")
    W.ellipsoid(x, top + 4 + int(r * 1.3), z, r * 0.8, r * 0.7, r * 0.8, scoops[1], mode="shell")
    if len(scoops) > 2:
        W.ellipsoid(x, top + 4 + int(r * 2.3), z, r * 0.55, r * 0.5, r * 0.55, scoops[2], mode="fill")
    W.ellipsoid(x, top + 4 + int(r * 2.9), z, 2, 2, 2, "cd_cherry")
    for _ in range(int(r * 4)):
        a, b = rng.uniform(0, 2 * math.pi), rng.uniform(0.0, 1.0)
        W.put(int(x + (r + 1.1) * math.cos(b) * math.cos(a)), int(top + 4 + r * 0.9 * math.sin(b)), int(z + (r + 1.1) * math.cos(b) * math.sin(a)),
              "cd_sprinkle")
    # a drip down the side
    for _ in range(5):
        a = rng.uniform(0, 2 * math.pi)
        for k in range(int(rng.integers(3, 7))):
            W.put(int(x + (r + 0.8) * math.cos(a)), top + 2 - k, int(z + (r + 0.8) * math.sin(a)), scoops[0])


def gummy(W, x, y, z, s, col, face=(0, 1)):
    W.ellipsoid(x, y + int(7 * s), z, 4.2 * s, 6.2 * s, 3.2 * s, col)
    W.ellipsoid(x, y + int(15 * s), z, 4.0 * s, 3.6 * s, 3.4 * s, col)
    for sgn in (-1, 1):
        W.ellipsoid(x + int(sgn * 3.2 * s), y + int(18 * s), z, 1.5 * s, 1.5 * s, 1.2 * s, col)
        W.ellipsoid(x + int(sgn * 5.8 * s), y + int(9 * s), z, 1.8 * s, 3.2 * s, 1.8 * s, col)
        W.ellipsoid(x + int(sgn * 2.4 * s), y + int(2 * s), z, 1.9 * s, 2.4 * s, 2.0 * s, col)
    fz = z + int(3.3 * s) * face[1]
    for sgn in (-1, 1):
        W.put(x + int(sgn * 1.6 * s), y + int(16 * s), fz, "b_black")
    W.put(x, y + int(14 * s), fz, "b_black")


def cake(W, rng, cx, cz, y0):
    """Three-layer cake with drips, cherries and candles."""
    layers = [(34, 9, "cd_side_c", "choc"), (26, 8, "cd_side_a", "cd_pink"), (18, 8, "cd_side_b", "cd_white")]
    y = y0
    for (r, h, side, top) in layers:
        W.cyl(cx, cz, y, y + h - 1, r, side, mode="wall")
        W.cyl(cx, cz, y + h, y + h, r, top, mode="fill")
        for k in range(0, 360, 14):
            a = math.radians(k)
            ln = int(rng.integers(2, h - 1))
            for t in range(ln):
                W.put(int(round(cx + (r + 0.6) * math.cos(a))), y + h - t, int(round(cz + (r + 0.6) * math.sin(a))), "cd_white" if side != "cd_side_b" else "cd_pink")
        for k in range(0, 360, 6):
            a = math.radians(k)
            W.put(int(round(cx + (r - 0.5) * math.cos(a))), y + h + 1, int(round(cz + (r - 0.5) * math.sin(a))), CANDY[(k // 6) % 8] if rng.random() < 0.85 else "cd_white")
        y += h + 1
    for k in range(10):
        a = k * math.pi / 5
        px, pz = int(round(cx + 11 * math.cos(a))), int(round(cz + 11 * math.sin(a)))
        for t in range(5):
            W.put(px, y + t, pz, "cd_candle")
        W.put(px, y + 5, pz, "cd_flame")
        W.put(px, y + 6, pz, "lit_yellowlight")
    for (dx, dz) in ((0, 0), (6, 2), (-5, 4), (1, -6)):
        W.ellipsoid(cx + dx, y + 2, cz + dz, 3, 3, 3, "cd_cherry")
    return y


def choc_fountain(W, rng, x, y, z):
    for (r, h, k) in ((9, 3, 0), (6, 3, 4), (3.5, 3, 8)):
        W.cyl(x, z, y + k, y + k + h - 1, r, "choc", mode="wall" if r > 2 else "fill")
        W.cyl(x, z, y + k + h, y + k + h, r + 1, "cd_wafer_dark", mode="wall")
    for t in range(12):
        W.put(x, y + 11 + t, z, "choc_light")
    for a in range(0, 360, 45):
        for t in range(1, 8):
            W.put(int(round(x + 3.5 * math.cos(math.radians(a)))), y + 8 - t // 2, int(round(z + 3.5 * math.sin(math.radians(a)))), "choc_light")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["candy"], m["city"]
    L = s1 - s0
    i0, i1 = A.i(s0), A.i(s1)

    # ------------------------------------------------------------------ road: pink / white neon edge, jersey barriers, candy-striped curbs
    edge = lambda s, u: "n_pink" if int(s // 3) % 2 == 0 else "n_white"
    st = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail="jersey", lanes=(0.0,), lane_dash=(3.0, 3.0), curbs=("k_curb_red", "k_curb_yel"),
                   curb_len=4.0, curb_rmax=120.0)
    stamp(ctx, A, s0, s1, st)
    W.tick("road")

    # ------------------------------------------------------------------ terrain: pastel clay hills, layered like a cake
    land = Land(ctx, A, i0, i1, margin=85, radius=66, hw=9.0)
    g, d, Tn, Sn = land.g, land.d, land.Tn, land.Sn
    amp = smoothstep(s0, s0 + 40, Sn) * (1 - smoothstep(s1 - 50, s1, Sn))
    n1 = fbm(g.X, g.Z, 120.0, 4, 31)
    n2 = fbm(g.X + 400, g.Z - 250, 46.0, 3, 17)
    H = -1.0 + amp * (30.0 * smoothstep(0.42, 0.80, n1) + 8.0 * smoothstep(0.38, 0.85, n2))
    taper = 1.0 - smoothstep(40.0, 60.0, d)
    H = H * taper + (-1.0) * (1 - taper)
    land.finish(H, d0=4.0, d1=20.0, mask_d=64)
    Ht, mask, slope = land.Ht, land.mask, land.slope
    # a chocolate river: crosses the road at the bridge
    sb = s0 + 0.52 * L
    Fb = Frame(A, sb)
    river = np.zeros(Ht.shape, bool)
    for v in range(-22, 23):
        for u in range(-90, 91):
            x, z = Fb.cell(u, v)
            ix, iz = x - g.x0, z - g.z0
            if 0 <= ix < g.nx and 0 <= iz < g.nz:
                river[ix, iz] = True
    river &= (d > 8.5) | (np.abs(Tn - Fb.T) > 100)
    Ht = np.where(river & (Ht < 6), -1, Ht)
    mats = ["cd_top", "cd_top_pink", "cd_top_blue", "cd_side_a", "cd_side_b", "cd_side_c", "choc"]
    pat = fbm(g.X, g.Z, 22.0, 2, 63)
    top_ids = pick([(pat > 0.62, 1), (pat < 0.3, 2), (river & (Ht < 0), 6)], 0)

    def side_fn(y, x, z):
        k = (y // 2) % 3
        return np.where(k == 0, 3, np.where(k == 1, 4, 5))

    land.Ht = Ht
    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask | river)
    W.tick("terrain")

    # ------------------------------------------------------------------ ground-contact helpers
    def ground(x, z):
        ix, iz = x - g.x0, z - g.z0
        if 0 <= ix < g.nx and 0 <= iz < g.nz and (mask | river)[ix, iz]:
            return int(Ht[ix, iz]) + 1
        return 0

    placed = []

    def free(x, z, r):
        return all((x - a) ** 2 + (z - b) ** 2 >= (r + rr) ** 2 for (a, b, rr) in placed)

    def spot(s, u, r):
        F = Frame(A, s)
        x, z = F.cell(u, 0)
        if blocked_by_road(ctx, x, z) and abs(u) < 14:
            return None
        if not free(x, z, r):
            return None
        placed.append((x, z, r))
        return x, ground(x, z), z

    # ------------------------------------------------------------------ the donut gate and the zone sign
    F0 = Frame(A, s0 + 20)
    cx, cz = F0.cell(0, 0)
    donut(W, rng, (cx, F0.T + 14, cz), (F0.t[0], 0, F0.t[1]), R=22, r=5.5)
    gate(ctx, A, s0 + 6, "CANDY", scale=2, text_mat="n_pink", board_mat="b_black", frame_mat="cd_pink", trim_mat="n_white", tower_u=40,
         board_half=40, tower_h=34)

    # ------------------------------------------------------------------ gumdrop slalom + candy canes (first 260 blocks)
    for k, s in enumerate(np.arange(s0 + 50, s0 + 270, 17.0)):
        for side in (-1, 1):
            q = spot(s + side * 5, side * rng.uniform(15, 30), 7)
            if q:
                gumdrop(W, rng, q[0], q[1], q[2], int(rng.integers(5, 10)), CANDY[int(rng.integers(0, 8))])
        q = spot(s + 8, (-1 if k % 2 else 1) * 11.5, 2)
        if q:
            cane(W, q[0], q[1], q[2], int(rng.integers(8, 13)), hook_dir=(0, 1) if k % 2 else (0, -1), hook_r=4)

    # ------------------------------------------------------------------ lollipop forest
    for k, s in enumerate(np.arange(s0 + 260, s0 + 540, 11.0)):
        for side in (-1, 1):
            if rng.random() < 0.85:
                q = spot(s + side * 3, side * rng.uniform(13, 38), 6)
                if q:
                    r_ = int(rng.integers(5, 9))
                    c1, c2 = CANDY[int(rng.integers(0, 8))], "cd_white"
                    lollipop(W, rng, q[0], q[1], q[2], int(rng.integers(8, 17)), r_, "x" if rng.random() < 0.5 else "z", (c1, c2))

    # ------------------------------------------------------------------ the candy-cane tunnel (80 blocks of arches)
    for s in np.arange(s0 + 420, s0 + 505, 7.0):
        F = Frame(A, s)
        T = F.T
        for k in range(0, 91):
            a = math.pi * k / 90
            u = 10.8 * math.cos(a)
            h = 11.5 * math.sin(a)
            for dv in (0, 1):
                x, z = F.cell(int(round(u)), dv)
                W.put(x, T + 1 + int(round(h)), z, "cd_cane_r" if (k // 6 + int(s // 7)) % 2 == 0 else "cd_cane_w")
        for side in (-1, 1):
            for y in range(max(-1, T - 4), T + 2):
                x, z = F.cell(side * 11, 0)
                W.put(x, y, z, "cd_cane_r")
    for s in np.arange(s0 + 425, s0 + 500, 15.0):
        Fm = Frame(A, s)
        x, z = Fm.cell(0, 0)
        W.put(x, Fm.T + 12, z, "lit_pink")

    # ------------------------------------------------------------------ chocolate river: wafer bridge, fountains, marshmallows
    Tb = Fb.T
    for v in range(-24, 25):
        for u in range(-9, 10):
            x, z = Fb.cell(u, v)
            if abs(u) >= 7.5 and not W.has(x, Tb, z):
                W.put(x, Tb, z, "cd_wafer")
    for v in range(-24, 25, 4):
        for u in (-8, 8):
            x, z = Fb.cell(u, v)
            for y in range(-1, Tb):
                W.put(x, y, z, "cd_wafer_dark")
    gate_idx = 0
    for v in (-26, 26):
        for side in (-1, 1):
            q = Fb.cell(side * 24, v)
            choc_fountain(W, rng, q[0], -1, q[1])
    for k in range(18):
        u, v = rng.uniform(-80, 80), rng.uniform(-18, 18)
        if abs(u) < 14:
            continue
        x, z = Fb.cell(int(u), int(v))
        W.box((x, -1, z), (x + 1, 0, z + 1), "cd_marsh")
    # waffle-cone pier lamps on the bridge
    for v in range(-20, 21, 10):
        for side in (-1, 1):
            x, z = Fb.cell(side * 8, v)
            for y in range(Tb + 1, Tb + 5):
                W.put(x, y, z, "cd_cane_w")
            W.put(x, Tb + 5, z, "lit_pink")

    # ------------------------------------------------------------------ the layered cake in the big bend, ice-cream towers, cupcakes
    cake_s = m["cake"]
    Fc = Frame(A, cake_s - 28)
    ccx, ccz = Fc.cell(-62, 0)
    cake(W, rng, ccx, ccz, 0)
    placed.append((ccx, ccz, 40))
    for k, s in enumerate((s0 + 120, s0 + 330, s0 + 610, s0 + 820, s0 + 1010)):
        for side in (1, -1):
            q = spot(s, side * rng.uniform(32, 46), 14)
            if q:
                ice_cream(W, rng, q[0], q[1], q[2], h=int(rng.integers(22, 34)), r=float(rng.integers(6, 10)),
                          scoops=[("cd_pink", "cd_white", "choc"), ("cd_cyan", "cd_pink", "cd_yellow"), ("choc", "cd_white", "cd_pink")][k % 3])
                break
    for k in range(26):
        s = s0 + rng.uniform(40, L - 60)
        q = spot(s, (1 if rng.random() < 0.5 else -1) * rng.uniform(16, 46), 10)
        if q:
            cupcake(W, rng, q[0], q[1], q[2], int(rng.integers(5, 9)), CANDY[int(rng.integers(0, 8))], ["cd_pink", "cd_white", "cd_cyan", "cd_yellow"][int(rng.integers(0, 4))])
    for k in range(10):
        s = s0 + rng.uniform(80, L - 80)
        q = spot(s, (1 if k % 2 else -1) * rng.uniform(17, 30), 8)
        if q:
            gummy(W, q[0], q[1], q[2], rng.uniform(0.9, 1.3), JELLY[int(rng.integers(0, 6))], face=(0, 1))
    # sprinkles on the hills and tufts of cotton candy
    ix, iz = np.nonzero((d > 6) & (d < 56) & mask & (Ht >= 0) & ~land.rb)
    sel = rng.random(ix.size) < 0.035
    W.put_arr(g.x0 + ix[sel], Ht[ix[sel], iz[sel]] + 1, g.z0 + iz[sel], "cd_sprinkle")
    for k in range(30):
        s = s0 + rng.uniform(0, L)
        F = Frame(A, s)
        x, z = F.cell(rng.uniform(-70, 70), 0)
        cloud(W, rng, x, int(rng.integers(52, 86)), z, int(rng.integers(10, 20)), int(rng.integers(4, 8)), int(rng.integers(8, 14)), n=2,
              m="cd_cotton_pink" if k % 2 else "cd_cotton_blue")
    W.tick("set pieces")

    # ------------------------------------------------------------------ road dressing
    for frac in (0.07, 0.17, 0.28, 0.4, 0.5, 0.62, 0.74, 0.86, 0.94):
        boost_pad(W, A, s0 + frac * L, 14, 4, "n_pink" if int(frac * 10) % 2 else "n_cyan", count=3, spacing=4)
    for frac in (0.1, 0.22, 0.34, 0.46, 0.58, 0.7, 0.82, 0.92):
        item_boxes(W, A, s0 + frac * L, 7.0, n=4)
    corner_furniture(W, A, i0, i1, 7.0, rmax=110.0, hay=True, stacks=False, wall_w=0.0, skid=True)
    flood_posts(W, A, i0, i1, 70, 10.5, 6, glow="kp_glow_warm")
    billboards(W, A, i0 + 30, i1 - 30, 150, 13.0, start=1)
    W.tick("dressing")
