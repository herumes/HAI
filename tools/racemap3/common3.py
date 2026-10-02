"""racemap3 common: imports of the shared racemap modules, the terrain helper (Land), road stamping that also registers the
head-room corridor, flight clearance, and a handful of shapes used by several zones."""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "racemap"))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

from core import (MAT, OX, OY, OZ, World, emit, fbm, kenc, load_known_names, mat, smoothstep, stair_rot, vnoise)  # noqa: E402,F401
from font import text_cells  # noqa: E402,F401
from kart import (boost_pad, corners, curvature, facing_driver_rot, finish_band, flag_rot, heading, lean_rot, outward,  # noqa: E402,F401
                  road_arrows, side_cell, start_grid, workwall_rot, zebra, along_rot, line_rot, arrow_rot, OPP)
from kartprops import (billboards, cone_gate, corner_furniture, flood_posts, skid_patch, speed_bumps, _gy)  # noqa: E402,F401
from kartroad import KartStyle, stamp_kart_road  # noqa: E402,F401
from kit import (Frame, bunting, cloud, hoop, item_boxes, lamp_posts, lbox, lcells, lsign, oak, pine, road_grid, spire,  # noqa: E402,F401
                 zone_gate, blob)
from path import Path, raster_band, DIRS  # noqa: E402,F401
from terrain import Grid, carve, centre_field, floating_skin, pick, ridge_field, skin, slope_of  # noqa: E402,F401


# --------------------------------------------------------------------------- sign registry (tools/racemap3/textcheck.py reads it)
import kit as _kit  # noqa: E402

ITEM_BOXES = False          # the floating Mario-Kart style item cubes above the road: decoration only, off by default
_item_boxes_orig = _kit.item_boxes


def item_boxes(*a, **k):
    return _item_boxes_orig(*a, **k) if ITEM_BOXES else None


def register_text(W, label, cells, view, right, kind="text", board=None):
    """Remember a sign for the audit: cells = [(x, y, z, material)], view = unit vector from the sign TOWARD the reader, right = unit
    vector of the reader's right hand, board = optional (u0, u1, y0, y1) of the plate it is meant to sit on (in cells along `right`)."""
    if not hasattr(W, "texts"):
        W.texts = []
    W.texts.append(dict(label=label, cells=cells, view=tuple(float(v) for v in view), right=tuple(float(v) for v in right), kind=kind,
                        zone=getattr(W, "_zone", None), board=board))


def lsign(W, F, text, u_center, y_top, v, scale, m, backing=None, pad=2):
    """kit.lsign with two fixes and a registry entry.  The original rounded u0 + c with Python's round-half-even, so a text with an
    odd pixel width (u0 = x.5) doubled some columns and dropped others - the "gibberish" lettering.  Reader is behind the sign."""
    w, h, cells = text_cells(text, scale)
    u0 = u_center - w / 2.0
    if backing:
        for u in range(int(math.floor(u0)) - pad, int(math.ceil(u0 + w)) + pad):
            for y in range(int(y_top - h) - pad + 1, int(y_top) + pad + 1):
                x, z = F.cell(u, v + 1)
                W.put(x, y, z, backing)
    cl = []
    for (c, r) in cells:
        x, z = F.cell(int(math.floor(u0 + c + 0.5)), v)
        W.put(x, int(y_top - r), z, m)
        cl.append((x, int(y_top - r), z, m))
    register_text(W, text, cl, (-F.t[0], 0.0, -F.t[1]), (F.r[0], 0.0, F.r[1]), "lsign")
    return w, h


_kit.lsign = lsign


