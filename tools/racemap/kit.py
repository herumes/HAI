"""racemap kit: materials and the reusable pieces (frames, gates, signs, item boxes, pads, lamps, hoops, trees...)."""
import math

import numpy as np

from core import MAT, World, mat, stair_rot
from font import text_cells
from path import Path
import kart  # noqa: F401  (registers the Kart_* materials the gates use)

# --------------------------------------------------------------------------- materials
CLAY = ["Black", "Blue", "Cyan", "Green", "GreenDark", "Grey", "Grey2", "Lime", "Orange", "Pink", "Purple", "Red", "Red2",
        "White", "Yellow", "Yellow2"]
for c in CLAY:
    mat("clay_" + c.lower(), "Soil_Clay_Smooth_" + c)
    mat("claystair_" + c.lower(), "Soil_Clay_Smooth_%s_Stairs" % c)
    mat("clayhalf_" + c.lower(), "Soil_Clay_Smooth_%s_Half" % c)
WOOL = ["Black", "Blue", "Blue_Light", "Cyan", "Cyan_Light", "Gray", "Gray_Light", "Green", "Green_Light", "Orange",
        "Orange_Light", "Pink", "Pink_Light", "Purple", "Purple_Light", "Red", "Red_Light", "White", "Yellow", "Yellow_Light"]
for c in WOOL:
    mat("wool_" + c.lower(), "Cloth_Block_Wool_" + c)
LIGHT = ["Blue", "Cyan", "Green", "Orange", "Pink", "Red", "White", "Yellow", "YellowLight"]
for c in LIGHT:
    mat("lit_" + c.lower(), "Build_Lightsource_" + c)
mat("lit_rainbow", ["Build_Lightsource_" + c for c in ["Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Pink"]])
mat("lit_cool", ["Build_Lightsource_Cyan", "Build_Lightsource_Blue", "Build_Lightsource_Pink", "Build_Lightsource_White"])
mat("lit_warm", ["Build_Lightsource_Orange", "Build_Lightsource_Yellow", "Build_Lightsource_YellowLight", "Build_Lightsource_Red"])

for nm in ("Black", "Grey", "GreyDark", "GreyLight", "White"):
    mat("b_" + nm.lower(), "Build_%s_Cube" % nm)
    mat("bstair_" + nm.lower(), "Build_%s_Stairs" % nm)
    mat("bhalf_" + nm.lower(), "Build_%s_Half" % nm)
    mat("bfence_" + nm.lower(), "Build_%s_Fence" % nm)

mat("asphalt", ["Build_GreyDark_Cube"], None)
mat("asphalt_mix", ["Build_GreyDark_Cube", "Build_GreyDark_Cube", "Build_GreyDark_Cube", "Build_GreyDark_Cube", "Build_Grey_Cube"])
mat("asphalt_stair", "Build_GreyDark_Stairs")
mat("line_white", "Build_White_Cube")
mat("curb_red", "Cloth_Block_Wool_Red")
mat("curb_white", "Cloth_Block_Wool_White")
mat("iron_fence", "Metal_Iron_Fence")
mat("iron_bars", "Deco_Iron_Bars")
mat("gold_brick", "Rock_Gold_Brick")
mat("gold_orn", "Rock_Gold_Brick_Ornate")
mat("gold_pillar", "Rock_Gold_Brick_Pillar_Middle")
mat("marble", "Rock_Marble_Brick_Smooth")
mat("marble_brick", "Rock_Marble_Brick")
mat("marble_pillar", "Rock_Marble_Brick_Pillar_Middle")
mat("marble_stair", "Rock_Marble_Brick_Stairs")
mat("marble_half", "Rock_Marble_Brick_Half")
mat("stone_brick", ["Rock_Stone_Brick", "Rock_Stone_Brick", "Rock_Stone_Brick_Smooth"])
mat("stone_cobble", ["Rock_Stone_Cobble", "Rock_Stone_Cobble", "Rock_Stone_Cobble_Mossy"])
mat("stone_stair", "Rock_Stone_Brick_Stairs")
mat("stone_half", "Rock_Stone_Brick_Half")
mat("darkwood_beam", "Wood_Darkwood_Beam")
mat("darkwood_fence", "Wood_Darkwood_Fence")
mat("darkwood_planks", "Wood_Darkwood_Planks")
mat("lantern", "Deco_Lantern")
mat("flag_orange", "Furniture_Flag_Orange")
mat("flag_red", "Furniture_Flag_Small_Red")
mat("torch", "Wood_Torch_Wall")
mat("brazier", "Furniture_Human_Ruins_Brazier")

