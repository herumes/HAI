"""Quick look at the world: top-down map, elevation-shaded, and side projections, as PNGs."""
import colorsys
import hashlib

import numpy as np
from PIL import Image, ImageDraw

from core import MAT, OX, OY, OZ

NAMED = {"Red": (220, 50, 50), "Orange": (240, 140, 40), "Yellow": (245, 220, 60), "YellowLight": (255, 240, 150), "Green": (60, 190, 70),
         "GreenDark": (30, 110, 50), "Lime": (150, 230, 60), "Cyan": (50, 210, 220), "Blue": (60, 90, 230), "Purple": (150, 70, 210),
         "Pink": (240, 110, 190), "White": (240, 240, 240), "Black": (25, 25, 30), "Grey": (130, 130, 135), "Gray": (130, 130, 135),
         "GreyDark": (70, 70, 78), "GreyLight": (185, 185, 190), "Red2": (200, 60, 70), "Yellow2": (230, 200, 50), "Grey2": (100, 100, 105)}


KART = {"Kart_Asphalt": (52, 54, 60), "Kart_Asphalt_Slab": (52, 54, 60), "Kart_Asphalt_Stairs": (60, 62, 68), "Kart_Concrete": (165, 168, 172),
        "Kart_Concrete_Stairs": (165, 168, 172), "Kart_Grass": (70, 200, 70), "Andesite_Smooth": (150, 152, 156),
        "Kart_Curb_Red": (230, 60, 60), "Kart_Curb_Yellow": (240, 210, 40), "Kart_Curb_Red_Block": (230, 60, 60),
        "Kart_Curb_Yellow_Block": (240, 210, 40), "Kart_Chevron_Block": (240, 200, 30), "Kart_Painted_Line": (240, 240, 240),
        "Kart_Painted_Yellow": (240, 210, 40), "Kart_Finish_Checker": (250, 250, 250), "Finish_Line": (235, 235, 235),
        "Kart_Pit_Box": (235, 200, 40), "Kart_Armco": (190, 195, 205), "Kart_Armco_Corner": (190, 195, 205),
        "Kart_Barrier_Metal": (170, 175, 190), "Kart_Barrier_Metal_Corner": (170, 175, 190), "Kart_Jersey": (200, 200, 200),
        "Kart_Tire_Wall": (20, 20, 22), "Kart_Hay_Bale": (220, 190, 90), "Kart_Catch_Fence": (150, 160, 170),
        "Red_Workwall": (200, 50, 50), "Red_Block": (200, 50, 50), "Flag": (240, 240, 240), "Kart_Billboard": (250, 120, 40),
        "Rotatesign": (250, 140, 30), "Rotatesign_Left": (250, 140, 30), "Kart_Arrow_Straight": (240, 240, 240)}
NEON_C = {"Black": (40, 40, 40), "Blue": (0, 70, 255), "Crimson": (255, 0, 80), "Cyan": (0, 255, 190), "Dark_Gray": (100, 100, 100),
          "Fire_Red": (255, 70, 0), "Green": (0, 255, 0), "Light_Blue": (0, 170, 255), "Light_Gray": (190, 190, 190), "Lime": (190, 240, 30),
          "Magenta": (255, 0, 220), "Orange": (255, 150, 0), "Pink": (240, 150, 190), "Purple": (115, 0, 255), "Red": (255, 0, 0),
          "White": (255, 255, 255), "Yellow": (255, 190, 0)}


def color_of(name):
    n = name
    if n in KART:
        return KART[n]
    if n.startswith("NoCube_Neon_Block_"):
        return NEON_C.get(n[len("NoCube_Neon_Block_"):], (255, 0, 255))
    if n.startswith("Kart_"):
        return (200, 200, 60)
    for k in sorted(NAMED, key=len, reverse=True):
        if ("_" + k + "_") in n + "_" or n.endswith("_" + k):
            if "Lightsource" in n or "Clay" in n or "Wool" in n or "Build_" in n:
                return NAMED[k]
    if "Lava" in n or "Incandescent" in n:
        return (255, 110, 30)
    if "Leaves" in n or "Grass" in n or "Plant" in n or "Fern" in n or "Moss" in n or "Bush" in n:
        return (60, 150, 60)
    if "Sand" in n:
        return (220, 190, 120)
    if "Snow" in n:
        return (235, 240, 250)
    if "Ice" in n:
        return (170, 220, 245)
    if "Crystal" in n:
        return (200, 90, 230)
    if "Coral" in n or "Seaweed" in n:
        return (240, 100, 120)
    if "Volcanic" in n or "Basalt" in n or "Magma" in n:
        return (70, 55, 55)
    if "Gold" in n:
        return (235, 190, 50)
    if "Wood" in n or "Dirt" in n or "Clay_Brick" in n:
        return (130, 90, 55)
    if "Metal" in n:
        return (150, 160, 170)
    if "Marble" in n or "Chalk" in n or "Calcite" in n:
        return (225, 225, 230)
    if "Rock" in n or "Stone" in n or "Soil" in n:
        return (125, 125, 130)
    h = int(hashlib.md5(n.encode()).hexdigest()[:6], 16)
    r, g, b = colorsys.hsv_to_rgb((h % 360) / 360.0, 0.5, 0.8)
    return int(r * 255), int(g * 255), int(b * 255)


