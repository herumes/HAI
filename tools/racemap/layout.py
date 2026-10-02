"""racemap layout: where the lap goes.  x east, z south, y up.

Path A  : start straight -> Meadows -> Canyon -> Crystal -> Abyss (sub) -> Frostpeak summit
Sky     : the plane flight from the Frostpeak launch to the Volcano runway (3D spline)
Path B  : Volcano -> Rainbow Road -> back onto the start straight
"""
import math

import numpy as np

from path import Path, arc, concat, fillet, spline

START_X = -200.0            # the ground route begins here (x), finish line is at x = 0

# Heights of the zone joints (road top block y)
T_CANYON = 0
T_ABYSS_PLATEAU = 14        # crystal exit / water entrance plateau
T_ABYSS_FLOOR = 0           # floor of the basin (water surface is WATER_Y)
WATER_Y = 11                # top water block; the basin walls go 2-3 above this
T_FROST_SUMMIT = 75
T_RUNWAY = 6
T_RIM = 50


def spiral(c, r0, r1, a0, a1, n=400):
    pts = []
    for i in range(n + 1):
        t = i / n
        a = math.radians(a0 + (a1 - a0) * t)
        r = r0 + (r1 - r0) * t
        pts.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
    return pts


# ------------------------------------------------------------------ path A
def _zoned_path(rows):
    """rows: (zone-or-None, x, z, r).  One filleted polyline; zone marks at the vertex they sit on."""
    verts = [(r[1], r[2]) for r in rows]
    radii = [r[3] for r in rows]
    pts = fillet(verts, radii)
    p = Path(pts)
    P = p.p
    for zone, x, z, r in rows:
        if zone:
            d = (P[:, 0] - x) ** 2 + (P[:, 1] - z) ** 2
            p.marks[zone] = float(p.s[int(np.argmin(d))])
    p.marks["end"] = p.length
    return p


A_ROWS = [
    ("start", START_X, 0, 0),
    ("meadows", 100, 0, 0),
    (None, 240, 0, 70), (None, 320, -60, 70), (None, 420, -60, 70), (None, 485, 0, 70), (None, 560, 0, 60),
    (None, 650, 0, 100),
    ("canyon", 650, 100, 60),
    (None, 600, 185, 75), (None, 670, 280, 85), (None, 600, 370, 85),
    ("crystal", 650, 455, 60),
    (None, 635, 545, 55), (None, 555, 570, 55), (None, 530, 650, 55), (None, 605, 685, 55), (None, 540, 720, 45),
    ("abyss", 465, 690, 60),
    (None, 400, 690, 70), (None, 330, 635, 80), (None, 240, 635, 80), (None, 175, 705, 80), (None, 100, 705, 75),
    (None, 40, 690, 60),
    ("frost", -20, 690, 0),
    (None, -260, 690, 40), (None, -260, 605, 40), (None, -22, 605, 0),
]


def build_path_a():
    p = _zoned_path(A_ROWS)
    return p, (-200.0, 600.0)


# ------------------------------------------------------------------ path B
B_ROWS = None


def build_path_b():
    VC = (-470.0, 350.0)
    pieces = [
        fillet([(-90, 300), (-345, 300), (-345, 350)], [0, 40, 0]),
        spiral(VC, 125, 68, 0, 270, 300),
    ]
    marks = {"runway": 0.0}
    pieces_len = [_plen(pc) for pc in pieces]
    marks["volcano"] = pieces_len[0]
    rb = fillet([(VC[0], VC[1] - 68), (-290, 282), (-220, 200), (-330, 120), (-430, 60), (-430, 0), (-200, 0)],
                [0, 60, 60, 70, 60, 50, 0])
    marks["rainbow"] = sum(pieces_len)
    pts = concat(*pieces, rb)
    p = Path(pts)
    p.marks.update(marks)
    p.marks["end"] = p.length
    return p, VC


def _plen(pts):
    P = np.array(pts, float)
    return float(np.hypot(np.diff(P[:, 0]), np.diff(P[:, 1])).sum())