# --------------------------------------------------------------------------- frames
class Frame:
    """A local frame on a path: u = to the driver's right, v = forward, h = up from the road surface."""

    def __init__(self, path, s):
        self.path = path
        self.s = s
        self.p, self.t, self.r = path.frame(s)
        self.T = path.Tat(s)
        self.ang = math.degrees(math.atan2(self.t[1], self.t[0]))

    def cell(self, u=0.0, v=0.0):
        q = self.p + self.r * u + self.t * v
        return int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))

    @property
    def axis_aligned(self):
        a = abs(self.ang) % 90
        return min(a, 90 - a) < 6

    def facing(self, back=False):
        """Compass letter of the travel direction (the side a sign's face points AWAY from)."""
        a = self.ang % 360
        names = ["e", "s", "w", "n"]
        d = names[int(round(a / 90.0)) % 4]
        if back:
            d = {"e": "w", "w": "e", "n": "s", "s": "n"}[d]
        return d


def lbox(W, F, u0, u1, v0, v1, y0, y1, m, rot=None, keep=False):
    """Fill a box in the frame: u in [u0,u1], v in [v0,v1] (ints, blocks), y in absolute [y0,y1]."""
    cells = set()
    for u in range(int(u0), int(u1) + 1):
        for v in range(int(v0), int(v1) + 1):
            cells.add(F.cell(u, v))
    xs = np.array([c[0] for c in cells])
    zs = np.array([c[1] for c in cells])
    for y in range(int(y0), int(y1) + 1):
        W.put_arr(xs, np.full(len(xs), y), zs, m, rot, keep)


def lcells(W, F, cells, m, rot=None):
    """cells: iterable of (u, y, v)."""
    for (u, y, v) in cells:
        x, z = F.cell(u, v)
        W.put(x, y, z, m, rot)


def lsign(W, F, text, u_center, y_top, v, scale, m, backing=None, pad=2):
    """Pixel text whose top row is at y_top, in the plane v (readable by a driver approaching from behind)."""
    w, h, cells = text_cells(text, scale)
    u0 = u_center - w / 2.0
    if backing:
        for u in range(int(math.floor(u0)) - pad, int(math.ceil(u0 + w)) + pad):
            for y in range(int(y_top - h) - pad + 1, int(y_top) + pad + 1):
                x, z = F.cell(u, v + 1)
                W.put(x, y, z, backing)
    for (c, r) in cells:
        x, z = F.cell(int(round(u0 + c)), v)
        W.put(x, int(y_top - r), z, m)
    return w, h


# --------------------------------------------------------------------------- road furniture
def checker(W, path, s0, length, hw, mats=("b_white", "b_black"), lift=0):
    """Checkered band across the road."""
    i0, i1 = path.i(s0), path.i(s0 + length)
    from path import raster_band
    cx, cz, si, uu = raster_band(path, i0, i1, -hw, hw)
    for x, z, sidx, u in zip(cx.tolist(), cz.tolist(), si.tolist(), uu.tolist()):
        s = path.s[sidx] - s0
        k = (int(math.floor(s)) + int(math.floor(u + 50))) % 2
        W.put(x, int(path.T[sidx]) + lift, z, mats[k])


def chevrons(W, path, s0, length, hw_inner, m, count=3, spacing=3, lift=0):
    """Boost-pad chevrons pointing along travel."""
    from path import paint

    def fn(s, u):
        v = s - s0
        for k in range(count):
            v0 = k * spacing
            if abs(abs(u) - (hw_inner - (v - v0))) < 0.85 and 0 <= v - v0 <= hw_inner + 0.5:
                return m
            if abs(abs(u) - (hw_inner - (v - v0))) < 0.85 and False:
                return m
        return None

    paint(W, path, path.i(s0), path.i(s0 + length), -hw_inner - 1, hw_inner + 1, fn, lift)


def item_boxes(W, path, s, hw, n=4, h0=5):
    """Floating rainbow item cubes across the road."""
    F = Frame(path, s)
    T = F.T
    for k in range(n):
        u = -hw * 0.66 + k * (hw * 1.32 / max(1, n - 1))
        cx, cz = F.cell(u, 0)
        W.box((cx - 1, T + h0, cz - 1), (cx + 1, T + h0 + 2, cz + 1), "lit_rainbow", mode="shell")
        W.put(cx, T + h0 + 1, cz, "lit_white")


def lamp_posts(W, path, i0, i1, every, off, h, pole="darkwood_fence", lamp="lit_yellowlight", both=True, skip=None):
    for i in range(i0 + every, i1, every):
        if skip and skip(i):
            continue
        T = int(path.T[i])
        for side in ((-1, 1) if both else (1,)):
            q = path.p[i] + path.r[i] * side * off
            x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
            for y in range(T, T + h):
                W.put(x, y, z, pole)
            W.put(x, T + h, z, lamp)


