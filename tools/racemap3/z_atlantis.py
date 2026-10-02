"""Zone 6: Atlantis Lagoon - a walled channel (YOU fill it with water up to y = WATER_Y), a seabed lane, coral gardens, a sunken city
with a trident monument, a roofed grotto, shipwrecks, a whale skeleton, a giant clam, sea-serpent rings.  Submarine zone.
Kart -> sub at the dive gate on the plateau, sub -> kart at the surface gate."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403
from layout3 import WATER_Y, LANE_Y
from zone_abyss import coral_tree, jelly, ship, whale  # noqa: F401  (also registers the ab_* / coral_* / kelp materials)

WALL_TOP = WATER_Y + 3
mat("at_marble", ["Rock_Marble_Brick", "Rock_Marble_Brick", "Rock_Marble_Brick_Smooth", "Rock_Aqua_Brick" if False else "Rock_Marble_Brick_Smooth"])
mat("at_aqua", ["Rock_Aqua", "Rock_Aqua", "Rock_Marble_Brick_Smooth"])
mat("at_aqua_brick", ["Rock_Stone_Aqua", "Rock_Marble_Brick_Smooth"])
mat("at_pillar", "Rock_Marble_Brick_Pillar_Middle")
mat("at_pillar_b", "Rock_Marble_Brick_Pillar_Base")
mat("at_orn", "Rock_Marble_Brick_Ornate")
mat("at_gold", "Rock_Gold_Brick_Smooth")
mat("at_gold_o", "Rock_Gold_Brick_Ornate")
mat("at_dome", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("at_glow", "Build_Lightsource_Cyan")
mat("at_glow2", "Build_Lightsource_Blue")
mat("at_glow3", "Build_Lightsource_Pink")
mat("at_glow4", "Build_Lightsource_White")
mat("at_stal", ["Rock_Stone_Stalactite_Large", "Rock_Stone_Stalactite_Small"])
mat("at_roof", ["Rock_Slate_Cobble", "Rock_Shale_Cobble", "Rock_Basalt_Brick"])
mat("at_clam_a", ["Soil_Clay_Smooth_Pink", "Soil_Clay_Smooth_White", "Soil_Clay_Smooth_Pink"])
mat("at_clam_b", ["Soil_Clay_Smooth_Purple", "Soil_Clay_Smooth_Pink"])
mat("at_pearl", "Rock_Gem_Diamond")
mat("at_ring_a", "NoCube_Neon_Block_Cyan")
mat("at_ring_b", "NoCube_Neon_Block_Magenta")
mat("at_serp", ["Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Green", "Soil_Clay_Smooth_Cyan", "Soil_Clay_Smooth_Blue"])
mat("at_serp_belly", ["Soil_Clay_Smooth_Lime", "Soil_Clay_Smooth_Yellow"])
mat("at_eye", "Build_Lightsource_Yellow")
mat("at_crystal", ["Rock_Crystal_Cyan_Large", "Rock_Crystal_Blue_Large", "Rock_Crystal_Pink_Large", "Rock_Crystal_Purple_Large"])
mat("at_crystal_b", ["Rock_Crystal_Cyan_Block", "Rock_Crystal_Blue_Block", "Rock_Crystal_Purple_Block"])


def inner_half(S, centers):
    """Half-width of the channel at path position S (array): 13, widening to 22 around the chamber centres."""
    w = np.full(np.shape(S), 13.0)
    for (c, r, extra) in centers:
        w = w + extra * np.exp(-((S - c) / r) ** 2)
    return w


def build(ctx):
    W, B, rng = ctx.W, ctx.B, ctx.rng
    m = B.marks
    s_oasis, s_surf, s_jungle = m["oasis"], m["surface"], m["jungle"]
    T = B.T
    idx = np.arange(len(B.s))
    s_dive = m["dive"]
    # the channel: from where the descending road first dips to 12 (the dam crest) to where it climbs back up through 12
    down = idx[(B.s > s_dive) & (T <= 12)]
    up = idx[(B.s < s_surf + 10) & (B.s > s_dive) & (T <= 12)]
    ch0, ch1 = float(B.s[down[0]]), float(B.s[up[-1]])
    Lc = ch1 - ch0
    ctx.marks["ch0"], ctx.marks["ch1"] = ch0, ch1
    print("  channel s %.0f .. %.0f (length %.0f)" % (ch0, ch1, Lc))
    i0, i1 = B.i(s_oasis), B.i(s_jungle)
    tick = W.tick
    # chambers (wider water) at the city plaza and the clam grotto
    s_plaza, s_clam = ch0 + 0.55 * Lc, ch0 + 0.40 * Lc
    s_gr0, s_gr1 = ch0 + 0.66 * Lc, ch0 + 0.80 * Lc            # the roofed grotto (narrow)
    centers = [(s_plaza, 38.0, 10.0), (s_clam, 28.0, 6.0), (ch0 + 0.2 * Lc, 40.0, 5.0)]

    # ------------------------------------------------------------------ road: plateau and ramps (jersey barriers), the seabed lane
    def rail(s, side):
        return "jersey" if B.Tat(s) >= 9 else None
    edge = lambda s, u: "n_cyan" if int(s // 3) % 2 == 0 else "n_white"
    st = KartStyle(hw=7.0, verge="k_conc", edge=edge, rail_fn=rail, lanes=(0.0,), lane_dash=(4.0, 4.0), curbs=())
    stamp(ctx, B, s_oasis, s_jungle, st, tmin=3)
    ctx.corridors.append((B, B.i(ch0 + 118), B.i(ch1 - 18), 6.6, None))     # keep the seabed lane itself free of coral and kelp
    tick("road")

    # ------------------------------------------------------------------ the basin
    land = Land(ctx, B, i0, i1, margin=72, radius=50, hw=0.0)
    g, d, Tn, Sn = land.g, land.d, land.Tn, land.Sn
    INNER = inner_half(Sn, centers)
    INNER = np.where((Sn > s_gr0) & (Sn < s_gr1), 11.0, INNER)
    inch = (Sn >= ch0) & (Sn <= ch1)
    H = np.where(d < INNER, np.where(inch, -1.0, WALL_TOP), np.where(d < INNER + 3, WALL_TOP, WALL_TOP - 0.85 * (d - INNER - 3)))
    H = np.where(d > 46, -1.0, H)
    road = d <= 9.8
    H = np.where(road, np.where(Tn > 0, Tn - 1, -1), H)
    Ht = np.maximum(np.rint(H).astype(np.int64), -1)
    mask = (d < 48) & ~(land.rb & (d > 30))
    inside = (d < INNER) & inch
    mats = ["ab_sand", "ab_rock_top", "ab_rock_side", "ab_rock"]
    top_ids = pick([((Ht >= WALL_TOP - 1) | (Ht >= 0), 1), (inside & (Ht < 0), 0)], 1)
    top_ids = np.where(Ht < 0, 0, top_ids)
    side_ids = pick([(d < INNER + 4, 2)], 3)
    skin(W, g, Ht, top_ids, side_ids, mats, mask=mask)
    # solid plugs across both ends of the channel (a skin is hollow underneath, so the dams need real blocks)
    for (sa, sb) in ((ch0 - 6, ch0 + 0.6), (ch1 - 0.6, ch1 + 6)):
        cx, cz, si, uu = raster_band(B, B.i(sa), B.i(sb), -(float(INNER.max()) + 6.0), float(INNER.max()) + 6.0)
        Tq = B.T[si]
        for y in range(0, WALL_TOP):
            top_ok = np.where(np.abs(uu) <= 9.6, Tq - 1, WALL_TOP - 1)
            sel = y <= top_ok
            if sel.any():
                W.put_arr(cx[sel], np.full(int(sel.sum()), y), cz[sel], "ab_rock")
    tick("basin")

    # water-level markers + coral on the inner wall faces
    wall_inner = (d >= INNER - 0.6) & (d < INNER + 1.0) & inch
    ix, iz = np.nonzero(wall_inner)
    for k in range(ix.size):
        x, z = g.x0 + int(ix[k]), g.z0 + int(iz[k])
        if ((x // 3 + z // 3) % 4) == 0:
            W.put(x, WATER_Y, z, "n_cyan")
        if rng.random() < 0.11:
            y = int(rng.integers(0, WATER_Y))
            W.put(x, y, z, "coral_any")
            if rng.random() < 0.5:
                W.put(x, min(WATER_Y, y + 1), z, "coral_any")
    tick("walls")

    # ------------------------------------------------------------------ seabed life
    def seabed(n, dmin, dmax, s_lo=None, s_hi=None):
        ok = inside & (d >= dmin) & (d <= INNER - 1.0 if dmax is None else d <= dmax)
        if s_lo is not None:
            ok &= (Sn >= s_lo) & (Sn <= s_hi)
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        return [(g.x0 + int(ix[k]), g.z0 + int(iz[k])) for k in sel]

    for (x, z) in seabed(3200, 9.6, None):
        W.put(x, 0, z, "kelp")
        if rng.random() < 0.3:
            for k in range(1, int(rng.integers(2, 8))):
                W.put(x, k, z, "kelp_stack")
    for (x, z) in seabed(260, 9.6, None):
        W.put(x, 0, z, "cbush_any")
    for (x, z) in seabed(120, 9.6, None):
        W.put(x, 0, z, "cmodel_any")
    for (x, z) in seabed(70, 11, None):
        coral_tree(W, rng, x, 0, z, int(rng.integers(6, 12)), ["coral_warm", "coral_cool", "coral_any"][int(rng.integers(0, 3))])
    for (x, z) in seabed(110, 9.6, None):
        W.put(x, 0, z, "starfish")
    for (x, z) in seabed(80, 9.8, None):
        jelly(W, rng, x, int(rng.integers(7, 10)), z)
    for (x, z) in seabed(90, 10.0, None):
        W.put(x, 1, z, "lit_cyan")
    tick("life")

    # ------------------------------------------------------------------ the sub lane: rings every 26 blocks, wobbling in height
    def lane_y(s):
        return LANE_Y + 1.4 * math.sin(s * 0.05)
    n_h = 0
    for sh in np.arange(ch0 + 135, ch1 - 20, 26.0):
        F = Frame(B, sh)
        cx, cz = F.cell(0, 0)
        cy = int(round(lane_y(sh)))
        hoop(W, (cx, cy, cz), (F.t[0], 0, F.t[1]), 6.9, "at_ring_a" if n_h % 2 == 0 else "at_ring_b", thick=2, ymin=5)
        if n_h % 3 == 0:
            hoop(W, (cx, cy, cz), (F.t[0], 0, F.t[1]), 8.6, "coral_any", thick=2, ymin=5)
        n_h += 1
    tick("hoops")

    # ------------------------------------------------------------------ dive gate (plateau) and surface gate
    Fd = Frame(B, ch0 - 26)
    for k, r in enumerate((11, 12.8, 14.6)):
        Fk = Frame(B, ch0 - 38 + k * 9)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 7, cz), (Fk.t[0], 0, Fk.t[1]), r, ["n_cyan", "n_white", "n_magenta"][k], thick=2, ymin=Fk.T + 1)
    gate(ctx, B, ch0 - 70, "ATLANTIS", scale=2, text_mat="n_cyan", board_mat="b_black", frame_mat="at_marble", trim_mat="n_magenta", tower_u=40,
         board_half=40, tower_h=34)
    for k, r in enumerate((11, 12.8, 14.6)):
        Fk = Frame(B, ch1 + 12 + k * 9)
        cx, cz = Fk.cell(0, 0)
        hoop(W, (cx, Fk.T + 7, cz), (Fk.t[0], 0, Fk.t[1]), r, ["n_yellow", "n_white", "n_lime"][k], thick=2, ymin=Fk.T + 1)
    gate(ctx, B, ch1 + 52, "SURFACE", scale=2, text_mat="n_yellow", board_mat="b_black", frame_mat="coral_warm", trim_mat="n_orange", tower_u=40,
         board_half=40, tower_h=34)
    tick("gates")

    # ------------------------------------------------------------------ coral arch where the lane enters the water
    Fa = Frame(B, ch0 + 40)
    for a in np.linspace(0, math.pi, 90):
        for dv in range(0, 4):
            x, z = Fa.cell(int(round(12.5 * math.cos(a))), dv)
            W.put(x, int(round(1 + 12.5 * math.sin(a))), z, ["coral_warm", "coral_cool"][(int(a * 10) + dv) % 2])

    # ------------------------------------------------------------------ the sunken city of Atlantis around the plaza
    Fp = Frame(B, s_plaza)
    # avenue of columns (both sides of the lane) approaching the plaza and leaving it
    for s in np.arange(s_plaza - 130, s_plaza + 120, 9.0):
        for side in (-1, 1):
            F = Frame(B, s)
            x, z = F.cell(side * 10.5, 0)
            if not inside[x - g.x0, z - g.z0] if (0 <= x - g.x0 < g.nx and 0 <= z - g.z0 < g.nz) else True:
                continue
            h = int(rng.integers(5, 12)) if rng.random() < 0.75 else 3
            W.put(x, 0, z, "at_pillar_b")
            for y in range(1, h):
                W.put(x, y, z, "at_pillar")
            if h >= 9:
                W.put(x, h, z, "at_orn")
                W.put(x, h + 1, z, "at_glow") if rng.random() < 0.4 else None
            else:
                for dx, dz in ((1, 0), (0, 1)):
                    W.put(x + dx, 0, z + dz, "at_marble")
    # domed temples and towers on both banks of the plaza
    for (ds, side, kind) in ((-70, 1, "dome"), (-28, -1, "tower"), (24, 1, "tower"), (60, -1, "dome"), (-4, 1, "dome")):
        F = Frame(B, s_plaza + ds)
        x, z = F.cell(side * 17.5, 0)
        if not (0 <= x - g.x0 < g.nx and 0 <= z - g.z0 < g.nz) or not inside[x - g.x0, z - g.z0]:
            continue
        if kind == "dome":
            W.cyl(x, z, 0, 4, 6, "at_marble", mode="wall")
            W.cyl(x, z, 5, 5, 7, "at_orn")
            W.ellipsoid(x, 5, z, 6.5, 5.5, 6.5, "at_dome", mode="shell", part="upper")
            for a in range(0, 360, 60):
                W.put(int(round(x + 6.5 * math.cos(math.radians(a)))), 3, int(round(z + 6.5 * math.sin(math.radians(a)))), "at_glow")
            W.put(x, 11, z, "at_gold")
            W.put(x, 12, z, "at_glow4")
        else:
            h = int(rng.integers(13, 17))
            W.cyl(x, z, 0, h, 3.2, "at_marble", mode="wall")
            for y in range(2, h, 3):
                W.put(x + 3, y, z, "at_glow")
                W.put(x - 3, y, z, "at_glow")
            for k in range(7):
                W.cyl(x, z, h + 1 + k, h + 1 + k, 4.2 - k * 0.6, "at_dome", mode="fill")
            for y in range(h + 8, h + 14):
                W.put(x, y, z, "at_gold")
    # the trident monument on the plaza (stands beside the lane, rises above the water)
    F = Frame(B, s_plaza + 8)
    tx, tz = F.cell(18, 0)
    W.cyl(tx, tz, 0, 3, 5, "at_marble")
    W.cyl(tx, tz, 4, 4, 6, "at_orn")
    for y in range(5, 34):
        W.put(tx, y, tz, "at_gold")
        if y < 30:
            W.put(tx, y, tz + 1, "at_gold")
    ux, uz = F.cell(18, 1)
    for k in range(-5, 6):
        W.put(tx + (k * 1 if abs(F.t[0]) < 0.5 else 0), 34, tz + (k if abs(F.t[0]) >= 0.5 else 0), "at_gold")
    for k in (-5, 0, 5):
        for y in range(34, 42):
            W.put(tx + (k if abs(F.t[0]) < 0.5 else 0), y, tz + (k if abs(F.t[0]) >= 0.5 else 0), "at_gold")
        W.put(tx + (k if abs(F.t[0]) < 0.5 else 0), 42, tz + (k if abs(F.t[0]) >= 0.5 else 0), "at_glow4")
    tick("city")

    # ------------------------------------------------------------------ wrecks, whale bones, the giant clam
    ship(W, rng, Frame(B, ch0 + 0.27 * Lc), 15.5, 0, L=32, beam=4, depth=8)
    ship(W, rng, Frame(B, ch0 + 0.88 * Lc), -15.5, 0, L=28, beam=4, depth=7)
    whale(W, Frame(B, ch0 + 0.33 * Lc), 0, half=11, n=9, spacing=4)
    # giant clam in the clam chamber
    Fc = Frame(B, s_clam + 6)
    cx, cz = Fc.cell(-17, 0)
    if 0 <= cx - g.x0 < g.nx and 0 <= cz - g.z0 < g.nz:
        W.ellipsoid(cx, 1, cz, 8, 3.2, 6, "at_clam_a", mode="shell", part="lower")
        W.ellipsoid(cx, 7, cz, 8, 3.2, 6, "at_clam_b", mode="shell", part="upper")
        for k in range(-6, 7, 3):
            for y in range(1, 8):
                W.put(cx + k, y, cz + 6, "at_clam_a")
        W.ellipsoid(cx, 3, cz, 2, 2, 2, "at_pearl")
        W.put(cx, 6, cz, "at_glow4")
    tick("wrecks")

    # ------------------------------------------------------------------ the grotto: a rock roof over the channel, stalactites, crystals
    n_roof = 0
    for s in np.arange(s_gr0, s_gr1, 1.0):
        F = Frame(B, s)
        for u in range(-13, 14):
            x, z = F.cell(u, 0)
            if abs(u) <= 12:
                W.put(x, WALL_TOP + 1, z, "at_roof")
                W.put(x, WALL_TOP + 2, z, "at_roof")
                n_roof += 1
        for side in (-1, 1):
            for u in (13, 14):
                x, z = F.cell(side * u, 0)
                W.put(x, WALL_TOP + 1, z, "at_roof")
        if int(s) % 5 == 0:
            for u in range(-9, 10, 3):
                if rng.random() < 0.6:
                    x, z = F.cell(u + int(rng.integers(-1, 2)), 0)
                    ln = int(rng.integers(1, 3))
                    for k in range(ln):
                        W.put(x, WALL_TOP - k, z, "at_stal")
        if int(s) % 11 == 0:
            for side in (-1, 1):
                x, z = F.cell(side * 10, 0)
                W.put(x, 1, z, "at_crystal")
                if rng.random() < 0.5:
                    W.put(x, WALL_TOP, z, "at_glow")
    print("  grotto roof cells:", n_roof)
    tick("grotto")

    # ------------------------------------------------------------------ two sea serpents weave along the banks (clear of the lane)
    s0 = ch0 + 0.84 * Lc
    for sd_ in (-1, 1):
        for k in range(64):
            s = s0 + k * 3.0
            F = Frame(B, s)
            a = k * 0.33 + (0.0 if sd_ < 0 else 1.7)
            u = sd_ * (10.4 + 1.6 * math.sin(a))
            y = lane_y(s) + 3.9 * math.cos(a)
            r = 2.3 + (0.8 if k < 3 else 0.0)
            x, z = F.cell(int(round(u)), 0)
            W.ellipsoid(x, int(round(y)), z, r, r, r + 0.4, "at_serp")
            W.put(x, int(round(y - r)), z, "at_serp_belly")
            if k % 6 == 0:
                W.put(x, int(round(y + r + 1)), z, "at_glow3")
    tick("serpent")

    # ------------------------------------------------------------------ rising bubbles (dotted columns) and light shafts
    for (x, z) in seabed(60, 9.8, None):
        h0 = int(rng.integers(0, 4))
        for k in range(h0, WATER_Y, int(rng.integers(2, 4))):
            W.put(x, k + 1, z, "at_glow4")
    print("  atlantis done")

    # ------------------------------------------------------------------ keep the sub lane free: erase anything within 4.6 of the lane axis above the seabed
    n_cl = 0
    for sh in np.arange(ch0 + 120, ch1 - 20, 1.0):
        F = Frame(B, sh)
        cxl, czl = F.cell(0, 0)
        cy_l = int(round(lane_y(sh)))
        for dx in range(-5, 6):
            for dz in range(-5, 6):
                for dy in range(-5, 6):
                    if dx * dx + dz * dz + dy * dy <= 21 and cy_l + dy >= 3:
                        k = kenc(cxl + dx, cy_l + dy, czl + dz)
                        if k in W.d:
                            nm = W.specs[W.d[k]][0]
                            if not nm.startswith("at_ring") and nm != "coral_any":
                                del W.d[k]
                                n_cl += 1
    print("  lane clearance removed", n_cl, "blocks")
