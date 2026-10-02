"""Zone 2: Neon Metropolis - a night city: neon-skinned skyscrapers, a figure-8 overpass, a tunnel through a mega-arch tower,
monorails, holograms, billboards, hovering cars.  Black asphalt with neon edges, steel-mesh barriers."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
import layout3

mat("cy_pave", ["Rock_Basalt_Brick_Smooth", "Rock_Basalt_Brick_Smooth", "Rock_Slate_Brick_Smooth"])
mat("cy_pave2", "Build_GreyDark_Cube")
mat("cy_body", ["Metal_Zinc_Smooth", "Metal_Zinc_Smooth", "Rock_Basalt_Brick_Smooth", "Build_GreyDark_Cube"])
mat("cy_body_l", ["Metal_Iron_Smooth", "Metal_Iron_Smooth", "Build_Grey_Cube"])
mat("cy_body_b", ["Build_Black_Cube", "Build_Black_Cube", "Metal_Zinc_Smooth"])
mat("cy_body_w", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey", "Soil_Clay_Smooth_White"])
mat("cy_trim", "Metal_Iron_Smooth")
mat("cy_glass_c", "NoCube_Neon_Block_Cyan")
mat("cy_glass_m", "NoCube_Neon_Block_Magenta")
mat("cy_glass_y", "NoCube_Neon_Block_Yellow")
mat("cy_glass_l", "NoCube_Neon_Block_Lime")
mat("cy_glass_o", "NoCube_Neon_Block_Orange")
mat("cy_glass_w", "NoCube_Neon_Block_White")
mat("cy_glass_b", "NoCube_Neon_Block_Light_Blue")
mat("cy_glass_p", "NoCube_Neon_Block_Purple")
mat("cy_deck", "Rock_Slate_Brick")
mat("cy_pier", "Rock_Slate_Brick_Pillar_Middle")
mat("cy_rail", "Metal_Zinc_Fence")
mat("cy_cable", "Metal_Iron_Fence")
mat("cy_beacon", "Build_Lightsource_Red")
mat("cy_tree", "Plant_Leaves_Oak")
GLASS = ["cy_glass_c", "cy_glass_m", "cy_glass_y", "cy_glass_l", "cy_glass_o", "cy_glass_w", "cy_glass_b", "cy_glass_p"]
BODIES = ["cy_body", "cy_body_l", "cy_body_b", "cy_body_w"]


def windows_face(W, rng, x0, z0, w, d, y0, y1, win, face, pitch=2, band=3, density=0.62):
    """Lit windows on one face of a box: face in 'n','s','e','w'.  Replaces wall blocks."""
    if face in ("n", "s"):
        zz = z0 if face == "n" else z0 + d - 1
        xs = np.arange(x0 + 1, x0 + w - 1, pitch)
        for y in range(y0, y1):
            if y % band == 0:
                continue
            sel = rng.random(len(xs)) < density
            for x in xs[sel]:
                W.put(int(x), y, zz, win)
    else:
        xx = x0 if face == "w" else x0 + w - 1
        zs = np.arange(z0 + 1, z0 + d - 1, pitch)
        for y in range(y0, y1):
            if y % band == 0:
                continue
            sel = rng.random(len(zs)) < density
            for z in zs[sel]:
                W.put(xx, y, int(z), win)


def tower_box(W, rng, x0, z0, w, d, h, body, win, crown="antenna", base_h=3, steps=0):
    """A skyscraper: hollow shell, lit window grid, neon roof rim, lobby band, antenna or helipad crown.  steps > 0 = setbacks."""
    y = 0
    cx0, cz0, cw, cd = x0, z0, w, d
    seg_h = max(14, h // (steps + 1))
    cur_top = 0
    for st in range(steps + 1):
        top = min(h, y + seg_h) if st < steps else h
        W.box((cx0, y, cz0), (cx0 + cw - 1, top, cz0 + cd - 1), body, mode="walls")
        for f in "nsew":
            windows_face(W, rng, cx0, cz0, cw, cd, y + (base_h if st == 0 else 1), top, win, f)
        # ledge
        W.box((cx0, top + 1, cz0), (cx0 + cw - 1, top + 1, cz0 + cd - 1), "cy_trim", mode="fill" if st == steps else "floor")
        for x in range(cx0, cx0 + cw):
            W.put(x, top + 1, cz0, win)
            W.put(x, top + 1, cz0 + cd - 1, win)
        for z in range(cz0, cz0 + cd):
            W.put(cx0, top + 1, z, win)
            W.put(cx0 + cw - 1, top + 1, z, win)
        y = top + 1
        cur_top = y
        cx0, cz0, cw, cd = cx0 + 3, cz0 + 3, cw - 6, cd - 6
        if cw < 8 or cd < 8:
            break
    cx = x0 + w // 2
    cz = z0 + d // 2
    if crown == "antenna":
        ht = int(rng.integers(10, 26))
        for k in range(ht):
            W.put(cx, cur_top + 1 + k, cz, "cy_trim")
        W.put(cx, cur_top + 1 + ht, cz, "cy_beacon")
        for k in (4, 9):
            if k < ht:
                W.box((cx - 2, cur_top + 1 + k, cz), (cx + 2, cur_top + 1 + k, cz), "cy_trim")
    elif crown == "helipad":
        r = min(cw, cd) // 2 - 1
        W.cyl(cx, cz, cur_top + 1, cur_top + 1, max(3, r), "cy_body_b")
        W.cyl(cx, cz, cur_top + 2, cur_top + 2, max(3, r) - 1, win, mode="wall")
        put_text(W, "H", cx, cur_top + 5, cz, 1, "cy_glass_w", plane="z") if False else None
    elif crown == "dome":
        W.ellipsoid(cx, cur_top + 1, cz, min(cw, cd) // 2, 6, min(cw, cd) // 2, win, mode="shell", part="upper")
    elif crown == "spire":
        for k in range(30):
            r = max(0.0, 5.0 * (1 - k / 30.0))
            if r < 0.6:
                W.put(cx, cur_top + 1 + k, cz, win)
            else:
                W.cyl(cx, cz, cur_top + 1 + k, cur_top + 1 + k, r, body, mode="wall" if r > 1.8 else "fill")
        W.put(cx, cur_top + 31, cz, "cy_beacon")
    return cur_top


def tower_round(W, rng, cx, cz, r, h, body, win):
    for y in range(0, h):
        W.cyl(cx, cz, y, y, r, body, mode="wall")
    for y in range(3, h, 4):
        for a in range(0, 360, int(max(6, 360 / (r * 2.2)))):
            if rng.random() < 0.7:
                W.put(int(round(cx + r * math.cos(math.radians(a)))), y, int(round(cz + r * math.sin(math.radians(a)))), win)
                W.put(int(round(cx + r * math.cos(math.radians(a)))), y + 1, int(round(cz + r * math.sin(math.radians(a)))), win)
    for y in range(8, h, 16):
        W.cyl(cx, cz, y, y, r + 1.5, "cy_trim", mode="wall")
        W.cyl(cx, cz, y + 1, y + 1, r + 1.5, win, mode="wall")
    W.cyl(cx, cz, h, h, r + 1, "cy_trim")
    for k in range(18):
        W.put(cx, h + 1 + k, cz, "cy_trim")
    W.put(cx, h + 19, cz, "cy_beacon")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    s0, s1 = m["city"], m["tower"]
    i0, i1 = A.i(s0), A.i(s1)
    s_c1, s_c2 = ctx.marks["cross1"], ctx.marks["cross2"]
    tick = W.tick

    # ------------------------------------------------------------------ roads: black asphalt, neon edge, steel-mesh barriers
    edge = lambda s, u: ["n_cyan", "n_magenta", "n_yellow", "n_magenta"][int(s // 4) % 4]
    base = dict(hw=7.0, verge="k_conc", edge=edge, lanes=(0.0,), lane_dash=(4.0, 4.0), lane_mat="k_yline", curbs=())
    # ground-level stretches (everything except the overpass), the overpass has a deck + piers
    over_a, over_b = s_c2 - 112, s_c2 + 112
    st_ground = KartStyle(rail="mesh", **base)
    stamp(ctx, A, s0, over_a, st_ground)
    # ramp up (piers), bridge over the crossing (no piers within 26 blocks of it), ramp down
    piers = dict(mat="cy_pier", every=10, size=2, inset=1.5, cap="cy_deck", bottom=0)
    stamp(ctx, A, over_a - 2, s_c2 - 26, KartStyle(rail="mesh", deck="cy_deck", piers=piers, **base))
    stamp(ctx, A, s_c2 - 28, s_c2 + 28, KartStyle(rail="mesh", deck="cy_deck", **base))
    stamp(ctx, A, s_c2 + 26, over_b + 2, KartStyle(rail="mesh", deck="cy_deck", piers=piers, **base))
    stamp(ctx, A, over_b, s1, st_ground)
    tick("roads")

    # ------------------------------------------------------------------ pavement along the roads (sidewalks), a plaza at the crossing
    for (a, b) in ((s0 - 20, over_a), (over_b, s1)):
        cx, cz, si, uu = raster_band(A, A.i(a), A.i(b), -15.0, 15.0)
        sel = np.abs(uu) >= 7.0
        W.put_arr(cx[sel], np.full(sel.sum(), -1), cz[sel], "cy_pave", keep=True)
    cx, cz, si, uu = raster_band(A, A.i(over_a), A.i(over_b), -15.0, 15.0)
    W.put_arr(cx, np.full(len(cx), -1), cz, "cy_pave", keep=True)
    tick("pavement")

    # ------------------------------------------------------------------ distance to the road for building placement
    xs0, xs1, zs0, zs1 = 940, 1780, -80, 740
    g = Grid(xs0, xs1, zs0, zs1)
    d, Tn, Sn = centre_field(g, A, A.i(s0 - 120), A.i(m["lip"]), 70, hw=0.0)
    flight_clear = Clear(ctx.SA, step=6)
    rects = []                                              # placed footprints (x0, z0, x1, z1)
    Fg = gate(ctx, A, s0 + 12, "NEON CITY", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="cy_body_b", trim_mat="n_magenta",
              tower_u=34, board_half=36, tower_h=40)
    gx = [Fg.cell(u, v) for u in (-60, 60) for v in (-9, 9)]
    rects.append((min(c[0] for c in gx), min(c[1] for c in gx), max(c[0] for c in gx), max(c[1] for c in gx)))
    Fs = gate(ctx, A, s1 + 40, "SKYPORT", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="cy_body_l", trim_mat="n_magenta",
              tower_u=36, board_half=38, tower_h=40)
    gx = [Fs.cell(u, v) for u in (-60, 60) for v in (-9, 9)]
    rects.append((min(c[0] for c in gx), min(c[1] for c in gx), max(c[0] for c in gx), max(c[1] for c in gx)))

    def fits(x0, z0, w, dd, margin=4):
        for (a, b, c, e) in rects:
            if x0 - margin < c and x0 + w + margin > a and z0 - margin < e and z0 + dd + margin > b:
                return False
        return True

    def road_dist(x0, z0, w, dd):
        """Smallest centre-line distance over the footprint's corners and edge midpoints."""
        best = 1e9
        for (x, z) in ((x0, z0), (x0 + w, z0), (x0, z0 + dd), (x0 + w, z0 + dd), (x0 + w // 2, z0), (x0 + w // 2, z0 + dd),
                       (x0, z0 + dd // 2), (x0 + w, z0 + dd // 2), (x0 + w // 2, z0 + dd // 2)):
            ix, iz = x - g.x0, z - g.z0
            if 0 <= ix < g.nx and 0 <= iz < g.nz:
                best = min(best, float(d[ix, iz]))
        return best

    # ------------------------------------------------------------------ the mega arch: the tunnel of street 6 runs through a tower pair
    Ft = Frame(A, s1 - 250)                                 # street x = 1590, heading south
    for side in (-1, 1):
        ua, ub = (side * 13, side * 40) if side > 0 else (side * 40, side * 13)
        lo, hi = min(ua, ub), max(ua, ub)
        x0, z0 = Ft.cell(lo, -22)
        x1, z1 = Ft.cell(hi, 22)
        xa, xb = min(x0, x1), max(x0, x1)
        za, zb = min(z0, z1), max(z0, z1)
        h = 118 if side > 0 else 104
        tower_box(W, rng, xa, za, xb - xa + 1, zb - za + 1, h, "cy_body_b", "cy_glass_c" if side > 0 else "cy_glass_m", crown="spire", steps=2)
        rects.append((xa, za, xb, zb))
    # the bridge joining the two at the top and a glowing sky-gate
    Tt = Ft.T
    for u in range(-13, 14):
        for v in range(-6, 7):
            x, z = Ft.cell(u, v)
            for y in (Tt + 24, Tt + 25, Tt + 26):
                W.put(x, y, z, "cy_body_b")
            if v in (-6, 6):
                W.put(x, Tt + 27, z, "cy_glass_w")
    for u in range(-13, 14):
        for dy in range(0, 3):
            x, z = Ft.cell(u, -7)
            W.put(x, Tt + 24 + dy, z, "cy_glass_m" if (u // 2) % 2 else "cy_glass_c")
    # tunnel box around the road: walls at |u| 9..10 up to T+11, roof T+12 (over the 6-high clearing)
    for v in range(-14, 15):
        for side in (-1, 1):
            for y in range(Tt, Tt + 12):
                x, z = Ft.cell(side * 10, v)
                W.put(x, y, z, "cy_body_b" if y % 4 else "cy_glass_c" if side > 0 else "cy_glass_m")
        for u in range(-10, 11):
            x, z = Ft.cell(u, v)
            W.put(x, Tt + 12, z, "cy_body_b")
            if v % 7 == 0 and abs(u) in (0, 5):
                W.put(x, Tt + 11, z, "lit_white")

    # ------------------------------------------------------------------ skyscrapers: lots lining the streets (two rows), tall ones behind
    n_placed = 0

    def try_tower(x0, z0, w, dd, h, row):
        nonlocal n_placed
        if x0 + w > xs1 - 5 or z0 + dd > zs1 - 5 or x0 < xs0 + 5 or z0 < zs0 + 5:
            return False
        if road_dist(x0, z0, w, dd) < 17.0 or not fits(x0, z0, w, dd, 3):
            return False
        cx, cz = x0 + w // 2, z0 + dd // 2
        if math.hypot(cx - layout3.TOWER[0], cz - layout3.TOWER[1]) < 150:
            return False
        rects.append((x0, z0, x0 + w - 1, z0 + dd - 1))
        W.box((x0 - 3, -1, z0 - 3), (x0 + w + 2, -1, z0 + dd + 2), "cy_pave2", mode="fill")
        body = BODIES[int(rng.integers(0, len(BODIES)))]
        win = GLASS[int(rng.integers(0, len(GLASS)))]
        if rng.random() < 0.2:
            tower_round(W, rng, cx, cz, max(7, min(w, dd) // 2 - 1), h, body, win)
        else:
            tower_box(W, rng, x0, z0, w, dd, h, body, win, crown=["antenna", "antenna", "spire", "dome", "helipad"][int(rng.integers(0, 5))],
                      steps=int(rng.integers(0, 3)) if h > 64 else 0)
        n_placed += 1
        return True

    for row, (umin, umax, hmin, hmax, wmax, target) in enumerate(((17, 30, 30, 62, 26, 54), (44, 78, 58, 112, 32, 34))):
        got = 0
        for attempt in range(6000):
            i = int(rng.integers(i0, i1))
            side = 1 if rng.random() < 0.5 else -1
            w = int(rng.integers(15, wmax))
            dd = int(rng.integers(15, wmax))
            u = rng.uniform(umin, umax) + max(w, dd) * 0.5
            q = A.p[i] + A.r[i] * side * u + A.t[i] * rng.uniform(-8, 8)
            h = int(rng.integers(hmin, hmax))
            if try_tower(int(q[0] - w / 2), int(q[1] - dd / 2), w, dd, h, row):
                got += 1
                if got >= target:
                    break
    print("  skyscrapers:", n_placed)
    tick("towers")

    # ------------------------------------------------------------------ parks in the leftover blocks
    parks = 0
    for attempt in range(900):
        i = int(rng.integers(i0, i1))
        q = A.p[i] + A.r[i] * (1 if rng.random() < 0.5 else -1) * rng.uniform(20, 70)
        w, dd = int(rng.integers(14, 28)), int(rng.integers(14, 28))
        x0, z0 = int(q[0] - w / 2), int(q[1] - dd / 2)
        if x0 + w > xs1 - 5 or z0 + dd > zs1 - 5 or x0 < xs0 + 5 or z0 < zs0 + 5:
            continue
        if road_dist(x0, z0, w, dd) < 17.0 or not fits(x0, z0, w, dd, 2):
            continue
        if math.hypot(x0 - layout3.TOWER[0], z0 - layout3.TOWER[1]) < 150:
            continue
        rects.append((x0, z0, x0 + w - 1, z0 + dd - 1))
        W.box((x0, -1, z0), (x0 + w - 1, -1, z0 + dd - 1), "k_grass")
        for _ in range(int(w * dd / 70)):
            px, pz = x0 + int(rng.integers(2, w - 2)), z0 + int(rng.integers(2, dd - 2))
            oak(W, rng, px, 0, pz, h=int(rng.integers(4, 7)))
        W.box((x0 + w // 2 - 2, -1, z0 + dd // 2 - 2), (x0 + w // 2 + 2, -1, z0 + dd // 2 + 2), "cy_glass_c" if parks % 2 else "cy_glass_m")
        parks += 1
        if parks >= 14:
            break
    print("  parks:", parks)
    tick("parks")

    # ------------------------------------------------------------------ monorails on pylons and hovering cars
    def monorail(p0, p1, y, train=True):
        n = int(max(abs(p1[0] - p0[0]), abs(p1[1] - p0[1])))
        for k in range(0, n + 1):
            x = int(round(p0[0] + (p1[0] - p0[0]) * k / max(1, n)))
            z = int(round(p0[1] + (p1[1] - p0[1]) * k / max(1, n)))
            W.put(x, y, z, "cy_trim")
            W.put(x, y + 1, z, "cy_glass_c" if (k // 3) % 2 else "cy_glass_m")
            if k % 24 == 0:
                for yy in range(0, y):
                    W.put(x, yy, z, "cy_pier")
                if k % 48 == 0:
                    W.box((x - 2, y - 1, z - 1), (x + 2, y - 1, z + 1), "cy_trim")
        if train:
            k0 = n // 3
            for k in range(k0, k0 + 22):
                x = int(round(p0[0] + (p1[0] - p0[0]) * k / max(1, n)))
                z = int(round(p0[1] + (p1[1] - p0[1]) * k / max(1, n)))
                for dx in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for yy in (y + 2, y + 3, y + 4):
                            W.put(x + dx, yy, z + dz, "cy_glass_w" if yy == y + 3 and (k % 3) else "cy_body_l")

    # rails run along z through the west block and along x through the north block (clear of the roads)
    monorail((1000, -30), (1000, 330), 34)
    monorail((1700, 330), (1700, 560), 40, train=False)
    tick("monorail")

    for k in range(40):
        x = int(rng.uniform(xs0 + 20, xs1 - 20))
        z = int(rng.uniform(zs0 + 20, zs1 - 20))
        y = int(rng.integers(28, 70))
        if road_dist(x, z, 1, 1) < 24 or any(a - 6 < x < c + 6 and b - 6 < z < e + 6 for (a, b, c, e) in rects):
            continue
        if math.hypot(x - layout3.TOWER[0], z - layout3.TOWER[1]) < 140:
            continue
        al = int(rng.integers(0, 2))
        W.box((x, y, z), (x + (4 if al == 0 else 2), y + 1, z + (2 if al == 0 else 4)), "cy_body_w")
        W.box((x, y + 2, z), (x + (4 if al == 0 else 2), y + 2, z + (2 if al == 0 else 4)), "cy_glass_c" if k % 2 else "cy_glass_m")
        W.put(x + 1, y - 1, z + 1, "cy_glass_w")
    tick("cars")

    # ------------------------------------------------------------------ holograms and billboards
    # a giant wireframe globe over the first plaza
    gx, gy, gz = 1180, 80, 40
    for k in range(6):
        hoop(W, (gx, gy, gz), (math.cos(k * math.pi / 6), 0.0, math.sin(k * math.pi / 6)), 26, "cy_glass_c" if k % 2 else "cy_glass_m", thick=1)
    hoop(W, (gx, gy, gz), (0, 1, 0), 26, "cy_glass_y", thick=1)
    for y in range(-1, gy - 26):
        W.put(gx, y, gz, "cy_trim")
    W.cyl(gx, gz, -1, 1, 6, "cy_body_b")
    # billboards: pixel text / icons on black boards beside the road, facing the road.  Each one is put where the whole sight line
    # from the road to the board (and the board itself) is empty - never inside a tower.
    def place_board(txt, col, s_pref, side_pref, scale):
        for sc in range(scale, 0, -1):
            if txt.startswith("icon:"):
                wd, ht = len(ICONS[txt[5:]][0]) * sc, len(ICONS[txt[5:]]) * sc
            else:
                wd, ht, _ = text_cells(txt, sc)
            half = wd // 2 + 3
            for ds in sorted(range(-240, 241, 8), key=abs):
                s_ = s_pref + ds
                if not (s0 + 20 < s_ < s1 - 20):
                    continue
                F = Frame(A, s_)
                if not F.axis_aligned or abs(s_ - s_c2) < 130:
                    continue
                for side in (side_pref, -side_pref):
                    for L in (22, 28, 34, 40):
                        yb = F.T + 8 + ht                             # top edge of the text
                        ok = True
                        for u in range(9, L + 3):
                            for v in range(-half, half + 1):
                                x, z = F.cell(side * u, v)
                                for y in range(F.T + 1, yb + 3):
                                    if W.has(x, y, z):
                                        ok = False
                                        break
                                if not ok:
                                    break
                            if not ok:
                                break
                        if not ok:
                            continue
                        x, z = F.cell(side * L, 0)
                        face = heading(-side * F.r[0], -side * F.r[1])
                        if txt.startswith("icon:"):
                            put_bitmap(W, ICONS[txt[5:]], x, yb, z, sc, col, face=face, backing="b_black", pad=2)
                        else:
                            put_text(W, txt, x, yb, z, sc, col, face=face, backing="b_black", pad=2)
                        for dd in (-half + 1, half - 1):
                            px, pz = (x + dd, z) if face in ("n", "s") else (x, z + dd)
                            for y in range(max(-1, F.T), yb - ht - 2):
                                W.put(px, y, pz, "cy_trim")
                        return True
        print("  WARNING: no free spot for the '%s' billboard" % txt)
        return False

    boards = [(s0 + 140, "NEON", "n_cyan", 1, 3), (s0 + 420, "RACE", "n_magenta", -1, 3), (s_c2 - 220, "GO", "n_yellow", 1, 3), (s1 - 120, "SKY", "n_lime", -1, 3),
              (s0 + 700, "icon:heart", "n_magenta", 1, 2), (s0 + 900, "icon:rocket", "n_cyan", -1, 2), (s0 + 330, "icon:star", "n_yellow", -1, 2),
              (s_c2 + 200, "icon:bolt", "n_orange", 1, 2), (s1 - 40, "icon:arrow", "n_lime", 1, 2), (s0 + 560, "icon:planet", "n_lightblue", 1, 2)]
    for (s, txt, col, side, sc) in boards:
        place_board(txt, col, s, side, sc)
    tick("holograms")

    # ------------------------------------------------------------------ boosts, item boxes, lamps (the arch was built before the towers)
    for frac in (0.08, 0.17, 0.27, 0.36, 0.5, 0.62, 0.73, 0.84, 0.93):
        s = s0 + frac * (s1 - s0)
        if abs(s - s_c2) < 130:
            continue
        boost_pad(W, A, s, 14, 4, "n_magenta" if int(frac * 10) % 2 else "n_cyan", count=3, spacing=4)
    for frac in (0.12, 0.22, 0.32, 0.42, 0.56, 0.68, 0.78, 0.88):
        s = s0 + frac * (s1 - s0)
        if abs(s - s_c2) < 130:
            continue
        item_boxes(W, A, s, 7.0, n=4)
    # lamp posts with neon heads
    for i in range(i0 + 40, i1, 60):
        T = int(A.T[i])
        if abs(A.s[i] - s_c2) < 120:
            continue
        for side in (-1, 1):
            x, z = side_cell(A, i, side * 10.5)
            if not W.has(x, T, z) or True:
                for y in range(T, T + 6):
                    W.put(x, y, z, "cy_trim")
                W.put(x, T + 6, z, "cy_glass_y")
    # traffic lights at two crossings
    corner_furniture(W, A, i0, i1, 7.0, rmax=60.0, stacks=True, wall_w=0.0, skid=True, need_support=False)
    for s in (s0 + 330, s1 - 260):
        zebra(W, A, s, 7.0, width=3)
    billboards(W, A, i0 + 50, i1 - 50, 180, 12.0, start=1, need_support=False)
    tick("dressing")