def hoop(W, c, d, r, m, thick=1, step=0.35, rot=None, ymin=-1):
    """A ring of radius r around centre c (x,y,z), lying in the plane perpendicular to direction d."""
    d = np.array(d, float)
    d /= np.linalg.norm(d)
    up = np.array([0, 1.0, 0]) if abs(d[1]) < 0.95 else np.array([1.0, 0, 0])
    a = np.cross(d, up)
    a /= np.linalg.norm(a)
    b = np.cross(d, a)
    n = max(24, int(2 * math.pi * r / step))
    th = np.linspace(0, 2 * math.pi, n, endpoint=False)
    pts = np.array(c, float)[None, :] + r * (np.cos(th)[:, None] * a[None, :] + np.sin(th)[:, None] * b[None, :])
    pts = np.floor(pts + 0.5).astype(np.int64)
    if thick > 1:
        o = np.arange(thick) - (thick - 1) // 2
        ox, oy, oz = np.meshgrid(o, o, o, indexing="ij")
        ox, oy, oz = ox.ravel(), oy.ravel(), oz.ravel()
        pts = (pts[:, None, :] + np.stack([ox, oy, oz], 1)[None, :, :]).reshape(-1, 3)
    keep = pts[:, 1] >= ymin
    pts = pts[keep]
    W.put_arr(pts[:, 0], pts[:, 1], pts[:, 2], m, rot)
    return a, b


def zone_gate(W, path, s, title, sub=None, scale=2, board_mat="b_black", text_mat="lit_white", frame_mat="gold_brick",
              trim_mat="lit_yellow", tower_h=None, extra_w=6, hw=7, tower_u=None, board_half=None, base_mat="k_finish_line"):
    """Big sign-arch over the road with pixel lettering, readable by the driver approaching."""
    F = Frame(path, s)
    if not F.axis_aligned:
        print("  note: gate '%s' is not axis aligned (%.0f deg)" % (title, F.ang))
    T = F.T
    w, h, _ = text_cells(title, scale)
    half = int(math.ceil(w / 2.0)) + extra_w
    half = max(half, hw + 6)
    if tower_u:
        half = tower_u
    bh = board_half or half
    th = tower_h or (h + 16)
    # towers
    for side in (-1, 1):
        u0 = side * half
        ua, ub = (u0 - 2, u0 + 2) if side > 0 else (u0 - 2, u0 + 2)
        lbox(W, F, ua, ub, -2, 2, T, T + th, frame_mat)
        lbox(W, F, ua + 1, ub - 1, -1, 1, T + th + 1, T + th + 3, frame_mat)
        for k in range(0, th, 4):
            lbox(W, F, ua, ub, -2, 2, T + k, T + k, trim_mat)
        if base_mat:                       # checkpoint feet: chequered Finish_Line blocks
            lbox(W, F, ua, ub, -2, 2, T, T + 1, base_mat)
        x, z = F.cell(u0, 0)
        W.put(x, T + th + 4, z, "lit_white")
    # board
    y_top = T + th - 1
    y_bot = y_top - h - 4
    lbox(W, F, -bh, bh, 0, 1, y_bot, y_top, board_mat)
    lbox(W, F, -bh, bh, -1, -1, y_bot, y_bot, frame_mat)
    lbox(W, F, -bh, bh, -1, -1, y_top, y_top, frame_mat)
    lbox(W, F, -bh, bh, 0, 0, y_top + 1, y_top + 1, trim_mat)
    lbox(W, F, -bh, bh, 0, 0, y_bot - 1, y_bot - 1, trim_mat)
    lsign(W, F, title, 0, y_top - 2 if not sub else y_top - 1, -1, scale, text_mat)
    if sub:
        lsign(W, F, sub, 0, y_bot + 6 + (0), -1, 1, "lit_yellow")
    return F


def bunting(W, path, i0, i1, every, off, h, mats=("wool_red", "wool_white", "wool_yellow", "wool_blue")):
    """Colour flags on poles along the road edge."""
    k = 0
    for i in range(i0, i1, every):
        T = int(path.T[i])
        for side in (-1, 1):
            q = path.p[i] + path.r[i] * side * off
            x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
            for y in range(T, T + h):
                W.put(x, y, z, "darkwood_fence")
            W.put(x, T + h, z, mats[k % len(mats)])
            W.put(x, T + h - 1, z, mats[k % len(mats)])
        k += 1