def _arrays(W):
    keys = np.fromiter(W.d.keys(), dtype=np.int64, count=len(W.d))
    vals = np.fromiter(W.d.values(), dtype=np.int32, count=len(W.d))
    X = (keys & 0x3FFF) - OX
    Z = ((keys >> 14) & 0x3FFF) - OZ
    Y = (keys >> 28) - OY
    return X, Y, Z, vals


def palette_colors(W):
    return np.array([color_of(MAT[m][0]) for (m, r) in W.specs], dtype=np.float32)


def topdown(W, out, bbox=None, scale=1, shade=True, ymax=None):
    X, Y, Z, V = _arrays(W)
    if bbox is None:
        bbox = (X.min() - 5, X.max() + 5, Z.min() - 5, Z.max() + 5)
    x0, x1, z0, z1 = [int(b) for b in bbox]
    sel = (X >= x0) & (X <= x1) & (Z >= z0) & (Z <= z1)
    if ymax is not None:
        sel &= Y <= ymax
    X, Y, Z, V = X[sel], Y[sel], Z[sel], V[sel]
    order = np.argsort(Y, kind="stable")
    X, Y, Z, V = X[order], Y[order], Z[order], V[order]
    w, h = x1 - x0 + 1, z1 - z0 + 1
    topv = np.full((w, h), -1, dtype=np.int32)
    topy = np.full((w, h), -9999, dtype=np.int32)
    topv[X - x0, Z - z0] = V
    topy[X - x0, Z - z0] = Y
    pal = palette_colors(W)
    img = np.zeros((w, h, 3), dtype=np.float32)
    img[:] = (18, 20, 28)
    has = topv >= 0
    img[has] = pal[topv[has]]
    if shade:
        yy = np.where(has, topy, 0).astype(np.float32)
        lo, hi = float(yy[has].min()) if has.any() else 0, float(yy[has].max()) if has.any() else 1
        f = 0.55 + 0.45 * (yy - lo) / max(1.0, hi - lo)
        # cheap relief: darken where the height drops toward +z / +x
        gx = np.zeros_like(yy)
        gz = np.zeros_like(yy)
        gx[1:] = yy[1:] - yy[:-1]
        gz[:, 1:] = yy[:, 1:] - yy[:, :-1]
        f = f + 0.05 * np.clip(-(gx + gz), -6, 6)
        img = img * np.clip(f, 0.25, 1.3)[:, :, None]
    im = Image.fromarray(np.clip(img.transpose(1, 0, 2), 0, 255).astype(np.uint8))
    if scale != 1:
        im = im.resize((int(w * scale), int(h * scale)), Image.NEAREST)
    im.save(out)
    return im.size


def side(W, out, axis="z", bbox=None, scale=1, other=None):
    """Projection onto x-y (looking north->south, axis='z') or z-y (looking west->east, axis='x').  Nearest cell wins."""
    X, Y, Z, V = _arrays(W)
    a, b = (X, Z) if axis == "z" else (Z, X)
    depth = b if axis == "z" else -b
    if other is not None:
        lo, hi = other
        sel = (b >= lo) & (b <= hi)
        a, Y, V, depth = a[sel], Y[sel], V[sel], depth[sel]
    if bbox is None:
        bbox = (a.min() - 2, a.max() + 2, max(Y.min() - 2, -5), Y.max() + 3)
    a0, a1, y0, y1 = [int(v) for v in bbox]
    sel = (a >= a0) & (a <= a1) & (Y >= y0) & (Y <= y1)
    a, Yc, Vc, dc = a[sel], Y[sel], V[sel], depth[sel]
    order = np.argsort(-dc, kind="stable")      # farthest first so nearest overwrites
    a, Yc, Vc = a[order], Yc[order], Vc[order]
    w, h = a1 - a0 + 1, y1 - y0 + 1
    grid = np.full((w, h), -1, dtype=np.int32)
    grid[a - a0, Yc - y0] = Vc
    pal = palette_colors(W)
    img = np.zeros((w, h, 3), dtype=np.float32)
    img[:] = (18, 20, 28)
    has = grid >= 0
    img[has] = pal[grid[has]]
    im = Image.fromarray(img.transpose(1, 0, 2).astype(np.uint8)[::-1])
    if scale != 1:
        im = im.resize((int(w * scale), int(h * scale)), Image.NEAREST)
    im.save(out)
    return im.size


