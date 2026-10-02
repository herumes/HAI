"""Zone 8: Dread Keep - a volcanic fortress over a lava lake: lava causeway, gatehouse with portcullis, a pillared great hall with
hanging crushers, a courtyard under a dragon's rib cage, a wall-walk along the ramparts, a lava bridge with fire pillars."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403

mat("dk_brick", ["Rock_Volcanic_Brick", "Rock_Volcanic_Brick", "Rock_Basalt_Brick", "Rock_Magma_Cooled_Brick"])
mat("dk_brick_s", ["Rock_Volcanic_Brick_Smooth", "Rock_Basalt_Brick_Smooth"])
mat("dk_dark", ["Rock_Basalt_Brick", "Build_Black_Cube", "Rock_Volcanic_Brick"])
mat("dk_orn", "Rock_Volcanic_Brick_Ornate")
mat("dk_pillar", "Rock_Volcanic_Brick_Pillar_Middle")
mat("dk_pillar_b", "Rock_Volcanic_Brick_Pillar_Base")
mat("dk_stair", "Rock_Volcanic_Brick_Stairs")
mat("dk_half", "Rock_Volcanic_Brick_Half")
mat("dk_roof", "Rock_Volcanic_Brick_Roof")
mat("dk_rock", ["Rock_Volcanic", "Rock_Basalt", "Rock_Volcanic_Cobble", "Rock_Magma_Cooled_Cobble"])
mat("dk_rock_side", ["Rock_Volcanic_Cobble", "Rock_Basalt_Cobble", "Rock_Volcanic_Brick", "Rock_Magma_Cooled_Cobble"])
mat("dk_crust", ["Rock_Magma_Cooled", "Rock_Magma_Cooled", "Rock_Volcanic", "Soil_Magma_Cooled_Gravel"])
mat("dk_lava", ["Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Incandescent"])
mat("dk_lava_glow", ["NoCube_Neon_Block_Fire_Red", "NoCube_Neon_Block_Orange", "NoCube_Neon_Block_Orange"])
mat("dk_fire", ["Build_Lightsource_Orange", "Build_Lightsource_Red", "Build_Lightsource_Yellow"])
mat("dk_bars", "Metal_Iron_Fence")
mat("dk_chain", "Deco_Iron_Chains")
mat("dk_metal", "Metal_Zinc_Smooth")
mat("dk_banner_r", "Cloth_Block_Wool_Red")
mat("dk_banner_k", "Cloth_Block_Wool_Black")
mat("dk_banner_y", "Cloth_Block_Wool_Yellow")
mat("dk_bone", ["Rock_Chalk", "Rock_Chalk", "Rock_Calcite"])
mat("dk_eye", "NoCube_Neon_Block_Red")
mat("dk_ember", "Build_Lightsource_Red")
mat("dk_smoke", ["Cloth_Block_Wool_Gray", "Cloth_Block_Wool_Black", "Cloth_Block_Wool_Gray_Light"])
mat("dk_gold", "Rock_Gold_Brick_Smooth")


def band(W, P, s_a, s_b, u_a, u_b, y0, y1, m, rel=True, m2=None, every=None, skip=None):
    """Fill the cells with lateral offset u_a..u_b of the path stretch s_a..s_b from y0..y1 (relative to the road when rel)."""
    cx, cz, si, uu = raster_band(P, P.i(s_a), P.i(s_b), u_a, u_b)
    T = P.T[si] if rel else np.zeros(len(cx), dtype=np.int64)
    S = P.s[si]
    if skip is not None:
        keep = ~skip(S, uu)
        cx, cz, T, S, uu = cx[keep], cz[keep], T[keep], S[keep], uu[keep]
    for y in range(int(y0), int(y1) + 1):
        if m2 is not None and every is not None:
            alt = ((S // every).astype(int) % 2) == 0
            W.put_arr(cx[alt], T[alt] + y, cz[alt], m)
            W.put_arr(cx[~alt], T[~alt] + y, cz[~alt], m2)
        else:
            W.put_arr(cx, T + y, cz, m)


def crenellate(W, P, s_a, s_b, u, y, m, period=2):
    """A row of merlons (every other block) along an offset line."""
    cx, cz, si, uu = raster_band(P, P.i(s_a), P.i(s_b), u - 0.5, u + 0.5)
    T = P.T[si]
    sel = (np.floor(P.s[si]).astype(int) // period) % 2 == 0
    W.put_arr(cx[sel], T[sel] + y, cz[sel], m)
    W.put_arr(cx[sel], T[sel] + y + 1, cz[sel], m)


def tower(W, rng, cx, cz, y0, h, r, roof=True):
    W.cyl(cx, cz, y0, y0 + h, r, "dk_brick", mode="wall")
    for y in range(y0 + 4, y0 + h, 7):
        for a in range(0, 360, 30):
            W.put(int(round(cx + (r + 0.2) * math.cos(math.radians(a)))), y, int(round(cz + (r + 0.2) * math.sin(math.radians(a)))), "dk_dark")
        W.put(int(round(cx + r)), y + 1, cz, "dk_fire")
        W.put(int(round(cx - r)), y + 1, cz, "dk_fire")
    W.cyl(cx, cz, y0 + h + 1, y0 + h + 1, r + 1.5, "dk_dark", mode="fill")
    for a in range(0, 360, 20):
        for dy in (2, 3):
            W.put(int(round(cx + (r + 1.5) * math.cos(math.radians(a)))), y0 + h + dy, int(round(cz + (r + 1.5) * math.sin(math.radians(a)))), "dk_brick") if (a // 20) % 2 == 0 else None
    if roof:
        for k in range(int(r * 1.8)):
            rr = r + 0.5 - k * 0.55
            if rr < 0.6:
                break
            W.cyl(cx, cz, y0 + h + 4 + k, y0 + h + 4 + k, rr, "dk_dark", mode="wall" if rr > 1.8 else "fill")
        W.put(cx, y0 + h + 6 + int(r * 1.8), cz, "dk_ember")
        for y in range(y0 + h + 4 + int(r * 1.8), y0 + h + 10 + int(r * 1.8)):
            W.put(cx, y, cz, "dk_metal")
        for dy in range(3):
            for dx in range(1, 6):
                W.put(cx + dx, y0 + h + 9 + int(r * 1.8) - dy, cz, "dk_banner_r")


def ribcage(W, F, T, half=9, h=13, thick=1):
    """One rib arch over the road (cross-section plane = frame cross-section)."""
    for k in range(0, 91):
        a = math.pi * k / 90
        u = half * math.cos(a)
        y = h * math.sin(a) ** 0.8
        for dv in range(thick + 1):
            x, z = F.cell(int(round(u)), dv)
            W.put(x, T + 2 + int(round(y)), z, "dk_bone")


def thwomp(W, F, T, y, size=3):
    """A hanging stone crusher: 2*size+1 cube with a neon face, chains up to the roof."""
    for u in range(-size, size + 1):
        for v in range(-size, size + 1):
            for yy in range(y, y + 2 * size + 1):
                x, z = F.cell(u, v)
                W.put(x, T + yy, z, "dk_dark" if (abs(u) == size or abs(v) == size or yy in (y, y + 2 * size)) else "dk_brick")
    for s_ in (-1, 1):
        x, z = F.cell(s_ * 1.6, -size - 1)
        W.put(x, T + y + 2 * size - 1, z, "dk_eye")
        W.put(x, T + y + 2 * size - 2, z, "dk_eye")
    for u in range(-2, 3):
        x, z = F.cell(u, -size - 1)
        W.put(x, T + y + 2, z, "b_white")
    for k in range(y + 2 * size + 1, y + 2 * size + 5):
        x, z = F.cell(0, 0)
        W.put(x, T + k, z, "dk_chain")


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s0, s1 = m["keep"], m["glacier"]
    L = s1 - s0
    i0, i1 = B.i(s0), B.i(s1)
    tick = W.tick
    S = lambda f: s0 + f * L

    # ------------------------------------------------------------------ road
    edge = lambda s, u: "n_orange" if int(s // 3) % 2 == 0 else "n_firered"
    s_hall0, s_hall1 = S(0.17), S(0.32)                 # the great hall (roofed)
    s_yard0, s_yard1 = S(0.33), S(0.52)                 # courtyard
    s_walk0, s_walk1 = S(0.53), S(0.74)                 # rampart wall-walk
    piers = dict(mat="dk_pillar", every=10, size=2, inset=1.6, cap="dk_dark", bottom=0, beam="dk_dark")
    base = dict(hw=7.0, verge="k_conc", edge=edge, lanes=(0.0,), lane_dash=(4.0, 4.0), lane_mat="k_yline", curbs=())
    stamp(ctx, B, s0, s_hall0, KartStyle(rail="mesh", deck="dk_dark", piers=piers, **base))
    stamp(ctx, B, s_hall0 - 2, s_hall1, KartStyle(rail="mesh", deck="dk_dark", **base))
    stamp(ctx, B, s_hall1 - 2, s_walk0, KartStyle(rail=None, rail_fn=lambda s, side: "tyre" if side < 0 else "armco", deck="dk_dark", piers=piers, **base))
    stamp(ctx, B, s_walk0 - 2, s_walk1, KartStyle(rail="chevron", deck="dk_dark", **base))
    stamp(ctx, B, s_walk1 - 2, s1, KartStyle(rail="mesh", deck="dk_dark", piers=piers, **base))
    tick("road")

    # ------------------------------------------------------------------ terrain: a lava lake with crags and the castle rock
    land = Land(ctx, B, i0, i1, margin=80, radius=60, hw=9.0)
    g, d, Tn, Sn = land.g, land.d, land.Tn, land.Sn
    amp = smoothstep(s0, s0 + 30, Sn) * (1 - smoothstep(s1 - 40, s1, Sn))
    n1 = fbm(g.X, g.Z, 90.0, 4, 131)
    n2 = fbm(g.X, g.Z, 24.0, 3, 133)
    Hn = -1.0 + 0.0 * n1
    crag = amp * (26.0 * smoothstep(0.52, 0.8, n1) + 6.0 * n2 * smoothstep(0.4, 0.7, n1))
    in_hall = (Sn > s_hall0 - 20) & (Sn < s_yard1 + 20)
    rock_h = np.where(in_hall, np.minimum(Tn - 1, 12.0) + 2.0 * n2, -1.0)         # castle rock under the hall and courtyard
    Hn = np.maximum(Hn + crag, np.where(d < 30, rock_h, -1.0))
    shoulder = np.where(Tn > 0, np.minimum(Tn - 1, np.where(Tn > 10, Tn - 7, Tn - 1)), -1)
    w = 1.0 - smoothstep(3.0, 14.0, d)
    Hc = Hn * (1 - w) + shoulder * w
    taper = 1.0 - smoothstep(38.0, 56.0, d)
    Hc = Hc * taper + (-1.0) * (1 - taper)
    Ht = np.maximum(np.rint(Hc).astype(np.int64), -1)
    mask = (d < 58) & ~(land.rb & (d > 14))
    slope = slope_of(Hc)
    land.Hc, land.Ht, land.mask, land.slope = Hc, Ht, mask, slope
    mats = ["dk_rock", "dk_rock_side", "dk_lava", "dk_crust", "dk_brick"]
    lava = (Ht <= 0) & (d > 10.5)
    top_ids = pick([(Ht >= 4, 0), (lava, 2), ((Ht > 0) & (Ht < 4), 3)], 0)

    def side_fn(y, x, z):
        return np.where(y < 2, 2, np.where((y // 3) % 2 == 0, 1, 4))

    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask)
    ix, iz = np.nonzero(lava & mask)
    sel = rng.random(ix.size) < 0.06
    W.put_arr(g.x0 + ix[sel], np.full(sel.sum(), -1), g.z0 + iz[sel], "dk_lava_glow")
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

    # ------------------------------------------------------------------ the gatehouse (towers, arch, portcullis, drawbridge chains)
    s_gh = axis_s(B, S(0.09), 200) or S(0.09)
    Fg = Frame(B, s_gh)
    Tg = Fg.T
    for side in (-1, 1):
        x, z = Fg.cell(side * 16, 0)
        tower(W, rng, x, z, -1, Tg + 28, 8)
    lbox(W, Fg, -16, 16, -4, 4, Tg + 18, Tg + 24, "dk_brick")
    lbox(W, Fg, -9, 9, -5, 5, Tg + 12, Tg + 17, "dk_brick")
    for u in range(-9, 10):
        for y in range(Tg + 9, Tg + 12):
            x, z = Fg.cell(u, -3)
            if u % 2 == 0:
                W.put(x, y, z, "dk_bars")
    # the name goes on a plate in front of the towers (they are 8 blocks deep), on its own two posts
    lbox(W, Fg, -33, 33, -11, -10, Tg + 30, Tg + 40, "b_black")
    for side in (-1, 1):
        lbox(W, Fg, side * 31, side * 33, -11, -10, max(-1, Tg - 6), Tg + 29, "dk_brick")
    lsign(W, Fg, "DREAD KEEP", 0, Tg + 38, -12, 1, "n_firered")
    for side in (-1, 1):
        for k in range(1, 8):
            x, z = Fg.cell(side * 8, -5 - k)
            W.put(x, Tg + 11 - k // 2, z, "dk_chain")
    tick("gatehouse")

    # ------------------------------------------------------------------ the great hall (tunnel): walls, roof >= T+13, pillars, banners, lava gutters
    band(W, B, s_hall0, s_hall1, 9.0, 11.0, 0, 13, "dk_brick", m2="dk_dark", every=4)
    band(W, B, s_hall0, s_hall1, -11.0, -9.0, 0, 13, "dk_brick", m2="dk_dark", every=4)
    band(W, B, s_hall0, s_hall1, -11.0, 11.0, 14, 14, "dk_dark")
    band(W, B, s_hall0, s_hall1, -10.0, 10.0, 15, 15, "dk_brick")
    # lava gutters outside the barriers (glowing) and ember lights in the roof
    for s in np.arange(s_hall0 + 4, s_hall1 - 2, 8.0):
        F = Frame(B, s)
        T = F.T
        for side in (-1, 1):
            x, z = F.cell(side * 8.8, 0)
            for y in range(T, T + 12):
                W.put(x, y, z, "dk_pillar")
            W.put(x, T, z, "dk_pillar_b")
            W.put(x, T + 12, z, "dk_orn")
            xb, zb = F.cell(side * 8.0, 2)
            W.put(xb, T + 1, zb, "dk_fire")
            for dy in range(4, 11):
                xx, zz = F.cell(side * 8.0, 4)
                W.put(xx, T + dy, zz, "dk_banner_r" if (int(s) // 8) % 2 else "dk_banner_k")
        x, z = F.cell(0, 0)
        for dy in range(11, 14):
            W.put(x, T + dy, z, "dk_chain")
        for du in (-2, 0, 2):
            xx, zz = F.cell(du, 0)
            W.put(xx, T + 10, zz, "dk_fire")
    for k, s in enumerate(np.arange(s_hall0 + 18, s_hall1 - 10, 30.0)):
        F = Frame(B, s)
        thwomp(W, F, F.T, 7 if k % 2 else 8, size=3)
    tick("hall")

    # ------------------------------------------------------------------ courtyard: curtain walls, towers, the dragon's rib cage, braziers
    for side in (-1, 1):
        band(W, B, s_yard0, s_yard1, side * 22.0, side * 23.5, -6, 22, "dk_brick", m2="dk_dark", every=6)
        crenellate(W, B, s_yard0, s_yard1, side * 22.5, 23, "dk_brick", period=3)
    for s in (s_yard0 + 8, S(0.43), s_yard1 - 8):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 25, 0)
            tower(W, rng, x, z, -1, F.T + 22 + int(rng.integers(0, 8)), 6)
    for k, s in enumerate(np.arange(s_yard0 + 14, s_yard1 - 20, 6.0)):
        F = Frame(B, s)
        ribcage(W, F, F.T, half=9.5, h=14 - (abs(k - 6) * 0.35))
    for s in np.arange(s_yard0 + 10, s_yard1 - 10, 24.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 13, 0)
            W.cyl(x, z, F.T, F.T + 3, 2, "dk_brick", mode="fill")
            for y in range(F.T + 4, F.T + 7):
                W.put(x, y, z, "dk_fire")
    Fk = Frame(B, S(0.43))
    x, z = Fk.cell(0, 0)
    for side in (-1, 1):
        xx, zz = Fk.cell(side * 15, 0)
        W.ellipsoid(xx, Fk.T + 6, zz, 3.4, 3.4, 3.4, "dk_bone", mode="shell")
        for sd in (-1, 1):
            xe, ze = Fk.cell(side * 15 + sd * 1.4, -3)
            W.put(xe, Fk.T + 7, ze, "dk_eye")
    tick("courtyard")

    # ------------------------------------------------------------------ rampart wall-walk: solid wall under the road, parapets, towers
    band(W, B, s_walk0, s_walk1, -9.0, 9.0, -9, -1, "dk_brick", m2="dk_dark", every=5)
    for side in (-1, 1):
        crenellate(W, B, s_walk0, s_walk1, side * 8.8, 1, "dk_brick", period=2)
    for k, s in enumerate(np.arange(s_walk0 + 24, s_walk1 - 10, 52.0)):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 14, 0)
            tower(W, rng, x, z, -1, F.T + 14, 5)
    for s in np.arange(s_walk0 + 12, s_walk1 - 6, 14.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 10.5, 0)
            W.put(x, F.T + 1, z, "dk_fire")
    tick("walls")

    # ------------------------------------------------------------------ lava bridge: fire pillars, arches, ember rain
    sb0, sb1 = S(0.76), S(0.94)
    for k, s in enumerate(np.arange(sb0, sb1, 16.0)):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 12.5, 0)
            for y in range(-1, F.T + 9):
                W.put(x, y, z, "dk_dark" if y % 2 else "dk_brick")
            for y in range(F.T + 9, F.T + 9 + int(rng.integers(5, 12))):
                W.put(x, y, z, "dk_fire")
    for s in np.arange(sb0 + 8, sb1, 32.0):
        F = Frame(B, s)
        for a in np.linspace(0, math.pi, 60):
            for dv in range(2):
                x, z = F.cell(int(round(12.5 * math.cos(a))), dv)
                W.put(x, F.T + 8 + int(round(7 * math.sin(a))), z, "dk_brick")
    for k in range(180):
        s = s0 + rng.uniform(0, L)
        F = Frame(B, s)
        x, z = F.cell(rng.uniform(-30, 30), 0)
        y = int(F.T + rng.uniform(3, 30))
        if not W.has(x, y, z):
            W.put(x, y, z, "dk_ember")
    tick("bridge")

    # ------------------------------------------------------------------ skulls, crags, smoke, dressing
    for k in range(10):
        q = spot(s0 + rng.uniform(40, L - 40), (1 if rng.random() < 0.5 else -1) * rng.uniform(16, 40), 6)
        if q and q[1] > 0:
            r_ = int(rng.integers(3, 6))
            W.ellipsoid(q[0], q[1] + r_, q[2], r_, r_ * 1.2, r_, "dk_rock", part="upper")
    for k in range(10):
        s = s0 + rng.uniform(40, L - 40)
        F = Frame(B, s)
        x, z = F.cell(rng.choice([-1, 1]) * rng.uniform(20, 46), 0)
        for t in range(26):
            W.put(int(x + rng.normal(0, 0.6 + t * 0.1)), int(10 + t * 1.6), int(z + rng.normal(0, 0.6 + t * 0.1)), "dk_smoke")
    for frac in (0.06, 0.15, 0.3, 0.5, 0.58, 0.7, 0.85, 0.94):
        boost_pad(W, B, S(frac), 14, 4, "n_firered" if int(frac * 10) % 2 else "n_orange", count=3, spacing=4)
    for frac in (0.08, 0.22, 0.4, 0.56, 0.66, 0.8, 0.9):
        item_boxes(W, B, S(frac), 7.0, n=4)
    corner_furniture(W, B, i0, i1, 7.0, rmax=100.0, hay=False, stacks=True, wall_w=0.0, skid=True, need_support=False)
    tick("dressing")
