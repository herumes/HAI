"""Zone 0: Starlight Stadium - start grid, finish gantry, grandstands, pit lane with garages, podium, rockets, floodlights, fireworks.
Built from the server's Kart_* blocks: Kart_Asphalt road flush with the ground (y = -1), decals on layer 0."""
import math

import numpy as np

from core import mat, stair_rot
from font import text_cells
from kart import (boost_pad, facing_driver_rot, finish_band, flag_rot, lean_rot, road_arrows, start_grid, workwall_rot, zebra)
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, bunting, item_boxes, lamp_posts, lbox, lsign, zone_gate
from path import Path, concat, spline

mat("plaza", ["Rock_Stone_Brick_Smooth", "Rock_Stone_Brick_Smooth", "Rock_Marble_Brick_Smooth"])
mat("plaza_dark", "Rock_Basalt_Brick_Smooth")
mat("seat_red", "Cloth_Block_Wool_Red_Stairs")
mat("seat_blue", "Cloth_Block_Wool_Blue_Stairs")
mat("seat_yellow", "Cloth_Block_Wool_Yellow_Stairs")
mat("seat_white", "Cloth_Block_Wool_White_Stairs")
mat("stand_body", ["Rock_Basalt_Brick", "Rock_Basalt_Brick", "Rock_Basalt_Brick_Smooth"])
mat("roof_panel_red", "Cloth_Block_Wool_Red")
mat("roof_panel_white", "Cloth_Block_Wool_White")
mat("crowd", ["Cloth_Block_Wool_Red", "Cloth_Block_Wool_Blue", "Cloth_Block_Wool_Yellow", "Cloth_Block_Wool_Green",
              "Cloth_Block_Wool_Pink", "Cloth_Block_Wool_Orange", "Cloth_Block_Wool_Purple", "Cloth_Block_Wool_Cyan"])
mat("crowd_head", ["Cloth_Block_Wool_Orange_Light", "Cloth_Block_Wool_Pink_Light", "Cloth_Block_Wool_Yellow_Light"])
mat("garage_wall", ["Andesite_Smooth", "Andesite_Smooth", "Andesite_Smooth", "Rock_Stone_Brick_Smooth"])
mat("hull_white", "Soil_Clay_Smooth_White")
mat("hull_red", "Soil_Clay_Smooth_Red")
mat("hull_grey", "Soil_Clay_Smooth_Grey")
mat("window_lit", "Build_Lightsource_Cyan")
mat("metal_grey", "Metal_Iron_Smooth")
mat("metal_dark", "Metal_Zinc_Smooth")
mat("rocket_flame", ["Build_Lightsource_Orange", "Build_Lightsource_Yellow", "Build_Lightsource_Red"])

TEAM = [("clay_red", "n_red"), ("clay_blue", "n_blue"), ("clay_yellow", "n_yellow"), ("clay_green", "n_green"), ("clay_orange", "n_orange")]


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


