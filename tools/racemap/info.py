"""Writes out/map_info.md: zone lengths, lap-time estimate, gate / trigger coordinates, water basin."""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np

import build
import layout


def main():
    ctx = build.build_world_for_info() if hasattr(build, "build_world_for_info") else None
    A, B, S = layout.build_path_a()[0], layout.build_path_b()[0], layout.build_sky()[0]
    build.set_heights(A, B, S)
    mA, mB, mS = A.marks, B.marks, S.marks
    sp = build.SPEED
    ch0, ch1 = mA["abyss"] + 58, mA["abyss"] + 495

    def pos(P, s, alt=None):
        i = P.i(s)
        y = int(P.T[i]) if alt is None else int(round(P.alt[i]))
        return "(%d, %d, %d)" % (round(P.p[i, 0]), y, round(P.p[i, 1]))

    def seg(P, a, b):
        return b - a

    rows = []
    # (zone, mode, length, start, end)
    rows.append(("Starlight Stadium (start / finish)", "kart", mA["meadows"] - mA["start"], pos(A, mA["start"]), pos(A, mA["meadows"])))
    rows.append(("Emerald Meadows", "kart", mA["canyon"] - mA["meadows"], pos(A, mA["meadows"]), pos(A, mA["canyon"])))
    rows.append(("Sunbaked Canyon", "kart", mA["crystal"] - mA["canyon"], pos(A, mA["canyon"]), pos(A, mA["crystal"])))
    rows.append(("Crystal Caverns", "kart", mA["abyss"] - mA["crystal"], pos(A, mA["crystal"]), pos(A, mA["abyss"])))
    # where the road dips below / climbs back above the water surface
    sw = np.nonzero((A.s > mA["abyss"]) & (A.s < mA["frost"]) & (A.T <= layout.WATER_Y))[0]
    w0, w1 = float(A.s[sw[0]]), float(A.s[sw[-1]])
    rows.append(("Dive ramp (kart)", "kart", w0 - mA["abyss"], pos(A, mA["abyss"]), pos(A, w0)))
    rows.append(("Coral Abyss (submarine)", "sub", w1 - w0, pos(A, w0), pos(A, w1)))
    rows.append(("Surface ramp (kart)", "kart", mA["frost"] - w1, pos(A, w1), pos(A, mA["frost"])))
    rows.append(("Frostpeak climb", "kart", mA["end"] - mA["frost"], pos(A, mA["frost"]), pos(A, mA["end"])))
    rows.append(("Sky Course (plane)", "plane", S.length, pos(S, 0, True), pos(S, S.length, True)))
    rows.append(("Runway + Emberforge Volcano", "kart", mB["rainbow"] - mB["runway"], pos(B, mB["runway"]), pos(B, mB["rainbow"])))
    rows.append(("Rainbow Road + Star Slide", "kart", mB["end"] - mB["rainbow"], pos(B, mB["rainbow"]), pos(B, mB["end"])))
    tot = {"kart": 0.0, "sub": 0.0, "plane": 0.0}
    lines = ["# Galaxy Grand Prix - World Tour v2 (Kart blocks): layout facts", "",
             "Coordinates are in script space: x east, y up, z south, origin = the block at your feet (the finish line is at 0, 0).", "",
             "| zone | mode | length (blocks) | from (x,y,z) | to (x,y,z) | time at assumed speed |", "|---|---|---|---|---|---|"]
    for name, mode, ln, a, b in rows:
        tot[mode] += ln
        lines.append("| %s | %s | %d | %s | %s | %.1f s |" % (name, mode, ln, a, b, ln / sp[mode]))
    t = sum(tot[k] / sp[k] for k in tot)
    lines += ["", "Totals: kart %.0f blocks, submarine %.0f, plane %.0f  ->  lap at assumed top speeds (kart %.0f, plane %.0f, sub %.0f blocks/s) = %.0f s" %
              (tot["kart"], tot["sub"], tot["plane"], sp["kart"], sp["plane"], sp["sub"], t), ""]
    lines += ["## Transform / trigger spots", ""]
    trig = [
        ("Finish / start line (Kart_Finish_Checker tiles on the layer above; the road surface is flush with the ground)", pos(A, 200.0)),
        ("Dive gate (kart -> sub): hoops over the top of the ramp", pos(A, ch0 + 6)),
        ("Sub course begins (floor of the basin)", pos(A, ch0 + 100)),
        ("Surface gate (sub -> kart): sign arch at the exit beach", pos(A, ch1 + 30)),
        ("Skyport launch lip (kart -> plane): last road block", pos(A, mA["end"])),
        ("Sky launch rings / first flight point", pos(S, 4, True)),
        ("Landing transform hoops (plane -> kart)", pos(S, S.length - 120, True)),
        ("Runway threshold (touch down here)", pos(B, 0)),
        ("Volcano gate", pos(B, mB["volcano"] - 8)),
        ("Rainbow Road warp rings", pos(B, mB["rainbow"] + 10)),
        ("Star Slide warp exit rings", pos(B, B.length - 30)),
    ]
    for n, p in trig:
        lines.append("- %s: %s" % (n, p))
    i0, i1 = A.i(ch0), A.i(ch1)
    lines += ["", "## Water (the one thing the tool cannot place)", "",
              "The Coral Abyss basin is walled and closed at both ends by solid plugs (checked with a flood fill: no leaks).",
              "Fill it with water up to y = %d (12 layers: y 0..%d). Volume is about 146,000 blocks; the fill area is roughly x 10..407, z 618..722." % (layout.WATER_Y, layout.WATER_Y),
              "Cyan marker blocks on the inner wall faces sit at y = %d so you can see the level." % layout.WATER_Y,
              "Channel starts at %s and ends at %s (centre line)." % (pos(A, ch0), pos(A, ch1))]
    out = os.path.join(build.OUT_DIR, "map_info.md")
    open(out, "w", encoding="utf8", newline="\n").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