def ftext(W, F, text, u_center, y_top, v, scale, mat_fn, label=None):
    """Lettering across the road in a path frame (reader behind it); mat_fn(col, row) -> material key or a plain key."""
    w, h, cells = text_cells(text, scale)
    u0 = u_center - w / 2.0
    cl = []
    for (c, r) in cells:
        x, z = F.cell(int(math.floor(u0 + c + 0.5)), v)
        mm = mat_fn(c, r) if callable(mat_fn) else mat_fn
        W.put(x, int(y_top - r), z, mm)
        cl.append((x, int(y_top - r), z, mm))
    register_text(W, label or text, cl, (-F.t[0], 0.0, -F.t[1]), (F.r[0], 0.0, F.r[1]), "ftext")
    return w, h


def nearest_s(P, x, z, lo=0.0, hi=1e9):
    sel = (P.s >= lo) & (P.s <= hi)
    d = (P.p[:, 0] - x) ** 2 + (P.p[:, 1] - z) ** 2
    d = np.where(sel, d, 1e18)
    return float(P.s[int(np.argmin(d))])


def find_straight(P, lo, hi, half=75.0, rmin=400.0, axis_tol=2.5, step=6.0):
    """Centre s of the straightest axis-aligned stretch (2 * half long) of P between lo and hi, or None."""
    kap = curvature(P)
    best, bs = 1e9, None
    for s in np.arange(lo, hi, step):
        a, b = P.i(s - half), P.i(s + half)
        mx = float(np.abs(kap[a:b:6]).max())
        t = P.t[P.i(s)]
        ang = math.degrees(math.atan2(t[1], t[0])) % 90
        off = min(ang, 90 - ang)
        if mx * rmin < 1.0 and off < axis_tol and mx < best:
            best, bs = mx, float(s)
    return bs


# --------------------------------------------------------------------------- road stamping
def stamp(ctx, P, s0, s1, style, steps=True, chunk=None, corridor=True, hw_clear=None, tmin=None):
    """Stamp the Kart road between path positions s0..s1 and register it for the head-room clearing at the end.
    chunk: stamp in pieces of that many blocks (needed when the road overlaps itself in plan, e.g. a spiral)."""
    i0, i1 = P.i(s0), P.i(s1)
    if chunk:
        n = int(math.ceil((s1 - s0) / chunk))
        for k in range(n):
            a = P.i(s0 + k * chunk - (2 if k else 0))
            b = P.i(min(s1, s0 + (k + 1) * chunk + 2))
            stamp_kart_road(ctx.W, P, a, b, style, steps=steps)
    else:
        stamp_kart_road(ctx.W, P, i0, i1, style, steps=steps)
    if corridor:
        ctx.corridors.append((P, i0, i1, hw_clear or (style.hw + 0.5), tmin))
    return i0, i1


def axis_s(P, s, window=120, tol=None, margin=6.0):
    """The position nearest to s (within `window`) where the road runs along a compass axis, else None.  The tightest tolerance
    that finds a spot within the window wins (1, 2, 3.5, then 5 degrees): lettering across a diagonal road is a staircase with gaps."""
    for t in ((tol,) if tol else (1.0, 2.0, 3.5, 5.0)):
        for ds in sorted(range(-int(window), int(window) + 1, 2), key=abs):
            s2 = s + ds
            if margin <= s2 <= P.length - margin:
                a = abs(Frame(P, s2).ang) % 90
                if min(a, 90 - a) < t:
                    return float(s2)
    return None