def build(ctx):
    W, A, rng = ctx.W, ctx.A, ctx.rng
    m = A.marks
    z_end = m["meadows"]
    st = KartStyle(hw=7.0, rail="armco", lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=())
    stamp_kart_road(W, A, A.i(0), A.i(z_end), st)

    fin_s = 200.0
    FIN = Frame(A, fin_s)

    # ---- the line, the grid, arrows and boost pads on the road
    finish_band(W, A, fin_s, 7.0, rows=2)
    n = start_grid(W, A, fin_s, 7.0, rows=5, row_gap=6.0, off=3.4)
    for s in np.arange(236.0, 300.0, 16.0):
        road_arrows(W, A, s, n=1, u=-3.5)
        road_arrows(W, A, s, n=1, u=3.5)
    boost_pad(W, A, 222, 14, 4, "n_orange", count=3, spacing=4)
    boost_pad(W, A, 262, 14, 4, "n_yellow", count=3, spacing=4)
    for s in (240, 282):
        item_boxes(W, A, s, 7.0, n=4)

    # ---- finish gantry: neon sign on an andesite frame whose feet are Finish_Line blocks
    zone_gate(W, A, fin_s, "FINISH", scale=1, text_mat="n_white", board_mat="b_black", frame_mat="k_andesite", trim_mat="n_yellow",
              tower_u=11, board_half=19, tower_h=24)
    F = Frame(A, fin_s)
    T = F.T
    for side in (-1, 1):
        u0 = side * 11
        lbox(W, F, u0 - 2, u0 + 2, -2, 2, T, T + 3, "k_finish_line")
    for u in range(-19, 20):
        for k in range(2):
            x, z = F.cell(u, -2)
            W.put(x, T + 12 + k, z, "k_finish_line" if (u + k) % 2 == 0 else "n_black")

    # ---- ground plaza: stone at the edges, Kart_Concrete beside the road, Kart_Grass lawns
    x0, x1 = -225, 135
    for z in range(-22, 34):
        W.put_arr(np.arange(x0, x1 + 1), np.full(x1 - x0 + 1, -1), np.full(x1 - x0 + 1, z), "plaza", keep=True)
    for z in range(-14, -9):                                   # lawn strip in front of the north stand
        W.put_arr(np.arange(-205, 106), np.full(311, -1), np.full(311, z), "k_grass")
    for z in range(35, 100):                                   # the spaceport lawn
        W.put_arr(np.arange(-175, 116), np.full(291, -1), np.full(291, z), "k_grass")

    # ---- north grandstand + giant lettering
    grandstand(W, rng, -150, 70, -16, 12)
    w, h, cells = text_cells("GALAXY GRAND PRIX", 2)
    x_mid = -40
    y_top = 12 + 9 + 2 + 14
    zrow = -16 - 12 - 5
    x_lo, x_hi = int(x_mid - w / 2), int(x_mid + w / 2)
    for (c, r) in cells:
        W.put(x_lo + c, y_top - r, zrow, "n_yellow")
    for x in range(x_lo - 3, x_hi + 4):
        for y in range(y_top - 17, y_top + 4):
            W.put(x, y, zrow - 1, "n_black")
        W.put(x, y_top + 4, zrow - 1, "n_cyan")
        W.put(x, y_top - 18, zrow - 1, "n_magenta")
    for x in (x_lo - 2, x_hi + 2, int(x_mid)):
        W.box((x, 12 + 9 + 1, zrow - 1), (x + 1, y_top - 18, zrow), "metal_grey")

    # ---- trackside: catch fence, billboards, flags, floodlights
    for x in range(-200, 101):
        W.put(x, 0, -9, "k_catch", 0)
    for k, x in enumerate(range(-190, 100, 24)):
        W.put(x, 0, -11, "k_billboard", facing_driver_rot("e"))
        W.put(x + 12, 0, -11, "k_flag", flag_rot("e"))
    for x in range(-196, 100, 12):
        W.put(x, 0, -10, "k_flood", lean_rot("s"))
    W.put(int(FIN.cell(-9, 0)[0]), 0, int(FIN.cell(-9, 0)[1]), "k_flag", flag_rot("e"))
    W.put(int(FIN.cell(9, 0)[0]), 0, int(FIN.cell(9, 0)[1]), "k_flag", flag_rot("e"))

    # ---- south: pit lane (Kart_Asphalt with pit boxes), concrete pit wall, garages with Red_Workwall
    PZ = 18                                                    # pit lane centre line z
    entry = line_path(spline([(-166, 0), (-150, 1), (-134, 7), (-118, 14), (-104, PZ)], 30))
    lane = line_path([(-104, PZ), (20, PZ)])
    exit_ = line_path(spline([(20, PZ), (32, PZ), (44, 14), (56, 7), (68, 2), (78, 0)], 30))
    pit_style = lambda rail: KartStyle(hw=3.5, rail=rail, lanes=(), edge_line=False, curbs=(), verge="k_conc")
    erase_rails(W, -170, -100, 6, 12)
    erase_rails(W, 18, 82, 6, 12)
    stamp_kart_road(W, entry, 0, len(entry) - 1, pit_style(None), steps=False)
    stamp_kart_road(W, exit_, 0, len(exit_) - 1, pit_style(None), steps=False)
    stamp_kart_road(W, lane, 0, len(lane) - 1,
                    KartStyle(hw=3.5, rail_fn=lambda s, side: "jersey" if side < 0 else None, lanes=(), edge_line=False, curbs=(), verge="k_conc"),
                    steps=False)
    for x in range(-104, 21):                                  # pit wall island between track and lane
        for z in range(9, 14):
            W.put(x, -1, z, "k_conc", keep=True)
        W.put(x, 0, 11, "k_catch", 0)
    for x in range(-100, 20, 12):
        W.put(x, 0, 12, "k_flood", lean_rot("s"))
        W.put(x + 6, 0, 10, "k_redblock")
    # apron + pit boxes + zebra crossing
    for x in range(-135, 75):
        for z in (22, 23):
            W.put(x, -1, z, "k_conc")
    for gi in range(10):
        gx = -130 + gi * 20
        col, neon = TEAM[gi % 5]
        for x in range(gx + 6, gx + 9):
            for z in (19, 20, 21):
                W.put(x, 0, z, "k_pitbox")
        # garage: 15 wide, 11 deep, 6 tall, open front; Andesite walls, team colour roof and neon strip
        W.box((gx, -1, 24), (gx + 14, -1, 34), "k_conc", mode="floor")
        W.box((gx, 0, 24), (gx + 14, 6, 34), "garage_wall", mode="shell")
        for x in range(gx + 2, gx + 13):
            for y in range(0, 5):
                W.erase(x, y, 24)
        W.box((gx - 1, 7, 23), (gx + 15, 7, 35), col, mode="fill")
        for x in range(gx, gx + 15):
            W.put(x, 5, 23, neon)
        # inside: workwalls on the back wall, cabinets, tyres, cones
        for x in range(gx + 2, gx + 13, 3):
            W.put(x, 0, 33, "k_workwall", workwall_rot("n"))
        W.put(gx + 1, 0, 25, "k_work", 0)
        W.put(gx + 13, 0, 25, "k_work", 0)
        W.put(gx + 3, 0, 29, "k_stack15")
        W.put(gx + 11, 0, 29, "k_stack12")
        W.put(gx + 7, 0, 30, "k_redblock")
        for y in range(0, 5):
            W.put(gx + 1, y, 24, col)
            W.put(gx + 13, y, 24, col)
        W.put_arr(np.arange(gx + 1, gx + 14), np.full(13, 5), np.full(13, 24), col)
        # canopy over the apron, sloping down toward the lane (high side south = rot 2) on andesite posts
        for x in range(gx + 1, gx + 14):
            W.put(x, 6, 22, "k_roof_slate" if gi % 2 == 0 else "k_roof_calcite", 2)
        for x in (gx + 1, gx + 7, gx + 13):
            for y in range(0, 6):
                W.put(x, y, 22, "k_andesite")
        for x in (gx + 3, gx + 11):
            W.put(x, 0, 21, "k_cone")
    zebra(W, lane, 46.0, 3.5, width=3)
    for z in range(15, 22):
        W.put(-96, 0, z, "k_bump")
        W.put(-95, 0, z, "k_bump")
    bunting(W, A, A.i(0), A.i(z_end + 20), 28, 10.5, 7)

    # ---- winner's podium on the lawn, with the trophy
    px, pz = 20, 40
    W.box((px - 4, 0, pz - 4), (px + 14, 0, pz + 4), "k_andesite")
    W.put(px, 1, pz, "k_pod2")
    W.put(px + 4, 1, pz, "k_pod1")
    W.put(px + 8, 1, pz, "k_pod3")
    W.put(px + 12, 1, pz, "k_trophy")
    for (cx_, cz_) in ((px - 4, pz - 4), (px + 14, pz - 4), (px - 4, pz + 4), (px + 14, pz + 4)):
        W.put(cx_, 1, cz_, "k_finish_line")
        W.put(cx_, 2, cz_, "k_flag", flag_rot("e"))
    # the big gold cup from before, and three rockets
    rocket(W, 6 - 120, 70, 0, 52, 7, "hull_white", "hull_red")
    rocket(W, -70, 76, 0, 40, 6, "hull_white", "clay_blue")
    rocket(W, -20, 70, 0, 62, 8, "clay_yellow", "hull_white")
    for y in range(0, 50):
        W.put(-8, y, 70, "metal_dark")
        W.put(-8, y, 71, "metal_dark")
        if y % 6 == 0:
            W.box((-9, y, 70), (-13, y, 71), "metal_dark")
    trophy(W, 34, 62)
    # tyre stacks and cones where the pit exit rejoins
    for k in range(6):
        W.put(96 + (k % 3), 0, 6 + k * 2, "k_stack15" if k % 2 == 0 else "k_stack12")
    for (x, z) in [(-215, -40), (-215, 40), (125, -40), (125, 40), (-80, 48)]:
        floodlight(W, x, z, 32)
    for k in range(14):
        cx = -200 + k * 26 + int(rng.integers(-8, 8))
        cz = int(rng.integers(-60, 90))
        cy = int(rng.integers(52, 86))
        col = ["n_red", "n_orange", "n_yellow", "n_lime", "n_cyan", "n_pink", "n_white"][k % 7]
        burst(W, rng, cx, cy, cz, int(rng.integers(7, 12)), col)


