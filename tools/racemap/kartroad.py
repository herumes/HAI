"""racemap kartroad: stamps a road out of the server's Kart_* blocks.

Cross-section from the centre line outward (per side):
    |u| < hw            Kart_Asphalt (or a themed lane), the outermost `edge_w` cells may be a neon strip
    hw .. hw+1          verge: Kart_Concrete at road level, carrying the guard rail one layer above
    hw+1 .. hw+1+ww     optional wall (hedge, rock ...) `wall_h` high
On top of the asphalt (layer road+1, all flat decals): dashed lane lines, edge lines, rumble curbs on the inside of bends.
"""
import math

import numpy as np

from core import kenc, stair_rot
from kart import (CORNER_ROT, DIRV, OPP, along_rot, cell_of, curvature, line_rot)
from path import DIRS, raster_band


class KartStyle:
    def __init__(self, hw=7.0, surface=None, edge=None, edge_w=1.0, stairs="k_asph_stair", verge="k_conc", verge_stairs="k_conc_stair",
                 rail="armco", rail_fn=None, wall=None, wall_h=1, wall_w=1.0, wall_period=6.0,
                 deck=None, piers=None, lanes=(0.0,), lane_dash=(3.0, 3.0), lane_mat="k_line", edge_line=True, edge_line_mat="k_line",
                 curbs=("k_curb_red",), curb_len=6.0, curb_rmax=130.0, chevron_rmax=0.0, verge_w=1.0, underside=None):
        self.hw = hw
        self.surface = surface            # fn(s, u) -> mat, or None = asphalt
        self.edge, self.edge_w = edge, edge_w
        self.stairs, self.verge, self.verge_stairs = stairs, verge, verge_stairs
        self.rail, self.rail_fn = rail, rail_fn
        self.wall, self.wall_h, self.wall_w, self.wall_period = wall, wall_h, wall_w, wall_period
        self.deck, self.piers = deck, piers
        self.lanes, self.lane_dash, self.lane_mat = lanes, lane_dash, lane_mat
        self.edge_line, self.edge_line_mat = edge_line, edge_line_mat
        self.curbs, self.curb_len, self.curb_rmax = curbs, curb_len, curb_rmax
        self.chevron_rmax = chevron_rmax
        self.verge_w = verge_w
        self.underside = underside        # material placed under the road too (thick decks), None = nothing

    def wall_mat(self, s, u, h):
        if not self.wall:
            return None
        w = self.wall
        if isinstance(w, str):
            return w
        if callable(w):
            return w(s, u, h)
        return w[int(s // self.wall_period) % len(w)]


def _chain(path, i0, i1, off, side):
    """Ordered, 4-connected cells of the offset line (side * off) with the sample index each belongs to."""
    cells = []
    last = None
    for i in range(i0, i1):
        q = path.p[i] + path.r[i] * side * off
        c = cell_of(q)
        if c != last:
            cells.append((c[0], c[1], i, q))
            last = c
    out = []
    for (x, z, i, q) in cells:
        if out:
            px, pz, pi, pq = out[-1]
            d = abs(x - px) + abs(z - pz)
            if d == 2 and abs(x - px) == 1:
                mid = (pq + q) / 2.0
                a, b = (x, pz), (px, z)
                da = (a[0] - mid[0]) ** 2 + (a[1] - mid[1]) ** 2
                db = (b[0] - mid[0]) ** 2 + (b[1] - mid[1]) ** 2
                c = a if da <= db else b
                out.append((c[0], c[1], pi, mid))
            elif d > 2:
                n = int(max(abs(x - px), abs(z - pz)))
                for k in range(1, n):
                    f = k / float(n)
                    cx, cz = int(round(px + (x - px) * f)), int(round(pz + (z - pz) * f))
                    if (cx, cz) != (out[-1][0], out[-1][1]):
                        # keep 4-connectivity
                        lx, lz = out[-1][0], out[-1][1]
                        if abs(cx - lx) + abs(cz - lz) == 2:
                            out.append((cx, lz, pi, pq))
                        out.append((cx, cz, pi, pq))
                if abs(x - out[-1][0]) + abs(z - out[-1][1]) == 2:
                    out.append((x, out[-1][1], i, q))
        if out and (x, z) == (out[-1][0], out[-1][1]):
            continue
        out.append((x, z, i, q))
    # drop A-B-A back-tracks
    res = []
    for c in out:
        if len(res) >= 2 and (res[-2][0], res[-2][1]) == (c[0], c[1]):
            res.pop()
            continue
        res.append(c)
    return res


def _nbr_dirs(a, b):
    """Compass letter from cell a toward cell b (they are 4-neighbours)."""
    dx, dz = b[0] - a[0], b[1] - a[1]
    if dx == 1:
        return "e"
    if dx == -1:
        return "w"
    if dz == 1:
        return "s"
    return "n"


def _put_rail(W, kind, x, y, z, dirs, tvec):
    if kind in ("armco", "mesh"):
        base = "k_armco" if kind == "armco" else "k_mesh"
        if len(dirs) == 2 and OPP[next(iter(dirs))] not in dirs:
            W.put(x, y, z, base + "_c", CORNER_ROT[frozenset(dirs)])
            return
        if dirs and dirs <= {"e", "w"}:
            rot = 0
        elif dirs and dirs <= {"n", "s"}:
            rot = 1
        else:
            rot = along_rot(tvec[0], tvec[1])
        W.put(x, y, z, base, rot)
    elif kind in ("jersey", "catch"):
        if dirs and dirs <= {"e", "w"}:
            rot = 0
        elif dirs and dirs <= {"n", "s"}:
            rot = 1
        else:
            rot = along_rot(tvec[0], tvec[1])
        W.put(x, y, z, "k_jersey" if kind == "jersey" else "k_catch", rot)
    elif kind == "tyre":
        W.put(x, y, z, "k_tyre")
    elif kind == "chevron":
        W.put(x, y, z, "k_chevron")
    elif kind == "hay":
        W.put(x, y, z, "k_hay")
    elif kind == "block":
        W.put(x, y, z, "k_curb_red_b")
    elif kind == "yblock":
        W.put(x, y, z, "k_curb_yel_b")


def stamp_kart_road(W, path, i0, i1, st, steps=True):
    hw, vw, ww = st.hw, st.verge_w, (st.wall_w if st.wall else 0.0)
    umax = hw + vw + ww
    cx, cz, si, uu = raster_band(path, i0, i1, -umax, umax)
    T = path.T[si]
    cellT = {}
    for x, z, t in zip(cx.tolist(), cz.tolist(), T.tolist()):
        cellT[(x, z)] = t
    surf_fn = st.surface
    for x, z, sidx, u, t in zip(cx.tolist(), cz.tolist(), si.tolist(), uu.tolist(), T.tolist()):
        s = float(path.s[sidx])
        a = abs(u)
        if a < hw:
            m = surf_fn(s, u) if surf_fn else "k_asphalt"
            if st.edge and a >= hw - st.edge_w:
                m = st.edge(s, u) or m
            W.put(x, t, z, m)
            W.surf[kenc(x, t, z)] = W.d[kenc(x, t, z)]
            if st.deck and t >= 2:
                W.put(x, t - 1, z, st.deck)
            if steps:
                for dx, dz, f in DIRS:
                    nt = cellT.get((x + dx, z + dz))
                    if nt is not None and nt == t + 1:
                        W.put(x, t + 1, z, st.stairs, rot=stair_rot(f))
                        break
        elif a < hw + vw:
            if st.verge:
                W.put(x, t, z, st.verge)
                if st.deck and t >= 2:
                    W.put(x, t - 1, z, st.deck)
                if steps:
                    for dx, dz, f in DIRS:
                        nt = cellT.get((x + dx, z + dz))
                        if nt is not None and nt == t + 1:
                            W.put(x, t + 1, z, st.verge_stairs, rot=stair_rot(f))
                            break
        elif st.wall:
            for h in range(st.wall_h):
                wm = st.wall_mat(s, u, h)
                if wm:
                    W.put(x, t + h, z, wm)
            if st.deck and t >= 2:
                W.put(x, t - 1, z, st.deck)

    if st.piers:
        pm = st.piers
        every = int(pm.get("every", 16))
        size = int(pm.get("size", 2))
        inset = float(pm.get("inset", 2.5))
        for sidx in range(i0 + every, i1 - every // 2, every * 2):
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
            qa = path.p[sidx] + path.r[sidx] * (-(hw - inset))
            qb = path.p[sidx] + path.r[sidx] * (hw - inset)
            W.line((qa[0], t - 2, qa[1]), (qb[0], t - 2, qb[1]), pm.get("beam", pm["mat"]))

    # ---- guard rails on the verge (one continuous 4-connected chain per side, corners turned to match)
    kap = curvature(path)
    if st.rail or st.rail_fn:
        for side in (-1, 1):
            chain = _chain(path, i0, i1, hw + vw * 0.5, side)
            for k, (x, z, i, q) in enumerate(chain):
                t = int(cellT.get((x, z), path.T[i]))
                kind = st.rail_fn(float(path.s[i]), side) if st.rail_fn else st.rail
                if st.chevron_rmax > 0 and abs(kap[i]) * st.chevron_rmax >= 1.0 and side * kap[i] < 0:
                    kind = "chevron"
                if not kind:
                    continue
                W.put(x, t, z, st.verge or "k_conc", keep=True) if st.verge else None
                dirs = set()
                if k > 0:
                    dirs.add(_nbr_dirs((x, z), chain[k - 1]))
                if k + 1 < len(chain):
                    dirs.add(_nbr_dirs((x, z), chain[k + 1]))
                _put_rail(W, kind, x, t + 1, z, dirs, path.t[i])

    # ---- flat decals on the asphalt (layer road+1); only on Kart_Asphalt cells, never on top of something already there
    def decal(x, z, m, rot=None):
        t = cellT.get((x, z))
        if t is None:
            return
        g = W.get(x, t, z)
        if g is None or g[0] != "k_asphalt":
            return
        if not W.has(x, t + 1, z):
            W.put(x, t + 1, z, m, rot)

    on, off = st.lane_dash if st.lane_dash else (0, 0)
    for u0 in st.lanes:
        c2x, c2z, s2, u2 = raster_band(path, i0, i1, u0 - 0.6, u0 + 0.6)
        for x, z, sidx, u in zip(c2x.tolist(), c2z.tolist(), s2.tolist(), u2.tolist()):
            if abs(u - u0) > 0.5:
                continue
            s = float(path.s[sidx])
            if on and (s % (on + off)) >= on:
                continue
            t = path.t[sidx]
            decal(x, z, st.lane_mat, line_rot(t[0], t[1]))
    if st.edge_line:
        c2x, c2z, s2, u2 = raster_band(path, i0, i1, -(hw - 1.0), hw - 1.0)
        for x, z, sidx, u in zip(c2x.tolist(), c2z.tolist(), s2.tolist(), u2.tolist()):
            if abs(u) < hw - 2.0:
                continue
            t = path.t[sidx]
            decal(x, z, st.edge_line_mat, line_rot(t[0], t[1]))
    if st.curbs:
        c2x, c2z, s2, u2 = raster_band(path, i0, i1, -hw, hw)
        for x, z, sidx, u in zip(c2x.tolist(), c2z.tolist(), s2.tolist(), u2.tolist()):
            if abs(u) < hw - 1.0 or st.edge:
                continue
            kp = kap[sidx]
            if abs(kp) * st.curb_rmax < 1.0 or u * kp <= 0:
                continue
            s = float(path.s[sidx])
            m = st.curbs[int(s // st.curb_len) % len(st.curbs)]
            t = path.t[sidx]
            decal(x, z, m, along_rot(t[0], t[1]))
