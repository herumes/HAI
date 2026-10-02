"""Zone 7: Emberforge Volcano - the landing runway, a spiral road up a lava-streaked cone, obsidian spires, smoke."""
import math

import numpy as np

from core import fbm, mat, smoothstep
from kart import along_rot, boost_pad, line_rot, road_arrows, side_cell
from kartprops import corner_furniture, flood_posts, skid_patch
from kartroad import KartStyle, stamp_kart_road
from kit import Frame, cloud, hoop, item_boxes, lbox, lsign, road_grid, spire, zone_gate
from path import raster_band
from terrain import Grid, carve, centre_field, pick, skin, slope_of
from zone_canyon import rock_arch

mat("vo_road", ["Rock_Volcanic_Brick_Smooth", "Rock_Volcanic_Brick_Smooth", "Rock_Volcanic_Brick_Smooth", "Rock_Basalt_Brick_Smooth"])
mat("vo_stair", "Rock_Volcanic_Brick_Stairs")
mat("vo_line", "Build_Lightsource_Orange")
mat("vo_wall", ["Rock_Volcanic_Brick", "Rock_Volcanic_Brick", "Rock_Magma_Cooled_Brick"])
mat("vo_wall2", "Rock_Magma_Cooled_Brick")
mat("run_road", ["Build_Black_Cube", "Build_Black_Cube", "Build_Black_Cube", "Build_GreyDark_Cube"])
mat("run_stair", "Build_Black_Stairs")
mat("vo_ash", ["Soil_Ash", "Rock_Basalt", "Rock_Volcanic", "Soil_Ash"])
mat("vo_gravel", ["Soil_Volcanic_Gravel", "Soil_Magma_Cooled_Gravel"])
mat("vo_rock_top", ["Rock_Volcanic", "Rock_Basalt", "Rock_Volcanic_Brick"])
mat("lava", ["Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Cracked_Incandescent"])
mat("vo_side_a", ["Rock_Volcanic_Brick", "Rock_Basalt_Brick", "Rock_Volcanic_Cobble"])
mat("vo_side_b", ["Rock_Magma_Cooled_Brick", "Rock_Magma_Cooled_Cobble", "Rock_Volcanic_Brick"])
mat("vo_side_c", ["Rock_Volcanic_Brick", "Rock_Volcanic_Cracked_Lava", "Rock_Volcanic_Brick", "Rock_Volcanic_Brick"])
mat("obs", ["Rock_Volcanic_Brick", "Build_Black_Cube", "Rock_Basalt_Brick"])
mat("smoke", ["Cloth_Block_Wool_Gray", "Cloth_Block_Wool_Gray", "Cloth_Block_Wool_Gray_Light", "Cloth_Block_Wool_Black"])
mat("ember", ["Build_Lightsource_Orange", "Build_Lightsource_Red", "Build_Lightsource_Yellow"])
mat("vo_fire", "Deco_Fire")
mat("skull", ["Rock_Chalk", "Rock_Chalk", "Rock_Calcite"])
mat("vo_gate", "Rock_Volcanic_Brick_Ornate")
mat("vo_pillar", "Rock_Volcanic_Brick_Pillar_Middle")


