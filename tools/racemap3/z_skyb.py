"""Plane zone B - Cosmic Gateway: launch from the glacier summit into space: a ring station, a cratered moon, a long asteroid
slalom, a banded gas giant with a ring gap to fly through, a black hole with an accretion disk, then the wormhole tunnel that
spits the plane out above the Celestial Ribbon landing strip.  The flight is ctx.SB (2D path) + SB.alt."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_rainbow import PLANETS, planet  # noqa: F401

mat("sb_moon", ["Rock_Chalk", "Rock_Calcite", "Rock_Chalk", "Rock_Slate_Cobble"])
mat("sb_moon_d", ["Rock_Slate", "Rock_Shale", "Rock_Basalt"])
mat("sb_ast", ["Rock_Basalt", "Rock_Volcanic", "Rock_Slate", "Rock_Basalt_Cobble", "Rock_Stone_Cobble"])
mat("sb_vein", ["Build_Lightsource_Cyan", "Build_Lightsource_Pink", "Build_Lightsource_Orange"])
mat("sb_ringdust", ["Soil_Clay_Smooth_Yellow2", "Soil_Clay_Smooth_Orange", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Yellow2", "Soil_Clay_Smooth_Pink"])
mat("sb_hole", "Build_Black_Cube")
mat("sb_disk_a", ["NoCube_Neon_Block_Orange", "NoCube_Neon_Block_Yellow", "NoCube_Neon_Block_Fire_Red"])
mat("sb_disk_b", ["NoCube_Neon_Block_White", "NoCube_Neon_Block_Light_Blue", "NoCube_Neon_Block_Magenta"])
mat("sb_jet", ["NoCube_Neon_Block_White", "NoCube_Neon_Block_Cyan"])
mat("sb_hull", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("sb_hull_d", "Metal_Iron_Smooth")
mat("sb_hull_z", "Metal_Zinc_Smooth")
mat("sb_panel", "Soil_Clay_Smooth_Blue")
mat("sb_star", "Build_Lightsource_White")
mat("sb_star_y", "Build_Lightsource_YellowLight")
mat("sb_star_c", "Build_Lightsource_Cyan")
mat("sb_star_p", "Build_Lightsource_Pink")
mat("trail", "Build_Lightsource_White")
SB_RINGS = ["n_cyan", "n_magenta", "n_yellow", "n_lime", "n_orange", "n_lightblue", "n_purple", "n_pink"]


def moon(W, rng, c, R, craters=14):
    """A cratered moon (hollow shell): the radius of the shell is dented by crater bowls with raised rims."""
    cx, cy, cz = c
    n = int(R) + 6
    ox, oy, oz = np.meshgrid(np.arange(-n, n + 1), np.arange(-n, n + 1), np.arange(-n, n + 1), indexing="ij")
    rho = np.sqrt(ox * ox + oy * oy + oz * oz) + 1e-6
    dirs = np.stack([ox, oy, oz], -1) / rho[..., None]
    reff = np.full(rho.shape, float(R))
    cen = []
    for k in range(craters):
        v = rng.normal(size=3)
        v /= np.linalg.norm(v)
        ar = rng.uniform(0.16, 0.42)
        depth = rng.uniform(3.0, 7.0)
        ang = np.arccos(np.clip(dirs @ v, -1, 1))
        bowl = np.where(ang < ar, depth * (1 - (ang / ar) ** 2), 0.0)
        rim = np.where((ang >= ar) & (ang < ar * 1.22), 1.6 * (1 - (ang - ar) / (ar * 0.22)), 0.0)
        reff = reff - bowl + rim
        cen.append((v, ar))
    shell = (rho <= reff + 0.5) & (rho > reff - 1.6)
    low = shell & (rho < R - 1.0)
    X, Y, Z = cx + ox, cy + oy, cz + oz
    W.put_arr(X[shell & ~low], Y[shell & ~low], Z[shell & ~low], "sb_moon")
    W.put_arr(X[low], Y[low], Z[low], "sb_moon_d")
    # a flag on the rim, and a little lander light
    v, ar = cen[0]
    top = np.array([cx, cy, cz]) + v * (R + 2)
    for k in range(6):
        W.put(int(top[0]), int(top[1] + k), int(top[2]), "sb_hull_d")
    W.put(int(top[0]) + 1, int(top[1] + 5), int(top[2]), "n_red")
    W.put(int(top[0]), int(top[1] + 6), int(top[2]), "n_white")


def asteroid(W, rng, x, y, z, r):
    rx, ry, rz = r * rng.uniform(0.8, 1.3), r * rng.uniform(0.7, 1.1), r * rng.uniform(0.8, 1.3)
    mode = "shell" if r > 4 else "fill"
    W.ellipsoid(x, y, z, rx, ry, rz, "sb_ast", mode=mode)
    for _ in range(int(rng.integers(1, 3))):
        W.ellipsoid(x + int(rng.integers(-r, r + 1)), y + int(rng.integers(-r // 2, r // 2 + 1)), z + int(rng.integers(-r, r + 1)), r * 0.45, r * 0.4, r * 0.45, "sb_ast", mode="fill")
    if r >= 4 and rng.random() < 0.7:
        a = rng.uniform(0, 2 * math.pi)
        for t in range(int(r * 1.2)):
            W.put(int(x + (rx + 0.2) * math.cos(a) * math.cos(t * 0.3)), int(y + t - r // 2), int(z + (rz + 0.2) * math.sin(a) * math.cos(t * 0.3)), "sb_vein")


def ring_station(W, rng, c, R, normal):
    """A big ring hull the plane flies through, with spokes to a hub, solar wings and beacons."""
    n = np.array(normal, float)
    n /= np.linalg.norm(n)
    torus(W, c, n, R, 3.4, "sb_hull")
    torus(W, c, n, R, 1.2, "sb_hull_d")
    up = np.array([0, 1.0, 0]) if abs(n[1]) < 0.95 else np.array([1.0, 0, 0])
    a = np.cross(n, up)
    a /= np.linalg.norm(a)
    b = np.cross(n, a)
    for k in range(6):
        th = k * math.pi / 3
        d = math.cos(th) * a + math.sin(th) * b
        # spokes stop short of the centre so the flight line stays clear
        for t in np.arange(R * 0.75, R - 3, 0.7):
            p = np.array(c) + d * t
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_hull_z")
        for t in np.arange(R + 3, R + 20, 1.0):
            for w_ in range(-4, 5):
                p = np.array(c) + d * t + n * w_ * 0.9
                W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_panel")
        p = np.array(c) + d * (R + 1)
        W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), 2, 2, 2, "lit_cyan")
    for k in range(24):
        th = k * 2 * math.pi / 24
        p = np.array(c) + (R) * (math.cos(th) * a + math.sin(th) * b) + n * 4
        W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "n_cyan" if k % 2 else "n_white")


def black_hole(W, rng, c, R, tilt, clear):
    cx, cy, cz = c
    W.ellipsoid(cx, cy, cz, R, R, R, "sb_hole")
    t = np.array(tilt, float)
    t /= np.linalg.norm(t)
    a = np.cross(t, np.array([0, 0, 1.0]))
    a /= np.linalg.norm(a)
    b = np.cross(t, a)
    n = 0
    for arm in range(3):
        for u in np.linspace(0, 1, 420):
            rr = R * 1.8 + 34 * u
            th = arm * 2 * math.pi / 3 + 5.6 * u
            for wid in (-1, 0, 1):
                p = np.array(c) + (rr + wid * 0.9) * (math.cos(th) * a + math.sin(th) * b) + t * rng.normal(0, 0.5)
                if clear(p[0], p[1], p[2]) < 13.0:
                    continue
                W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_disk_a" if u < 0.55 else "sb_disk_b", keep=True)
                n += 1
    for u in np.linspace(R + 1.5, R + 2.5, 3):
        for th in np.linspace(0, 2 * math.pi, 120, endpoint=False):
            p = np.array(c) + u * (math.cos(th) * a + math.sin(th) * b)
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_disk_a", keep=True)
    for sgn in (-1, 1):
        for k in range(int(R + 2), int(R + 46)):
            for dxx, dzz in ((0, 0), (1, 0)):
                p = np.array(c) + t * sgn * k
                W.put(int(round(p[0])) + dxx, int(round(p[1])), int(round(p[2])) + dzz, "sb_jet")
    print("  black hole disk blocks:", n)


def build(ctx):
    W, S, rng = ctx.W, ctx.SB, ctx.rng
    L = S.length
    m = S.marks
    clear = Clear(S, step=3)
    tick = W.tick
    s_moon, s_belt, s_giant, s_hole, s_worm, s_app = m["moon"], m["belt"], m["giant"], m["hole"], m["wormhole"], m["approach"]

    # ------------------------------------------------------------------ rings along the whole flight
    n_r = 0
    for s in np.arange(36.0, s_worm - 10, 36.0):
        c, f, rr, u = frame3(S, s)
        r = 11.5 if s < s_belt - 220 else 10.0 if s < s_giant else 11.0
        hoop(W, c, f, r, SB_RINGS[n_r % len(SB_RINGS)], thick=2)
        if n_r % 4 == 0:
            hoop(W, c, f, r + 2.6, "trail", thick=1)
        n_r += 1
    for s in np.arange(6.0, L - 6.0, 7.0):
        c = P3(S, s)
        if clear(c[0], c[1] - 7, c[2]) >= 7.5:
            W.put(int(round(c[0])), int(round(c[1])) - 7, int(round(c[2])), "trail")
    print("  sky-B rings:", n_r)
    tick("rings")

    # ------------------------------------------------------------------ launch tunnel, transform arch, the ring station
    for k in range(14):
        c, f, rr, u = frame3(S, 6 + k * 3.2)
        hoop(W, c, f, 12.8 - k * 0.1, ["n_cyan", "n_white"][k % 2], thick=1)
    for k, r in enumerate((18, 20.5, 23)):
        c, f, rr, u = frame3(S, 62 + k * 6)
        hoop(W, c, f, r, ["n_white", "n_yellow", "n_cyan"][k], thick=2)
    c, f, rr, u = frame3(S, 330.0)
    ring_station(W, rng, tuple(int(round(v)) for v in c), 38.0, f)
    tick("station")

    # ------------------------------------------------------------------ the moon (beside the line on the first turn)
    c, f, rr, u = frame3(S, s_moon + 20)
    side = -1 if np.cross(f, rr)[1] > 0 else 1
    R_m = 44
    q = c + rr * 1.0 * (R_m + 30) + np.array([0, -4.0, 0])
    q2 = c - rr * 1.0 * (R_m + 30)
    # pick the side with the centre of the turn (inside of the bend is the far side of the circle)
    for cand in (q, q2):
        if clear(cand[0], cand[1], cand[2]) > R_m + 24:
            moon(W, rng, tuple(int(round(v)) for v in cand), R_m)
            print("  moon at", cand)
            break
    tick("moon")

    # ------------------------------------------------------------------ the asteroid belt: a long slalom with neon gates
    placed = []
    sa0, sa1 = s_belt - 190, s_belt + 260
    n_a = 0
    for attempt in range(1500):
        s = rng.uniform(sa0, sa1)
        c, f, rr, u = frame3(S, s)
        ang = rng.uniform(0, 2 * math.pi)
        d_ = rng.uniform(18, 78)
        p = c + d_ * (math.cos(ang) * rr + 0.62 * math.sin(ang) * u) + f * rng.uniform(-5, 5)
        r = int(rng.choice([2, 2, 3, 3, 4, 5, 6, 8, 10], p=[0.17, 0.17, 0.15, 0.15, 0.12, 0.1, 0.07, 0.05, 0.02]))
        if clear(p[0], p[1], p[2]) < r + 14:
            continue
        if any(math.dist((p[0], p[1], p[2]), (a_, b_, c_)) < r + rr_ + 3 for (a_, b_, c_, rr_) in placed):
            continue
        placed.append((p[0], p[1], p[2], r))
        asteroid(W, rng, int(p[0]), int(p[1]), int(p[2]), r)
        n_a += 1
        if n_a >= 130:
            break
    print("  asteroids:", n_a)
    for s in np.arange(sa0 + 20, sa1, 40.0):
        c, f, rr, u = frame3(S, s)
        hoop(W, c, f, 8.5, "n_white", thick=1)
    tick("belt")

    # ------------------------------------------------------------------ the gas giant with a ring gap to fly through
    c, f, rr, u = frame3(S, s_giant)
    Rg = 46
    cen = c + rr * (Rg + 56)
    if clear(cen[0], cen[1], cen[2]) < Rg + 30:
        cen = c - rr * (Rg + 56)
    cx_, cy_, cz_ = int(cen[0]), int(cen[1]), int(cen[2])
    planet(W, rng, cx_, cy_, cz_, Rg, PLANETS[0], ring=False)
    t = np.array([0.3, 1.0, 0.25])
    t /= np.linalg.norm(t)
    a_ = np.cross(t, np.array([0, 0, 1.0]))
    a_ /= np.linalg.norm(a_)
    b_ = np.cross(t, a_)
    n_ring = 0
    for rr_ in np.arange(Rg * 1.35, Rg * 2.1, 0.7):
        n_t = int(2 * math.pi * rr_ / 0.7)
        for th in np.linspace(0, 2 * math.pi, n_t, endpoint=False):
            p = np.array([cx_, cy_, cz_]) + rr_ * (math.cos(th) * a_ + math.sin(th) * b_)
            if clear(p[0], p[1], p[2]) < 15.0:
                continue
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_ringdust")
            n_ring += 1
    print("  gas giant ring blocks:", n_ring)
    for k, (dd, rr2) in enumerate(((-Rg * 3.4, 6), (Rg * 3.0, 8))):
        pm = cen + rr * dd * 0.7 + np.array([0, 20 * (k - 0.5), 0])
        if clear(pm[0], pm[1], pm[2]) > rr2 + 20:
            W.ellipsoid(int(pm[0]), int(pm[1]), int(pm[2]), rr2, rr2, rr2, ["sb_moon", "sb_moon_d"][k], mode="shell")
    tick("giant")

    # ------------------------------------------------------------------ the black hole on the sharp turn
    c, f, rr, u = frame3(S, s_hole)
    ch = c + rr * 62
    if clear(ch[0], ch[1], ch[2]) < 50:
        ch = c - rr * 62
    black_hole(W, rng, tuple(int(round(v)) for v in ch), 13, (0.2, 1.0, 0.15), clear)
    tick("hole")

    # ------------------------------------------------------------------ the wormhole tunnel: rings that tighten and twist
    s_w0, s_w1 = s_worm - 40, s_app + 30
    n_w = 0
    for s in np.arange(s_w0, s_w1, 4.2):
        c, f, rr, u = frame3(S, s)
        t_ = (s - s_w0) / (s_w1 - s_w0)
        R_w = 16.5 - 5.0 * math.sin(t_ * math.pi) ** 0.7 + 1.0 * math.sin(t_ * 14)
        col = SB_RINGS[(n_w // 2) % len(SB_RINGS)]
        hoop(W, c, f, R_w, col, thick=1)
        # a rotating spoke pair (helical look)
        for k in range(3):
            th = n_w * 0.21 + k * 2 * math.pi / 3
            for rr_ in np.arange(R_w - 4, R_w, 0.8):
                p = c + rr_ * (math.cos(th) * rr + math.sin(th) * u)
                W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "sb_disk_b" if k else "sb_disk_a")
        n_w += 1
    print("  wormhole rings:", n_w)
    tick("wormhole")

    # ------------------------------------------------------------------ stars, nebulae, comets, planets
    reg = (-1500, 320, -720, 460)
    n_star = 3600
    sx_ = rng.uniform(reg[0], reg[1], n_star)
    sz_ = rng.uniform(reg[2], reg[3], n_star)
    sy_ = rng.uniform(110, 292, n_star)
    cnt = 0
    for x, y, z in zip(sx_, sy_, sz_):
        if clear(x, y, z) < 12:
            continue
        r_ = rng.random()
        W.put(int(x), int(y), int(z), "sb_star" if r_ < 0.78 else "sb_star_y" if r_ < 0.88 else "sb_star_c" if r_ < 0.95 else "sb_star_p", keep=True)
        cnt += 1
    print("  stars:", cnt)
    for k in range(7):
        R = int(rng.integers(16, 26))
        for _ in range(60):
            x, z = rng.uniform(reg[0], reg[1]), rng.uniform(reg[2], reg[3])
            y = rng.uniform(140, 262)
            if clear(x, y, z) > R + 34 and all(math.dist((x, y, z), (a_, b_, c_)) > R + rr_ + 10 for (a_, b_, c_, rr_) in placed):
                cloud(W, rng, int(x), int(y), int(z), R, int(R * 0.45), int(R * 0.8), n=1, m=["neb_pink", "neb_blue", "neb_teal"][k % 3])
                placed.append((x, y, z, R))
                break
    for k in range(5):
        for _ in range(60):
            x, z = rng.uniform(reg[0], reg[1]), rng.uniform(reg[2], reg[3])
            y = rng.uniform(130, 285)
            if clear(x, y, z) < 40:
                continue
            ang = rng.uniform(0, 2 * math.pi)
            W.ellipsoid(int(x), int(y), int(z), 3, 3, 3, "lit_cyan")
            for t in range(1, 55):
                px, py, pz = x - t * math.cos(ang), y + t * 0.3, z - t * math.sin(ang)
                for _ in range(2):
                    W.put(int(px + rng.normal(0, 0.7 + t * 0.03)), int(py + rng.normal(0, 0.7)), int(pz + rng.normal(0, 0.7 + t * 0.03)),
                          "lit_white" if t < 24 else "lit_cyan", keep=True)
            break
    for k, R in enumerate((16, 11, 13)):
        for _ in range(80):
            x, z = rng.uniform(reg[0] + 100, reg[1] - 100), rng.uniform(reg[2], reg[3])
            y = rng.uniform(150, 262)
            if clear(x, y, z) > R + 40 and all(math.dist((x, y, z), (a_, b_, c_)) > R + rr_ + 14 for (a_, b_, c_, rr_) in placed):
                planet(W, rng, int(x), int(y), int(z), R, PLANETS[(k + 3) % len(PLANETS)], ring=(k == 0))
                placed.append((x, y, z, R))
                break
    tick("space")

    # ------------------------------------------------------------------ landing transform gate and the approach lights over the ribbon strip
    for k, r in enumerate((20, 23, 26)):
        c, f, rr, u = frame3(S, L - 170 + k * 8)
        hoop(W, c, f, r, ["n_cyan", "n_white", "n_lime"][k], thick=2)
    for s in np.arange(L - 230, L - 4, 10.0):
        c, f, rr, u = frame3(S, s)
        for side in (-1, 1):
            q = c + rr * side * 16
            W.put(int(q[0]), int(round(c[1])) - 6, int(q[2]), "lit_white")
    print("  sky-B done; flight length %.0f" % L)