def gate(ctx, P, s, title, **kw):
    """Sign arch over the road.  The arch is moved (up to 120 blocks) to the nearest stretch where the road runs along an axis
    (a diagonal arch would be a staircase), the board is widened (or the lettering shrunk) so the text always fits on it with a
    margin, and the towers get legs down to the ground when the road is raised."""
    s2 = axis_s(P, s, 260)
    if s2 is None:
        print("  WARNING: no axis-aligned stretch near s=%.0f for the '%s' arch" % (s, title))
        s2 = s
    scale = kw.get("scale", 1)
    # fit: the board half-width must reach past the text by 4 blocks; widen it up to 58, else use a smaller scale
    while True:
        w, h, _ = text_cells(title, scale)
        need = int(math.ceil(w / 2.0)) + 4
        if need <= 58 or scale == 1:
            break
        scale -= 1
    kw = dict(kw, scale=scale)
    tu = kw.get("tower_u") or 0
    bh = kw.get("board_half") or tu
    if need > bh:
        bh = need
        kw["board_half"] = bh
    if tu and tu < bh + 3:
        kw["tower_u"] = bh + 3
    F = zone_gate(ctx.W, P, s2, title, **kw)
    half = kw.get("tower_u")
    if half and F.T > 1:
        m_ = kw.get("frame_mat", "gold_brick")
        for side in (-1, 1):
            u0 = side * half
            lbox(ctx.W, F, u0 - 2, u0 + 2, -2, 2, -1, F.T - 1, m_, keep=True)
    return F


# --------------------------------------------------------------------------- terrain
class Land:
    """A grid around a stretch of road with the distance / road-height / path-position fields every terrain pass needs."""

    def __init__(self, ctx, path, i0, i1, margin=70, radius=56, hw=9.0, box=None):
        self.ctx, self.path = ctx, path
        xs, zs = path.p[i0:i1, 0], path.p[i0:i1, 1]
        if box is None:
            box = (xs.min() - margin, xs.max() + margin, zs.min() - margin, zs.max() + margin)
        self.g = Grid(*box)
        self.d, self.Tn, self.Sn = centre_field(self.g, path, i0, i1, radius, hw=hw)
        self.rb = road_grid(ctx, self.g)
        self.i0, self.i1, self.radius = i0, i1, radius
        self.Ht = None
        self.mask = None
        self.slope = None

    @property
    def X(self):
        return self.g.X

    @property
    def Z(self):
        return self.g.Z

    def finish(self, H, d0=4.0, d1=18.0, mask_d=None, noise=None, keep_roads=True, floor=-1):
        """Blend the landform H toward the road shoulder near the road and quantise it.  Sets Ht, mask, slope."""
        shoulder = np.where(self.Tn > 0, self.Tn - 1, floor)
        Hc = carve(H, self.d, shoulder, d0, d1, noise=noise)
        Ht = np.maximum(np.rint(Hc).astype(np.int64), floor)
        md = mask_d if mask_d is not None else self.radius - 4
        mask = (self.d < md) & (Ht >= floor)
        if keep_roads:
            mask = mask & ~(self.rb & (self.d > 12))
        self.Hc, self.Ht, self.mask = Hc, Ht, mask
        self.slope = slope_of(Hc)
        return Ht

    def skin(self, top_ids, side_ids, mats, mask=None):
        return skin(self.ctx.W, self.g, self.Ht, top_ids, side_ids, mats, mask=self.mask if mask is None else mask)

    def spots(self, rng, n, dmin, dmax, slope_max=1.0, hmin=-1, hmax=999, extra=None, min_gap=0, ground_only=True):
        """Random surface cells (x, y, z) of the skin: distance to the road in dmin..dmax, gentle slope, optional extra mask."""
        ok = (self.d >= dmin) & (self.d <= dmax) & (self.Ht >= hmin) & (self.Ht <= hmax) & (self.slope < slope_max) & self.mask & ~self.rb
        if extra is not None:
            ok = ok & extra
        ix, iz = np.nonzero(ok)
        if ix.size == 0:
            return []
        sel = rng.choice(ix.size, size=min(n, ix.size), replace=False)
        out = [(self.g.x0 + int(ix[k]), int(self.Ht[ix[k], iz[k]]), self.g.z0 + int(iz[k])) for k in sel]
        if min_gap:
            keep = []
            for (x, y, z) in out:
                if all(abs(x - a) >= min_gap or abs(z - c) >= min_gap for (a, b, c) in keep):
                    keep.append((x, y, z))
            out = keep
        return out


