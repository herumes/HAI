from lib import *
import lib
import kit
from kit import *
from PIL import Image, ImageDraw

kit.NX, kit.NY, kit.NZ = 76, 82, 112
import importlib
# rebuild the grids for the bigger volume
kit.X, kit.Y, kit.Z = np.meshgrid(np.arange(76, dtype=np.float32), np.arange(82, dtype=np.float32), np.arange(112, dtype=np.float32), indexing="ij")
X, Y, Z = kit.X, kit.Y, kit.Z
NX, NY, NZ = 76, 82, 112

R = random.Random(11)
pal.update({
    "bone": ["Rock_Marble", "Rock_Marble", "Rock_Chalk"],
    "bone2": ["Rock_Chalk", "Rock_Salt", "Rock_Marble", "Rock_Chalk"],
    "tooth": ["Rock_Marble", "Rock_Marble", "Rock_Salt"],
    "dark": ["Rock_Basalt", "Rock_Slate", "Rock_Basalt"],
    "orange": "NoCube_Neon_Block_Orange",
    "yellow": "NoCube_Neon_Block_Yellow",
    "green": "NoCube_Neon_Block_Green",
    "purple": "NoCube_Neon_Block_Purple",
    "nblack": "NoCube_Neon_Block_Black",
    "fred": "NoCube_Neon_Block_Fire_Red",
    "earth": ["Soil_Ash", "Soil_Ash", "Soil_Mud", "Soil_Dirt_Burnt", "Soil_Ash"],
    "rock": ["Rock_Shale", "Rock_Basalt", "Rock_Slate", "Rock_Shale"],
    "mossy": ["Plant_Moss_Block_Green_Dark", "Soil_Ash", "Soil_Mud"],
    "bark": ["Wood_Burnt_Trunk_Full", "Wood_Burnt_Trunk_Full", "Wood_Petrified_Trunk_Full"],
    "path": ["Rock_Slate_Brick", "Rock_Stone_Brick_Mossy", "Rock_Slate_Brick", "Rock_Stone_Cobble_Mossy"],
    "pump": ["Rock_Dawnstone", "Rock_Dawnstone", "Rock_Dawnstone", "Rock_Peach_Brick"],
    "lite_y": "Build_Lightsource_Yellow",
    "lite_o": "Build_Lightsource_Orange",
    "lite_g": "Build_Lightsource_Green",
    "stem": ["Wood_Jungle_Trunk_Full", "Wood_Jungle_Trunk_Full", "Wood_Bottletree_Trunk_Full"],
})

SX, Z0 = 36, 36
world = {}


def put(cells, cls, owner, over=True):
    for c in cells:
        c = (int(c[0]), int(c[1]), int(c[2]))
        if not over and c in world:
            continue
        world[c] = (cls, owner)


def maskcells(mask):
    return [tuple(int(v) for v in p) for p in np.argwhere(mask)]


def W(u, y, v):
    return (SX + u, y, Z0 + v)


def sym(f):
    return np.minimum(f(1), f(-1))


def rbox(c, h, r):
    return box_sdf(c, (h[0] - r, h[1] - r, h[2] - r)) - r


def chain(pointsr, cls, owner, over=True):
    cs = set()
    last = None
    for (a, ra), (b, rb) in zip(pointsr, pointsr[1:]):
        n = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]), abs(b[2] - a[2]))) + 1
        for i in range(n + 1):
            t = i / n
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
            r = ra + (rb - ra) * t
            c0 = (round(p[0]), round(p[1]), round(p[2]))
            if r <= 1.05:
                if last is not None:
                    cur = last
                    while cur != c0:
                        d = [c0[k] - cur[k] for k in range(3)]
                        k = max(range(3), key=lambda k: abs(d[k]))
                        cur = tuple(cur[j] + (1 if d[k] > 0 else -1 if j == k else 0) if j == k else cur[j] for j in range(3))
                        cs.add(cur)
                cs.add(c0)
            else:
                for c in thick([c0], max(0.5, r)):
                    cs.add(c)
            last = c0
    put(sorted(cs), cls, owner, over)
    return cs


extras = []   # (block, attrs, [cells]) emitted as 'set' ops at the end


def xtra(block, cells, **attrs):
    extras.append((block, attrs, [tuple(int(round(v)) for v in c) for c in cells], lib.PART))


# ======================================================================= SKULL
cran = ell(W(0, 24, -1), (15.5, 13, 15.5))
maxi = rbox(W(0, 13.5, 8.5), (8.8, 7.2, 8.6), 3.8)
arch = sym(lambda s: cap(W(s * 12.8, 18, 2), W(s * 9.6, 14.8, 13.5), 2.3))
ram = sym(lambda s: tcap(W(s * 11.8, 13, -1), W(s * 9.8, 1, 7), 2.8, 2.2))
brow = sym(lambda s: ell(W(s * 6.9, 27.6, 12.7), (5.6, 2.3, 3.6)))
occ = ell(W(0, 9, -6), (12, 9, 11))
body = union(cran, maxi, occ, k=2.2)
body = union(body, arch, ram, brow, k=1.3)
sock = sym(lambda s: np.minimum(ell(W(s * 6.7, 22, 12.7), (4.7, 4.7, 4.7)), ell(W(s * 8.6, 21, 11.8), (3.8, 3.8, 3.8))))
nose = np.minimum(ell(W(0, 15.5, 16), (2.4, 3.6, 4)), ell(W(0, 12.6, 16), (3.4, 1.5, 4)))
T = 0.95
solid = (body < 0) & (body >= -T) & ~(sock < 0) & ~(nose < 0) & (Y >= 0)
N1 = vnoise(6, 3)
cls = np.full(solid.shape, "bone", dtype=object)
cls[N1 > 0.6] = "bone2"
for c in maskcells(solid):
    put([c], cls[c], "sym")

