"""Zone 11: Celestial Ribbon - the finale: a floating road through deep space (landing strip for the plane, planets and rings,
a sun, a space station, constellations, a spiral galaxy), then the long Star Slide that comes down onto the start straight."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_rainbow import PLANETS, planet  # noqa: F401  (also registers the planet / nebula materials)

PAL = ["n_cyan", "n_lightblue", "n_blue", "n_purple", "n_magenta", "n_pink", "n_orange", "n_yellow", "n_lime"]
mat("rb3_deck", "Build_Black_Cube")
mat("rb3_star", "Build_Lightsource_White")
mat("rb3_star_y", "Build_Lightsource_YellowLight")
mat("rb3_star_c", "Build_Lightsource_Cyan")
mat("rb3_star_p", "Build_Lightsource_Pink")
mat("rb3_sun", "Build_Lightsource_YellowLight")
mat("rb3_sun2", ["Build_Lightsource_Orange", "Build_Lightsource_Yellow", "Build_Lightsource_YellowLight"])
mat("rb3_flare", "Build_Lightsource_Orange")
mat("rb3_panel", "Soil_Clay_Smooth_Blue")
mat("rb3_panel_g", "Metal_Zinc_Smooth")
mat("rb3_hull", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("rb3_hull_d", "Metal_Iron_Smooth")
mat("rb3_ring_dust", ["Soil_Clay_Smooth_Yellow2", "Soil_Clay_Smooth_Orange", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Pink"])
mat("rb3_moon", ["Rock_Chalk", "Rock_Calcite", "Rock_Slate"])
mat("rb3_ast", ["Rock_Basalt", "Rock_Volcanic", "Rock_Slate", "Rock_Basalt_Cobble"])


def spiral_galaxy(W, rng, cx, cy, cz, R, arms=3, n=900):
    for arm in range(arms):
        for t in np.linspace(0.08, 1.0, n // arms):
            a = arm * 2 * math.pi / arms + 5.2 * t
            rr = 6 + R * t
            px, py, pz = cx + rr * math.cos(a), cy + 3.0 * math.sin(a * 0.5) + rng.normal(0, 1.4 * (1 - t) + 0.5), cz + rr * math.sin(a) * 0.5
            col = ["rb3_star", "rb3_star_c", "rb3_star_p", "rb3_star_y"][int(rng.integers(0, 4)) if t > 0.25 else 3]
            W.put(int(px + rng.normal(0, 1.8)), int(py), int(pz + rng.normal(0, 1.6)), col, keep=True)
    W.ellipsoid(cx, cy, cz, 6, 4, 6, "rb3_star_y", mode="shell")


def constellation(W, pts, cx, cy, cz, scale, plane="z", node="rb3_star_y", line="n_cyan"):
    P = [(cx + p[0] * scale, cy + p[1] * scale, cz) if plane == "z" else (cx, cy + p[1] * scale, cz + p[0] * scale) for p in pts]
    for k in range(len(P) - 1):
        if pts[k][2] if len(pts[k]) > 2 else True:
            W.line(P[k], P[k + 1], line, keep=True)
    for p in P:
        W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), 2, 2, 2, node)


def station(W, rng, cx, cy, cz, along="x"):
    """A small space station: a long hull, ring section, two solar wings, a dish."""
    for k in range(-14, 15):
        x, z = (cx + k, cz) if along == "x" else (cx, cz + k)
        r = 3.2 if abs(k) < 11 else 2.2
        W.cyl(x, z, cy - 3, cy + 3, r, "rb3_hull", mode="wall") if False else None
    if along == "x":
        for k in range(-16, 17):
            W.box((cx + k, cy - 2, cz - 2), (cx + k, cy + 2, cz + 2), "rb3_hull" if k % 6 else "rb3_hull_d", mode="walls")
        for sgn in (-1, 1):
            W.box((cx + sgn * 6, cy, cz - 22), (cx + sgn * 6 + 1, cy, cz + 22), "rb3_panel")
            for k in range(-22, 23, 4):
                W.put(cx + sgn * 6, cy + 1, cz + k, "rb3_panel_g")
    else:
        for k in range(-16, 17):
            W.box((cx - 2, cy - 2, cz + k), (cx + 2, cy + 2, cz + k), "rb3_hull" if k % 6 else "rb3_hull_d", mode="walls")
        for sgn in (-1, 1):
            W.box((cx - 22, cy, cz + sgn * 6), (cx + 22, cy, cz + sgn * 6 + 1), "rb3_panel")
    hoop(W, (cx, cy, cz), (1, 0, 0) if along == "z" else (0, 0, 1), 8, "rb3_hull_d", thick=2)
    W.put(cx, cy + 3, cz, "n_red")
    for k in (-10, 0, 10):
        W.put(cx + (k if along == "x" else 3), cy + 3, cz + (3 if along == "x" else k), "lit_cyan")
    W.ellipsoid(cx + (18 if along == "x" else 0), cy + 1, cz + (0 if along == "x" else 18), 3, 3, 3, "rb3_hull_d", mode="shell", part="upper")


def ctx_axis_s(P, s, window=150):
    """The nearest position to s (within `window`) where the road runs along an axis, so lettering across it is not a staircase."""
    for ds in sorted(range(-window, window + 1, 2), key=abs):
        if 6.0 <= s + ds <= P.length - 6.0 and Frame(P, s + ds).axis_aligned:
            return s + ds
    return s


def build(ctx):
    W, C, rng = ctx.W, ctx.C, ctx.rng
    m = C.marks
    L = C.length
    tick = W.tick
    s_slide = 1100.0

    # ------------------------------------------------------------------ roads: landing strip, ribbon, star slide
    def wall(s, u, h):
        return PAL[int(s // 9) % len(PAL)]

    def edge(s, u):
        return PAL[int(s // 5) % len(PAL)] if int(s // 5) % 3 else "n_white"

    strip = KartStyle(hw=10.0, verge="n_black", edge=edge, rail="mesh", wall=wall, wall_h=1, deck="rb3_deck", lanes=(-5.0, 0.0, 5.0), lane_dash=(6.0, 6.0),
                      edge_line=False, curbs=())
    ribbon = KartStyle(hw=8.0, verge="n_black", edge=edge, rail="mesh", wall=wall, wall_h=1, deck="rb3_deck", lanes=(0.0,), lane_dash=(4.0, 4.0),
                       edge_line=False, curbs=())
    slide = KartStyle(hw=8.5, verge="n_black", edge=edge, rail="mesh", wall=wall, wall_h=1, deck="rb3_deck", lanes=(-2.9, 2.9), lane_dash=(4.0, 4.0),
                      edge_line=False, curbs=(), piers=dict(mat="rb3_hull_d", every=40, size=2, inset=2.0, cap="rb3_deck", beam="rb3_hull_d", bottom=0))
    stamp(ctx, C, 0, 310, strip)
    for k, s in enumerate(np.arange(310, 334, 4.0)):
        stamp(ctx, C, s - 1, s + 5, KartStyle(hw=10.0 - 2.0 * (k + 1) / 6.0, verge="n_black", edge=edge, rail="mesh", deck="rb3_deck", lanes=(), curbs=(), edge_line=False), steps=False)
    stamp(ctx, C, 330, s_slide, ribbon)
    stamp(ctx, C, s_slide - 2, L, slide)
    tick("road")

    # ------------------------------------------------------------------ landing platform: a hex deck with neon grid under the strip, transform gate
    for s in np.arange(0, 310, 2.0):
        F = Frame(C, s)
        for u in range(-16, 17):
            if abs(u) >= 11 and (int(s) // 2 + u) % 4 == 0:
                x, z = F.cell(u, 0)
                W.put(x, F.T - 1, z, "n_cyan" if u > 0 else "n_magenta")
    for k, r in enumerate((15, 17.5, 20)):
        Fk = Frame(C, 26 + k * 8)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 8, cz), (Fk.t[0], 0, Fk.t[1]), r, ["n_cyan", "n_white", "n_lime"][k], thick=2, ymin=Fk.T + 1)
    for s in np.arange(4, 300, 10.0):
        for side in (-1, 1):
            F = Frame(C, s)
            x, z = F.cell(side * 12.5, 0)
            W.put(x, F.T + 1, z, "lit_white" if int(s // 10) % 2 else "lit_cyan")
    gate(ctx, C, 120.0, "CELESTIAL RIBBON", scale=2, text_mat="n_white", board_mat="b_black", frame_mat="rb3_hull_d", trim_mat="n_cyan", tower_u=36, board_half=38,
         tower_h=34)
    tick("landing")

    # ------------------------------------------------------------------ the 3D samples of the road for clearance checks
    pts = np.stack([C.p[::3, 0], C.T[::3].astype(float), C.p[::3, 1]], 1)

    def clear(x, y, z):
        return float(np.sqrt(((pts - np.array([x, y, z])) ** 2).sum(1)).min())

    placed = []

    def spot(R, margin, region, yr, tries=500):
        for _ in range(tries):
            x, z = rng.uniform(region[0], region[1]), rng.uniform(region[2], region[3])
            y = rng.uniform(*yr)
            if clear(x, y, z) < R + margin:
                continue
            if any(math.dist((x, y, z), (px, py, pz)) < R + pr + 14 for px, py, pz, pr in placed):
                continue
            placed.append((x, y, z, R))
            return int(x), int(y), int(z)
        return None

    reg = (-1160, -380, -540, 60)
    sign_spots = []
    for (s_, txt_) in ((s_slide + 100, "WARP"), (s_slide + 330, "HOME")):
        s_ = ctx_axis_s(C, s_)
        F_ = Frame(C, s_)
        cx_, cz_ = F_.cell(0, 0)
        sign_spots.append((s_, txt_))
        placed.append((cx_, F_.T + 50, cz_, 60))              # keep planets / asteroids away from the lettering

    # ------------------------------------------------------------------ planets, a ringed giant, moons, a sun, nebulae
    sizes = [30, 22, 17, 26, 12, 10, 8]
    for k, R in enumerate(sizes):
        q = spot(R, 22, reg, (30, 150))
        if q:
            planet(W, rng, q[0], q[1], q[2], R, PLANETS[k % len(PLANETS)], ring=(k in (0, 3)))
            if k in (0, 3):
                pass
    print("  ribbon planets:", len(placed))
    q = spot(34, 30, (-1150, -500, -500, -40), (60, 130))
    if q:
        sx, sy, sz = q
        W.ellipsoid(sx, sy, sz, 33, 33, 33, "rb3_sun", mode="shell")
        W.ellipsoid(sx, sy, sz, 26, 26, 26, "rb3_sun2", mode="shell")
        for k in range(36):
            a = k * 2 * math.pi / 36
            for t in range(34, 52):
                W.put(int(sx + t * math.cos(a)), sy + int(t * math.sin(a) * 0.8), sz, "rb3_flare")
        ctx.marks["sun"] = (sx, sy, sz)
    for k in range(5):
        R = int(rng.integers(14, 24))
        q = spot(R, 20, reg, (30, 130))
        if q:
            cloud(W, rng, q[0], q[1], q[2], R, int(R * 0.5), int(R * 0.8), n=2, m=["neb_pink", "neb_blue", "neb_teal"][k % 3])
    for k in range(70):
        R_ = int(rng.integers(2, 5))
        q = spot(R_, 10, reg, (25, 140), tries=60)
        if q:
            W.ellipsoid(q[0], q[1], q[2], R_ + 1, R_, R_ + 1, "rb3_ast")
    tick("planets")

    # ------------------------------------------------------------------ a space station beside the road, satellites
    for (s, side, along) in ((500.0, 1, "x"), (860.0, -1, "z")):
        F = Frame(C, s)
        c = F.p
        q = (c[0] + F.r[0] * side * 40, F.T + 6, c[1] + F.r[1] * side * 40)
        if clear(q[0], q[1], q[2]) > 28:
            station(W, rng, int(q[0]), int(q[1]), int(q[2]), along=along)
    tick("station")

    # ------------------------------------------------------------------ stars, constellations, comets, a spiral galaxy
    clear_sb = Clear(ctx.SB, step=3)
    n_star = 3200
    sx_ = rng.uniform(reg[0] - 80, reg[1] + 160, n_star)
    sz_ = rng.uniform(reg[2] - 80, reg[3] + 120, n_star)
    sy_ = rng.uniform(8, 190, n_star)
    cnt = 0
    for x, y, z in zip(sx_, sy_, sz_):
        if clear(x, y, z) < 11 or clear_sb(x, y, z) < 12:
            continue
        r_ = rng.random()
        W.put(int(x), int(y), int(z), "rb3_star" if r_ < 0.78 else "rb3_star_y" if r_ < 0.88 else "rb3_star_c" if r_ < 0.95 else "rb3_star_p", keep=True)
        cnt += 1
    print("  ribbon stars:", cnt)
    DIPPER = [(0, 0), (1.2, -0.3), (2.3, -0.1), (3.2, 0.8), (4.6, 1.1), (5.0, 2.4), (3.6, 2.5), (3.2, 0.8)]
    ORION = [(0, 0, 0), (1, 2, 1), (2, 4, 1), (3, 2, 1), (4, 0, 1), (2, 2, 0), (1.4, 1.2, 1), (2.6, 1.2, 0)]
    for (cx, cy, cz, sc, pl, pts_) in ((-1130, 150, -300, 12, "z", DIPPER), (-980, 175, -520, 10, "x", [(p[0], p[1]) for p in ORION]),
                                      (-560, 170, -480, 11, "z", DIPPER)):
        if clear(cx, cy, cz) > 30:
            constellation(W, pts_, cx, cy, cz, sc, plane=pl)
    spiral_galaxy(W, rng, -1000, 175, -80, 70)
    for k in range(5):
        x, z = rng.uniform(reg[0], reg[1]), rng.uniform(reg[2], reg[3])
        y = rng.uniform(40, 150)
        if clear(x, y, z) < 25:
            continue
        ang = rng.uniform(0, 2 * math.pi)
        W.ellipsoid(int(x), int(y), int(z), 3, 3, 3, "lit_cyan")
        for t in range(1, 50):
            px, py, pz = x - t * math.cos(ang), y + t * 0.3, z - t * math.sin(ang)
            for _ in range(2):
                W.put(int(px + rng.normal(0, 0.7 + t * 0.03)), int(py + rng.normal(0, 0.7)), int(pz + rng.normal(0, 0.7 + t * 0.03)),
                      "lit_white" if t < 22 else "lit_cyan", keep=True)
    tick("space")

    # ------------------------------------------------------------------ ribbon dressing: boosts, items, light posts, gravity rings, star dust under the deck
    for s in np.arange(350.0, s_slide - 20, 48.0):
        boost_pad(W, C, s, 14, 5, "n_white", count=3, spacing=4)
    for s in np.arange(340.0, s_slide - 20, 66.0):
        item_boxes(W, C, s, 8.0, n=5)
    for s in np.arange(s_slide + 10, L - 40, 40.0):
        boost_pad(W, C, s, 16, 5, "n_cyan", count=4, spacing=4)
    for s in np.arange(340.0, s_slide, 5.0):
        F = Frame(C, s)
        for u in range(-6, 7, 3):
            if rng.random() < 0.5:
                x, z = F.cell(u + int(rng.integers(-1, 2)), 0)
                W.put(x, F.T - 2 - int(rng.integers(0, 4)), z, "rb3_star_c" if rng.random() < 0.5 else "rb3_star_p")
    for s in np.arange(s_slide + 12, L - 50, 9.0):
        Fk = Frame(C, s)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 8, cz), (Fk.t[0], 0, Fk.t[1]), 13.5, PAL[int(s // 9) % len(PAL)], thick=1, ymin=Fk.T + 1)
    for k in range(7):
        Fk = Frame(C, L - 36 + k * 3.2)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 6, cz), (Fk.t[0], 0, Fk.t[1]), 10 + k * 1.5, ["n_white", "n_yellow", "n_cyan"][k % 3], thick=2, ymin=Fk.T + 1)
    for (s, txt) in sign_spots:
        F = Frame(C, s)
        ftext(W, F, txt, 0, F.T + 52, 0, 3, lambda c_, r_: PAL[(c_ // 6) % len(PAL)])
        w, h, cells = text_cells(txt, 3)                      # a second layer behind it makes the letters 2 blocks thick
        for (c_, r_) in cells:
            x, z = F.cell(int(math.floor(-w / 2.0 + c_ + 0.5)), 1)
            W.put(x, F.T + 52 - r_, z, PAL[(c_ // 6) % len(PAL)])
    corner_furniture(W, C, C.i(0), C.i(L - 10), 8.0, rmax=90.0, stacks=False, skid=False, need_support=False, wall_w=1.0)
    for s in np.arange(60.0, L - 40, 90.0):
        F = Frame(C, s)
        for side in (-1, 1):
            x, z = F.cell(side * 11.0, 0)
            for y in range(F.T, F.T + 7):
                W.put(x, y, z, "kp_pole")
            W.put(x, F.T + 7, z, "kp_glow_cyan")
    tick("dressing")
