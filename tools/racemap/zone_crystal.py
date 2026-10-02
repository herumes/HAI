"""Zone 3: Crystal Caverns - a rocky tube mountain around the road, glowing crystals, stalactites, light shafts."""
import math

import numpy as np

from core import fbm, mat, smoothstep, vnoise
from kart import boost_pad, road_arrows
from kartprops import corner_furniture
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, item_boxes, lbox, zone_gate
from path import raster_band

mat("cr_rock", ["Rock_Slate_Cobble", "Rock_Slate_Cobble", "Rock_Shale_Cobble", "Rock_Basalt_Brick", "Rock_Slate_Brick", "Rock_Shale_Brick"])
mat("cr_rock_dark", ["Rock_Basalt_Brick", "Rock_Slate_Brick", "Rock_Basalt_Brick_Smooth"])
mat("cr_floor", ["Rock_Slate_Brick_Smooth", "Rock_Slate_Brick_Smooth", "Rock_Basalt_Brick_Smooth"])
mat("cr_deck", "Rock_Slate_Brick")
mat("cr_pier", "Rock_Slate_Brick_Pillar_Middle")
mat("cr_road", ["Build_Black_Cube", "Build_Black_Cube", "Build_Black_Cube", "Build_GreyDark_Cube"])
mat("cr_road_stair", "Build_Black_Stairs")
mat("cr_vein", ["Build_Lightsource_Cyan", "Build_Lightsource_Pink", "Build_Lightsource_Blue"])
for nm, key in [("Blue", "cr_blue"), ("Cyan", "cr_cyan"), ("Green", "cr_green"), ("Pink", "cr_pink"), ("Purple", "cr_purple"),
                ("White", "cr_white"), ("Yellow", "cr_yellow"), ("Red", "cr_red")]:
    mat(key, "Rock_Crystal_%s_Block" % nm)
mat("cr_any", ["Rock_Crystal_Blue_Block", "Rock_Crystal_Cyan_Block", "Rock_Crystal_Pink_Block", "Rock_Crystal_Purple_Block",
               "Rock_Crystal_Green_Block"])
mat("cr_small", ["Rock_Crystal_Blue_Small", "Rock_Crystal_Cyan_Small", "Rock_Crystal_Pink_Small", "Rock_Crystal_Purple_Small"])
mat("cr_medium", ["Rock_Crystal_Blue_Medium", "Rock_Crystal_Cyan_Medium", "Rock_Crystal_Pink_Medium", "Rock_Crystal_Purple_Medium"])
mat("cr_large", ["Rock_Crystal_Blue_Large", "Rock_Crystal_Cyan_Large", "Rock_Crystal_Pink_Large", "Rock_Crystal_Purple_Large"])
mat("stal_l", "Rock_Stone_Stalactite_Large")
mat("stal_s", "Rock_Stone_Stalactite_Small")
mat("cr_lamp", "Deco_Lantern_Ceiling")
mat("cr_iri", "Rock_Crystal_Iridescent_Large")


def spike(W, base, d, length, r0, m):
    d = np.array(d, float)
    d /= np.linalg.norm(d)
    for i in range(int(length)):
        c = np.array(base, float) + d * i
        rad = r0 * (1 - i / float(length)) + 0.35
        W.ellipsoid(int(round(c[0])), int(round(c[1])), int(round(c[2])), rad, rad, rad, m)