cz = Z0 + 8.5
for k in range(-8, 9):
    th = math.radians(k * 10.5)
    x0 = int(round(SX + 8.6 * math.sin(th)))
    z0 = int(round(cz + 8.3 * math.cos(th)))
    ln = 5 if abs(k) in (2, 3) else R.choice([3, 4, 4])
    for dy in range(ln):
        for dx in ((0, 1) if abs(k) < 6 else (0,)):
            put([(x0 + dx, 6 - dy, z0), (x0 + dx, 6 - dy, z0 - 1)], "tooth", "sym")
for k in range(-9, 10):
    th = math.radians(k * 9.5)
    x0 = int(round(SX + 7.9 * math.sin(th)))
    z0 = int(round(cz + 7.6 * math.cos(th)))
    if abs(x0 - SX) <= 3:
        continue
    for dy in range(2):
        put([(x0, dy, z0), (x0, dy, z0 - 1)], "bone", "sym")
    if k % 2 == 0:
        for dy in range(2, 2 + R.choice([2, 3])):
            put([(x0, dy, z0)], "tooth", "sym")
for s in (-1, 1):
    c = W(s * 6.8, 22, 12.7)
    d = np.sqrt((X - c[0]) ** 2 + (Y - c[1]) ** 2 + (Z - c[2]) ** 2)
    put(maskcells((d >= 4.5) & (d < 5.8) & (Z < c[2] - 1.0)), "dark", "sym")
    put(maskcells(ell(W(s * 6.9, 22, 7.2), (2.5, 2.5, 2.5)) < 0), "lite_o", "sym")
c = W(0, 14.5, 16)
d = np.sqrt((X - c[0]) ** 2 + (Y - c[1]) ** 2 + (Z - c[2]) ** 2)
put(maskcells((d >= 4.4) & (d < 5.6) & (Z < c[2] - 1.5) & (np.abs(X - c[0]) < 3.5)), "dark", "sym")
bodymask = body < 0

# ======================================================================= RIB AVENUE (symmetric)
rib_z = [62, 68, 74, 80, 86]
rib_h = [19, 18, 17, 16, 15]
rib_w = [12.5, 12, 11.5, 11, 10.5]
for z, h, hw in zip(rib_z, rib_h, rib_w):
    cells = set()
    prev = None
    for i in range(0, 29):
        ph = math.pi * i / 28
        u = hw * math.cos(ph)
        y = h * math.sin(ph) ** 0.75
        p = (SX + u, y, z)
        if prev:
            for c in bres((round(prev[0]), round(prev[1]), round(prev[2])), (round(p[0]), round(p[1]), round(p[2]))):
                cells.add(c)
        prev = p
    cells = thick(sorted(cells), 1.05)
    put([c for c in cells if c[1] >= 0], "bone", "sym")
    for s in (-1, 1):
        put([(round(SX + s * hw) + dx, 0, z + dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1)], "rock", "sym")
    # hanging lantern at the crown
    put([(SX, h - 2, z), (SX, h - 3, z)], "Deco_Rope", "sym")
    put([(SX, h - 4, z)], "Deco_Halloween_Pumpkin_Scary", "sym")


# ======================================================================= PATH, POSTS (symmetric)
for z in range(44, 96):
    for x in range(31, 42):
        put([(x, 0, z)], "path", "sym")
for z in (65, 71, 77, 83, 89, 95):
    for x in (30, 42):
        put([(x, 0, z), (x, 1, z)], "dark", "sym")
        put([(x, 2, z)], "Furniture_Temple_Dark_Brazier", "sym")
for z in range(52, 96, 6):
    for x in (34, 38):
        put([(x, 0, z)], "lite_g", "sym")
# hall braziers
for z in (47, 53):
    for x in (30, 42):
        put([(x, 0, z), (x, 1, z), (x, 2, z)], "dark", "sym")
        put([(x, 3, z)], "Furniture_Temple_Dark_Brazier", "sym")

