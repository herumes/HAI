"""Verification of the finished world (all zones): kart roads (holes, wrong floor, head-room, step spacing), joins between the
path segments, flight lines (plane A / B), the submarine lane, the Atlantis basin (flood fill), height cap, per-zone counts.

    python check3.py            build everything (~2.5 min) and check
    python check3.py --cached   reuse out/world.pkl from the last full check
"""
import collections
import math
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import build3  # noqa: E402
import layout3  # noqa: E402
from core import MAT, OY, kenc  # noqa: E402

SURF = {"k_asphalt", "k_conc", "k_curb_yel_b", "k_curb_red_b", "k_finish_line", "k_checker"}
PKL = os.path.join(HERE, "out", "world.pkl")


def load_ctx(cached):
    if cached and os.path.exists(PKL):
        import importlib
        for z in build3.ZONES:
            importlib.import_module("z_" + z)
        ctx = build3.make_ctx()
        with open(PKL, "rb") as f:
            ctx.W.d, ctx.W.specs, ctx.W.spec_ids, ctx.W.surf, ctx.W.zcount, ctx.marks = pickle.load(f)
        tp = os.path.join(HERE, "out", "texts.pkl")
        if os.path.exists(tp):
            with open(tp, "rb") as f:
                ctx.W.texts = pickle.load(f)
        print("loaded cached world:", len(ctx.W.d), "blocks")
        return ctx
    ctx = build3.build_zones(build3.ZONES)
    below = [k for k in ctx.W.d if (k >> 28) - OY < -1]
    for k in below:
        del ctx.W.d[k]
    with open(os.path.join(HERE, "out", "texts.pkl"), "wb") as f:
        pickle.dump(getattr(ctx.W, "texts", []), f, protocol=4)
    with open(PKL, "wb") as f:
        pickle.dump((ctx.W.d, ctx.W.specs, ctx.W.spec_ids, ctx.W.surf, ctx.W.zcount, ctx.marks), f, protocol=4)
    return ctx


def zone_of(P, i):
    zones = sorted((v, k) for k, v in P.marks.items() if k != "end")
    s = P.s[i]
    z = None
    for v, k in zones:
        if v <= s:
            z = k
    return z


def cluster(lst, key=lambda e: e[0], gap=16):
    out = []
    for e in lst:
        if out and key(e) - key(out[-1][-1]) <= gap:
            out[-1].append(e)
        else:
            out.append([e])
    return out


def road_check(ctx, name, P, hw=6.4, head=2, sub_skip=None):
    """Every cell of the driving surface must hold a road block (asphalt / concrete / neon / curb block) with `head` free cells above."""
    W = ctx.W
    holes, blocked, wrong = [], [], []
    for i in range(0, len(P.s) - 1):
        if sub_skip and sub_skip(P.s[i]):
            continue
        T = int(P.T[i])
        for u in np.arange(-hw, hw + 0.01, 0.5):
            q = P.p[i] + P.r[i] * u
            x, z = int(math.floor(q[0] + 0.5)), int(math.floor(q[1] + 0.5))
            tc, seen = None, None
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
                (holes if seen is None else wrong).append((i, round(float(u), 1), x, T, z) if seen is None else (i, round(float(u), 1), x, T, z, seen))
                continue
            for h in range(2, 2 + head):
                sid = W.d.get(kenc(x, tc + h, z))
                if sid is not None:
                    blocked.append((i, round(float(u), 1), x, tc + h, z, W.specs[sid][0]))
                    break
    print("%s: holes %d  not-road-surface %d  blocked-headroom %d" % (name, len(holes), len(wrong), len(blocked)))
    for label, lst in (("hole", holes), ("wrong-floor", wrong), ("blocked", blocked)):
        c = collections.Counter(zone_of(P, e[0]) for e in lst)
        if c:
            print("   %s by zone:" % label, dict(c))
        for grp in cluster(lst)[:6]:
            e = grp[0]
            print("      %s near s=%.0f (%s) x=%d y=%d z=%d %s n=%d" % (label, P.s[e[0]], zone_of(P, e[0]), e[2], e[3], e[4], e[5] if len(e) > 5 else "", len(grp)))
    return holes, wrong, blocked


def step_check(P, name):
    T = P.T
    ch = np.nonzero(np.diff(T) != 0)[0]
    if ch.size < 2:
        return
    gaps = np.diff(ch) * 0.5
    jumps = [int(abs(T[c + 1] - T[c])) for c in ch]
    bad = [(round(float(P.s[ch[k]])), round(float(gaps[k]), 1)) for k in range(len(gaps)) if gaps[k] < 3.0]
    print("%s: %d steps, max step %d, closest pair %.1f blocks, steps closer than 3 blocks: %d" % (name, ch.size, max(jumps), gaps.min(), len(bad)))
    if bad:
        print("   first:", bad[:6])
    # steepest stretch (rise over 24 blocks)
    w = 48
    rise = np.abs(T[w:] - T[:-w])
    j = int(np.argmax(rise))
    print("   steepest 24-block stretch: rise %d at s=%.0f (1 in %.1f)" % (rise[j], P.s[j], 24.0 / max(1, rise[j])))


