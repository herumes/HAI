"""racemap path: centre lines (lines, arcs, fillets, splines), height profiles and road stamping."""
import math

import numpy as np

from core import MAT, kenc, stair_rot

STEP = 0.5                  # path sample spacing in blocks


# --------------------------------------------------------------------------- geometry builders
def arc(c, r, a0, a1, step=0.25):
    """Points on a circle.  Angle 0 = +x, 90 = +z (south).  Sweep a0 -> a1 (degrees, sign = direction)."""
    n = max(2, int(abs(math.radians(a1 - a0)) * r / step))
    out = []
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        out.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
    return out


def fillet(verts, radii):
    """Polyline through verts (x, z) with a circular fillet at each inner vertex."""
    if not isinstance(radii, (list, tuple)):
        radii = [radii] * len(verts)
    out = [tuple(map(float, verts[0]))]
    for i in range(1, len(verts) - 1):
        A = np.array(verts[i - 1], float)
        V = np.array(verts[i], float)
        B = np.array(verts[i + 1], float)
        r = float(radii[i])
        if r <= 0.01:
            out.append(tuple(V))
            continue
        u1 = (A - V) / np.linalg.norm(A - V)
        u2 = (B - V) / np.linalg.norm(B - V)
        cosphi = float(np.clip(np.dot(u1, u2), -1, 1))
        phi = math.acos(cosphi)
        if phi > math.pi - 1e-3 or phi < 1e-3:
            out.append(tuple(V))
            continue
        d = r / math.tan(phi / 2)
        dmax = 0.5 * min(np.linalg.norm(A - V), np.linalg.norm(B - V))
        if i == 1:
            dmax = min(np.linalg.norm(A - V) * 0.98, np.linalg.norm(B - V) * 0.5)
        if i == len(verts) - 2:
            dmax = min(np.linalg.norm(A - V) * 0.5, np.linalg.norm(B - V) * 0.98)
        if d > dmax:
            d = dmax
            r = d * math.tan(phi / 2)
        P1 = V + u1 * d
        P2 = V + u2 * d
        bis = (u1 + u2)
        bis /= np.linalg.norm(bis)
        C = V + bis * (r / math.sin(phi / 2))
        a1 = math.atan2(P1[1] - C[1], P1[0] - C[0])
        a2 = math.atan2(P2[1] - C[1], P2[0] - C[0])
        da = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi
        n = max(3, int(abs(da) * r / 0.25))
        out.append(tuple(P1))
        for k in range(1, n):
            a = a1 + da * k / n
            out.append((C[0] + r * math.cos(a), C[1] + r * math.sin(a)))
        out.append(tuple(P2))
    out.append(tuple(map(float, verts[-1])))
    return out


def spline(points, samples_per_seg=40):
    """Catmull-Rom (centripetal-ish) curve through the points; first/last segments are extended."""
    P = [np.array(p, float) for p in points]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(samples_per_seg):
            t = k / samples_per_seg
            t2, t3 = t * t, t * t * t
            q = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3)
            out.append(tuple(q))
    out.append(tuple(P[-2]))
    return out


def concat(*pieces):
    out = []
    for p in pieces:
        for q in p:
            if not out or abs(out[-1][0] - q[0]) + abs(out[-1][1] - q[1]) > 1e-6:
                out.append(tuple(q))
    return out