# ======================================================================= GIANT PUMPKINS (symmetric)
for sgn in (-1, 1):
    pc = (SX + sgn * 16, 5.6, 99)
    ang = np.arctan2(Z - pc[2], X - pc[0])
    lobe = 1 + 0.07 * np.cos(10 * ang)
    n = np.sqrt(((X - pc[0]) / (6.6 * lobe)) ** 2 + ((Y - pc[1]) / (5.6)) ** 2 + ((Z - pc[2]) / (6.6 * lobe)) ** 2)
    shell = (n < 1.0) & (n >= 1 - 1.15 / 5.6) & (Y >= 0)
    for c in maskcells(shell):
        a_ = math.atan2(c[2] - pc[2], c[0] - pc[0])
        put([c], "fred" if math.cos(10 * a_) < -0.35 else "orange", "sym")
    eyes = [(-4, 2), (-3, 2), (-2, 2), (-1, 2), (-3, 3), (-2, 3), (-3, 4)]
    eyesR = [(-dx, dy) for dx, dy in eyes]
    mouth = [(dx, -1) for dx in range(-5, 6)] + [(dx, -2) for dx in range(-4, 5) if dx % 2 == 0] + [(dx, 0) for dx in (-5, -3, -1, 1, 3, 5)]
    nose = [(0, 1), (0, 0), (-1, 1), (1, 1)]
    for dx, dy in eyes + eyesR + mouth + nose:
        x = int(round(pc[0] + dx))
        y = int(round(pc[1] + dy))
        zz = None
        for z in range(int(pc[2]), int(pc[2]) + 10):
            if (x, y, z) in world and world[(x, y, z)][0] in ("orange", "fred"):
                zz = z
        if zz is not None:
            put([(x, y, zz)], "yellow" if (dx + dy) % 3 else "lite_y", "sym")
    # stem + curl
    for y in range(11, 15):
        put([(int(pc[0]), y, int(pc[2])), (int(pc[0]) + 1, y, int(pc[2]))], "stem", "sym")
    put([(int(pc[0]) + 2, 14, int(pc[2])), (int(pc[0]) + 3, 14, int(pc[2])), (int(pc[0]) + 3, 13, int(pc[2]))], "stem", "sym")

def prune(owner, minsize):
    cells = {c for c, v in world.items() if v[1] == owner}
    seen = set()
    N6 = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    removed = 0
    for c in list(cells):
        if c in seen:
            continue
        comp = [c]
        seen.add(c)
        i = 0
        while i < len(comp):
            x, y, z = comp[i]
            i += 1
            for dx, dy, dz in N6:
                n = (x + dx, y + dy, z + dz)
                if n in cells and n not in seen:
                    seen.add(n)
                    comp.append(n)
        if len(comp) < minsize:
            for q in comp:
                del world[q]
            removed += len(comp)
    print("pruned", owner, removed)


prune("sym", 12)
# ======================================================================= emit symmetric group + mirror
sk = Vox()
sk.cells = {c: v[0] for c, v in world.items() if v[1] == "sym"}
nops = sk.emit(xmax=SX)
lib.ops.append({"op": "mirror", "axis": "x", "at": SX})
print("sym ops", nops, "cells", len(sk.cells))
symcells = set(sk.cells.keys())

# ======================================================================= CLIFFS + MOUND
Nh = vnoise(8, 21)
Nh2 = vnoise(3.5, 22)
Nh3 = vnoise(2.2, 23)
SPIRES = [(10, 27, 24, 5.5), (63, 25, 20, 5.0), (6, 40, 12, 4.2), (68, 38, 10, 4.2), (17, 20, 15, 4.2), (56, 20, 13, 4.2)]
H = np.zeros((NX, NZ), dtype=np.float32)
for x in range(NX):
    for z in range(NZ):
        dx = (x - SX) / 30.0
        dz = (z - 27) / 21.0
        r2 = dx * dx + dz * dz
        base = 8.5 * max(0.0, 1 - r2) ** 0.85
        for side in (-1, 1):
            if 16 <= z <= 78:
                xc = SX + side * (27 - 0.12 * (z - 16))
                w = 8.5 - 0.06 * (z - 16)
                hh = 10.0 * max(0.0, 1 - (z - 16) / 48.0) ** 1.3
                base = max(base, hh * math.exp(-((x - xc) / w) ** 2))
        for (sx_, sz_, sh_, sr_) in SPIRES:
            d = math.hypot(x - sx_, (z - sz_) * 0.9)
            if d < sr_:
                base = max(base, sh_ * (1 - d / sr_) ** 0.7)
        base += 3.0 * (Nh[x, 4, z] - 0.5) * 2 * min(1, base / 5)
        base += 1.4 * (Nh2[x, 5, z] - 0.5) * 2 * min(1, base / 3)
        base += 0.9 * (Nh3[x, 6, z] - 0.5) * 2 * min(1, base / 3)
        if z > Z0 + 6 and abs(x - SX) < 15:
            base *= max(0.0, min(1.0, (abs(x - SX) - 9) / 6.0))
        H[x, z] = base if base > 0.7 else 0


def ground(x, z):
    return int(H[x, z]) + 1 if H[x, z] > 0 else 0


mound = {}
for x in range(NX):
    for z in range(NZ):
        h = int(H[x, z])
        if h <= 0:
            continue
        nb = [H[min(NX - 1, x + 1), z], H[max(0, x - 1), z], H[x, min(NZ - 1, z + 1)], H[x, max(0, z - 1)]]
        lo = int(max(0, min(nb)))
        slope = max(abs(h - int(v)) for v in nb)
        for y in range(max(0, min(lo, h)), h + 1):
            c = (x, y, z)
            if c in symcells or bodymask[x, y, z]:
                continue
            top = (y == h)
            if slope >= 3 and not top:
                k = "rock"
            elif top and Nh2[x, 6, z] > 0.64 and h > 3:
                k = "mossy"
            elif slope >= 2:
                k = "rock"
            else:
                k = "earth"
            mound[c] = k
for c, k in mound.items():
    world[c] = (k, "mound")

