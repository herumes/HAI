"""Plane zone A - Stratos Cloudlands: launch tunnel, a flyable sky whale, thunderstorm canyon with lightning, a rainbow-arch
slalom, a cloud palace with a great hall to fly through, a pirate airship armada, floating islands, runway approach.
The flight is ctx.SA (2D path) + SA.alt."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_sky import island

mat("sa_cloud", ["Cloth_Block_Wool_White", "Cloth_Block_Wool_White", "Cloth_Block_Wool_Gray_Light"])
mat("sa_storm", ["Cloth_Block_Wool_Gray", "Cloth_Block_Wool_Gray", "Cloth_Block_Wool_Black", "Cloth_Block_Wool_Gray_Light"])
mat("sa_storm_d", ["Cloth_Block_Wool_Black", "Cloth_Block_Wool_Gray"])
mat("sa_bolt", ["NoCube_Neon_Block_Yellow", "NoCube_Neon_Block_White"])
mat("sa_whale_a", ["Soil_Clay_Smooth_Blue", "Soil_Clay_Smooth_Blue", "Soil_Clay_Smooth_Cyan"])
mat("sa_whale_b", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Cloth_Block_Wool_Cyan_Light"])
mat("sa_whale_glow", "Build_Lightsource_Cyan")
mat("sa_marble", ["Rock_Marble_Brick_Smooth", "Rock_Marble_Brick_Smooth", "Rock_Marble_Brick"])
mat("sa_marble_p", "Rock_Marble_Brick_Pillar_Middle")
mat("sa_marble_pb", "Rock_Marble_Brick_Pillar_Base")
mat("sa_gold", "Rock_Gold_Brick_Smooth")
mat("sa_gold_o", "Rock_Gold_Brick_Ornate")
mat("sa_dome", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("sa_roof", "Soil_Clay_Smooth_Red")
mat("sa_rock", ["Rock_Chalk", "Rock_Calcite", "Rock_Stone_Cobble"])
mat("sa_rock_d", ["Rock_Calcite", "Rock_Marble"])
mat("sa_hull", ["Wood_Darkwood_Planks", "Wood_Darkwood_Planks", "Wood_Blackwood_Planks"])
mat("sa_deck", "Wood_Hardwood_Planks")
mat("sa_mast", "Wood_Darkwood_Beam")
mat("sa_sail", ["Cloth_Block_Wool_White", "Cloth_Block_Wool_White", "Cloth_Block_Wool_Gray_Light"])
mat("sa_sail_r", ["Cloth_Block_Wool_Red", "Cloth_Block_Wool_White"])
mat("sa_envelope_r", "Cloth_Block_Wool_Red")
mat("sa_envelope_w", "Cloth_Block_Wool_White")
mat("sa_envelope_b", "Cloth_Block_Wool_Blue")
mat("sa_envelope_y", "Cloth_Block_Wool_Yellow")
mat("sa_rope", "Wood_Hardwood_Fence")
mat("sa_cannon", "Build_Black_Cube")
mat("sa_spark", "Build_Lightsource_Orange")
RB = ["cloth_r", "cloth_o", "cloth_y", "cloth_g", "cloth_c", "cloth_b", "cloth_p"]
for _k, _v in zip(RB, ["Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple"]):
    mat(_k, "Cloth_Block_Wool_" + _v)
RINGS = ["ring_a", "ring_b", "ring_c", "ring_d", "ring_e", "ring_f"]
for _k, _v in zip(RINGS, ["Cyan", "Magenta", "Yellow", "Lime", "Orange", "Light_Blue"]):
    mat(_k, "NoCube_Neon_Block_" + _v)
mat("trail", "Build_Lightsource_White")


def sec(S, name_pts):
    return [nearest_s(S, x, z) for (x, z) in name_pts]


# --------------------------------------------------------------------------- set pieces
def storm_cloud(W, rng, cx, cy, cz, R, H):
    """A towering thundercloud: three dark puffs, a flat anvil on top, lightning down the side."""
    n = 3
    for k in range(n):
        r = R * (1.0 - 0.14 * k) * rng.uniform(0.85, 1.05)
        yk = cy + k * H / n
        ox, oz = rng.normal(0, R * 0.12, 2)
        W.ellipsoid(int(cx + ox), int(yk + H / n * 0.5), int(cz + oz), r, H / n * 0.75, r, "sa_storm_d" if k < 1 else "sa_storm", mode="shell")
    W.ellipsoid(cx, int(cy + H), cz, R * 1.35, H * 0.1, R * 1.35, "sa_storm", mode="shell")
    for b in range(int(rng.integers(2, 4))):
        a = rng.uniform(0, 2 * math.pi)
        x, z = cx + R * 0.7 * math.cos(a), cz + R * 0.7 * math.sin(a)
        y = cy + 2
        n_ = int(rng.integers(26, 55))
        for k in range(n_):
            x += rng.integers(-1, 2)
            z += rng.integers(-1, 2)
            W.put(int(x), int(y - k), int(z), "sa_bolt")
            if k % 9 == 4 and rng.random() < 0.6:
                for j in range(1, 7):
                    W.put(int(x + j * (1 if b % 2 else -1)), int(y - k - j // 2), int(z), "sa_bolt")


def ring_ellipse(W, cx, cy, cz, a, b, m):
    n = int(2 * math.pi * max(a, b) / 0.6) + 8
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    X = np.floor(cx + a * np.cos(th) + 0.5).astype(np.int64)
    Z = np.floor(cz + b * np.sin(th) + 0.5).astype(np.int64)
    W.put_arr(X, np.full(n, cy), Z, m)


def whale_tunnel(W, rng, c, axis, L=96, r0=12.5, r1=17.0, r2=11.0):
    """A hollow sky whale the plane flies through: mouth on the -axis end, tail fluke on the +axis end.  axis 'x' or 'z'."""
    cx, cy, cz = c
    n = int(L) + 2
    a = np.arange(-n // 2, n // 2 + 1)
    prof = np.where(a < 0, r0 + (r1 - r0) * np.sin(np.clip((a + L / 2) / (L / 2), 0, 1) * math.pi / 2),
                    r1 + (r2 - r1) * np.clip(a / (L / 2), 0, 1))
    R = int(r1) + 3
    b, d_ = np.meshgrid(np.arange(-R, R + 1), np.arange(-R, R + 1), indexing="ij")
    rho = np.hypot(b, d_)
    for i, ai in enumerate(a):
        r = prof[i]
        shell = (rho <= r + 0.5) & (rho > r - 2.0)
        belly = d_ < -r * 0.15
        for (sel, m) in ((shell & belly, "sa_whale_b"), (shell & ~belly, "sa_whale_a")):
            bb, dd = b[sel], d_[sel]
            if axis == "x":
                W.put_arr(np.full(bb.shape, cx + ai), cy + dd, cz + bb, m)
            else:
                W.put_arr(cx + bb, cy + dd, np.full(bb.shape, cz + ai), m)
    # throat ribs (glowing) and baleen at the mouth
    for ai in range(-int(L / 2) + 2, int(L / 2) - 8, 6):
        r = float(prof[ai + n // 2])
        for t in np.linspace(math.pi * 1.1, math.pi * 1.9, 24):
            bb, dd = r * math.cos(t) * 0.97, r * math.sin(t) * 0.97
            if axis == "x":
                W.put(cx + ai, int(round(cy + dd)), int(round(cz + bb)), "sa_whale_glow")
            else:
                W.put(int(round(cx + bb)), int(round(cy + dd)), cz + ai, "sa_whale_glow")
    for t in np.linspace(0, 2 * math.pi, 40, endpoint=False):
        bb, dd = r0 * math.cos(t), r0 * math.sin(t)
        for k in range(1, 5):
            if dd > -r0 * 0.4 and axis == "x":
                W.put(cx - n // 2 - k + 2, int(round(cy + dd)), int(round(cz + bb)), "sa_whale_b")
            elif dd > -r0 * 0.4:
                W.put(int(round(cx + bb)), int(round(cy + dd)), cz - n // 2 - k + 2, "sa_whale_b")
    # eyes, flippers, tail fluke, blowhole spout
    sgn = 1
    for side in (-1, 1):
        ex = (-L // 4)
        if axis == "x":
            W.ellipsoid(cx + ex, cy + 3, cz + side * int(r1 + 0.5), 2.5, 2.5, 1.5, "b_white")
            W.ellipsoid(cx + ex, cy + 3, cz + side * int(r1 + 1.5), 1.3, 1.3, 1.0, "b_black")
            W.ellipsoid(cx + 4, cy - 6, cz + side * int(r1 + 6), 9, 2, 6, "sa_whale_a")
        else:
            W.ellipsoid(cx + side * int(r1 + 0.5), cy + 3, cz + ex, 1.5, 2.5, 2.5, "b_white")
            W.ellipsoid(cx + side * int(r1 + 1.5), cy + 3, cz + ex, 1.0, 1.3, 1.3, "b_black")
            W.ellipsoid(cx + side * int(r1 + 6), cy - 6, cz + 4, 6, 2, 9, "sa_whale_a")
    ft = n // 2 + 8
    for side in (-1, 1):
        for k in range(0, 22):
            lat, up = side * (14 + k * 1.1), 3 + k * 0.5
            if axis == "x":
                W.ellipsoid(cx + ft + int(k * 0.5), int(cy + up), int(cz + lat), 6, 1.4, 3, "sa_whale_a")
            else:
                W.ellipsoid(int(cx + lat), int(cy + up), cz + ft + int(k * 0.5), 3, 1.4, 6, "sa_whale_a")
    for k in range(30):
        yy = cy + int(r1) + 2 + k
        if axis == "x":
            W.put(cx - 6, yy, cz, "sa_whale_glow")
        else:
            W.put(cx, yy, cz - 6, "sa_whale_glow")
    # a hole in the roof for the spout (the cell stays solid; spout stands on top)


def rainbow_arch(W, F, R, y_base, thick=2, band=2):
    """A half ring of the seven colours standing on y_base, plane = the flight frame's cross-section."""
    for bi, m in enumerate(RB):
        r0 = R - (bi + 1) * band
        r1 = R - bi * band
        for k in range(0, 181, 1):
            a = math.radians(k)
            for rr in np.arange(r0, r1, 0.5):
                u = rr * math.cos(a)
                h = rr * math.sin(a)
                for v in range(thick):
                    x, z = F.cell(int(round(u)), v)
                    W.put(x, int(y_base + round(h)), z, m)


