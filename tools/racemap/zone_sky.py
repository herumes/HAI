"""Zone 6: the Sky Course (plane only) - launch tube, floating islands, World Tree helix, crystal slalom, aurora tunnel, dive.

The flight is ctx.S (2D path) + S.alt.  Nothing here is a road: the plane flies through ring gates, guided by light trails.
"""
import math

import numpy as np

from core import fbm, mat, smoothstep
from kit import Frame, cloud, hoop, lbox, lsign, oak
from font import text_cells
from terrain import floating_skin

mat("sk_grass", ["Soil_Grass_Full", "Soil_Grass_Full", "Soil_Grass_Sunny_Full"])
mat("sk_dirt", "Soil_Dirt")
mat("sk_rock", ["Rock_Stone_Cobble", "Rock_Stone_Cobble_Mossy", "Rock_Shale_Cobble", "Rock_Stone_Cobble"])
mat("sk_rock_dark", ["Rock_Slate_Cobble", "Rock_Shale_Cobble"])
mat("tree_trunk", ["Wood_Wisteria_Wild_Trunk"])
mat("tree_leaf", ["Plant_Leaves_Wisteria_Wild", "Plant_Leaves_Wisteria_Wild", "Plant_Leaves_Wisteria_Wild", "Plant_Leaves_Fig_Blue"])
mat("tree_fruit", "Build_Lightsource_YellowLight")
mat("tree_root", "Wood_Wisteria_Wild_Roots")
mat("sky_fall", ["Build_Lightsource_Cyan", "Cloth_Block_Wool_Cyan_Light", "Cloth_Block_Wool_White"])
mat("envelope_a", "Cloth_Block_Wool_Red")
mat("envelope_b", "Cloth_Block_Wool_White")
mat("envelope_c", "Cloth_Block_Wool_Blue")
mat("gondola", "Wood_Darkwood_Planks")
mat("gondola_roof", "Wood_Darkwood_Roof")
mat("ring_a", "NoCube_Neon_Block_Cyan")
mat("ring_b", "NoCube_Neon_Block_Magenta")
mat("ring_c", "NoCube_Neon_Block_Yellow")
mat("ring_d", "NoCube_Neon_Block_Lime")
mat("ring_e", "NoCube_Neon_Block_Orange")
mat("ring_f", "NoCube_Neon_Block_Light_Blue")
mat("trail", "Build_Lightsource_White")
mat("shard", ["Rock_Crystal_Blue_Block", "Rock_Crystal_Cyan_Block", "Rock_Crystal_Pink_Block", "Rock_Crystal_Purple_Block"])
mat("shard_core", "Build_Lightsource_White")
RINGS = ["ring_a", "ring_b", "ring_c", "ring_d", "ring_e", "ring_f"]


def P3(S, s):
    i = S.i(s)
    return np.array([S.p[i, 0], S.alt[i], S.p[i, 1]])


def fdir(S, s):
    i = S.i(s)
    j = min(len(S.s) - 1, i + 6)
    k = max(0, i - 6)
    d = np.array([S.p[j, 0] - S.p[k, 0], S.alt[j] - S.alt[k], S.p[j, 1] - S.p[k, 1]])
    return d / np.linalg.norm(d)


class Clear:
    """Distance to the flight line, so scenery never blocks it."""

    def __init__(self, S):
        self.pts = np.stack([S.p[::4, 0], S.alt[::4], S.p[::4, 1]], 1)

    def __call__(self, x, y, z):
        return float(np.sqrt(((self.pts - np.array([x, y, z])) ** 2).sum(1)).min())