# ======================================================================= MOON + silhouettes
MCx, MCy, MCz = 36, 56, 10
moon = ell((MCx, MCy, MCz), (24.4, 24.4, 0.7)) < 0
Nm = vnoise(6, 31)
Nm2 = vnoise(2.5, 32)
mc = maskcells(moon)
for c in mc:
    k = "orange"
    if Nm[c] > 0.68:
        k = "yellow"
    elif Nm2[c] < 0.07:
        k = "fred"
    put([c], k, "moon")
rim = maskcells((ell((MCx, MCy, MCz + 1), (24.4, 24.4, 0.7)) < 0) & (ell((MCx, MCy, MCz + 1), (22.8, 22.8, 0.7)) >= 0))
put(rim, "orange", "moon")


def silhouette(draw_fn, w, h, ox, oy, z, cls="nblack", scale=1.0):
    S_ = 6
    img = Image.new("L", (w * S_, h * S_), 0)
    d = ImageDraw.Draw(img)
    draw_fn(d, S_ * scale)
    img = img.resize((w, h), Image.BOX)
    cells = []
    for r in range(h):
        for c in range(w):
            if img.getpixel((c, r)) > 110:
                cells.append((ox + c, oy + (h - 1 - r), z))
    put(cells, cls, "moonart")
    return cells


def witch(d, S_):
    P = lambda x, y: (x * S_, y * S_)
    # broom handle
    d.line([P(2, 17), P(33, 9)], fill=255, width=int(1.3 * S_))
    # bristles
    d.polygon([P(0, 12), P(5, 17), P(3, 19), P(-1, 21)], fill=255)
    d.polygon([P(1, 14), P(8, 17.5), P(4, 21)], fill=255)
    # robe + cloak
    d.polygon([P(15, 9), P(23, 10), P(26, 14), P(20, 14), P(14, 15), P(10, 13)], fill=255)
    d.polygon([P(18, 9), P(22, 3.5), P(24, 9)], fill=255)          # torso up
    d.ellipse([P(20, 1.2), P(24.5, 6)], fill=255)                  # head
    # hat
    d.polygon([P(19, 2.2), P(25.5, 2.2), P(23, -4), P(21.5, -5.5)], fill=255)
    d.line([P(17.5, 2.4), P(27.5, 2.4)], fill=255, width=int(1.2 * S_))
    # hair streaming back
    d.polygon([P(20, 3), P(14, 2), P(10, 6), P(16, 5)], fill=255)
    # arm to broom
    d.line([P(22, 8), P(27, 10.5)], fill=255, width=int(1.0 * S_))
    # cat
    d.ellipse([P(28.5, 5), P(31.5, 9.2)], fill=255)
    d.polygon([P(28.5, 5.5), P(29, 3.5), P(30, 5)], fill=255)
    d.polygon([P(30.3, 5), P(31, 3.5), P(31.8, 5.6)], fill=255)


def bat(d, S_):
    P = lambda x, y: (x * S_, y * S_)
    d.ellipse([P(5, 3), P(7.2, 6)], fill=255)
    d.polygon([P(5.2, 4), P(3, 1.5), P(0, 3), P(1.5, 3.8), P(0.5, 5.5), P(3, 4.8)], fill=255)
    d.polygon([P(7, 4), P(9.2, 1.5), P(12.2, 3), P(10.7, 3.8), P(11.7, 5.5), P(9.2, 4.8)], fill=255)
    d.polygon([P(5.4, 3.2), P(5.8, 1.8), P(6.2, 3.2)], fill=255)


import witch3
put(witch3.cells(21, 55, MCz + 1), 'nblack', 'moonart')
silhouette(bat, 13, 8, 22, 38, MCz + 1)
silhouette(bat, 13, 8, 45, 37, MCz + 1)
silhouette(bat, 13, 8, 34, 44, MCz + 1)

# ======================================================================= TREE (behind the skull, holding the moon)
tz = MCz + 1.6
trunk = [((15, 3, 18), 2.9), ((15, 14, 16), 2.6), ((16, 26, 14), 2.2), ((18, 40, 12.6), 1.9), ((20, 52, 11.8), 1.6), ((20, 58, tz), 1.4)]
chain(trunk, "bark", "tree")
hub = (20, 58, tz)


def limb(p, ang, length, rad, depth):
    if depth > 3 or length < 4:
        return
    steps = max(2, int(length / 4))
    pts_ = [(p, rad)]
    cur = p
    a = ang
    for i in range(steps):
        a += R.uniform(-0.3, 0.3)
        cur = (cur[0] + math.cos(a) * length / steps, cur[1] + math.sin(a) * length / steps, tz + R.uniform(-0.3, 0.3))
        pts_.append((cur, max(0.5, rad * (1 - (i + 1) / steps * 0.55))))
    chain(pts_, "bark", "tree")
    for k in range(R.choice([1, 2, 2])):
        limb(pts_[R.randint(1, len(pts_) - 1)][0], a + R.choice([-1, 1]) * R.uniform(0.5, 1.0), length * R.uniform(0.5, 0.7), max(0.5, rad * 0.62), depth + 1)
    limb(cur, a + R.uniform(-0.3, 0.3), length * 0.6, max(0.5, rad * 0.6), depth + 1)


