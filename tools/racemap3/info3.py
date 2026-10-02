"""Writes out/map_info3.md: zone lengths, lap-time estimate, gate / trigger coordinates, the water basin."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import build3  # noqa: E402
import layout3  # noqa: E402


def main(water_volume=None):
    ctx = build3.make_ctx()
    A, B, C, SA, SB = ctx.A, ctx.B, ctx.C, ctx.SA, ctx.SB
    mA, mB, mC = A.marks, B.marks, C.marks
    sp = build3.SPEED
    # the water channel (same rule as z_atlantis)
    idx = np.arange(len(B.s))
    down = idx[(B.s > mB["dive"]) & (B.T <= 12)]
    up = idx[(B.s < mB["surface"] + 10) & (B.s > mB["dive"]) & (B.T <= 12)]
    ch0, ch1 = float(B.s[down[0]]), float(B.s[up[-1]])
    sub = idx[(B.s > mB["dive"]) & (B.s < mB["surface"]) & (B.T <= layout3.WATER_Y)]
    w0, w1 = float(B.s[sub[0]]), float(B.s[sub[-1]])

    def pos(P, s, alt=False):
        i = P.i(s)
        y = int(round(P.alt[i])) if alt else int(P.T[i])
        return "(%d, %d, %d)" % (round(P.p[i, 0]), y, round(P.p[i, 1]))

    rows = [
        ("Nova Station Spaceport (start / finish)", "kart", mA["candy"] - 0, pos(A, 0), pos(A, mA["candy"])),
        ("Candy Comet Hills", "kart", mA["city"] - mA["candy"], pos(A, mA["candy"]), pos(A, mA["city"])),
        ("Neon Metropolis (figure-8 overpass, mega-arch tunnel)", "kart", mA["tower"] - mA["city"], pos(A, mA["city"]), pos(A, mA["tower"])),
        ("Skyport Tower (spiral climb + launch pad)", "kart", mA["lip"] - mA["tower"], pos(A, mA["tower"]), pos(A, mA["lip"])),
        ("Stratos Cloudlands (flight A)", "plane", SA.length, pos(SA, 0, True), pos(SA, SA.length, True)),
        ("Golden Dunes (runway + pyramid hall)", "kart", mB["oasis"], pos(B, 0), pos(B, mB["oasis"])),
        ("Lagoon dive ramp (kart)", "kart", w0 - mB["oasis"], pos(B, mB["oasis"]), pos(B, w0)),
        ("Atlantis Lagoon (submarine)", "sub", w1 - w0, pos(B, w0), pos(B, w1)),
        ("Lagoon surface ramp (kart)", "kart", mB["jungle"] - w1, pos(B, w1), pos(B, mB["jungle"])),
        ("Ancient Jungle", "kart", mB["keep"] - mB["jungle"], pos(B, mB["jungle"]), pos(B, mB["keep"])),
        ("Dread Keep", "kart", mB["glacier"] - mB["keep"], pos(B, mB["keep"]), pos(B, mB["glacier"])),
        ("Aurora Glacier (climb + summit pad)", "kart", mB["pad"] - mB["glacier"], pos(B, mB["glacier"]), pos(B, mB["pad"])),
        ("Cosmic Gateway (flight B)", "plane", SB.length, pos(SB, 0, True), pos(SB, SB.length, True)),
        ("Celestial Ribbon", "kart", 1100.0, pos(C, 0), pos(C, 1100.0)),
        ("Star Slide", "kart", C.length - 1100.0, pos(C, 1100.0), pos(C, C.length)),
    ]
    tot = {"kart": 0.0, "sub": 0.0, "plane": 0.0}
    L = ["# Universe Grand Prix - Cosmic Odyssey v3: layout facts", "",
         "Coordinates are in script space: x east, y up, z south, origin = the block at your feet (the finish line is at 0, -1, 0).", "",
         "| zone | mode | length (blocks) | from (x,y,z) | to (x,y,z) | time at assumed speed |", "|---|---|---|---|---|---|"]
    for name, mode, ln, a, b in rows:
        tot[mode] += ln
        L.append("| %s | %s | %d | %s | %s | %.1f s |" % (name, mode, ln, a, b, ln / sp[mode]))
    t = sum(tot[k] / sp[k] for k in tot)
    L += ["", "Totals: kart %.0f blocks, submarine %.0f, plane %.0f  ->  lap at assumed top speeds (kart %.0f, plane %.0f, sub %.0f blocks/s) = %.0f s (%.1f min)" %
          (tot["kart"], tot["sub"], tot["plane"], sp["kart"], sp["plane"], sp["sub"], t, t / 60.0),
          "At twice those speeds the lap is %.0f s." % (t / 2.0), ""]
    L += ["## Transform / trigger spots", ""]
    trig = [
        ("Finish / start line (Kart_Finish_Checker on the layer above the flush road)", pos(A, 420.0)),
        ("Star Slide warp-exit rings (the rings over the first stretch of the start straight)", pos(A, 40.0)),
        ("Skyport spiral starts (kart climbs 100 blocks around the tower)", pos(A, ctx.marks["spiral0"])),
        ("Launch pad lip, kart -> plane (last road block)", pos(A, mA["lip"])),
        ("Plane A launch tunnel / transform arch", pos(SA, 62.0, True)),
        ("Plane A landing transform hoops (plane -> kart)", pos(SA, SA.length - 150.0, True)),
        ("Runway threshold (touch down here, heading west)", pos(B, 0)),
        ("Pyramid hall (road passes through it)", pos(B, ctx.marks["pyramid"])),
        ("Dive gate hoops, kart -> sub (on the lagoon plateau)", pos(B, ch0 - 30)),
        ("Dam crest / channel starts (water side)", pos(B, ch0)),
        ("Sub course begins (seabed)", pos(B, w0)),
        ("Surface gate hoops, sub -> kart", pos(B, ch1 + 14)),
        ("Dread Keep gatehouse", pos(B, mB["keep"] + 0.11 * (mB["glacier"] - mB["keep"]))),
        ("Summit pad lip, kart -> plane", pos(B, mB["pad"])),
        ("Plane B launch tunnel / transform arch", pos(SB, 62.0, True)),
        ("Plane B landing transform hoops (plane -> kart)", pos(SB, SB.length - 170.0, True)),
        ("Ribbon landing strip start (touch down heading north)", pos(C, 0)),
        ("Star Slide start", pos(C, 1100.0)),
    ]
    for n_, p_ in trig:
        L.append("- %s: %s" % (n_, p_))
    L += ["", "## Water (the one thing the tool cannot place)", "",
          "The Atlantis channel is walled and closed at both ends by solid dams (checked with a flood fill: no leaks).",
          "Fill it with water up to y = %d (12 layers: y 0..%d). %s" % (layout3.WATER_Y, layout3.WATER_Y,
                                                                           ("Volume is about %d blocks." % water_volume) if water_volume else ""),
          "Cyan marker blocks on the inner wall faces sit at y = %d so you can see the level." % layout3.WATER_Y,
          "Channel from %s to %s along the road (the dams sit at both ends, the grotto roof covers about 130 blocks in the middle)." % (pos(B, ch0), pos(B, ch1)), ""]
    out = os.path.join(build3.OUT_DIR, "map_info3.md")
    os.makedirs(build3.OUT_DIR, exist_ok=True)
    open(out, "w", encoding="utf8", newline="\n").write("\n".join(L) + "\n")
    print("\n".join(L))


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else None)