def island(W, rng, cx, cy, cz, R, depth, seed, trees=True, tower=False):
    n = int(R * 1.45) + 4
    dx, dz = np.meshgrid(np.arange(-n, n + 1), np.arange(-n, n + 1), indexing="ij")
    X, Z = cx + dx, cz + dz
    r = np.hypot(dx, dz) / R
    a = 1.0 + 0.45 * (fbm(X.astype(float), Z.astype(float), R * 0.9, 3, seed) - 0.5)
    rr = r / a
    inside = rr < 1.0
    hump = 3.5 * np.clip(1 - rr * rr, 0, 1) + 2.2 * (fbm(X.astype(float), Z.astype(float), 7.0, 2, seed + 7) - 0.5)
    top = np.where(inside, cy + np.rint(hump).astype(np.int64), -9999)
    under = depth * (1 - np.clip(rr, 0, 1) ** 1.3) * (0.75 + 0.5 * fbm(X.astype(float), Z.astype(float), 9.0, 2, seed + 13))
    bot = np.where(inside, top - 3 - np.rint(under).astype(np.int64), 9999)
    floating_skin(W, cx - n, cz - n, top, bot, "sk_grass", "sk_dirt", "sk_rock", top_layers=3, sub_mat="sk_rock_dark")
    if trees:
        ix, iz = np.nonzero(inside & (rr < 0.82))
        if ix.size:
            sel = rng.choice(ix.size, size=max(2, int(R * 0.7)), replace=False)
            for k in sel:
                x, z = cx - n + int(ix[k]), cz - n + int(iz[k])
                oak(W, rng, x, int(top[ix[k], iz[k]]) + 1, z, h=int(rng.integers(4, 8)))
        sel = rng.random(ix.size) < 0.07
        W.put_arr((cx - n + ix[sel]), top[ix[sel], iz[sel]] + 1, (cz - n + iz[sel]), "flowers")
    # a glowing waterfall off one edge
    a0 = rng.uniform(0, 2 * math.pi)
    ex, ez = int(cx + math.cos(a0) * R * 0.95), int(cz + math.sin(a0) * R * 0.95)
    y0 = cy + 1
    for k in range(int(depth * 0.9)):
        for dxx in (0, 1):
            W.put(ex + dxx, y0 - k, ez, "sky_fall")
    return n, top, rr


def airship(W, rng, cx, cy, cz, L=46, R=9, along="x"):
    cols = ["envelope_a", "envelope_b", "envelope_c", "envelope_b"]
    rx, ry = (L / 2.0, R) if along == "x" else (R, R)
    rz = R if along == "x" else L / 2.0
    Rx, Ry, Rz = int(rx) + 2, int(ry) + 2, int(rz) + 2
    ox, oy, oz = np.meshgrid(np.arange(-Rx, Rx + 1), np.arange(-Ry, Ry + 1), np.arange(-Rz, Rz + 1), indexing="ij")
    nrm = (ox / (rx + 0.5)) ** 2 + (oy / (ry + 0.5)) ** 2 + (oz / (rz + 0.5)) ** 2
    inn = (ox / max(0.5, rx - 0.8)) ** 2 + (oy / max(0.5, ry - 0.8)) ** 2 + (oz / max(0.5, rz - 0.8)) ** 2
    sel = (nrm <= 1) & (inn > 1)
    ox, oy, oz = ox[sel], oy[sel], oz[sel]
    stripe = ((ox if along == "x" else oz) + 200) // 5 % 4
    for k in range(4):
        q = stripe == k
        W.put_arr(cx + ox[q], cy + oy[q], cz + oz[q], cols[k])
    # gondola, fins, propeller
    if along == "x":
        W.box((cx - 6, cy - ry - 4, cz - 2), (cx + 6, cy - ry - 1, cz + 2), "gondola")
        W.box((cx - 7, cy - ry, cz - 2), (cx + 7, cy - ry, cz + 2), "gondola_roof")
        for k in range(-5, 6, 2):
            W.put(cx + k, cy - ry - 2, cz - 3, "lit_yellowlight")
        for dy in (-1, 1):
            W.box((int(cx - rx - 4), cy + dy * 3, cz - 1), (int(cx - rx), cy + dy * 7, cz + 1), "envelope_a")
        W.box((int(cx - rx - 4), cy - 7, cz), (int(cx - rx), cy + 7, cz), "envelope_a")
        px = int(cx + rx + 2)
        for k in range(-3, 4):
            W.put(px, cy + k, cz, "b_greydark")
            W.put(px, cy, cz + k, "b_greydark")
    else:
        W.box((cx - 2, cy - ry - 4, cz - 6), (cx + 2, cy - ry - 1, cz + 6), "gondola")
        W.box((cx - 2, cy - ry, cz - 7), (cx + 2, cy - ry, cz + 7), "gondola_roof")
        for k in range(-5, 6, 2):
            W.put(cx - 3, cy - ry - 2, cz + k, "lit_yellowlight")
        W.box((cx, cy - 7, int(cz - rz - 4)), (cx, cy + 7, int(cz - rz)), "envelope_a")
        W.box((cx - 7, cy, int(cz - rz - 4)), (cx + 7, cy, int(cz - rz)), "envelope_a")