# --------------------------------------------------------------------------- organics
def blob(W, rng, cx, cy, cz, r, m, n=5, squash=0.8, keep=False):
    """A lumpy ball (hollow when big): a few overlapping ellipsoids."""
    mode = "shell" if r > 3.2 else "fill"
    W.ellipsoid(cx, cy, cz, r, r * squash, r, m, keep=keep, mode=mode)
    for _ in range(n):
        ox, oz = rng.uniform(-r * 0.6, r * 0.6, 2)
        oy = rng.uniform(-r * 0.2, r * 0.4)
        rr = r * rng.uniform(0.5, 0.75)
        W.ellipsoid(int(cx + ox), int(cy + oy), int(cz + oz), rr, rr * squash, rr, m, keep=keep, mode="shell" if rr > 3.2 else "fill")


def oak(W, rng, x, y, z, h=None, leaf="leaf_oak", trunk="trunk_oak"):
    h = h or int(rng.integers(5, 9))
    for k in range(h):
        W.put(x, y + k, z, trunk)
    r = 2.4 + h * 0.32
    blob(W, rng, x, y + h, z, r, leaf, n=3)


def pine(W, rng, x, y, z, h=None, leaf="leaf_fir", trunk="trunk_fir", snow=None):
    h = h or int(rng.integers(9, 15))
    for k in range(h):
        W.put(x, y + k, z, trunk)
    layers = max(3, h // 2)
    for i in range(layers):
        r = 1 + (layers - i) * 0.85
        yy = y + 2 + i * 2
        W.cyl(x, z, yy, yy, r, leaf, mode="wall" if r > 2.2 else "fill")
        if r > 2.2:
            W.cyl(x, z, yy + 1, yy + 1, r - 1.0, leaf, mode="wall" if r > 3.4 else "fill")
        if snow and i % 2 == 0 and r > 1.6:
            W.cyl(x, z, yy + 2, yy + 2, r - 1.5, snow, mode="fill") if r < 3.2 else None
    W.put(x, y + h, z, leaf)
    W.put(x, y + h + 1, z, leaf)


mat("leaf_oak", ["Plant_Leaves_Oak", "Plant_Leaves_Oak", "Plant_Leaves_Oak", "Plant_Leaves_Maple"])
mat("trunk_oak", "Wood_Oak_Trunk")
mat("leaf_fir", ["Plant_Leaves_Fir", "Plant_Leaves_Fir", "Plant_Leaves_Fir_Snow"])
mat("trunk_fir", "Wood_Fir_Trunk")
mat("leaf_birch", ["Plant_Leaves_Birch", "Plant_Leaves_Birch", "Plant_Leaves_Autumn"])
mat("trunk_birch", "Wood_Birch_Trunk")
mat("leaf_palm", "Plant_Leaves_Palm")
mat("trunk_palm", "Wood_Palm_Trunk")
mat("flowers", ["Plant_Flower_Common_Pink", "Plant_Flower_Common_Yellow", "Plant_Flower_Common_White", "Plant_Flower_Common_Red",
                "Plant_Flower_Common_Blue", "Plant_Grass_Lush_Tall", "Plant_Grass_Lush", "Plant_Flower_Tall_Yellow"])
mat("grass_tall", ["Plant_Grass_Lush_Tall", "Plant_Grass_Lush", "Plant_Grass_Lush_Short"])


mat("cloud", ["Cloth_Block_Wool_White", "Cloth_Block_Wool_White", "Cloth_Block_Wool_Gray_Light"])


def cloud(W, rng, cx, cy, cz, sx, sy, sz, n=3, m="cloud"):
    """A puffy cloud: a hollow ellipsoid plus a few overlapping hollow puffs."""
    W.ellipsoid(cx, cy, cz, sx, sy, sz, m, mode="shell")
    for _ in range(n):
        ox, oz = rng.uniform(-sx * 0.8, sx * 0.8), rng.uniform(-sz * 0.6, sz * 0.6)
        r = rng.uniform(0.45, 0.75)
        W.ellipsoid(int(cx + ox), int(cy + rng.uniform(-1, sy * 0.4)), int(cz + oz), sx * r, sy * r * 1.1, sz * r, m, mode="shell")


def spire(W, x, y, z, h, r0, m, lean=(0.0, 0.0), hollow=True, rtop=0.4):
    """A tapering vertical spike (hollow rings), leaning (lean = blocks of drift per block of height)."""
    for k in range(int(h)):
        r = r0 * (1 - k / float(h)) + rtop
        cx, cz = int(round(x + lean[0] * k)), int(round(z + lean[1] * k))
        W.cyl(cx, cz, y + k, y + k, r, m, mode="wall" if (hollow and r > 1.9) else "fill")


def road_grid(ctx, g):
    """Boolean grid (like g.X): True where any lap road (plus a margin) runs, so decoration keeps off it."""
    keys = (g.X + 4096) | ((g.Z + 4096) << 14)
    return np.isin(keys, ctx.road_keys)