def cluster(W, rng, base, up, n=None, scale=1.0, cols=None):
    cols = cols or ["cr_blue", "cr_cyan", "cr_pink", "cr_purple", "cr_white", "cr_green"]
    n = n or int(rng.integers(3, 7))
    col = cols[int(rng.integers(0, len(cols)))]
    for _ in range(n):
        d = np.array(up, float) + rng.normal(0, 0.45, 3)
        ln = rng.uniform(4, 9) * scale
        r0 = rng.uniform(1.2, 2.2) * (0.6 + 0.4 * scale)
        spike(W, (base[0] + rng.normal(0, 1.2), base[1], base[2] + rng.normal(0, 1.2)), d, ln, r0, col if rng.random() < 0.75 else cols[int(rng.integers(0, len(cols)))])


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["crystal"], m["abyss"]
    L = s1 - s0
    i0, i1 = A.i(s0), A.i(s1)

    # ------------------------------------------------------------------ road (neon asphalt, deck + piers where it climbs)
    st = KartStyle(hw=7.0, verge="k_conc", edge=lambda s, u: "n_cyan" if int(s // 3) % 2 == 0 else "n_magenta",
                   rail_fn=lambda s, side: "mesh" if A.Tat(s) >= 2 else "armco", wall=("cr_rock_dark",), wall_h=2, deck="cr_deck",
                   piers=dict(mat="cr_pier", every=10, size=2, inset=1.5), lane_dash=(4.0, 4.0))
    stamp_kart_road(W, A, i0, i1, st)

    # ------------------------------------------------------------------ the tube
    tA, tB = A.i(s0 + 24), A.i(s1 - 14)
    idx = np.arange(tA, tB)
    P, R, T, S = A.p[idx], A.r[idx], A.T[idx], A.s[idx]
    th = np.linspace(0.0, math.pi, 110)
    IN_U, IN_H, THICK = 13.0, 15.0, 3
    nz = vnoise(S[:, None] / 7.0 + 0 * th[None, :], th[None, :] * 2.2 + 0 * S[:, None], 1.0, 17)
    nz2 = vnoise(S[:, None] / 2.7 + 0 * th[None, :], th[None, :] * 5.0 + 0 * S[:, None], 1.0, 29)
    a = 1.0 + 0.30 * (nz - 0.5) + 0.10 * (nz2 - 0.5)
    shell = []
    for k in range(THICK):
        u = (IN_U + k) * a * np.cos(th)[None, :]
        h = (IN_H + k) * a * np.sin(th)[None, :]
        x = P[:, 0:1] + R[:, 0:1] * u
        z = P[:, 1:2] + R[:, 1:2] * u
        y = T[:, None] + 1 + h
        shell.append((np.floor(x + 0.5).astype(np.int64), np.floor(y + 0.5).astype(np.int64), np.floor(z + 0.5).astype(np.int64)))
    X = np.concatenate([q[0].ravel() for q in shell])
    Y = np.concatenate([q[1].ravel() for q in shell])
    Z = np.concatenate([q[2].ravel() for q in shell])
    key = (X + 4096) | ((Z + 4096) << 14) | ((Y + 1024) << 28)
    _, first = np.unique(key, return_index=True)
    X, Y, Z = X[first], Y[first], Z[first]
    pick_rock = rng.random(X.size)
    W.put_arr(X[pick_rock < 0.88], Y[pick_rock < 0.88], Z[pick_rock < 0.88], "cr_rock")
    W.put_arr(X[pick_rock >= 0.88], Y[pick_rock >= 0.88], Z[pick_rock >= 0.88], "cr_rock_dark")
    # legs down to the ground and the cave floor beside the road
    leg = []
    for side in (-1, 1):
        for k in range(THICK):
            u = side * (IN_U + k) * a[:, 0 if side < 0 else -1]
            x = np.floor(P[:, 0] + R[:, 0] * u + 0.5).astype(np.int64)
            z = np.floor(P[:, 1] + R[:, 1] * u + 0.5).astype(np.int64)
            for y in range(0, int(T.max()) + 2):
                sel = y <= T + 1
                if sel.any():
                    W.put_arr(x[sel], np.full(sel.sum(), y), z[sel], "cr_rock" if y % 3 else "cr_rock_dark")
    # cave floor between road wall and tube wall, thin deck under it
    cx, cz, si, uu = raster_band(A, tA, tB, -IN_U + 0.5, IN_U - 0.5)
    TT = A.T[si]
    far = np.abs(uu) >= 9.6
    W.put_arr(cx[far], TT[far], cz[far], "cr_floor")
    deck = (TT >= 2) & far
    W.put_arr(cx[deck], TT[deck] - 1, cz[deck], "cr_deck")
    # mountain hummocks on top of the tube, and crystal spires growing out of the roof
    for k in range(tA, tB, 30):
        rr = rng.uniform(7, 12)
        side = rng.uniform(-9, 9)
        q = A.p[k] + A.r[k] * side
        W.ellipsoid(int(q[0]), int(A.T[k] + IN_H + 3), int(q[1]), rr, rr * 0.7, rr, "cr_rock", part="upper")
    for k in range(tA + 10, tB - 10, 48):
        side = rng.uniform(-6, 6)
        q = A.p[k] + A.r[k] * side
        top = int(A.T[k] + IN_H + THICK + 2)
        for _ in range(int(rng.integers(2, 5))):
            d = np.array([rng.normal(0, 0.35), 1.0, rng.normal(0, 0.35)])
            spike(W, (q[0] + rng.normal(0, 2), top, q[1] + rng.normal(0, 2)), d, rng.uniform(14, 34), rng.uniform(2.4, 4.4),
                  ["cr_blue", "cr_cyan", "cr_pink", "cr_purple", "cr_white"][int(rng.integers(0, 5))])

    # ------------------------------------------------------------------ inside: crystals, stalactites, veins, light shafts, lamps
    for k in range(tA + 6, tB - 6, 5):
        for side in (-1, 1):
            if rng.random() < 0.42:
                ang = rng.uniform(0.05, math.pi - 0.05)
                ui = IN_U * math.cos(ang) * 0.97 * float(a[k - tA, int(ang / math.pi * 109)])
                hi = IN_H * math.sin(ang) * 0.97 * float(a[k - tA, int(ang / math.pi * 109)])
                q = A.p[k] + A.r[k] * ui
                y = int(A.T[k] + 1 + hi)
                nrm = np.array([-math.cos(ang) * A.r[k][0], -math.sin(ang), -math.cos(ang) * A.r[k][1]])
                cluster(W, rng, (q[0], y, q[1]), nrm + np.array([0, 0.2, 0]), n=int(rng.integers(2, 5)), scale=rng.uniform(0.6, 1.3))
    # floor crystals along the cave floor edge
    for k in range(tA + 4, tB - 4, 9):
        for side in (-1, 1):
            q = A.p[k] + A.r[k] * side * rng.uniform(10.5, 12.8)
            cluster(W, rng, (q[0], int(A.T[k] + 1), q[1]), (0, 1, 0), n=int(rng.integers(2, 5)), scale=rng.uniform(0.5, 1.1))
    # stalactites
    for k in range(tA + 3, tB - 3, 3):
        if rng.random() < 0.7:
            ang = rng.uniform(0.25, math.pi - 0.25)
            ui = IN_U * math.cos(ang) * 0.92 * float(a[k - tA, int(ang / math.pi * 109)])
            hi = IN_H * math.sin(ang) * 0.92 * float(a[k - tA, int(ang / math.pi * 109)])
            q = A.p[k] + A.r[k] * ui
            x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
            y = int(A.T[k] + 1 + hi)
            ln = int(rng.integers(2, 6))
            for t in range(ln):
                W.put(x, y - t, z, "cr_rock_dark" if t < ln - 1 else "stal_l")
    # glowing veins
    for k in range(tA, tB, 2):
        if rng.random() < 0.3:
            ang = rng.uniform(0.1, math.pi - 0.1)
            ui = (IN_U) * math.cos(ang) * float(a[k - tA, int(ang / math.pi * 109)])
            hi = (IN_H) * math.sin(ang) * float(a[k - tA, int(ang / math.pi * 109)])
            q = A.p[k] + A.r[k] * ui
            for t in range(int(rng.integers(3, 10))):
                ang2 = ang + t * 0.03
                ui2 = IN_U * math.cos(ang2) * float(a[k - tA, min(109, int(ang2 / math.pi * 109))])
                hi2 = IN_H * math.sin(ang2) * float(a[k - tA, min(109, int(ang2 / math.pi * 109))])
                q2 = A.p[k] + A.r[k] * ui2
                W.put(int(math.floor(q2[0] + 0.5)), int(A.T[k] + 1 + hi2), int(math.floor(q2[1] + 0.5)), "cr_vein")
    # light shafts through the roof
    for frac in (0.12, 0.27, 0.44, 0.6, 0.78, 0.9):
        k = A.i(s0 + frac * L)
        q = A.p[k]
        ytop = int(A.T[k] + IN_H + THICK + 2)
        for dx in range(-3, 4):
            for dz in range(-3, 4):
                if dx * dx + dz * dz <= 9:
                    for y in range(int(A.T[k] + IN_H - 4), ytop + 2):
                        W.erase(int(q[0]) + dx, y, int(q[1]) + dz)
        for y in range(int(A.T[k] + 6), ytop + 34):
            W.put(int(q[0]), y, int(q[1]), "lit_white")
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for y in range(ytop - 2, ytop + 16):
                W.put(int(q[0]) + dx, y, int(q[1]) + dz, "lit_cyan")
    # hanging lanterns
    for k in range(tA + 8, tB - 8, 18):
        ui = 0.0
        q = A.p[k]
        x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
        ytop = int(A.T[k] + 1 + IN_H * 0.97)
        W.put(x, ytop, z, "cr_rock_dark")
        W.put(x, ytop - 1, z, "cr_lamp")

    # ------------------------------------------------------------------ portals + road dressing
    zone_gate(W, A, s0 + 10, "CRYSTAL", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="cr_white", trim_mat="n_magenta",
              tower_u=46, board_half=46, tower_h=30)
    for frac in (0.08, 0.2, 0.32, 0.46, 0.6, 0.72, 0.85):
        boost_pad(W, A, s0 + 24 + frac * (L - 40), 14, 4, "n_magenta", count=3, spacing=4)
    for frac in (0.14, 0.3, 0.42, 0.56, 0.7, 0.82):
        item_boxes(W, A, s0 + 24 + frac * (L - 40), 7.0, n=4)
    for s in np.arange(s0 + 60, s1 - 30, 38.0):
        road_arrows(W, A, s, n=1, u=-3.5)
        road_arrows(W, A, s, n=1, u=3.5)
    corner_furniture(W, A, A.i(s0 + 30), A.i(s1 - 20), 7.0, rmax=100.0, stacks=False, dy=1, skid=True)
