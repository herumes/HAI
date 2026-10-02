"""Zone 0: Nova Station Spaceport - the start / finish.  Road flush with the ground (y = -1), decals on layer 0.

Race-control building with a control tower behind the finish line, two grandstands, a starting-light gantry, a pit lane with
twelve garages, launch pads with rockets, a planet monument, the Star Slide arrival rings, space-elevator mast, floodlights."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from zone_start import burst, floodlight, grandstand, rocket, trophy  # noqa: F401  (also registers their materials)
from path import Path

mat("ns_floor", ["Rock_Stone_Brick_Smooth", "Rock_Stone_Brick_Smooth", "Rock_Marble_Brick_Smooth"])
mat("ns_floor2", "Rock_Basalt_Brick_Smooth")
mat("ns_hull", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("ns_hull2", "Soil_Clay_Smooth_Grey2")
mat("ns_panel", "Metal_Iron_Smooth")
mat("ns_panel2", "Metal_Zinc_Smooth")
mat("ns_glass", "Build_Lightsource_Cyan")
mat("ns_glass2", "Build_Lightsource_Blue")
mat("ns_accent", "Soil_Clay_Smooth_Red")
mat("ns_accent_b", "Soil_Clay_Smooth_Blue")
mat("ns_rail", "Metal_Iron_Fence")
mat("ns_dish", "Metal_Iron_Smooth")
mat("ns_truss", "Metal_Zinc_Smooth")
mat("ns_fuel", ["Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Grey"])
mat("ns_planet", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Green"])
mat("ns_ring", ["NoCube_Neon_Block_Magenta", "NoCube_Neon_Block_Purple"])
mat("ns_star", "Build_Lightsource_White")


def line_path(pts):
    p = Path(pts)
    p.T[:] = -1
    return p


def erase_rails(W, x0, x1, z0, z1, y=0):
    for x in range(x0, x1 + 1):
        for z in range(z0, z1 + 1):
            for yy in (y, y + 1):
                g = W.get(x, yy, z)
                if g is not None and g[0] in ("k_armco", "k_armco_c", "k_chevron"):
                    W.erase(x, yy, z)


TEAMS = [("clay_red", "n_red"), ("clay_blue", "n_blue"), ("clay_yellow", "n_yellow"), ("clay_green", "n_green"),
         ("clay_orange", "n_orange"), ("clay_purple", "n_purple")]


def control_building(W, rng, cx, z0, z1, hw_=44):
    """Race control: a long white building with a window band over the road and a round tower in the middle."""
    x0, x1 = cx - hw_, cx + hw_
    W.box((x0, 0, z0), (x1, 9, z1), "stand_body", mode="shell")
    # ground floor: open arcade on the south face with pillars every 6
    for x in range(x0 + 2, x1 - 1):
        if (x - x0) % 6 != 0:
            for y in range(1, 6):
                W.erase(x, y, z1)
    for x in range(x0, x1 + 1, 6):
        for y in range(0, 7):
            W.put(x, y, z1 + 1, "ns_panel")
        W.put(x, 7, z1 + 1, "n_cyan")
    W.box((x0, 10, z0), (x1, 10, z1), "ns_panel2")
    # upper level: glass band facing the road, red / white trim
    W.box((x0, 11, z0), (x1, 21, z1), "ns_hull", mode="shell")
    for x in range(x0 + 1, x1):
        for y in range(13, 19):
            W.put(x, y, z1, "ns_glass")
    for x in range(x0 + 3, x1, 6):
        for y in range(11, 22):
            W.put(x, y, z1, "ns_hull2")
    W.box((x0 - 1, 22, z0 - 1), (x1 + 1, 22, z1 + 2), "ns_panel2")
    for x in range(x0 - 1, x1 + 2):
        W.put(x, 22, z1 + 2, "n_cyan" if (x // 3) % 2 == 0 else "n_white")
    # a continuous red stripe under the glass
    for x in range(x0, x1 + 1):
        W.put(x, 12, z1 + 1, "ns_accent")
    # balcony rail
    for x in range(x0, x1 + 1):
        W.put(x, 11, z1 + 2, "ns_rail")
    W.box((x0, 11, z1 + 1), (x1, 11, z1 + 2), "ns_panel2", mode="floor")
    # tower in the middle
    tz = (z0 + z1) // 2
    W.cyl(cx, tz, 23, 52, 8, "ns_hull", mode="wall")
    for y in range(26, 50, 8):
        W.cyl(cx, tz, y, y, 8.6, "ns_panel2", mode="wall")
    for y in range(24, 52, 4):
        for a in range(0, 360, 45):
            q = (cx + 8.6 * math.cos(math.radians(a)), tz + 8.6 * math.sin(math.radians(a)))
            W.put(int(round(q[0])), y, int(round(q[1])), "ns_glass2")
    W.cyl(cx, tz, 53, 53, 11, "ns_panel2")
    W.cyl(cx, tz, 54, 58, 10, "ns_glass", mode="wall")
    for a in range(0, 360, 30):
        for y in range(54, 59):
            W.put(int(round(cx + 10 * math.cos(math.radians(a)))), y, int(round(tz + 10 * math.sin(math.radians(a)))), "ns_panel2")
    W.cyl(cx, tz, 59, 59, 12, "ns_panel2")
    W.cyl(cx, tz, 60, 60, 10, "ns_hull")
    W.ellipsoid(cx, 61, tz, 8, 6, 8, "ns_panel", mode="shell", part="upper")
    for y in range(66, 86):
        W.put(cx, y, tz, "ns_panel")
    for y in range(70, 86, 4):
        W.box((cx - 2, y, tz), (cx + 2, y, tz), "ns_panel")
    W.put(cx, 86, tz, "n_red")
    W.put(cx, 87, tz, "n_red")
    # radar dishes on the roof
    for dx in (-30, 30):
        for y in range(23, 29):
            W.put(cx + dx, y, z0 + 8, "ns_dish")
        W.ellipsoid(cx + dx, 31, z0 + 8, 6, 3, 6, "ns_dish", mode="shell", part="upper")
        W.put(cx + dx, 34, z0 + 8, "lit_red")


def gantry(W, F, T):
    """Starting-light gantry across the road at the finish line: two towers, a truss, five light pods, lettering."""
    for side in (-1, 1):
        u0 = side * 12
        ua, ub = sorted((u0 - 2, u0 + 2))
        lbox(W, F, ua, ub, -2, 2, T + 1, T + 26, "k_andesite")
        lbox(W, F, ua, ub, -2, 2, T, T + 3, "k_finish_line")
        for y in range(T + 4, T + 26, 4):
            lbox(W, F, ua - 0, ub + 0, -2, 2, y, y, "n_cyan")
        lbox(W, F, ua + 1, ub - 1, -1, 1, T + 27, T + 29, "ns_panel2")
        x, z = F.cell(u0, 0)
        W.put(x, T + 30, z, "lit_white")
    # beam: a truss two blocks deep
    for u in range(-13, 14):
        for v in (-1, 0, 1):
            x, z = F.cell(u, v)
            W.put(x, T + 21, z, "ns_truss")
            W.put(x, T + 24, z, "ns_truss")
        x, z = F.cell(u, 0)
        for y in (T + 22, T + 23):
            if u % 2 == 0:
                W.put(x, y, z, "ns_truss")
    # five starting lights
    for k, u in enumerate((-8, -4, 0, 4, 8)):
        for du in range(-1, 1):
            for dy in range(0, 3):
                x, z = F.cell(u + du, -2)
                W.put(x, T + 17 + dy, z, "n_red" if k < 3 else "n_green")
        for du in range(-1, 1):
            for dy in range(0, 3):
                x, z = F.cell(u + du, -1)
                W.put(x, T + 17 + dy, z, "b_black")
    for u in range(-13, 14):
        for dy in range(15, 21):
            x, z = F.cell(u, -1)
            if not W.has(x, T + dy, z):
                W.put(x, T + dy, z, "b_black")
    # a plate above the beam (26 wide, between the towers) with GO in 2x lettering: 22 wide, so it fits with a block to spare
    lbox(W, F, -12, 12, -1, 0, T + 26, T + 41, "b_black")
    for u in range(-12, 13):
        x, z = F.cell(u, -1)
        W.put(x, T + 41, z, "n_cyan" if u % 2 else "n_magenta")
        W.put(x, T + 26, z, "n_cyan" if u % 2 else "n_magenta")
    lsign(W, F, "GO", 0, T + 39, -2, 2, "n_yellow")
    for u in range(-13, 14):
        x, z = F.cell(u, -1)
        W.put(x, T + 25, z, "n_cyan" if u % 2 else "n_magenta")


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    z_end = m["candy"]
    fin_s = 420.0
    FIN = Frame(A, fin_s)
    T0 = -1

    # ------------------------------------------------------------------ road
    hw = 8.0
    st = KartStyle(hw=hw, rail="armco", lanes=(-4.0, 4.0), lane_dash=(4.0, 4.0), curbs=())
    stamp(ctx, A, 0, z_end - 36, st)
    for k, s in enumerate(np.arange(z_end - 36, z_end, 6.0)):                         # a short taper to the Candy road width
        w_ = hw - 1.0 * (k + 1) / 6.0
        stamp(ctx, A, s - 1.0, s + 7.0, KartStyle(hw=w_, rail="armco", lanes=(), curbs=(), edge_line=True), steps=False)

    # ------------------------------------------------------------------ the line, the grid, boosts, items
    finish_band(W, A, fin_s, hw, rows=2)
    start_grid(W, A, fin_s, hw, rows=5, row_gap=6.0, off=3.6)
    for s in np.arange(250.0, 380.0, 26.0):
        road_arrows(W, A, s, n=1, u=-4.0)
        road_arrows(W, A, s, n=1, u=4.0)
    for s in np.arange(470.0, 580.0, 26.0):
        road_arrows(W, A, s, n=1, u=-4.0)
        road_arrows(W, A, s, n=1, u=4.0)
    boost_pad(W, A, 450, 14, 4, "n_orange", count=3, spacing=4)
    boost_pad(W, A, 520, 14, 4, "n_yellow", count=3, spacing=4)
    for s in (475, 545):
        item_boxes(W, A, s, hw, n=4)

    # ------------------------------------------------------------------ gantry, race control, stands
    gantry(W, FIN, T0)
    control_building(W, rng, 0, -60, -20)
    grandstand(W, rng, -346, -52, -14, 12)
    grandstand(W, rng, 52, 176, -14, 12)
    # giant lettering ABOVE the west stand's roof (the roof is at y 21): one line, on a black plate and four masts
    put_text(W, "COSMIC ODYSSEY", -199, 46, -33, 2, "n_cyan", face="s", backing="b_black", pad=3)
    for x in (-276, -226, -172, -122):
        W.box((x, 22, -36), (x + 1, 44, -35), "ns_panel")
    # "GRAND PRIX" on the control building, above its roof slab (y 22), in front of the tower
    put_text(W, "GRAND PRIX", 0, 31, -19, 1, "n_yellow", face="s", backing="b_black", pad=2)
    for x in (-33, 33):
        W.box((x, 23, -20), (x, 32, -19), "ns_panel")
    # scoreboard wall on the east stand
    W.box((78, 23, -30), (150, 40, -29), "b_black")
    put_text(W, "LAP 3 OF 3", 114, 38, -28, 1, "n_lime", face="s")
    put_text(W, "GO GO GO", 114, 30, -28, 1, "n_orange", face="s")
    for x in (80, 114, 148):
        W.box((x, 22, -30), (x, 23, -29), "ns_panel")

    # ------------------------------------------------------------------ the plaza and lawns
    for z in range(-30, 140):
        x0, x1 = (-445, 215) if z < 100 else (-300, 120)
        W.put_arr(np.arange(x0, x1 + 1), np.full(x1 - x0 + 1, -1), np.full(x1 - x0 + 1, z), "ns_floor", keep=True)
    for z in range(-13, -9):
        W.put_arr(np.arange(-340, 176), np.full(516, -1), np.full(516, z), "k_grass")
    for z in range(36, 100):
        W.put_arr(np.arange(-250, 110), np.full(360, -1), np.full(360, z), "k_grass")

    # trackside: catch fence, billboards, flags, floodlight posts
    for x in range(-340, 180):
        W.put(x, 0, -10, "k_catch", 0)
    for k, x in enumerate(range(-330, 180, 28)):
        W.put(x, 0, -12, "k_billboard", facing_driver_rot("e"))
        W.put(x + 14, 0, -12, "k_flag", flag_rot("e"))
    for x in range(-340, 180, 14):
        W.put(x, 0, -11, "k_flood", lean_rot("s"))
    for u in (-10, 10):
        x, z = FIN.cell(u, 0)
        W.put(x, 0, z, "k_flag", flag_rot("e"))

    # ------------------------------------------------------------------ the Star Slide arrival: rings over the first stretch
    for k in range(9):
        Fk = Frame(A, 46 + k * 7.5)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 8, cz), (Fk.t[0], 0, Fk.t[1]), 12 + 0.9 * k,
             ["n_white", "n_cyan", "n_magenta", "n_yellow"][k % 4], thick=1, ymin=0)
    gate(ctx, A, 16.0, "WELCOME HOME", scale=1, text_mat="n_white", board_mat="b_black", frame_mat="ns_panel", trim_mat="n_cyan",
         tower_u=20, board_half=24, tower_h=26)

    # ------------------------------------------------------------------ pit lane: entry, lane, exit, wall island, twelve garages
    PZ = 18
    X0, X1 = -296, 40
    entry = line_path(layout_spline([(-352, 0), (-336, 1), (-322, 6), (-308, 13), (-296, PZ)]))
    lane = line_path([(X0, PZ), (X1, PZ)])
    exit_ = line_path(layout_spline([(X1, PZ), (X1 + 14, PZ - 1), (X1 + 30, 13), (X1 + 48, 6), (X1 + 64, 1), (X1 + 80, 0)]))
    erase_rails(W, -360, -296, 6, 12)
    erase_rails(W, X1, X1 + 84, 6, 12)
    pit = lambda rail: KartStyle(hw=3.5, rail=rail, lanes=(), edge_line=False, curbs=(), verge="k_conc")
    stamp_kart_road(W, entry, 0, len(entry) - 1, pit(None), steps=False)
    stamp_kart_road(W, exit_, 0, len(exit_) - 1, pit(None), steps=False)
    stamp_kart_road(W, lane, 0, len(lane) - 1, KartStyle(hw=3.5, rail_fn=lambda s, side: "jersey" if side < 0 else None, lanes=(), edge_line=False,
                                                        curbs=(), verge="k_conc"), steps=False)
    for x in range(X0, X1 + 1):                                  # the pit wall island between track and lane
        for z in range(9, 14):
            W.put(x, -1, z, "k_conc", keep=True)
        W.put(x, 0, 11, "k_catch", 0)
    for x in range(X0 + 4, X1, 12):
        W.put(x, 0, 12, "k_flood", lean_rot("s"))
        W.put(x + 6, 0, 10, "k_redblock")
    for x in range(X0 - 14, X1 + 20):
        for z in (22, 23):
            W.put(x, -1, z, "k_conc")
    n_gar = 12
    for gi in range(n_gar):
        gx = X0 + 6 + gi * 26
        col, neon = TEAMS[gi % len(TEAMS)]
        for x in range(gx + 6, gx + 9):
            for z in (19, 20, 21):
                W.put(x, 0, z, "k_pitbox")
        W.box((gx, -1, 24), (gx + 15, -1, 36), "k_conc", mode="floor")
        W.box((gx, 0, 24), (gx + 15, 7, 36), "garage_wall", mode="shell")
        for x in range(gx + 2, gx + 14):
            for y in range(0, 5):
                W.erase(x, y, 24)
        W.box((gx - 1, 8, 23), (gx + 16, 8, 37), col, mode="fill")
        for x in range(gx, gx + 16):
            W.put(x, 5, 23, neon)
            W.put(x, 6, 23, neon)
        for x in range(gx + 2, gx + 14, 3):
            W.put(x, 0, 35, "k_workwall", workwall_rot("n"))
        W.put(gx + 1, 0, 25, "k_work", 0)
        W.put(gx + 14, 0, 25, "k_work", 0)
        W.put(gx + 3, 0, 30, "k_stack15")
        W.put(gx + 12, 0, 30, "k_stack12")
        W.put(gx + 7, 0, 31, "k_redblock")
        for y in range(0, 6):
            W.put(gx + 1, y, 24, col)
            W.put(gx + 14, y, 24, col)
        W.put_arr(np.arange(gx + 1, gx + 15), np.full(14, 5), np.full(14, 24), col)
        for x in range(gx + 1, gx + 15):
            W.put(x, 6, 22, "k_roof_slate" if gi % 2 == 0 else "k_roof_calcite", 2)
        for x in (gx + 1, gx + 8, gx + 14):
            for y in range(0, 6):
                W.put(x, y, 22, "k_andesite")
        W.put(gx + 3, 0, 21, "k_cone")
        W.put(gx + 12, 0, 21, "k_cone_s")
    zebra(W, lane, 150.0, 3.5, width=3)
    for z in range(15, 22):
        W.put(X0 + 8, 0, z, "k_bump")
        W.put(X0 + 9, 0, z, "k_bump")
    bunting(W, A, A.i(0), A.i(z_end + 20), 28, 10.5, 7)

    # ------------------------------------------------------------------ podium, trophy, launch pads with rockets, planet monument
    px, pz = 30, 52
    W.box((px - 5, 0, pz - 5), (px + 16, 0, pz + 5), "k_andesite")
    W.put(px, 1, pz, "k_pod2")
    W.put(px + 5, 1, pz, "k_pod1")
    W.put(px + 10, 1, pz, "k_pod3")
    W.put(px + 15, 1, pz, "k_trophy")
    for (cx_, cz_) in ((px - 5, pz - 5), (px + 16, pz - 5), (px - 5, pz + 5), (px + 16, pz + 5)):
        W.put(cx_, 1, cz_, "k_finish_line")
        W.put(cx_, 2, cz_, "k_flag", flag_rot("e"))
    trophy(W, 76, 72)
    for (cx_, cz_, h_, r_, b1, b2) in ((-150, 66, 58, 8, "hull_white", "hull_red"), (-96, 72, 44, 6, "hull_white", "clay_blue"),
                                       (-52, 66, 70, 9, "clay_yellow", "hull_white")):
        W.cyl(cx_, cz_, 0, 1, r_ + 5, "ns_floor2")
        rocket(W, cx_, cz_, 0, h_, r_, b1, b2)
        # gantry tower beside each rocket
        gx = cx_ + r_ + 4
        for y in range(0, h_ - 2):
            for dx, dz in ((0, 0), (3, 0), (0, 3), (3, 3)):
                W.put(gx + dx, y, cz_ + dz - 1, "ns_truss")
            if y % 6 == 0:
                W.box((gx, y, cz_ - 1), (gx + 3, y, cz_ + 2), "ns_truss", mode="walls")
        for y in range(10, h_ - 4, 12):
            W.box((cx_ + r_ - 1, y, cz_), (gx, y, cz_ + 1), "ns_truss")
    # fuel tanks
    for k, (tx, tz) in enumerate(((-205, 80), (-190, 90), (-215, 96))):
        W.cyl(tx, tz, 0, 9, 5, "ns_fuel", mode="wall")
        W.cyl(tx, tz, 10, 10, 5, "ns_fuel")
        W.ellipsoid(tx, 10, tz, 5, 3, 5, "ns_fuel", mode="shell", part="upper")
        for y in (2, 5, 8):
            W.cyl(tx, tz, y, y, 5.4, "ns_accent", mode="wall")
    # planet monument: a floating banded sphere with a neon ring, over a plinth
    cx_, cy_, cz_ = 130, 70, 100
    n = 26
    ox, oy, oz = np.meshgrid(np.arange(-n, n + 1), np.arange(-n, n + 1), np.arange(-n, n + 1), indexing="ij")
    d2 = ox * ox + oy * oy + oz * oz
    sel = (d2 <= (24.5) ** 2) & (d2 > 22.0 ** 2)
    ox, oy, oz = ox[sel], oy[sel], oz[sel]
    wob = 2.2 * np.sin(ox * 0.25) + 1.6 * np.sin(oz * 0.31)
    idx = (((oy + 24 + wob) / 49.0) * 6).astype(int) % 6
    bands = ["clay_cyan", "clay_blue", "clay_white", "clay_cyan", "clay_green", "clay_blue"]
    for k in range(6):
        q = idx == k
        W.put_arr(cx_ + ox[q], cy_ + oy[q], cz_ + oz[q], bands[k])
    torus(W, (cx_, cy_, cz_), (0.35, 1.0, 0.2), 40, 1.3, "ns_ring")
    torus(W, (cx_, cy_, cz_), (0.35, 1.0, 0.2), 46, 0.9, "n_cyan")
    for y in range(0, 44):
        W.put(cx_, y, cz_, "ns_panel")
        W.put(cx_ + 1, y, cz_, "ns_panel")
    W.cyl(cx_, cz_, 0, 2, 7, "ns_floor2")
    # space elevator mast beyond the west end
    mx, mz = -462, -70
    for y in range(0, 262):
        W.box((mx, y, mz), (mx + 2, y, mz + 2), "ns_truss", mode="walls" if y % 10 else "fill")
    for y in range(20, 262, 40):
        W.box((mx - 3, y, mz - 3), (mx + 5, y, mz + 5), "ns_panel2", mode="walls")
        W.put(mx + 1, y + 1, mz + 1, "n_cyan")
    for y in range(0, 262, 3):
        W.put(mx + 1, y, mz + 1, "n_cyan" if (y // 3) % 2 else "n_white")
    W.cyl(mx + 1, mz + 1, 262, 264, 6, "ns_panel2")
    W.put(mx + 1, 265, mz + 1, "n_red")
    # floodlight towers and fireworks
    for (x, z) in ((-410, -42), (-410, 52), (-260, -80), (230, -42), (230, 52), (-130, 110), (60, 150)):
        floodlight(W, x, z, 36)
    for k in range(20):
        cx = -420 + k * 34 + int(rng.integers(-10, 10))
        cz = int(rng.integers(-100, 120))
        cy = int(rng.integers(70, 120))
        col = ["n_red", "n_orange", "n_yellow", "n_lime", "n_cyan", "n_pink", "n_white", "n_magenta"][k % 8]
        burst(W, rng, cx, cy, cz, int(rng.integers(8, 14)), col)
    # tyre stacks and cones where the pit exit rejoins
    for k in range(6):
        W.put(X1 + 96 + (k % 3), 0, 6 + k * 2, "k_stack15" if k % 2 == 0 else "k_stack12")


def layout_spline(pts, n=30):
    from path import spline
    return spline(pts, n)