def grandstand(W, rng, xa, xb, z_face, rows, north=True):
    sgn = -1 if north else 1
    colors = ["seat_red", "seat_white", "seat_blue", "seat_yellow"]
    up = "n" if north else "s"
    for x in range(xa, xb + 1):
        sec = ((x - xa) // 14) % 4
        for k in range(rows):
            z = z_face + sgn * k
            if k > 0:
                W.put_arr(np.full(k, x), np.arange(0, k), np.full(k, z), "stand_body")
            seat = k % 3 != 2
            W.put(x, k, z, colors[sec] if seat else "stand_body", rot=stair_rot(up) if seat else None)
            if seat and rng.random() < 0.28:
                W.put(x, k + 1, z, "crowd")
                W.put(x, k + 2, z, "crowd_head")
        zb = z_face + sgn * rows
        W.put_arr(np.full(rows + 4, x), np.arange(0, rows + 4), np.full(rows + 4, zb), "stand_body")
    zr0, zr1 = z_face + sgn * 2, z_face + sgn * (rows + 4)
    zlo, zhi = min(zr0, zr1), max(zr0, zr1)
    yroof = rows + 9
    for x in range(xa, xb + 1):
        panel = "roof_panel_red" if ((x - xa) // 7) % 2 == 0 else "roof_panel_white"
        for z in range(zlo, zhi + 1):
            W.put(x, yroof, z, panel)
        W.put(x, yroof - 1, z_face + sgn * 2, "n_white")
    for x in range(xa + 1, xb, 12):
        zc = z_face + sgn * (rows + 3)
        W.box((x, rows + 3, zc), (x + 1, yroof - 1, zc + sgn), "metal_grey")


def trophy(W, cx, cz):
    """A giant golden cup on a marble plinth."""
    W.box((cx - 9, 0, cz - 9), (cx + 9, 1, cz + 9), "marble")
    W.box((cx - 7, 2, cz - 7), (cx + 7, 3, cz + 7), "marble_brick")
    y = 4
    W.cyl(cx, cz, y, y + 1, 5.5, "gold_brick")
    for k in range(10):
        r = 2.4 + (1.3 if k in (4, 5) else 0)
        W.cyl(cx, cz, y + 2 + k, y + 2 + k, r, "gold_brick")
    yb = y + 12
    for k in range(17):
        t = k / 16.0
        r = 3.5 + 8.5 * (t ** 0.75)
        W.cyl(cx, cz, yb + k, yb + k, r, "gold_brick", mode="wall" if k else "fill")
    W.cyl(cx, cz, yb + 17, yb + 17, 12.6, "gold_orn", mode="wall")
    for sgn in (-1, 1):
        for i in range(0, 25):
            a = math.pi * i / 24
            px = cx + sgn * (11 + 5 * math.sin(a))
            py = yb + 4 + 10 * (1 - math.cos(a)) * 0.9
            W.ellipsoid(int(round(px)), int(round(py)), cz, 1.3, 1.3, 1.3, "gold_brick")
    W.ellipsoid(cx, yb + 21, cz, 3, 3, 3, "lit_yellowlight", mode="shell")
    for k in range(3):
        W.put(cx - 1 + k, 2, cz - 10, "n_yellow")


def rocket(W, cx, cz, y0, h, r, body, accent):
    W.cyl(cx, cz, y0 + 0, y0 + 2, r - 2, "metal_dark")
    W.cyl(cx, cz, y0 + 3, y0 + 4, r - 1, "metal_dark")
    for y in range(y0 + 5, y0 + h):
        band = ((y - y0) // 6) % 3 == 2
        W.cyl(cx, cz, y, y, r, accent if band else body, mode="wall")
    for k in range(4):
        a = k * math.pi / 2
        x = int(round(cx + (r) * math.cos(a)))
        z = int(round(cz + (r) * math.sin(a)))
        wy = y0 + int(h * 0.55)
        W.put(x, wy, z, "window_lit")
        W.put(x, wy + 1, z, "window_lit")
    cone = int(r * 2.6)
    for i in range(cone):
        rr = r * (1 - i / cone)
        if rr < 0.6:
            break
        W.cyl(cx, cz, y0 + h + i, y0 + h + i, rr, accent, mode="fill" if rr < 2 else "wall")
    W.put(cx, y0 + h + cone, cz, "n_red")
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        dx, dz = math.cos(a), math.sin(a)
        for i in range(0, int(h * 0.3)):
            for j in range(0, int(r * 1.3) - i // 3):
                x = int(round(cx + dx * (r + j)))
                z = int(round(cz + dz * (r + j)))
                W.put(x, y0 + 5 + i, z, accent)
    for i in range(5):
        W.cyl(cx, cz, y0 - 1 - i, y0 - 1 - i, max(0.5, r - 2 - i), "rocket_flame") if y0 - 1 - i >= 0 else None


def floodlight(W, x, z, h):
    W.box((x, 0, z), (x + 1, h, z + 1), "metal_grey")
    W.box((x - 3, h + 1, z - 1), (x + 4, h + 4, z + 2), "b_grey")
    for xx in range(x - 3, x + 5):
        for yy in range(h + 1, h + 5):
            W.put(xx, yy, z + 2 if z > 0 else z - 2, "lit_white")
    W.put(x, h + 5, z, "lit_yellowlight")


def burst(W, rng, cx, cy, cz, r, m):
    n = 70
    for _ in range(n):
        v = rng.normal(size=3)
        v /= np.linalg.norm(v)
        p = v * r
        W.put(int(cx + p[0]), int(cy + p[1]), int(cz + p[2]), m)
    W.put(cx, cy, cz, m)