def world_tree(W, rng, cx, cz, top_y=196):
    # roots
    for k in range(7):
        a = k * 2 * math.pi / 7 + 0.3
        for t in range(0, 30):
            rr = 7 + t * 0.85
            y = int(10 - t * 0.36)
            if y < 0:
                break
            x, z = int(cx + rr * math.cos(a)), int(cz + rr * math.sin(a))
            W.box((x, y, z), (x + 1, y + 1, z + 1), "tree_root" if t > 6 else "tree_trunk")
    # trunk
    for y in range(-1, top_y):
        r = 9.0 - 4.2 * (y / top_y) + 0.9 * math.sin(y * 0.17)
        W.cyl(cx, cz, y, y, r, "tree_trunk", mode="wall")
    for y in range(-1, 6):
        W.cyl(cx, cz, y, y, 9.5, "tree_trunk", mode="fill")
    # branches with leaf puffs
    ang = 0.0
    for y in range(92, 182, 9):
        ang += 2.4
        ln = 26 + 14 * math.sin(y * 0.2)
        ln = min(ln, 36)
        r0 = 8.0 - 4.2 * (y / top_y)
        ex, ez = cx + (r0 + ln) * math.cos(ang), cz + (r0 + ln) * math.sin(ang)
        ey = y + 6
        W.line((cx + r0 * math.cos(ang), y, cz + r0 * math.sin(ang)), (ex, ey, ez), "tree_trunk", rad=1.3)
        W.ellipsoid(int(ex), ey + 1, int(ez), 8, 5, 8, "tree_leaf", mode="shell")
        for _ in range(int(rng.integers(3, 7))):
            fx, fz = ex + rng.normal(0, 4), ez + rng.normal(0, 4)
            for t in range(int(rng.integers(3, 8))):
                W.put(int(fx), ey - 3 - t, int(fz), "tree_fruit" if t == 0 else "tree_leaf")
            W.put(int(fx), ey - 4 - int(rng.integers(3, 8)), int(fz), "tree_fruit")
    # canopy
    for (dx, dz, dy, rr) in ((0, 0, 0, 30), (16, 4, -6, 19), (-16, 6, -5, 20), (4, -17, -5, 19), (3, 18, -7, 18), (-10, -12, 6, 16), (12, 12, 7, 14)):
        W.ellipsoid(cx + dx, top_y + dy, cz + dz, rr, rr * 0.55, rr, "tree_leaf", mode="shell")
    for _ in range(90):
        a = rng.uniform(0, 2 * math.pi)
        rr = rng.uniform(4, 27)
        W.put(int(cx + rr * math.cos(a)), int(top_y - 13 + rng.uniform(-3, 4)), int(cz + rr * math.sin(a)), "tree_fruit")


def shard(W, rng, cx, cy, cz, h, r, mat_="shard"):
    W.ellipsoid(cx, cy, cz, r, h, r, mat_, mode="shell")
    W.box((cx, cy - int(h * 0.6), cz), (cx, cy + int(h * 0.6), cz), "shard_core")