for ang, ln, rd in [(2.2, 20, 1.3), (1.65, 24, 1.2), (2.8, 17, 1.1), (1.2, 14, 1.0), (3.3, 12, 0.9)]:
    limb(hub, ang, ln, rd, 0)
# ======================================================================= DETAILS ON THE SKULL (cracks, moss)
pal["crack"] = ["Rock_Shale", "Rock_Slate"]
pal["moss"] = ["Plant_Moss_Block_Green_Dark", "Plant_Moss_Block_Green_Dark", "Plant_Moss_Block_Green"]


def surf_y(x, z):
    t = 1 - ((x - SX) / 15.5) ** 2 - ((z - (Z0 - 1)) / 15.5) ** 2
    return 24 + 13 * math.sqrt(max(t, 0))


def polyline(pp, n=12):
    out = []
    for a_, b_ in zip(pp, pp[1:]):
        for t in np.linspace(0, 1, n):
            x = a_[0] + (b_[0] - a_[0]) * t
            z = a_[1] + (b_[1] - a_[1]) * t
            out.append((x, surf_y(x, z), z))
    return out


cracks = polyline([(SX - 1, Z0 - 4), (SX - 3, Z0 + 1), (SX - 2, Z0 + 5), (SX - 5, Z0 + 8), (SX - 4, Z0 + 12)]) + \
         polyline([(SX - 3, Z0 + 1), (SX - 8, Z0 - 1), (SX - 11, Z0 + 3)]) + \
         polyline([(SX + 6, Z0 - 8), (SX + 5, Z0 - 2), (SX + 8, Z0 + 3), (SX + 7, Z0 + 8)])
Nm3 = vnoise(3, 55)
for c, (cl, ow) in list(world.items()):
    if ow == "sym" and cl in ("bone", "bone2"):
        x, y, z = c
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 + (z - p[2]) ** 2 < 0.95 for p in cracks):
            world[c] = ("crack", "over")
        elif y >= 27 and (x, y + 1, z) not in world and Nm3[x, y, z] > 0.66:
            world[c] = ("moss", "over")

# ======================================================================= INTERIOR OF THE SKULL
lib.PART = 'A'
for (u, v) in [(0, -4), (-7, -7), (7, -7), (0, 5), (-9, 0), (9, 0), (0, -10)]:
    x, z = SX + u, Z0 + v
    for y in range(36, 12, -1):
        if (x, y, z) in world and (x, y - 1, z) not in world:
            xtra("Furniture_Kweebec_Lantern_Ceiling", [(x, y - 1, z)])
            break
sph([SX, 4, Z0 - 4], [3.4, 2.9, 3.4], "dark", "shell", "lower")
cyl([SX, 4, Z0 - 4], 2.4, 1, "green")
xtra("Build_Lightsource_Green", [(SX, 4, Z0 - 4)])
xtra("Wood_Burnt_Trunk_Full", [(SX - 1, 0, Z0 - 5), (SX + 1, 0, Z0 - 5), (SX - 1, 0, Z0 - 3), (SX + 1, 0, Z0 - 3), (SX, 0, Z0 - 5), (SX, 0, Z0 - 3), (SX - 1, 0, Z0 - 4), (SX + 1, 0, Z0 - 4)])
xtra("Deco_Fire", [(SX, 0, Z0 - 4)])
xtra("Deco_Bone_Skulls", [(SX - 6, 0, Z0 - 8), (SX + 5, 0, Z0 - 2), (SX + 7, 0, Z0 - 9)])
xtra("Deco_Bone_Pile", [(SX - 4, 0, Z0 + 2), (SX + 8, 0, Z0 + 3), (SX - 8, 0, Z0 - 3)])
box([SX - 3, 0, 24], [SX + 3, 1, 26], "slate")
setb([SX, 2, 25], "Furniture_Dungeon_Chest_Legendary_Large", rot=0)
pts([(SX - 3, 2, 24), (SX + 3, 2, 24)], "Deco_Halloween_Pumpkin_Scary")
pts([(SX - 2, 2, 26), (SX + 2, 2, 26), (SX - 1, 2, 24), (SX + 1, 2, 24)], "Furniture_Ancient_Candle")
for cx_ in (SX - 9, SX + 9):
    box([cx_, 0, 27], [cx_, 3, 27], "dark")
    setb([cx_, 4, 27], "Furniture_Ancient_Candle")
xtra("Deco_SpiderWeb_Full", [(SX - 10, 14, Z0 - 6), (SX + 10, 15, Z0 - 7), (SX - 3, 33, Z0 - 10), (SX + 6, 32, Z0 + 2)])

# ======================================================================= GRAVEYARD
lib.PART = 'C'
pal["grave_dirt"] = ["Soil_Dirt_Burnt", "Soil_Mud", "Soil_Dirt_Burnt"]
pal["blackwood"] = ["Wood_Blackwood_Planks", "Wood_Darkwood_Planks"]
pal["slate"] = ["Rock_Slate_Brick", "Rock_Slate_Brick", "Rock_Stone_Brick_Mossy"]
occ_ = set()
tombs = {0: [], 1: [], 2: [], 3: []}
zones_excl = [(7, 20, 66, 82), (52, 66, 64, 80), (26, 46, 50, 99)]