# --------------------------------------------------------------------------- the path
class Path:
    def __init__(self, pts):
        P = np.array(pts, float)
        seg = np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1]))
        keep = np.concatenate(([True], seg > 1e-9))
        P = P[keep]
        seg = np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1]))
        cum = np.concatenate(([0.0], np.cumsum(seg)))
        L = cum[-1]
        n = int(L / STEP) + 1
        s = np.linspace(0, L, n)
        self.s = s
        self.p = np.stack([np.interp(s, cum, P[:, 0]), np.interp(s, cum, P[:, 1])], 1)
        t = np.zeros_like(self.p)
        k = 4
        t[k:-k] = self.p[2 * k:] - self.p[:-2 * k]
        t[:k] = self.p[k] - self.p[0]
        t[-k:] = self.p[-1] - self.p[-k - 1]
        t /= np.linalg.norm(t, axis=1)[:, None]
        self.t = t
        self.r = np.stack([-t[:, 1], t[:, 0]], 1)        # viewer's right-hand side (x east, z south)
        self.T = np.zeros(n, dtype=np.int64)
        self.marks = {}

    def __len__(self):
        return len(self.s)

    @property
    def length(self):
        return float(self.s[-1])

    def i(self, s):
        return int(min(len(self.s) - 1, max(0, round(s / STEP))))

    def mark(self, name, s=None):
        self.marks[name] = self.length if s is None else s

    def set_heights(self, keys, smooth=True):
        """keys: [(s, T), ...] ascending.  Linear (or smoothstepped) between keys, rounded to whole blocks."""
        ks = np.array([k[0] for k in keys], float)
        ts = np.array([k[1] for k in keys], float)
        if smooth:
            out = np.empty_like(self.s)
            for j in range(len(ks) - 1):
                sel = (self.s >= ks[j]) & (self.s <= ks[j + 1])
                u = (self.s[sel] - ks[j]) / max(1e-9, ks[j + 1] - ks[j])
                u = u * u * (3 - 2 * u)
                out[sel] = ts[j] + (ts[j + 1] - ts[j]) * u
            out[self.s < ks[0]] = ts[0]
            out[self.s > ks[-1]] = ts[-1]
        else:
            out = np.interp(self.s, ks, ts)
        self.T = np.rint(out).astype(np.int64)

    def min_radius(self, window=8):
        """Smallest turning radius along the path (circumradius of samples `window` apart)."""
        k = window
        a, b, c = self.p[:-2 * k], self.p[k:-k], self.p[2 * k:]
        ab = np.linalg.norm(b - a, axis=1)
        bc = np.linalg.norm(c - b, axis=1)
        ca = np.linalg.norm(a - c, axis=1)
        cross = np.abs((b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0]))
        with np.errstate(divide="ignore", invalid="ignore"):
            rad = ab * bc * ca / (2 * cross)
        rad[~np.isfinite(rad)] = 1e9
        j = int(np.argmin(rad))
        return float(rad[j]), float(self.s[j + k])

    def frame(self, s):
        i = self.i(s)
        return self.p[i], self.t[i], self.r[i]

    def world(self, s, u=0.0, v=0.0):
        """Point at path position s, lateral u (right of travel), v metres further along the tangent."""
        i = self.i(s)
        q = self.p[i] + self.r[i] * u + self.t[i] * v
        return q[0], q[1]

    def Tat(self, s):
        return int(self.T[self.i(s)])


# --------------------------------------------------------------------------- rasterising bands
def raster_band(path, i0, i1, umin, umax, du=0.5, pad=0):
    """Cells covered by the band umin..umax (right of travel) between samples i0..i1, each cell taken
    by the sample it is closest to (smallest |u|).  Returns arrays cx, cz, sidx, u."""
    idx = np.arange(i0, i1)
    P = path.p[idx]
    R = path.r[idx]
    us = np.arange(umin, umax + 1e-6, du)
    cx = np.floor(P[:, 0:1] + R[:, 0:1] * us[None, :] + 0.5).astype(np.int64)
    cz = np.floor(P[:, 1:2] + R[:, 1:2] * us[None, :] + 0.5).astype(np.int64)
    si = np.broadcast_to(idx[:, None], cx.shape)
    uu = np.broadcast_to(us[None, :], cx.shape)
    cx, cz, si, uu = cx.ravel(), cz.ravel(), si.ravel(), uu.ravel()
    order = np.lexsort((np.abs(uu), cz, cx))
    cx, cz, si, uu = cx[order], cz[order], si[order], uu[order]
    first = np.ones(len(cx), bool)
    first[1:] = (cx[1:] != cx[:-1]) | (cz[1:] != cz[:-1])
    return cx[first], cz[first], si[first], uu[first]


