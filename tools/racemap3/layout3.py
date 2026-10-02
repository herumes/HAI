"""racemap3 layout: where the lap goes.  x east, z south, y up.  Everything is a fillet polyline (x, z, radius) so a full
circle is just a regular polygon with the fillet radius = the circle radius.

    A  : Nova Station -> Candy Comet Hills -> Neon Metropolis -> Skyport tower (spiral climb)       kart
    SA : Stratos Cloudlands flight                                                                    plane
    B  : Golden Dunes (runway) -> Dive ramp -> Atlantis (sub) -> Surface ramp -> Jungle -> Dread Keep -> Glacier summit
    SB : Cosmic Gateway flight (moon, asteroid belt, gas giant, black hole, wormhole)                  plane
    C  : Celestial Ribbon -> Star Slide -> the start straight                                         kart
"""
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "racemap"))
sys.path.insert(0, HERE)

from path import Path, fillet  # noqa: E402

START_X = -420.0            # the ground route begins here; the finish line is at x = 0
WATER_Y = 11                # top water block of the Atlantis channel
LANE_Y = 5.5                # centre height of the submarine lane


def spiral_rows(cx, cz, R0, R1, a0, a1, n=None):
    """Rows (None, x, z, r) of a circular arc / spiral a0 -> a1 degrees (0 = +x, 90 = +z, so a decreasing angle runs
    counter-clockwise on the map), radius R0 -> R1, as a circumscribed polygon with tangent fillets.  The first / last rows are
    the exact end points; the path leaves the start along the tangent, so the caller's previous vertex must lie on that line."""
    sweep = a1 - a0
    n = n or max(2, int(math.ceil(abs(sweep) / 22.5)))
    step = sweep / n
    half = math.radians(abs(step) / 2.0)
    rows = [(None, cx + R0 * math.cos(math.radians(a0)), cz + R0 * math.sin(math.radians(a0)), 0.0)]
    for k in range(1, n + 1):
        Rm = R0 + (R1 - R0) * (k - 0.5) / n
        a = math.radians(a0 + step * (k - 0.5))
        Rv = Rm / math.cos(half)
        rows.append((None, cx + Rv * math.cos(a), cz + Rv * math.sin(a), Rm))
    rows.append((None, cx + R1 * math.cos(math.radians(a1)), cz + R1 * math.sin(math.radians(a1)), 0.0))
    return rows


def arc_rows(cx, cz, R, a0, a1, n=None):
    return spiral_rows(cx, cz, R, R, a0, a1, n)


def build_path(rows, marks_end="end"):
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
    p.marks[marks_end] = p.length
    return p


def set_altitude(p, keys):
    """Flight altitude per sample (smoothstepped between the (s, y) keys)."""
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


def R(zone, x, z, r=0.0):
    return (zone, x, z, r)


def mark(rows, zone):
    rows[-1] = (zone,) + tuple(rows[-1][1:])


# --------------------------------------------------------------------------------------------- path A (ground, part 1)
CITY_CROSS = (1300.0, 170.0)    # where the second city street crosses over the first
TOWER = (1440.0, 630.0)     # skyport tower centre
TOWER_R0, TOWER_R1, TOWER_SWEEP = 108.0, 62.0, 450.0


def rows_A():
    rows = [
        R("station", START_X, 0), R("candy", 200, 0),
        # ---- Candy Comet Hills
        R(None, 330, 0, 60), R(None, 420, -90, 80), R(None, 570, -110, 90), R(None, 690, -20, 85),
        R(None, 770, 90, 85), R(None, 880, 100, 100), R("cake", 960, 10, 70), R(None, 1030, -100, 70),
        # ---- Neon Metropolis (grid, with an overpass at x=1330, z=170)
        R("city", 1070, 100, 40), R(None, 1300, 100, 30), R(None, 1300, 250, 30), R(None, 1130, 250, 30), R(None, 1130, 170, 30),
        R(None, 1590, 170, 30), R("tower", 1590, TOWER[1] - TOWER_R0, 30),
    ]
    cx, cz = TOWER
    sp = spiral_rows(cx, cz, TOWER_R0, TOWER_R1, -90.0, -90.0 - TOWER_SWEEP)
    rows += sp
    ex, ez = sp[-1][1], sp[-1][2]
    rows.append(R("lip", ex, ez + 90.0))                 # the launch pad: a straight 90 blocks south from the end of the spiral
    return rows


def build_A():
    return build_path(rows_A())