def blocked(x, z):
    for (x0, x1, z0, z1) in zones_excl:
        if x0 <= x <= x1 and z0 <= z <= z1:
            return True
    return False


pal['patch'] = ['Soil_Dirt_Burnt_Half', 'Soil_Gravel_Half', 'Soil_Dirt_Burnt_Half', 'Soil_Mud']
for side, cols in ((-1, [9, 13, 17, 21]), (1, [51, 55, 59, 63])):
    for row, zz in enumerate(range(58, 92, 3)):
        for cx in cols:
            if R.random() < 0.2:
                continue
            x = cx + R.randint(-1, 1)
            z = zz + R.randint(-1, 1)
            if blocked(x, z) or any(abs(x - a) < 3 and abs(z - b) < 3 for a, b in occ_):
                continue
            occ_.add((x, z))
            g = ground(x, z)
            for pdx in (-1, 0, 1):
                for pdz in (-1, 0, 1):
                    if R.random() < 0.75 and ground(x + pdx, z + pdz) == g:
                        put([(x + pdx, g, z + pdz)], 'patch', 'patchcells', over=False)
            face = 3 if side < 0 else 1       # towards the avenue
            kind = R.random()
            mx = x + (1 if side < 0 else -1)
            if kind < 0.56:
                tombs[face].append((x, g, z))
                put([(mx, g, z)], "grave_dirt", "grave")
                if R.random() < 0.35:
                    xtra('Furniture_Ancient_Candle', [(mx, g + 1, z)])
            elif kind < 0.74:
                put([(x, g, z), (x, g + 1, z), (x, g + 2, z), (x, g + 3, z), (x - 1, g + 2, z), (x + 1, g + 2, z)], "blackwood", "grave")
                put([(mx, g, z)], "grave_dirt", "grave")
            elif kind < 0.86:
                put([(x, g + k_, z) for k_ in range(5)], "slate", "grave")
                put([(x, g + 5, z)], "Rock_Stone_Brick_Half", "grave")
            elif kind < 0.93:
                put([(x, g, z), (x, g + 1, z)], "slate", "grave")
                xtra("Furniture_Temple_Dark_Statue", [(x, g + 2, z)], rot=face)
            else:
                put([(x, g, z), (x, g + 1, z)], "slate", "grave")
                xtra("Deco_Halloween_Pumpkin_Scary", [(x, g + 2, z)])
for rot_, cells_ in tombs.items():
    if cells_:
        xtra("Furniture_Village_Tombstone", cells_, rot=rot_)
# open graves with coffins
for (gx, gz) in [(61, 59), (11, 62)]:
    xtra("Furniture_Ancient_Coffin", [(gx, 0, gz)], rot=1)
    xtra("Deco_Bone_Skulls", [(gx + 3, 0, gz - 1)])
    xtra("Deco_Bone_Pile", [(gx - 2, 0, gz + 3)])
# dead shrubs in the plots
shrubs = []
for _ in range(95):
    x = R.choice([R.randint(5, 24), R.randint(48, 67)])
    z = R.randint(56, 94)
    if blocked(x, z) or (x, z) in occ_:
        continue
    shrubs.append((x, ground(x, z), z))
xtra("Plant_Bush_Dead_Twisted", shrubs[:30])
xtra("Plant_Grass_Gnarled_Tall", shrubs[30:60])
xtra("Plant_Bush_Dead", shrubs[60:])