def build(ctx):
    W, S, rng = ctx.W, ctx.S, ctx.rng
    m = S.marks
    L = S.length
    clear = Clear(S)
    tick = W.tick

    # ------------------------------------------------------------------ ring gates along the whole flight
    rings = 0
    for k, s in enumerate(np.arange(30.0, L - 30.0, 33.0)):
        sec = "launch"
        r = 11.0
        if s > m["descent"]:
            r = 10.5 - 3.5 * (s - m["descent"]) / (L - m["descent"])
        elif s > m["tunnel"]:
            r = 12.5
        elif s > m["slalom"]:
            r = 9.5
        elif s > m["helix"]:
            r = 11.5
        c = P3(S, s)
        d = fdir(S, s)
        hoop(W, c, d, r, RINGS[k % 6], thick=2)
        if k % 4 == 0:
            hoop(W, c, d, r + 2.6, "trail", thick=1)
        rings += 1
    for s in np.arange(6.0, L - 6.0, 7.0):
        c = P3(S, s)
        W.put(int(round(c[0])), int(round(c[1])) - 8, int(round(c[2])), "trail")
    tick("rings+trail")

    # ------------------------------------------------------------------ launch tube (accelerating rings)
    for k in range(14):
        s = 4 + k * 3.2
        c = P3(S, s)
        d = fdir(S, s)
        hoop(W, c, d, 12.5 - k * 0.12, ["ring_c", "ring_e"][k % 2], thick=1)
    # floating 'SKY' sign over the launch
    F = Frame(S, 30.0)
    cx, cz = F.cell(0, 0)
    y_sign_top = 100 + 45
    w, h, cells = text_cells("SKY", 6)
    u0 = -w // 2
    for (c_, r_) in cells:
        x, z = F.cell(u0 + c_, 0)
        W.put(x, y_sign_top - r_, z, "n_white")
    for u in range(u0 - 4, u0 + w + 4):
        for y in range(y_sign_top - h - 3, y_sign_top + 4):
            x, z = F.cell(u, 1)
            W.put(x, y, z, "b_black")
        x, z = F.cell(u, 0)
        W.put(x, y_sign_top + 4, z, "n_cyan")
        W.put(x, y_sign_top - h - 3, z, "n_magenta")
    for u in (u0 - 4, u0 + w + 3):
        lbox(W, F, u - 1, u + 1, 0, 2, 30, y_sign_top + 4, "b_grey")
    tick("launch")

    # ------------------------------------------------------------------ floating islands around the first half
    isl = 0
    specs = []
    for s in np.arange(L * 0.0 + 150, m["helix"] - 40, 62.0):
        side = 1 if isl % 2 == 0 else -1
        c = P3(S, s)
        F = Frame(S, s)
        lat = side * rng.uniform(44, 62)
        q = S.p[S.i(s)] + S.r[S.i(s)] * lat
        cy = int(c[1] + rng.uniform(-22, 6))
        R = rng.uniform(17, 27)
        if clear(q[0], cy, q[1]) < R + 14:
            continue
        specs.append((int(q[0]), cy, int(q[1]), R))
        isl += 1
    for k, (x, y, z, R) in enumerate(specs):
        island(W, rng, x, y, z, R, depth=int(R * 1.0), seed=70 + k)
    print("  sky islands:", len(specs))
    tick("islands")
    # the ring-shaped island the plane flies through (near the middle of the archipelago)
    s_t = m["islands"] + 250
    c = P3(S, s_t)
    d = fdir(S, s_t)
    # torus island: a ring of rock and grass perpendicular to the flight line
    a = np.cross(d, np.array([0, 1.0, 0]))
    a /= np.linalg.norm(a)
    b = np.cross(d, a)
    Rm, rt = 25.0, 8.0
    for th in np.linspace(0, 2 * math.pi, 420, endpoint=False):
        ctr = c + Rm * (math.cos(th) * a + math.sin(th) * b)
        for ph in np.linspace(0, 2 * math.pi, 26, endpoint=False):
            nrm = math.cos(ph) * (math.cos(th) * a + math.sin(th) * b) + math.sin(ph) * d
            p = ctr + rt * nrm
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sk_rock" if ph < 4.2 else "sk_grass")
    tick("torus")

    # ------------------------------------------------------------------ clouds under the flight
    for k in range(11):
        x = rng.uniform(-40, 600)
        z = rng.uniform(210, 640)
        y = rng.uniform(30, 64)
        cloud(W, rng, int(x), int(y), int(z), int(rng.integers(14, 26)), int(rng.integers(4, 8)), int(rng.integers(10, 18)), n=2)
    tick("clouds")

    # ------------------------------------------------------------------ the World Tree
    cx, cz = int(ctx.HC[0]), int(ctx.HC[1])
    world_tree(W, rng, cx, cz, top_y=198)
    tick("tree")

    # ------------------------------------------------------------------ crystal slalom: floating shards either side
    s = m["slalom"] + 8
    k = 0
    while s < m["tunnel"] - 12:
        side = 1 if k % 2 == 0 else -1
        c = P3(S, s)
        F = Frame(S, s)
        q = S.p[S.i(s)] + S.r[S.i(s)] * side * 17
        shard(W, rng, int(q[0]), int(c[1] + rng.integers(-6, 7)), int(q[1]), int(rng.integers(26, 38)), int(rng.integers(4, 7)),
              ["shard"][0])
        s += 26
        k += 1
    tick("slalom")

    # ------------------------------------------------------------------ aurora tunnel (frames every 4 blocks + rails)
    t0, t1 = m["tunnel"] + 40, m["descent"] - 30
    n = 0
    for s in np.arange(t0, t1, 4.0):
        c = P3(S, s)
        d = fdir(S, s)
        hoop(W, c, d, 14.0, RINGS[(n // 2) % 6], thick=1)
        n += 1
    for k in range(8):
        a = k * math.pi / 4
        pts = []
        for s in np.arange(t0, t1, 3.0):
            c = P3(S, s)
            d = fdir(S, s)
            u = np.cross(d, np.array([0, 1.0, 0]))
            u /= np.linalg.norm(u)
            v = np.cross(d, u)
            p = c + 14.0 * (math.cos(a) * u + math.sin(a) * v)
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "trail")
    tick("tunnel")

    # ------------------------------------------------------------------ airships + high islands beside the dive
    for (s, lat, along, dy) in ((m["islands"] + 120, 70, "x", 6), (m["tunnel"] + 90, -74, "z", 4)):
        c = P3(S, s)
        q = S.p[S.i(s)] + S.r[S.i(s)] * lat
        airship(W, rng, int(q[0]), int(c[1] + dy), int(q[1]), along=along)
    for k in range(3):
        s = m["descent"] + 30 + k * 70
        c = P3(S, s)
        q = S.p[S.i(s)] + S.r[S.i(s)] * (55 if k % 2 == 0 else -55)
        cy = int(c[1] - 10)
        if clear(q[0], cy, q[1]) > 40:
            island(W, rng, int(q[0]), cy, int(q[1]), 14, 16, 120 + k, trees=False)
    tick("airships")

    # ------------------------------------------------------------------ transform gates: launch and landing
    for (s, cols) in ((20.0, ("n_yellow", "n_white")), (L - 120.0, ("n_cyan", "n_white"))):
        c = P3(S, s)
        d = fdir(S, s)
        hoop(W, c, d, 22, cols[0], thick=3)
        hoop(W, c, d, 25.5, cols[1], thick=2)
        hoop(W, c, d, 19, cols[1], thick=1)
    # runway approach lights on the descent
    for s in np.arange(L - 200, L - 4, 10.0):
        c = P3(S, s)
        F = Frame(S, s)
        for side in (-1, 1):
            q = S.p[S.i(s)] + S.r[S.i(s)] * side * 16
            W.put(int(q[0]), int(round(c[1])) - 6, int(q[1]), "lit_white")
    print("  sky rings:", rings)