def blocked_by_road(ctx, x, z, margin=0):
    """True when (x, z) is within the kart-road footprint (plus margin) of any road of the lap."""
    rs = getattr(ctx, "_road_set", None)
    if rs is None:
        rs = ctx._road_set = set(ctx.road_keys.tolist())
    return ((int(x) + OX) | ((int(z) + OZ) << 14)) in rs


# --------------------------------------------------------------------------- flights
class Clear:
    """Distance from a point to a flight line (3D), so scenery never blocks a plane."""

    def __init__(self, S, step=3):
        self.pts = np.stack([S.p[::step, 0], S.alt[::step], S.p[::step, 1]], 1)

    def __call__(self, x, y, z):
        return float(np.sqrt(((self.pts - np.array([x, y, z])) ** 2).sum(1)).min())


def P3(S, s):
    i = S.i(s)
    return np.array([S.p[i, 0], S.alt[i], S.p[i, 1]])


def fdir(S, s):
    i = S.i(s)
    j = min(len(S.s) - 1, i + 6)
    k = max(0, i - 6)
    d = np.array([S.p[j, 0] - S.p[k, 0], S.alt[j] - S.alt[k], S.p[j, 1] - S.p[k, 1]])
    return d / np.linalg.norm(d)


def frame3(S, s):
    """Orthonormal frame on a flight line: (centre, forward, right, up)."""
    c, f = P3(S, s), fdir(S, s)
    up0 = np.array([0.0, 1.0, 0.0])
    r = np.cross(f, up0)
    n = np.linalg.norm(r)
    r = r / n if n > 1e-6 else np.array([1.0, 0, 0])
    u = np.cross(r, f)
    return c, f, r, u


def hoop3(W, S, s, R, m, thick=2):
    c, f, r, u = frame3(S, s)
    return hoop(W, c, f, R, m, thick=thick)


# --------------------------------------------------------------------------- shapes
def torus(W, c, d, R, r, m, step=0.4):
    """A solid ring (tube radius r) of centre-line radius R around centre c, lying in the plane normal to d."""
    d = np.array(d, float)
    d /= np.linalg.norm(d)
    up = np.array([0, 1.0, 0]) if abs(d[1]) < 0.95 else np.array([1.0, 0, 0])
    a = np.cross(d, up)
    a /= np.linalg.norm(a)
    b = np.cross(d, a)
    n = max(24, int(2 * math.pi * R / step))
    pts = []
    for th in np.linspace(0, 2 * math.pi, n, endpoint=False):
        pc = np.array(c, float) + R * (math.cos(th) * a + math.sin(th) * b)
        pts.append(pc)
    rr = int(math.ceil(r)) + 1
    ox, oy, oz = np.meshgrid(*(np.arange(-rr, rr + 1),) * 3, indexing="ij")
    sel = ox * ox + oy * oy + oz * oz <= r * r + 0.25
    ox, oy, oz = ox[sel], oy[sel], oz[sel]
    P = np.floor(np.array(pts) + 0.5).astype(np.int64)
    X = (P[:, 0:1] + ox[None, :]).ravel()
    Y = (P[:, 1:2] + oy[None, :]).ravel()
    Z = (P[:, 2:3] + oz[None, :]).ravel()
    W.put_arr(X, Y, Z, m)


def column(W, x, z, y0, y1, r, m, cap=None, base=None):
    """A round column (solid for r < 2, hollow otherwise)."""
    W.cyl(x, z, y0, y1, r, m, mode="fill" if r < 2.1 else "wall")
    if cap:
        W.cyl(x, z, y1 + 1, y1 + 1, r + 1, cap)
    if base:
        W.cyl(x, z, y0, y0, r + 1, base)


def dome(W, cx, cy, cz, r, m, mode="shell"):
    W.ellipsoid(cx, cy, cz, r, r, r, m, mode=mode, part="upper")