# ------------------------------------------------------------------ the flight
def build_sky():
    """The plane flight as a 2D Path plus an altitude per sample (Path.alt).  Sections are marked by name."""
    HC = (480.0, 440.0)
    verts = [(-5, 606), (200, 615), (310, 570), (420, 600), (540, 590), (540, 440)]
    radii = [0, 80, 90, 90, 100, 0]
    p1 = fillet(verts, radii)
    helix = arc(HC, 60, 0, -360)
    p2 = fillet([(540, 440), (540, 380), (525, 300), (400, 255), (180, 300), (-90, 300)], [0, 0, 70, 90, 120, 0])
    pts = concat(p1, helix, p2)
    p = Path(pts)
    # marks by nearest sample
    def nearest(x, z):
        return float(p.s[int(np.argmin((p.p[:, 0] - x) ** 2 + (p.p[:, 1] - z) ** 2))])
    p.marks = {"launch": 0.0, "islands": nearest(200, 615), "helix": nearest(540, 445) + 1, "slalom": nearest(540, 380) + 120,
               "tunnel": nearest(400, 255) - 10, "descent": nearest(180, 300), "end": p.length}
    # helix really starts where the arc starts (first sample within 1 block of (610,440) after the approach)
    hs = None
    for i in range(len(p.s) - 1):
        if abs(p.p[i, 0] - 540) < 0.6 and abs(p.p[i, 1] - 440) < 0.6 and p.s[i] > 500:
            hs = float(p.s[i]); break
    p.marks["helix"] = hs if hs else p.marks["helix"]
    p.marks["slalom"] = p.marks["helix"] + 2 * math.pi * 60 + 10
    return p, HC


def set_altitude(p, keys):
    ks = np.array([k[0] for k in keys], float)
    ys = np.array([k[1] for k in keys], float)
    out = np.empty_like(p.s)
    for j in range(len(ks) - 1):
        sel = (p.s >= ks[j]) & (p.s <= ks[j + 1])
        u = (p.s[sel] - ks[j]) / max(1e-9, ks[j + 1] - ks[j])
        u = u * u * (3 - 2 * u)
        out[sel] = ys[j] + (ys[j + 1] - ys[j]) * u
    out[p.s < ks[0]] = ys[0]
    out[p.s > ks[-1]] = ys[-1]
    p.alt = out


def spline3(ctrl, step=1.0):
    P = [np.array(p, float) for p in ctrl]
    P = [2 * P[0] - P[1]] + P + [2 * P[-1] - P[-2]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        seglen = np.linalg.norm(p2 - p1)
        n = max(4, int(seglen / step * 1.5))
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    out.append(P[-2])
    arr = np.array(out)
    seg = np.linalg.norm(np.diff(arr, axis=0), axis=1)
    cum = np.concatenate(([0], np.cumsum(seg)))
    n = int(cum[-1] / step) + 1
    s = np.linspace(0, cum[-1], n)
    return np.stack([np.interp(s, cum, arr[:, i]) for i in range(3)], 1), s


if __name__ == "__main__":
    A, FC = build_path_a()
    B, VC = build_path_b()
    S, HC = build_sky()
    print("A length %.0f  radius min %.1f at s=%.0f" % ((A.length,) + A.min_radius()))
    print({k: round(v) for k, v in A.marks.items()})
    print("B length %.0f  radius min %.1f at s=%.0f" % ((B.length,) + B.min_radius()))
    print({k: round(v) for k, v in B.marks.items()})
    print("sky length %.0f  radius min %.1f at s=%.0f" % ((S.length,) + S.min_radius()))
    print({k: round(v) for k, v in S.marks.items()})
    def seglens(P):
        ks = sorted((v, k) for k, v in P.marks.items())
        return {ks[i][1]: round(ks[i + 1][0] - ks[i][0]) for i in range(len(ks) - 1)}
    print("A zones", seglens(A)); print("B zones", seglens(B)); print("sky zones", seglens(S))
    kart = A.length - seglens(A)["abyss"] + B.length
    sub = seglens(A)["abyss"]
    print("kart %.0f sub %.0f plane %.0f -> at 40/35/70 b/s: %.0f s" % (kart, sub, S.length, kart / 40 + sub / 35 + S.length / 70))