# --------------------------------------------------------------------------------------------- path SA (flight A)
def rows_SA():
    A = build_A()
    lx, lz = A.p[A.i(A.marks["lip"])]
    return [R("launch", lx, lz), R(None, lx, 900, 120), R("storm", 1830, 900, 140), R(None, 1830, 1150, 140), R("whales", 1500, 1150, 130),
            R(None, 1500, 1400, 130), R("slalom", 1870, 1400, 130), R(None, 1870, 1660, 130), R("approach", 1700, 1660, 0),
            R("end", 1480, 1660)]


# --------------------------------------------------------------------------------------------- path B (ground, part 2)
RUNWAY_X0 = 1480.0
RUNWAY_Z = 1660.0


def rows_B():
    rows = [R("runway", RUNWAY_X0, RUNWAY_Z), R("dunes", 1180, RUNWAY_Z, 70),
            R(None, 1130, 1770, 70), R(None, 880, 1770, 70), R(None, 790, 1690, 80), R(None, 690, 1690, 80),
            R(None, 600, 1790, 90), R(None, 480, 1860, 80), R(None, 380, 1790, 80), R("oasis", 300, 1740, 70), R("dive", 140, 1740, 0),
                        R("atlantis", -60, 1740, 80), R(None, -240, 1850, 100), R(None, -430, 1700, 100), R(None, -610, 1850, 100),
            R("surface", -770, 1750, 80), R("jungle", -860, 1760, 70),
            R(None, -1030, 1690, 80), R(None, -830, 1620, 90), R(None, -1030, 1545, 90), R(None, -830, 1470, 90), R(None, -1030, 1395, 90),
            R("keep", -800, 1240, 60),
            R(None, -800, 1100, 70), R(None, -620, 1040, 80), R(None, -760, 950, 90), R(None, -960, 880, 90), R(None, -800, 790, 80),
            R("glacier", -760, 700, 60),
            R(None, -580, 620, 90), R(None, -780, 520, 90), R(None, -580, 420, 90), R(None, -780, 320, 90),
            R("summit", -780, 170, 60), R("pad", -640, 170, 0)]
    return rows


def rows_SB():
    return [R("launch", -640, 170), R(None, -300, 170, 130), R("moon", -150, -60, 140), R("belt", -420, -280, 150),
            R("giant", -760, -330, 150), R("hole", -920, -120, 140), R("wormhole", -960, 150, 120), R(None, -1180, 150, 120),
            R("approach", -1180, -60, 0), R("end", -1180, -240)]


def rows_C():
    return [R("ribbon", -1180, -240), R(None, -1180, -440, 70), R(None, -760, -440, 70), R(None, -760, -300, 70), R(None, -1000, -300, 70),
            R(None, -1000, -170, 70), R(None, -700, -170, 70), R("slide", -700, 0, 70), R(None, -420, 0, 0)]


def plot(paths, out):
    from PIL import Image, ImageDraw
    allp = np.concatenate([p.p for p in paths.values()])
    x0, x1 = allp[:, 0].min() - 100, allp[:, 0].max() + 100
    z0, z1 = allp[:, 1].min() - 100, allp[:, 1].max() + 100
    sc = 0.4
    W, H = int((x1 - x0) * sc), int((z1 - z0) * sc)
    im = Image.new("RGB", (W, H), (14, 16, 26))
    d = ImageDraw.Draw(im)
    cols = {"A": (240, 120, 120), "SA": (120, 200, 255), "B": (240, 200, 90), "SB": (150, 160, 255), "C": (255, 120, 220)}
    for k, p in paths.items():
        pts = [((x - x0) * sc, (z - z0) * sc) for x, z in p.p[::6]]
        d.line(pts, fill=cols[k], width=2 if k in ("SA", "SB") else 3)
        for name, s in p.marks.items():
            q = p.p[p.i(s)]
            d.text(((q[0] - x0) * sc + 4, (q[1] - z0) * sc + 2), name, fill=(255, 255, 255))
    im.save(out)
    return (W, H)


def all_paths():
    A = build_A()
    SA = build_path(rows_SA())
    B = build_path(rows_B())
    SB = build_path(rows_SB())
    C = build_path(rows_C())
    return {"A": A, "SA": SA, "B": B, "SB": SB, "C": C}


if __name__ == "__main__":
    P = all_paths()
    tot = 0
    for k, p in P.items():
        r, at = p.min_radius()
        print("%-2s length %5.0f  min radius %5.1f at s=%.0f   %s" % (k, p.length, r, at, {a: round(b) for a, b in p.marks.items()}))
        tot += p.length
    print("total", round(tot))
    print(plot(P, os.path.join(HERE, "out", "layout.png")))