def flight_check(ctx, S, name, radius=6.0):
    W = ctx.W
    offs = [(dx, dy, dz) for dx in range(-7, 8) for dy in range(-7, 8) for dz in range(-7, 8) if dx * dx + dy * dy + dz * dz <= radius * radius]
    bad = {}
    for i in range(0, len(S.s), 2):
        fx, fy, fz = int(math.floor(S.p[i, 0] + .5)), int(math.floor(S.alt[i] + .5)), int(math.floor(S.p[i, 1] + .5))
        for dx, dy, dz in offs:
            k = kenc(fx + dx, fy + dy, fz + dz)
            if k in W.d:
                nm = W.specs[W.d[k]][0]
                bad.setdefault(nm, []).append((round(float(S.s[i])), fx + dx, fy + dy, fz + dz))
                break
    print("%s flight line (radius %.0f): %s" % (name, radius, {k: len(v) for k, v in bad.items()} or "clear"))
    for k, v in bad.items():
        print("    ", k, v[:5])
    return bad


def sub_check(ctx, radius=4.0):
    W, B = ctx.W, ctx.B
    ch0, ch1 = ctx.marks["ch0"], ctx.marks["ch1"]
    bad = {}
    for i in range(B.i(ch0 + 125), B.i(ch1 - 20), 2):
        cx, cz = B.p[i]
        s = B.s[i]
        y0 = layout3.LANE_Y + 1.4 * math.sin(s * 0.05)
        for dy in range(-5, 6):
            for ox in range(-5, 6):
                for oz in range(-5, 6):
                    if ox * ox + oz * oz + dy * dy > radius * radius:
                        continue
                    k = kenc(int(math.floor(cx + .5)) + ox, int(round(y0)) + dy, int(math.floor(cz + .5)) + oz)
                    if k in W.d:
                        nm = W.specs[W.d[k]][0]
                        if nm.startswith("k_") or nm.startswith("ab_") or int(round(y0)) + dy <= 1:
                            continue
                        bad.setdefault(nm, []).append(round(float(s)))
    print("sub lane (radius %.0f): %s" % (radius, {k: (len(v), v[:3]) for k, v in bad.items()} or "clear"))
    return bad


def joins(ctx):
    A, B, C, SA, SB = ctx.A, ctx.B, ctx.C, ctx.SA, ctx.SB
    def end(P, alt=False):
        return (round(float(P.p[-1, 0]), 1), round(float(P.alt[-1] if alt else P.T[-1]), 1), round(float(P.p[-1, 1]), 1))
    def start(P, alt=False):
        return (round(float(P.p[0, 0]), 1), round(float(P.alt[0] if alt else P.T[0]), 1), round(float(P.p[0, 1]), 1))
    print("join C end -> A start    :", end(C), "->", start(A))
    print("join A lip -> SA start   :", end(A), "->", start(SA, True))
    print("join SA end -> B start   :", end(SA, True), "->", start(B))
    print("join B pad -> SB start   :", end(B), "->", start(SB, True))
    print("join SB end -> C start   :", end(SB, True), "->", start(C))
    for a, b, nm in ((C, A, "C->A"), (A, SA, "A->SA"), (SA, B, "SA->B"), (B, SB, "B->SB"), (SB, C, "SB->C")):
        da = a.t[-1]
        db = b.t[0]
        ang = math.degrees(math.acos(max(-1, min(1, float(da[0] * db[0] + da[1] * db[1])))))
        print("   heading change at %s: %.1f deg" % (nm, ang))


def main(argv):
    ctx = load_ctx("--cached" in argv)
    W = ctx.W
    A, B, C, SA, SB = ctx.A, ctx.B, ctx.C, ctx.SA, ctx.SB
    print("\n=== block counts ===")
    tot = len(W.d)
    ys = [(k >> 28) - OY for k in W.d]
    print("total blocks %d   y range %d .. %d" % (tot, min(ys), max(ys)))
    for z, n in sorted(W.zcount.items(), key=lambda kv: -kv[1]):
        print("   %-10s %9d" % (z, n))
    print("\n=== joins ===")
    joins(ctx)
    print("\n=== kart roads ===")
    ch0, ch1 = ctx.marks.get("ch0", 0), ctx.marks.get("ch1", 0)
    for nm, P in (("A", A), ("B", B), ("C", C)):
        step_check(P, nm)
    road_check(ctx, "A", A)
    road_check(ctx, "B", B)
    road_check(ctx, "C", C)
    print("\n=== min turning radius per zone (kart) ===")
    for nm, P in (("A", A), ("B", B), ("C", C)):
        r, s = P.min_radius(window=8)
        print("   %s: min radius %.1f at s=%.0f (%s)" % (nm, r, s, zone_of(P, P.i(s))))
    print("\n=== flights ===")
    flight_check(ctx, SA, "SA", 6.0)
    flight_check(ctx, SB, "SB", 6.0)
    print("\n=== submarine lane ===")
    sub_check(ctx, 4.0)
    print("\n=== water basin (flood fill) ===")
    import floodtest3
    vol, leak = floodtest3.run(ctx)
    print("water volume %d, leak %s" % (vol, leak))
    if "--emit" in argv:
        from core import emit
        out = os.path.join(build3.OUT_DIR, build3.OUT_NAME)
        emit(ctx.W, build3.NAME, ctx.known, out_path=out)
        open(os.path.join(build3.OUT_DIR, "water_volume.txt"), "w").write(str(vol))


if __name__ == "__main__":
    main(sys.argv[1:])