# iron fences with stone posts
for xf in (5, 67):
    line([xf, 0, 56], [xf, 0, 94], "Metal_Iron_Fence")
    for zp in range(56, 95, 6):
        put([(xf, 0, zp), (xf, 1, zp), (xf, 2, zp)], "slate", "post")
        if (zp // 6) % 2:
            xtra("Deco_Halloween_Pumpkin_Scary", [(xf, 3, zp)])
        else:
            put([(xf, 3, zp)], "purple", "post")


def ghost(x0, y0, z0):
    put([(x, y, z) for x in range(x0, x0 + 3) for y in range(y0, y0 + 3) for z in range(z0, z0 + 3)], "Cloth_Block_Wool_White", "ghost")
    put([(x0 + 1, y0 + 3, z0 + 1), (x0, y0 + 3, z0 + 1), (x0 + 2, y0 + 3, z0 + 1), (x0 + 1, y0 + 3, z0), (x0 + 1, y0 + 3, z0 + 2),
         (x0 - 1, y0 + 1, z0 + 1), (x0 + 3, y0 + 2, z0 + 1)], "Cloth_Block_Wool_White", "ghost")
    put([(x0, y0 + 2, z0 + 2), (x0 + 2, y0 + 2, z0 + 2), (x0 + 1, y0 + 1, z0 + 2)], "nblack", "ghost")
    for c in [(x0 + 1, y0, z0 + 2), (x0, y0, z0 + 1), (x0 + 2, y0, z0)]:
        world.pop(c, None)


ghost(21, 0, 68)
ghost(50, 0, 85)
ghost(29, 0, 92)

# ======================================================================= CRYPT (left plot)
box([8, 0, 67], [18, 1, 79], "slate", "walls")
box([8, 2, 67], [18, 7, 79], "Rock_Stone_Brick_Mossy", "walls")
for (x_, z_) in [(8, 67), (18, 67), (8, 79), (18, 79), (8, 73), (18, 70), (18, 76)]:
    box([x_, 0, z_], [x_, 7, z_], "Rock_Stone_Brick_Pillar_Middle")
box([8, 8, 67], [18, 8, 79], "slate", "walls")
lib.ops.append({"op": "gable", "from": [8, 9, 67], "to": [18, 9, 79], "block": "Rock_Slate_Brick_Roof", "ridge": "z", "cap": "Rock_Slate_Brick_Roof_Flat",
                "overhang": 1, "end_overhang": 1, "gable_fill": "Rock_Stone_Brick_Mossy"})
box([18, 1, 73], [18, 3, 73], "air")
setb([18, 1, 73], "Furniture_Ancient_Door", rot=1)
setb([18, 4, 73], "Rock_Stone_Brick_Ornate")
setb([19, 5, 73], "Deco_Bone_Skulls")
stairs([19, 0, 73], "w", 1, 3, "Rock_Stone_Brick_Stairs")
for z_ in (70, 76):
    box([18, 3, z_], [18, 5, z_], "green")
    box([8, 3, z_], [8, 5, z_], "green")
setb([13, 12, 67], "green")
setb([13, 13, 67], "green")
pts([(8, 9, 66), (18, 9, 66), (8, 9, 80), (18, 9, 80)], "Deco_Halloween_Pumpkin_Scary")

# ======================================================================= WITCH'S CAULDRON CAMP (right plot)
cx_, cz_ = 59, 71
sph([cx_, 4, cz_], [3.6, 3.0, 3.6], "dark", "shell", "lower")
cyl([cx_, 4, cz_], 3.1, 1, "dark", "wall")
cyl([cx_, 4, cz_], 2.3, 1, "green")
xtra("Build_Lightsource_Green", [(cx_, 4, cz_)])
xtra("Wood_Burnt_Trunk_Full", [(cx_ - 2, 0, cz_ - 2), (cx_ + 2, 0, cz_ - 2), (cx_ - 2, 0, cz_ + 2), (cx_ + 2, 0, cz_ + 2), (cx_ - 2, 1, cz_ - 2), (cx_ + 2, 1, cz_ + 2), (cx_ - 2, 1, cz_ + 2), (cx_ + 2, 1, cz_ - 2), (cx_ - 1, 0, cz_), (cx_ + 1, 0, cz_), (cx_, 0, cz_ - 1), (cx_, 0, cz_ + 1)])
xtra("Deco_Fire", [(cx_, 0, cz_)])
xtra("Deco_Bone_Skulls", [(cx_ + 5, 0, cz_ + 1), (cx_ - 5, 0, cz_ - 2)])
xtra("Deco_Bone_Ribs", [(cx_ + 4, 0, cz_ - 4)])
pts([(cx_ - 4, 0, cz_ + 4), (cx_ - 4, 1, cz_ + 4), (cx_ - 4, 2, cz_ + 4), (cx_ - 4, 3, cz_ + 4)], "Wood_Darkwood_Fence")
setb([cx_ - 4, 4, cz_ + 4], "Furniture_Lumberjack_Lantern")


# ======================================================================= EXTRA SET PIECES
pal["bone"] = ["Rock_Marble", "Rock_Marble", "Rock_Marble", "Rock_Chalk", "Rock_Salt"]


def small_tree(x, z, h, seed):
    rr = random.Random(seed)
    g = ground(x, z)
    cs = {}
    pr = [((x, g, z), 1.25), ((x + rr.uniform(-1, 1), g + h * 0.5, z + rr.uniform(-1, 1)), 0.9), ((x + rr.uniform(-2, 2), g + h, z + rr.uniform(-2, 2)), 0.6)]
    chain(pr, "bark", "tree2")
    top = pr[-1][0]
    ends = []
    for k in range(4):
        a_ = rr.uniform(0, 6.28)
        l_ = rr.uniform(4, 6.5)
        y0 = g + h * rr.uniform(0.55, 0.95)
        p0 = (x + (top[0] - x) * 0.6, y0, z + (top[2] - z) * 0.6)
        p1 = (p0[0] + math.cos(a_) * l_, y0 + rr.uniform(1.5, 3.5), p0[2] + math.sin(a_) * l_)
        p2 = (p1[0] + math.cos(a_ + 0.5) * 2.5, p1[1] + rr.uniform(0.5, 2.0), p1[2] + math.sin(a_ + 0.5) * 2.5)
        chain([(p0, 0.9), (p1, 0.6), (p2, 0.5)], "bark", "tree2")
        ends.append(p1)
    return ends


for (tx_, tz_, th_, sd_) in [(12, 90, 12, 1), (62, 61, 13, 2), (64, 91, 11, 3), (6, 60, 10, 4)]:
    ends = small_tree(tx_, tz_, th_, sd_)
    for e in ends[:2]:
        ex, ey, ez = int(round(e[0])), int(round(e[1])), int(round(e[2]))
        if (ex, ey, ez) not in world:
            continue
        for k in range(1, 4):
            world[(ex, ey - k, ez)] = ("Deco_Rope", "hang")
        world[(ex, ey - 4, ez)] = ("Deco_Halloween_Pumpkin_Scary", "hang")


def hand(x, z, sc, flip=1):
    g = ground(x, z)
    fa = chain([((x, g, z), 1.1 * sc), ((x + 0.6 * sc, g + 4.5 * sc, z), 0.9 * sc)], "bone", "hand")
    wy = g + 5 * sc
    put([(x + dx_, int(round(wy)), z + dz_) for dx_ in range(-2, 3) for dz_ in (-1, 0, 1)], "bone", "hand")
    for i, fx in enumerate((-2, -1, 0, 1)):
        ln = (4.5, 5.5, 5.8, 4.8)[i] * sc
        pr = [((x + fx + 0.5, wy + 1, z), 0.6), ((x + fx + 0.5, wy + ln * 0.6, z - 0.5 * flip), 0.55), ((x + fx + 0.5 + 0.4 * (i - 1.5), wy + ln, z - 2.2 * flip * sc), 0.5)]
        chain(pr, "bone", "hand")
    chain([((x - 2, wy, z), 0.7), ((x - 4.2 * sc, wy + 2.5 * sc, z - 0.5), 0.6), ((x - 5.0 * sc, wy + 4.5 * sc, z - 1.5), 0.5)], "bone", "hand")


hand(21, 56, 1.0)
hand(53, 94, 1.25)
hand(26, 96, 0.9, -1)
hand(67, 75, 1.1)

# glowing purple summoning circles: one before the mouth, one under the cauldron in the hall
pal["purple"] = "NoCube_Neon_Block_Purple"


def circle_sigil(cx, cz, r, y=0):
    cells = []
    for dz_ in range(-int(r) - 1, int(r) + 2):
        for dx_ in range(-int(r) - 1, int(r) + 2):
            d = math.hypot(dx_, dz_)
            if r - 0.55 < d <= r + 0.45:
                cells.append((cx + dx_, y, cz + dz_))
    star = [(cx + (r - 0.3) * math.sin(math.radians(72 * k)), cz - (r - 0.3) * math.cos(math.radians(72 * k))) for k in range(5)]
    for i in range(5):
        a_, b_ = star[i], star[(i + 2) % 5]
        for c in bres((round(a_[0]), y, round(a_[1])), (round(b_[0]), y, round(b_[1]))):
            cells.append(c)
    put(cells, "purple", "sigil")


circle_sigil(SX, Z0 + 19, 5)
circle_sigil(SX, Z0 - 4, 8)
xtra('Deco_Scarecrow', [(27, 0, 97), (45, 0, 97)])
# cobwebs strung inside the rib arches
webs = []
for z, h, hw in zip(rib_z, rib_h, rib_w):
    for s_ in (-1, 1):
        webs.append((round(SX + s_ * (hw * 0.62)), round(h * 0.83), z))
        webs.append((round(SX + s_ * (hw * 0.9)), round(h * 0.45), z))
xtra("Deco_SpiderWeb_Full", webs)
# gate pillars and a string of lights
for gx in (30, 42):
    put([(gx, y, 100) for y in range(0, 7)], "slate", "gate")
    put([(gx, 7, 100)], "Furniture_Temple_Dark_Brazier", "gate")
put([(x, 6, 100) for x in range(31, 42)], "Deco_Halloween_Lights", "gate")

# crows on the dead tree
rr_ = random.Random(5)
cand = [c for c, v in world.items() if v[1] == "tree" and 60 <= c[1] <= 84 and (c[0], c[1] + 1, c[2]) not in world and (c[0] + 1, c[1] + 1, c[2]) not in world]
rr_.shuffle(cand)
for (x, y, z) in cand[:5]:
    world[(x, y + 1, z)] = ("Build_Black_Cube", "tree")
    world[(x + 1, y + 1, z)] = ("Build_Black_Cube", "tree")
    world[(x + 2, y + 1, z)] = ("orange", "tree")
    world[(x, y + 2, z)] = ("Build_Black_Cube", "tree")

# ======================================================================= emit the parts
PARTOF = {"sym": "A", "over": "A", "sigil": "A", "moon": "B", "moonart": "B", "tree": "B"}
by_part = {"A": {}, "B": {}, "C": {}}
for c, (cl, ow) in world.items():
    if ow == "sym":
        continue
    if c[0] < 0 or c[1] < 0 or c[1] > 88:
        continue
    by_part[PARTOF.get(ow, "C")][c] = cl
for P in "ABC":
    lib.PART = P
    v = Vox()
    v.cells = by_part[P]
    n = v.emit()
    print(P, "emit ops", n, "cells", len(v.cells))
for block, attrs, cells, part in extras:
    if block.startswith('Deco_SpiderWeb'):
        cells = [c for c in cells if any((c[0] + d[0], c[1] + d[1], c[2] + d[2]) in world for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)))]
    lib.PART = part
    pts(cells, block, **attrs)
lib.PART = "A"
lib.ops.append({"op": "scatter", "from": [18, 6, 20], "to": [54, 36, 60], "block": "Plant_Vine_Hanging", "where": "under", "chance": 0.05, "seed": 9})
lib.write("script_ALL.json", "The Hollow Moon - complete", "ABC")
for P, nm in (("A", "Hollow Moon 1 - Skull, Ribs, Avenue, Pumpkins"), ("B", "Hollow Moon 2 - Harvest Moon and Dead Tree"), ("C", "Hollow Moon 3 - Cliffs, Graveyard, Crypt, Camp")):
    lib.write("script_%s.json" % P, nm, P)