_FACE = {"s": (1, "z", (0, 0, -1)), "n": (-1, "z", (0, 0, 1)), "w": (1, "x", (1, 0, 0)), "e": (-1, "x", (-1, 0, 0))}


def _put_plane(W, cells, x, y, z, m, face, backing_cells=None, back_m=None, label="?"):
    sign, plane, back = _FACE[face]
    reg = []
    for (u, yy) in cells:
        if plane == "z":
            W.put(x + sign * u, yy, z, m)
            reg.append((x + sign * u, yy, z, m))
        else:
            W.put(x, yy, z + sign * u, m)
            reg.append((x, yy, z + sign * u, m))
    if backing_cells:
        for (u, yy) in backing_cells:
            if plane == "z":
                W.put(x + sign * u + back[0], yy, z + back[2], back_m)
            else:
                W.put(x + back[0], yy, z + sign * u + back[2], back_m)
    right = (sign, 0, 0) if plane == "z" else (0, 0, sign)
    register_text(W, label, reg, (-back[0], 0, -back[2]), right, "plane")


def put_text(W, text, x, y, z, scale, m, face="s", backing=None, pad=2):
    """Pixel text on a vertical plane, readable by a viewer standing on the `face` side of the board ('n','s','e','w' = the compass
    side of the text the viewer is on).  (x, y, z) = centre of the top edge of the text; the backing plate goes on the far side.
    Returns (width, height)."""
    w, h, cells = text_cells(text, scale)
    u0 = -w // 2
    cl = [(u0 + c, y - r) for (c, r) in cells]
    bk = None
    if backing:
        bk = [(u, yy) for u in range(u0 - pad, u0 + w + pad) for yy in range(y - h - pad + 1, y + pad + 1)]
    _put_plane(W, cl, x, y, z, m, face, bk, backing, label=text)
    return w, h


def put_bitmap(W, rows, x, y, z, scale, m, face="s", backing=None, pad=1):
    """A pixel picture (list of strings, '#' = on) on a vertical plane, same conventions as put_text.  (x, y, z) = centre of the top edge."""
    wd, ht = len(rows[0]), len(rows)
    u0 = -(wd * scale) // 2
    cl = []
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == "#":
                for dx in range(scale):
                    for dy in range(scale):
                        cl.append((u0 + c * scale + dx, y - r * scale - dy))
    bk = None
    if backing:
        bk = [(u, yy) for u in range(u0 - pad, u0 + wd * scale + pad) for yy in range(y - ht * scale - pad + 1, y + pad + 1)]
    _put_plane(W, cl, x, y, z, m, face, bk, backing, label="icon")
    return wd * scale, ht * scale


ICONS = {
    "heart": [".##...##.", "####.####", "#########", "#########", ".#######.", "..#####..", "...###...", "....#...."],
    "star": ["....#....", "....#....", "...###...", "#########", ".#######.", "..#####..", "..##.##..", ".##...##."],
    "rocket": ["...##...", "..####..", "..####..", "..#..#..", "..####..", "..####..", ".######.", "##.##.##", "#..##..#", "...##..."],
    "planet": ["..#####..", ".#######.", "#########", "#########", "#########", ".#######.", "..#####.."],
    "arrow": ["....#....", "...###...", "..#####..", ".#######.", "...###...", "...###...", "...###..."],
    "bolt": ["....###", "...###.", "..###..", ".######", "...###.", "..###..", ".##...."],
}


def box_wall(W, a, b, m, mode="walls"):
    W.box(a, b, m, mode=mode)


def cells_free(W, x, y, z, h=1):
    return all(not W.has(x, y + k, z) for k in range(h))


def around(rng, n, cx, cz, rmin, rmax):
    """n random (x, z) points in an annulus."""
    out = []
    for _ in range(n):
        a = rng.uniform(0, 2 * math.pi)
        r = rng.uniform(rmin, rmax)
        out.append((int(cx + r * math.cos(a)), int(cz + r * math.sin(a))))
    return out