def skull(W, x, y, z, s=1.0, facing=(0, -1)):
    W.ellipsoid(x, y, z, 5 * s, 4 * s, 5.5 * s, "skull", mode="shell")
    W.ellipsoid(x + facing[0] * 4, y - 2, z + facing[1] * 4, 3.2 * s, 2.0 * s, 3.2 * s, "skull", mode="shell")
    px, pz = -facing[1], facing[0]
    for sgn in (-1, 1):
        ex, ez = x + facing[0] * 4 * s + px * sgn * 2.2 * s, z + facing[1] * 4 * s + pz * sgn * 2.2 * s
        W.ellipsoid(int(ex), y + 1, int(ez), 1.4 * s, 1.4 * s, 1.4 * s, "lit_red")
    # horns
    for sgn in (-1, 1):
        for k in range(int(6 * s)):
            W.put(int(x + px * sgn * (4 * s + k * 0.7)), int(y + 3 * s + k * 0.9), int(z + pz * sgn * (4 * s + k * 0.7)), "skull")


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s0, s1 = 0.0, m["rainbow"]
    i0, i1 = B.i(s0), B.i(s1)
    VCx, VCz = ctx.VC
    tick = W.tick

    # ------------------------------------------------------------------ roads: runway (wide), taper, spiral road
    s_run = 215.0
    def curbblock(s, u):
        return "k_curb_yel_b" if int(s // 4) % 2 == 0 else "k_curb_red_b"

    run = KartStyle(hw=11.0, verge="k_conc", rail=None, edge=curbblock, lanes=(-5.5, 0.0, 5.5), lane_dash=(8.0, 6.0), edge_line=True)
    stamp_kart_road(W, B, B.i(0), B.i(s_run), run)
    # taper from 11 to 7 over 36 blocks
    for k, s in enumerate(np.arange(s_run, s_run + 36, 4.0)):
        hw = 11.0 - 4.0 * (k + 1) / 9.0
        ts = KartStyle(hw=hw, verge="k_conc", rail="armco", wall=("vo_wall",), wall_h=1, lanes=(), edge_line=True, curbs=())
        stamp_kart_road(W, B, B.i(s), B.i(s + 4.5), ts)
    m_vol = m["volcano"]
    vo = KartStyle(hw=7.0, verge="k_conc", rail_fn=lambda s, side: "tyre" if (side < 0 and s > m_vol) else "armco",
                   edge=lambda s, u: "n_orange" if int(s // 3) % 2 == 0 else "n_firered", wall=("vo_wall", "vo_wall2"), wall_period=4.0,
                   wall_h=2, deck="vo_wall", lane_mat="k_yline")
    stamp_kart_road(W, B, B.i(s_run + 36), B.i(s1 + 20), vo)
    # runway dressing: threshold piano keys, touchdown skid marks, centre arrows, edge floodlights
    from kart import line_rot as _lr
    for k in range(9):
        for ss in np.arange(6.0, 14.0, 1.0):
            i = B.i(ss)
            x, z = side_cell(B, i, -10 + 2.2 * k + 0.5)
            t = B.t[i]
            W.put(x, int(B.T[i]) + 1, z, "k_line", _lr(t[0], t[1]))
    for s in (60, 100, 140, 180):
        for u0 in (-7.5, 7.5):
            for ss in np.arange(s, s + 12.0, 1.0):
                i = B.i(ss)
                x, z = side_cell(B, i, u0)
                t = B.t[i]
                W.put(x, int(B.T[i]) + 1, z, "k_line", _lr(t[0], t[1]))
    for s in (44, 52):
        skid_patch(W, B, s, -8.0, -6.0, "k_skid_straight")
        skid_patch(W, B, s, 6.0, 8.0, "k_skid_straight")
    for s in np.arange(20, s_run - 10, 18.0):
        road_arrows(W, B, s, n=1, u=-2.75)
        road_arrows(W, B, s, n=1, u=2.75)
    for s in np.arange(10, s_run, 9.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 11.5, 0)
            from kart import lean_rot, outward, heading, OPP
            h_ = heading(F.t[0], F.t[1])
            W.put(x, F.T + 1, z, "k_flood", lean_rot(OPP[outward(h_, side)]))
    boost_pad(W, B, 230, 14, 4, "n_orange", count=3, spacing=4)
    tick("roads")

    # ------------------------------------------------------------------ terrain: the cone
    g = Grid(-690, -75, 145, 570)
    d, Tn, Sn = centre_field(g, B, i0, B.i(s1 + 90), 70, hw=12.0)
    dx, dz = g.X - VCx, g.Z - VCz
    r = np.hypot(dx, dz)
    th = np.arctan2(dz, dx)
    prof = np.full(r.shape, -1.0)
    prof = np.where(r < 145, 6 - (r - 125) * 0.35, prof)
    prof = np.where(r < 125, 6 + 44 * (125 - r) / 57.0, prof)
    prof = np.where(r < 68, 50 + 12 * smoothstep(68, 58, r) * 1.0, prof)
    prof = np.where(r < 58, 62 - 22 * smoothstep(58, 44, r), prof)
    prof = np.where(r < 44, 36.0, prof)
    gully = 4.0 * np.sin(9 * th + 2.0 * fbm(g.X, g.Z, 60, 2, 3)) * smoothstep(60, 100, r) * (1 - smoothstep(118, 140, r))
    crag = 5.0 * (fbm(g.X, g.Z, 18.0, 3, 51) - 0.5) * smoothstep(50, 70, r)
    H = np.where(r < 150, prof + gully + crag, -1.0)
    shoulder = np.where(Tn > 0, Tn - 1, -1)
    Hc = carve(H, d, shoulder, 3.0, 15.0, noise=crag * 0.5)
    Ht = np.maximum(np.rint(Hc).astype(np.int64), -1)
    mask = ((r < 152) | (d < 26)) & (Ht >= -1)
    rb = road_grid(ctx, g)
    mask = mask & ~(rb & (d > 12))
    slope = slope_of(Hc)
    # lava flows down the flanks + the lake in the crater
    lava = np.zeros(r.shape, bool)
    for k in range(9):
        a = k * 2 * math.pi / 9 + 0.35
        wob = rng.uniform(0, 6)
        for rr in np.arange(56, 150, 0.8):
            aa = a + 0.12 * math.sin(rr * 0.09 + wob) + (rr - 56) * 0.0008
            fx, fz = VCx + rr * math.cos(aa), VCz + rr * math.sin(aa)
            ix0, iz0 = int(fx) - g.x0, int(fz) - g.z0
            w = 1.6 + 1.0 * math.sin(rr * 0.2 + k)
            for ddx in range(-2, 3):
                for ddz in range(-2, 3):
                    if ddx * ddx + ddz * ddz <= w * w and 0 <= ix0 + ddx < g.nx and 0 <= iz0 + ddz < g.nz:
                        lava[ix0 + ddx, iz0 + ddz] = True
    lava &= (d > 14)
    lake = (r < 44)
    mats = ["vo_ash", "lava", "vo_rock_top", "vo_gravel", "vo_side_a", "vo_side_b", "vo_side_c"]
    top_ids = pick([(slope > 1.0, 2), (r > 120, 3), (lava, 1), (lake, 1)], 0)

    def side_fn(y, x, z):
        n = fbm(x.astype(float), z.astype(float), 9.0, 2, 57)
        return np.where(n > 0.66, 6, np.where(y % 7 < 3, 4, 5))

    skin(W, g, Ht, top_ids, side_fn, mats, mask=mask)
    tick("cone")

    # ------------------------------------------------------------------ props
    def cand(n, rmin, rmax, dmin, slope_max=9):
        ok = (r >= rmin) & (r <= rmax) & (d >= dmin) & (slope < slope_max) & mask & ~lava & ~rb
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), int(Ht[ix[k], iz[k]]), g.z0 + int(iz[k])) for k in sel]

    for (x, y, z) in cand(28, 60, 146, 13):
        spire(W, x, y + 1, z, int(rng.integers(10, 26)), rng.uniform(2.5, 4.5), "obs", lean=(rng.normal(0, 0.08), rng.normal(0, 0.08)))
    for (x, y, z) in cand(90, 20, 150, 13):
        rr = int(rng.integers(1, 3))
        W.ellipsoid(x, y + 1, z, rr + 1, rr, rr + 1, "obs", part="upper")
    for (x, y, z) in cand(30, 0, 147, 12):
        W.put(x, y + 1, z, "vo_fire")
    # smoke plume and embers over the crater
    for k in range(10):
        cx = VCx + rng.normal(0, 6) + k * 2.0
        cz = VCz + rng.normal(0, 6)
        rad = 9 + k * 1.5
        cloud(W, rng, int(cx), 76 + k * 13, int(cz), int(rad), int(rad * 0.45), int(rad * 0.9), n=1, m="smoke")
    for _ in range(260):
        a = rng.uniform(0, 2 * math.pi)
        rr = rng.uniform(0, 50)
        W.put(int(VCx + rr * math.cos(a)), int(rng.uniform(44, 150)), int(VCz + rr * math.sin(a)), "ember")
    tick("props")

    # ------------------------------------------------------------------ set pieces
    # the lava lake needs a fountain and a bone-gate skull on the rim
    for k in range(6):
        a = k * math.pi / 3
        for y in range(36, 36 + 10 + 4 * (k % 3)):
            W.put(int(VCx + 14 * math.cos(a)), y, int(VCz + 14 * math.sin(a)), "lava")
    for y in range(36, 70):
        W.put(int(VCx), y, int(VCz), "lit_orange")
        if y % 3 == 0:
            for dxx, dzz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                W.put(int(VCx) + dxx, y, int(VCz) + dzz, "lava")
    Fg = Frame(B, m["volcano"] + 6)
    # the gate: obsidian pillars with a glowing sign
    zone_gate(W, B, m["volcano"] - 8, "VOLCANO", scale=2, text_mat="n_orange", board_mat="b_black", frame_mat="vo_gate", trim_mat="n_red",
              tower_u=42, board_half=42, tower_h=30)
    for frac in (0.25, 0.6):
        Fa = Frame(B, m["volcano"] + frac * (m["rainbow"] - m["volcano"]))
        rock_arch(W, rng, Fa, 13, 14, 4, "obs", base_down=max(0, Fa.T - 1))
    L = m["rainbow"] - m["volcano"]
    for frac in (0.08, 0.2, 0.34, 0.5, 0.64, 0.78):
        boost_pad(W, B, m["volcano"] + frac * L, 14, 4, "n_orange", count=3, spacing=4)
    for frac in (0.14, 0.3, 0.46, 0.58, 0.72, 0.88):
        item_boxes(W, B, m["volcano"] + frac * L, 7.0, n=4)
    corner_furniture(W, B, B.i(m["volcano"]), B.i(m["rainbow"]), 7.0, rmax=100.0, stacks=False, skid=True)
    flood_posts(W, B, B.i(m["volcano"]), B.i(m["rainbow"]), 56, 10.5, 6, glow="kp_glow_orange", pole="vo_wall")
    # braziers along the runway exit
    for s in np.arange(30, 215, 26.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 15, 0)
            W.box((x, F.T, z), (x, F.T + 3, z), "vo_pillar")
            W.put(x, F.T + 4, z, "vo_fire")
    # runway welcome: skulls flanking the start of the runway
    for side in (-1, 1):
        F = Frame(B, 4.0)
        x, z = F.cell(side * 22, 0)
        skull(W, x, F.T + 9, z, 1.6, facing=(int(round(F.t[0])), int(round(F.t[1]))))
        for y in range(F.T, F.T + 7):
            W.box((x - 1, y, z - 1), (x + 1, y, z + 1), "vo_pillar")
    tick("setpieces")