def galleon(W, rng, cx, cy, cz, along="x", L=44, B=9, balloons=True):
    """A flying pirate galleon: curved hull, deck, three masts with sails, an envelope on top, rope lines, glowing lanterns."""
    sgn = 1
    for v in range(-L // 2, L // 2 + 1):
        t = (v + L / 2) / float(L)
        w = B * (math.sin(math.pi * (0.04 + 0.92 * t)) ** 0.7) * (1.0 + 0.0 * t)
        dep = 6.0 * (0.5 + 0.5 * math.sin(math.pi * (0.04 + 0.92 * t)) ** 0.5)
        lift = 4.0 * max(0.0, (t - 0.8) / 0.2) ** 2
        for u in range(-int(w) - 1, int(w) + 2):
            ru = abs(u) / max(1.0, w)
            if ru > 1.0:
                continue
            hb = -dep * math.sqrt(max(0.0, 1 - ru * ru))
            for h in range(int(hb), 1):
                if h == int(hb) or ru > 0.82 or h == 0:
                    x, z = (cx + v, cz + u) if along == "x" else (cx + u, cz + v)
                    W.put(x, int(cy + h + lift), z, "sa_deck" if h == 0 else "sa_hull")
    for (vm, hm, sail) in ((int(L * 0.28), 22, "sa_sail"), (int(L * -0.05), 28, "sa_sail_r"), (int(-L * 0.3), 20, "sa_sail")):
        for k in range(hm):
            x, z = (cx + vm, cz) if along == "x" else (cx, cz + vm)
            W.put(x, cy + 1 + k, z, "sa_mast")
        for k in range(4, hm - 1, 8):
            for w in range(-B + 1, B):
                for kk in range(0, 6):
                    if rng.random() < 0.97:
                        x, z = (cx + vm + 1, cz + w) if along == "x" else (cx + w, cz + vm + 1)
                        W.put(x, cy + k + kk, z, sail)
        x, z = (cx + vm, cz) if along == "x" else (cx, cz + vm)
        W.put(x, cy + hm + 1, z, "n_red")
        W.put(x, cy + hm + 2, z, "n_red")
    for v in range(-L // 2 + 2, L // 2 - 1, 4):
        for side in (-1, 1):
            x, z = (cx + v, cz + side * (B - 1)) if along == "x" else (cx + side * (B - 1), cz + v)
            W.put(x, cy + 1, z, "sa_cannon")
            W.put(x, cy + 2, z, "sa_rope")
    x, z = (cx - L // 2 - 2, cz) if along == "x" else (cx, cz - L // 2 - 2)
    W.put(x, cy + 1, z, "lit_yellowlight")
    if balloons:
        e = ["sa_envelope_r", "sa_envelope_w", "sa_envelope_b", "sa_envelope_y"][int(rng.integers(0, 4))]
        W.ellipsoid(cx, cy + 36, cz, (L * 0.42 if along == "x" else 9), 9, (9 if along == "x" else L * 0.42), e, mode="shell")
        for k in range(-1, 2, 2):
            for dz in (-4, 4):
                x, z = (cx + k * 8, cz + dz) if along == "x" else (cx + dz, cz + k * 8)
                for y in range(cy + 4, cy + 28):
                    W.put(x, y, z, "sa_rope")


def cloud_palace(W, rng, c, flight_y):
    """A floating palace on a rock island with a great hall (axis x) the plane flies through at flight_y."""
    cx, cy, cz = c
    yf = int(flight_y) - 12                                     # floor of the hall
    hl, hw = 52, 15                                             # half length, half width of the hall
    # floating base: marble slab, then an inverted rock cone
    W.box((cx - hl - 10, yf - 3, cz - 34), (cx + hl + 10, yf - 1, cz + 34), "sa_marble")
    for k in range(0, 30):
        f = 1.0 - k / 30.0
        ring_ellipse(W, cx, yf - 4 - k, cz, (hl + 10) * f ** 0.8 + 2, 34 * f ** 0.8 + 2, "sa_rock" if k % 3 else "sa_rock_d")
        if k % 6 == 5:
            ring_ellipse(W, cx, yf - 4 - k, cz, (hl + 10) * f ** 0.8 + 1, 34 * f ** 0.8 + 1, "sa_rock_d")
    W.cyl(cx, cz, yf - 34, yf - 34, 3, "sa_rock")
    # hall floor, walls, vault
    W.box((cx - hl, yf, cz - hw), (cx + hl, yf, cz + hw), "sa_marble", mode="floor")
    for z in (-hw, hw):
        W.box((cx - hl, yf + 1, cz + z), (cx + hl, yf + 26, cz + z), "sa_marble", mode="floor" if False else "fill")
    for z in range(-hw + 1, hw):
        for x in range(cx - hl, cx + hl + 1):
            pass
    # tall arched windows along both walls (neon), columns every 8
    for x in range(cx - hl + 4, cx + hl - 3, 8):
        for z in (-hw, hw):
            for y in range(yf + 5, yf + 20):
                W.put(x, y, cz + z, "cy_glass_b") if False else W.put(x, y, cz + z, "n_lightblue")
            for y in range(yf + 1, yf + 26):
                W.put(x - 2, y, cz + z, "sa_marble_p") if (x - cx) % 8 == 4 else None
        for z in (-hw + 2, hw - 2):
            for y in range(yf + 1, yf + 22):
                W.put(x, y, cz + z, "sa_marble_p")
            W.put(x, yf + 1, cz + z, "sa_marble_pb")
    # vaulted roof: half-cylinder shell of radius hw around the axis at height yf + 20
    for a in np.linspace(0, math.pi, 90):
        for dv in (0, 1):
            zz = int(round(cz + (hw + 1) * math.cos(a)))
            yy = int(round(yf + 20 + (hw - 3) * math.sin(a)))
            for x in range(cx - hl, cx + hl + 1):
                W.put(x, yy + dv, zz, "sa_marble" if (x // 4) % 2 else "sa_gold")
    # carve the interior of the hall (air) so the walls above do not fill it
    for x in range(cx - hl + 1, cx + hl):
        for z in range(cz - hw + 1, cz + hw):
            for y in range(yf + 1, yf + 40):
                if (z - cz) ** 2 / float(hw - 1) ** 2 + ((y - (yf + 20)) / float(hw - 3)) ** 2 < 1.0 or y < yf + 20:
                    W.erase(x, y, z)
    # restore pillars & lights inside
    for x in range(cx - hl + 4, cx + hl - 3, 8):
        for z in (-hw + 2, hw - 2):
            for y in range(yf + 1, yf + 20):
                W.put(x, y, cz + z, "sa_marble_p")
            W.put(x, yf + 1, cz + z, "sa_marble_pb")
        for z in (-hw + 3, hw - 3):
            W.put(x, yf + 19, cz + z, "lit_yellowlight")
    for x in range(cx - hl + 2, cx + hl - 1, 6):
        W.put(x, yf + 34, cz, "lit_white")
    # end portals: gold rings
    for sx in (-1, 1):
        for a in np.linspace(0, math.pi, 80):
            zz = int(round(cz + (hw + 2) * math.cos(a)))
            yy = int(round(yf + 20 + (hw - 1) * math.sin(a)))
            for dx in range(0, 3):
                W.put(cx + sx * (hl + dx), yy, zz, "sa_gold_o")
    # corner towers with cones and flags, a central dome on the roof
    for sx in (-1, 1):
        for sz in (-1, 1):
            tx, tz = cx + sx * (hl + 4), cz + sz * 26
            W.cyl(tx, tz, yf, yf + 40, 5, "sa_marble", mode="wall")
            for k in range(14):
                W.cyl(tx, tz, yf + 41 + k, yf + 41 + k, 6.5 - k * 0.45, "sa_roof", mode="wall" if 6.5 - k * 0.45 > 1.6 else "fill")
            for y in range(yf + 55, yf + 63):
                W.put(tx, y, tz, "sa_mast")
            for dd in range(1, 5):
                W.put(tx + dd, yf + 62, tz, "cloth_r")
                W.put(tx + dd, yf + 61, tz, "cloth_y")
            for y in range(yf + 6, yf + 40, 8):
                W.put(tx, y, tz + 5 * sz, "lit_yellowlight")
    W.ellipsoid(cx, yf + 32, cz, 14, 12, 14, "sa_dome", mode="shell", part="upper")
    for y in range(yf + 44, yf + 58):
        W.put(cx, y, cz, "sa_gold")
    W.ellipsoid(cx, yf + 59, cz, 2, 2, 2, "lit_yellowlight")
    # waterfalls of light off the edges
    for sz in (-1, 1):
        for x in range(cx - hl + 6, cx + hl - 5, 22):
            for y in range(yf - 30, yf - 1):
                W.put(x, y, cz + sz * 33, "sky_fall")
                W.put(x + 1, y, cz + sz * 33, "sky_fall")


def build(ctx):
    W, S, rng = ctx.W, ctx.SA, ctx.rng
    L = S.length
    clear = Clear(S, step=3)
    tick = W.tick
    m = S.marks

    # ------------------------------------------------------------------ rings along the whole flight, section-coloured
    s_whale = nearest_s(S, 1600, 900)
    s_storm0 = nearest_s(S, 1830, 960)
    s_slalom0 = nearest_s(S, 1500, 1190)
    s_palace = nearest_s(S, 1690, 1400)
    s_arm0 = nearest_s(S, 1870, 1460)
    ranges = [(0, s_storm0 - 10, ["ring_c", "ring_a"], 11.0), (s_storm0 - 10, s_slalom0, ["ring_c", "ring_e"], 10.0),
              (s_slalom0, s_palace - 120, RINGS, 11.0), (s_palace - 120, s_arm0, ["ring_d", "ring_a"], 11.0),
              (s_arm0, m["approach"], ["ring_e", "ring_c"], 10.5), (m["approach"], L, ["ring_d", "n_white"], 10.5)]
    n_r = 0
    for (a, b, cols, r) in ranges:
        for s in np.arange(a + 14, b - 6, 31.0):
            if abs(s - s_palace) < 70 or abs(s - s_whale) < 60:
                continue
            c, f, rr, u = frame3(S, s)
            hoop(W, c, f, r, cols[n_r % len(cols)], thick=2)
            if n_r % 5 == 0:
                hoop(W, c, f, r + 2.6, "trail", thick=1)
            n_r += 1
    for s in np.arange(6.0, L - 6.0, 7.0):
        c = P3(S, s)
        if clear(c[0], c[1] - 7, c[2]) >= 7.5:
            W.put(int(round(c[0])), int(round(c[1])) - 7, int(round(c[2])), "trail")
    print("  sky-A rings:", n_r)
    tick("rings")

    # ------------------------------------------------------------------ launch tunnel (accelerating rings right after the pad)
    for k in range(14):
        c, f, rr, u = frame3(S, 6 + k * 3.2)
        hoop(W, c, f, 12.8 - k * 0.1, ["ring_c", "ring_e"][k % 2], thick=1)
    # the transform arch (kart -> plane) just past the lip
    for k, rd in enumerate((18, 20.5, 23)):
        c, f, rr, u = frame3(S, 60 + k * 6)
        hoop(W, c, f, rd, ["n_white", "n_yellow", "n_cyan"][k], thick=2)
    tick("launch")

    # ------------------------------------------------------------------ the whale tunnel on the first straight
    cw = P3(S, s_whale)
    fx = fdir(S, s_whale)
    axis = "x" if abs(fx[0]) > abs(fx[2]) else "z"
    whale_tunnel(W, rng, (int(round(cw[0])), int(round(cw[1])), int(round(cw[2]))), axis, L=96)
    print("  whale axis", axis, "at", cw)
    # two companion whales (solid, small) flying beside the line
    for (ds, lat, dy) in ((-160, 60, 12), (140, -64, -8)):
        c = P3(S, s_whale + ds)
        F = Frame3 = frame3(S, s_whale + ds)
        q = c + F[2] * lat
        bx, by, bz = int(round(q[0])), int(round(q[1] + dy)), int(round(q[2]))
        if clear(bx, by, bz) > 40:
            W.ellipsoid(bx, by, bz, 28, 10, 11, "sa_whale_a", mode="shell")
            W.ellipsoid(bx, by - 3, bz, 28, 7, 8, "sa_whale_b", mode="shell", part="lower")
            W.ellipsoid(bx - 30, by + 4, bz, 9, 1.5, 12, "sa_whale_a")
            W.put(bx + 20, by + 4, bz + 10, "b_black")
    tick("whales")

    # ------------------------------------------------------------------ the thunderstorm canyon (legs 2-3)
    placed = []
    for s in np.arange(s_storm0, s_slalom0 - 20, 70.0):
        for side in (-1, 1):
            c, f, rr, u = frame3(S, s + side * 9)
            q = c + rr * side * rng.uniform(46, 70)
            R = rng.uniform(15, 22)
            H = rng.uniform(44, 70)
            by = int(c[1] - rng.uniform(25, 45))
            if clear(q[0], by + H * 0.5, q[2]) < R + 30 or clear(q[0], by, q[2]) < R + 24:
                continue
            if any(math.hypot(q[0] - a, q[2] - b) < R + r + 8 for (a, b, r) in placed):
                continue
            placed.append((q[0], q[2], R))
            storm_cloud(W, rng, int(q[0]), by, int(q[2]), R, H)
    print("  storm clouds:", len(placed))
    tick("storm")

    # ------------------------------------------------------------------ the rainbow arch slalom (leg 4)
    for k, s in enumerate(np.arange(s_slalom0 + 20, s_palace - 150, 34.0)):
        c, f, rr, u = frame3(S, s)
        f2 = f.copy()
        F = Frame(S, s)
        side = 1 if k % 2 == 0 else -1
        # the arch frame is horizontal: use the path frame; shifted so the opening is centred on the line
        lat = side * 4
        cxx, czz = F.cell(lat, 0)
        class _F:
            pass
        Fa = F
        # rebuild the cell function with the arch centred at u = lat
        def cell(u_, v_, F=F, lat=lat):
            return F.cell(u_ + lat, v_)
        Fa = type("FA", (), {"cell": staticmethod(cell)})
        rainbow_arch(W, Fa, 31, int(c[1] - 6), thick=2, band=2)
        # a puffy cloud under each foot
        for sd in (-1, 1):
            xx, zz = cell(sd * 28, 0)
            cloud(W, rng, xx, int(c[1] - 9), zz, 9, 4, 8, n=1, m="sa_cloud")
    tick("arches")

    # ------------------------------------------------------------------ the cloud palace on leg 5
    cp = P3(S, s_palace)
    fp = fdir(S, s_palace)
    if abs(fp[0]) > abs(fp[2]):
        cloud_palace(W, rng, (int(round(cp[0])), int(round(cp[1])), int(round(cp[2]))), cp[1])
        print("  palace at", cp)
    else:
        print("  WARNING: palace leg is not axis aligned", fp)
    tick("palace")

    # ------------------------------------------------------------------ the armada (leg 6) and islands
    for k, s in enumerate(np.arange(s_arm0 - 10, m["approach"] - 10, 52.0)):
        side = 1 if k % 2 == 0 else -1
        c, f, rr, u = frame3(S, s)
        q = c + rr * side * rng.uniform(42, 60)
        by = int(c[1] + rng.uniform(-14, 10))
        if clear(q[0], by, q[2]) < 38:
            continue
        axis = "x" if abs(f[0]) < abs(f[2]) else "z"
        galleon(W, rng, int(q[0]), by, int(q[2]), along="z" if axis == "x" else "x", L=int(rng.integers(38, 52)))
    tick("armada")
    isl = 0
    for s in np.arange(150, L - 300, 130.0):
        side = 1 if isl % 2 == 0 else -1
        c, f, rr, u = frame3(S, s + 40)
        lat = side * rng.uniform(58, 84)
        q = c + rr * lat
        cy = int(c[1] + rng.uniform(-30, -6))
        R = rng.uniform(14, 22)
        if clear(q[0], cy, q[2]) < R + 28 or abs(s - s_whale) < 120 or abs(s - s_palace) < 140:
            continue
        if any(math.hypot(q[0] - a, q[2] - b) < R + r + 10 for (a, b, r) in placed):
            continue
        placed.append((q[0], q[2], R))
        island(W, rng, int(q[0]), cy, int(q[2]), R, depth=int(R), seed=210 + isl)
        isl += 1
    print("  islands:", isl)
    tick("islands")

    # ------------------------------------------------------------------ puffy white clouds around, a sun gate at the end of the storm
    for k in range(22):
        s = rng.uniform(100, L - 100)
        c, f, rr, u = frame3(S, s)
        q = c + rr * rng.choice([-1, 1]) * rng.uniform(60, 170)
        y = int(c[1] + rng.uniform(-60, 14))
        if clear(q[0], y, q[2]) < 38:
            continue
        cloud(W, rng, int(q[0]), y, int(q[2]), int(rng.integers(12, 22)), int(rng.integers(4, 8)), int(rng.integers(9, 16)), n=1, m="sa_cloud")
    tick("clouds")

    # ------------------------------------------------------------------ landing transform gate and the approach lights
    for k, r in enumerate((20, 23, 26)):
        c, f, rr, u = frame3(S, L - 150 + k * 8)
        hoop(W, c, f, r, ["n_cyan", "n_white", "n_lime"][k], thick=2)
    for s in np.arange(L - 260, L - 4, 10.0):
        c, f, rr, u = frame3(S, s)
        for side in (-1, 1):
            q = c + rr * side * 16
            W.put(int(q[0]), int(round(c[1])) - 6, int(q[2]), "lit_white")
    print("  sky-A done; flight length %.0f" % L)
