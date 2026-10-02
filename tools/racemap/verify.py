"""Road checks on the finished world: holes in the driving surface, things blocking head-room, step spacing."""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build
from core import kenc

HW = {"A": 7.0, "B": 7.0}
ROAD_OK = {"k_asphalt", "k_asph_stair", "k_conc", "k_conc_stair", "k_curb_yel_b", "k_curb_red_b", "k_finish_line", "k_checker"}


SURF = {"k_asphalt", "k_conc", "k_curb_yel_b", "k_curb_red_b", "k_finish_line", "k_checker"}


def check(ctx, name, P, i0, i1, hw, head=2, verbose=True):
    """Every cell of the driving surface must hold a Kart road block (asphalt, concrete, neon strip, curb block) and have
    `head` free cells above it (decals / stairs on the layer above are fine)."""
    W = ctx.W
    holes, blocked, wrong = [], [], []
    for i in range(i0, i1):
        T = int(P.T[i])
        for u in np.arange(-hw + 0.5, hw - 0.49, 0.5):
            q = P.p[i] + P.r[i] * u
            x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
            tc = None
            seen = None
            for yy in (T + 1, T, T - 1):
                sid = W.d.get(kenc(x, yy, z))
                if sid is None:
                    continue
                nm = W.specs[sid][0]
                if seen is None:
                    seen = nm
                if nm in SURF or nm.startswith("n_"):
                    tc = yy
                    break
            if tc is None:
                if seen is None:
                    holes.append((i, round(float(u), 1), x, T, z))
                else:
                    wrong.append((i, round(float(u), 1), x, T, z, seen))
                continue
            for h in range(2, 2 + head):
                sid = W.d.get(kenc(x, tc + h, z))
                if sid is not None:
                    blocked.append((i, round(float(u), 1), x, tc + h, z, W.specs[sid][0]))
                    break
    if verbose:
        import collections
        print("%s: samples %d..%d  holes %d  blocked-headroom %d  not-road-surface %d" % (name, i0, i1, len(holes), len(blocked), len(wrong)))
        if wrong:
            print("   not-road-surface by material:", collections.Counter(w[5] for w in wrong).most_common(8))
            for grp in cluster(wrong, gap=30)[:10]:
                e = grp[0]
                print("   wrong-floor cluster near s=%.0f x=%d z=%d y=%d %s n=%d" % (P.s[e[0]], e[2], e[4], e[3], e[5], len(grp)))
    check.wrong = wrong
    return holes, blocked


def steps(P, name):
    T = P.T
    ch = np.nonzero(np.diff(T) != 0)[0]
    if ch.size < 2:
        return
    gaps = np.diff(ch) * 0.5
    bad = [(round(float(P.s[ch[k]])), round(float(gaps[k]), 1), int(T[ch[k]]), int(T[ch[k] + 1])) for k in range(len(gaps)) if gaps[k] < 3.0]
    jumps = [int(abs(T[c + 1] - T[c])) for c in ch]
    print("%s: %d steps, max step %d, closest pair %.1f blocks, steps closer than 3 blocks: %d" % (name, ch.size, max(jumps), gaps.min(), len(bad)))
    if bad:
        print("   first:", bad[:6])


def cluster(lst, key=lambda e: e[0], gap=12):
    out = []
    for e in lst:
        if out and key(e) - key(out[-1][-1]) <= gap:
            out[-1].append(e)
        else:
            out.append([e])
    return out


if __name__ == "__main__":
    ctx = build.main([])
    A, B = ctx.A, ctx.B
    mA = A.marks
    for nm, P, rng_ in (("A", A, (0, len(A))), ("B", B, (0, len(B)))):
        steps(P, nm)
    # A: skip the sub zone floor (no walls) - still check the surface
    hA, bA = check(ctx, "A", A, 0, len(A) - 1, 7.0)
    hB, bB = check(ctx, "B", B, 0, len(B) - 1, 7.0)
    for nm, P, h, b in (("A", A, hA, bA), ("B", B, hB, bB)):
        zones = sorted((v, k) for k, v in P.marks.items() if k != "end")
        def zone_of(i):
            s = P.s[i]
            z = None
            for v, k in zones:
                if v <= s:
                    z = k
            return z
        import collections
        print(nm, "holes by zone:", collections.Counter(zone_of(e[0]) for e in h))
        print(nm, "blocked by zone:", collections.Counter(zone_of(e[0]) for e in b))
        for grp in cluster(h)[:8]:
            e = grp[0]
            print("   hole cluster near s=%.0f (%s) x=%d z=%d T=%d n=%d" % (P.s[e[0]], zone_of(e[0]), e[2], e[4], e[3], len(grp)))
        mats = collections.Counter(e[5] for e in b)
        print(nm, "blocking materials:", mats.most_common(8))
        for grp in cluster(b, gap=20)[:12]:
            e = grp[0]
            print("   blocked cluster near s=%.0f (%s) x=%d y=%d z=%d %s n=%d" % (P.s[e[0]], zone_of(e[0]), e[2], e[3], e[4], e[5], len(grp)))


def flight_check(ctx, radius=5.0):
    """Anything solid within `radius` of the plane's flight line (and the sub lane)?"""
    W, S, A = ctx.W, ctx.S, ctx.A
    import itertools
    offs = [(dx, dy, dz) for dx in range(-6, 7) for dy in range(-6, 7) for dz in range(-6, 7) if dx * dx + dy * dy + dz * dz <= radius * radius]
    bad = {}
    for i in range(0, len(S.s), 2):
        cx, cy, cz = S.p[i, 0], S.alt[i], S.p[i, 1]
        fx, fy, fz = int(math.floor(cx + .5)), int(math.floor(cy + .5)), int(math.floor(cz + .5))
        for dx, dy, dz in offs:
            k = kenc(fx + dx, fy + dy, fz + dz)
            if k in W.d:
                nm = W.specs[W.d[k]][0]
                bad.setdefault(nm, []).append((round(float(S.s[i])), fx + dx, fy + dy, fz + dz))
                break
    print("flight line: blocks within %.0f: %s" % (radius, {k: len(v) for k, v in bad.items()}))
    for k, v in bad.items():
        print("   ", k, v[:4])
    # sub lane
    m = A.marks
    ch0, ch1 = m["abyss"] + 150, m["abyss"] + 405
    bad2 = {}
    for i in range(A.i(ch0), A.i(ch1), 2):
        cx, cz = A.p[i]
        y0 = 4.6 + 1.6 * math.sin(A.s[i] * 0.05) - 0.6
        for dy in range(-3, 4):
            for ox in range(-3, 4):
                for oz in range(-3, 4):
                    if ox * ox + oz * oz + dy * dy > 16:
                        continue
                    k = kenc(int(math.floor(cx + .5)) + ox, int(round(y0)) + dy, int(math.floor(cz + .5)) + oz)
                    if k in W.d:
                        nm = W.specs[W.d[k]][0]
                        bad2.setdefault(nm, []).append(round(float(A.s[i])))
    print("sub lane: blocks within 4:", {k: len(v) for k, v in bad2.items()})
    return bad, bad2


if __name__ == "__main__" and "--flight" in sys.argv:
    pass