def iso(W, out, bbox, az=35.0, el=35.0, scale=2.0, size=None, ymax=None, ymin=None, bg=(14, 16, 26)):
    """Oblique voxel view of the box bbox=(x0,x1,z0,z1[,y0,y1]).  az: camera azimuth (0 = looking north->south... degrees).
    Nearest voxel wins; top faces are lit."""
    X, Y, Z, V = _arrays(W)
    x0, x1, z0, z1 = [int(b) for b in bbox[:4]]
    sel = (X >= x0) & (X <= x1) & (Z >= z0) & (Z <= z1)
    if len(bbox) >= 6:
        sel &= (Y >= bbox[4]) & (Y <= bbox[5])
    X, Y, Z, V = X[sel], Y[sel], Z[sel], V[sel]
    if X.size == 0:
        return None
    # exposed top faces
    from core import kenc
    keys = set(W.d.keys())
    above = np.array([((int(x) + OX) | ((int(z) + OZ) << 14) | ((int(y) + 1 + OY) << 28)) in keys for x, y, z in zip(X.tolist(), Y.tolist(), Z.tolist())]) if X.size < 1_500_000 else np.zeros(X.size, bool)
    A, E = np.radians(az), np.radians(el)
    cx, cz = (x0 + x1) / 2.0, (z0 + z1) / 2.0
    xr, zr = X - cx, Z - cz
    sx = (xr * np.cos(A) - zr * np.sin(A)) * scale
    depthh = xr * np.sin(A) + zr * np.cos(A)
    sy = -(Y * np.cos(E) - depthh * np.sin(E)) * scale
    depth = depthh * np.cos(E) + Y * np.sin(E)          # larger = closer to the camera for this convention
    order = np.argsort(depth, kind="stable")             # far first, near overwrites
    sx, sy, V, above = sx[order], sy[order], V[order], above[order]
    px0, py0 = sx.min() - 4, sy.min() - 4
    w, h = int(sx.max() - px0 + 6), int(sy.max() - py0 + 6)
    if w * h > 60_000_000:
        raise ValueError("image too big %dx%d" % (w, h))
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:] = bg
    pal = palette_colors(W)
    col = pal[V]
    col = col * np.where(above, 0.62, 1.0)[:, None]
    col = np.clip(col, 0, 255).astype(np.uint8)
    sz = size or max(1, int(round(scale * 1.15)))
    ix = (sx - px0).astype(np.int64)
    iy = (sy - py0).astype(np.int64)
    for dx in range(sz):
        for dy in range(sz):
            img[np.clip(iy + dy, 0, h - 1), np.clip(ix + dx, 0, w - 1)] = col
    Image.fromarray(img).save(out)
    return (w, h)


def section(W, path, s, out, umax=40, y0=-3, y1=40, scale=8, vwidth=1):
    """Cross-section of the world across the path at position s (u = to the right of travel)."""
    i = path.i(s)
    p, r, t = path.p[i], path.r[i], path.t[i]
    pal = palette_colors(W)
    w, h = int(2 * umax + 1), y1 - y0 + 1
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:] = (14, 16, 26)
    from core import kenc
    for ui, u in enumerate(range(-int(umax), int(umax) + 1)):
        for dv in range(vwidth):
            q = p + r * u + t * dv
            x, z = int(np.floor(q[0] + 0.5)), int(np.floor(q[1] + 0.5))
            for y in range(y0, y1 + 1):
                sid = W.d.get(kenc(x, y, z))
                if sid is not None and (img[h - 1 - (y - y0), ui] == (14, 16, 26)).all():
                    img[h - 1 - (y - y0), ui] = pal[sid].astype(np.uint8)
    im = Image.fromarray(img).resize((w * scale, h * scale), Image.NEAREST)
    im.save(out)
    return im.size