# --------------------------------------------------------------------------- road stamping
class RoadStyle:
    """What a stretch of road is made of.  surface/curb/wall are functions of (s, u) -> material key or None."""

    def __init__(self, surface, stairs, curb=None, wall=None, hw=7.0, curb_w=1.0, wall_w=1.0, wall_h=2,
                 deck=None, piers=None, top_fn=None, stair_curb=True):
        self.surface, self.stairs, self.curb, self.wall = surface, stairs, curb, wall
        self.hw, self.curb_w, self.wall_w, self.wall_h = hw, curb_w, wall_w, wall_h
        self.deck = deck              # material under the road surface (one block) or None
        self.piers = piers            # dict(mat=, every=, size=, cap=) or None
        self.top_fn = top_fn          # optional fn(s, u) -> mat placed above the wall (posts, lamps)
        self.stair_curb = stair_curb


DIRS = [(1, 0, "e"), (-1, 0, "w"), (0, 1, "s"), (0, -1, "n")]


def stamp_road(W, path, i0, i1, st, steps=True):
    umax = st.hw + st.curb_w + st.wall_w
    cx, cz, si, uu = raster_band(path, i0, i1, -umax, umax)
    T = path.T[si]
    cellT = {}
    for x, z, t in zip(cx.tolist(), cz.tolist(), T.tolist()):
        cellT[(x, z)] = t
    hw, cw, ww = st.hw, st.curb_w, st.wall_w
    for x, z, sidx, u, t in zip(cx.tolist(), cz.tolist(), si.tolist(), uu.tolist(), T.tolist()):
        s = float(path.s[sidx])
        a = abs(u)
        if a < hw:
            m = st.surface(s, u)
            road = True
        elif a < hw + cw:
            m = st.curb(s, u) if st.curb else None
            road = True
        else:
            m = None
            road = False
        if road:
            if m:
                W.put(x, t, z, m)
            if st.deck and t >= 2:
                W.put(x, t - 1, z, st.deck)
            if steps:
                # a step up or down: a stair on the low side, tall side toward the high cell
                for dx, dz, f in DIRS:
                    nt = cellT.get((x + dx, z + dz))
                    if nt is not None and nt == t + 1:
                        if a < hw or st.stair_curb:
                            W.put(x, t + 1, z, st.stairs, rot=stair_rot(f))
                        break
        elif st.wall:
            for h in range(st.wall_h):
                wm = st.wall(s, u, h)
                if wm:
                    W.put(x, t + h, z, wm)
            if st.deck and t >= 2:
                W.put(x, t - 1, z, st.deck)
            if st.top_fn:
                tm = st.top_fn(s, u)
                if tm:
                    W.put(x, t + st.wall_h, z, tm)
    if st.piers:
        pm = st.piers
        every = int(pm.get("every", 16))
        size = int(pm.get("size", 2))
        inset = float(pm.get("inset", 2.5))
        for sidx in range(i0 + every, i1 - every // 2, every * 2):   # samples are 0.5 apart
            t = int(path.T[sidx])
            ybot = pm.get("bottom", 0)
            if t - 2 < ybot:
                continue
            for side in (-1, 1):
                q = path.p[sidx] + path.r[sidx] * side * (hw - inset)
                px, pz = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
                W.box((px, ybot, pz), (px + size - 1, t - 2, pz + size - 1), pm["mat"])
                if pm.get("cap"):
                    W.box((px - 1, t - 2, pz - 1), (px + size, t - 2, pz + size), pm["cap"])
            # cross beam between the two columns
            qa = path.p[sidx] + path.r[sidx] * (-(hw - inset))
            qb = path.p[sidx] + path.r[sidx] * (hw - inset)
            W.line((qa[0], t - 2, qa[1]), (qb[0], t - 2, qb[1]), pm.get("beam", pm["mat"]))


def paint(W, path, i0, i1, umin, umax, fn, lift=0):
    """Override road-surface cells: fn(s, u) -> (material, rot) | material | None."""
    cx, cz, si, uu = raster_band(path, i0, i1, umin, umax)
    for x, z, sidx, u in zip(cx.tolist(), cz.tolist(), si.tolist(), uu.tolist()):
        r = fn(float(path.s[sidx]), u)
        if r is None:
            continue
        rot = None
        if isinstance(r, tuple):
            r, rot = r
        W.put(x, int(path.T[sidx]) + lift, z, r, rot)
