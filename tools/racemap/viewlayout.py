"""Draws the lap layout (centre lines by zone) to a PNG so the route can be judged before any block exists."""
import sys

import numpy as np
from PIL import Image, ImageDraw

from layout import build_path_a, build_path_b, build_sky

COL = {"start": (255, 255, 255), "meadows": (80, 220, 90), "canyon": (240, 170, 70), "crystal": (200, 90, 230),
       "abyss": (60, 140, 255), "frost": (170, 230, 255), "runway": (255, 255, 0), "volcano": (255, 70, 40),
       "rainbow": (255, 120, 220)}


def zone_of(path, s):
    best, bn = -1, None
    for k, v in path.marks.items():
        if k != "end" and v <= s + 1e-6 and v > best:
            best, bn = v, k
    return bn


def main(out="layout.png", scale=0.8):
    A, FC = build_path_a()
    B, VC = build_path_b()
    S, HC = build_sky()
    allp = np.concatenate([A.p, B.p, S.p])
    x0, z0 = allp.min(0) - 60
    x1, z1 = allp.max(0) + 60
    W, H = int((x1 - x0) * scale), int((z1 - z0) * scale)
    img = Image.new("RGB", (W, H), (20, 22, 30))
    d = ImageDraw.Draw(img)

    def px(x, z):
        return ((x - x0) * scale, (z - z0) * scale)

    for x in range(int(x0 // 100 * 100), int(x1) + 1, 100):
        d.line([px(x, z0), px(x, z1)], fill=(40, 44, 56))
        d.text(px(x, z0), str(x), fill=(120, 120, 140))
    for z in range(int(z0 // 100 * 100), int(z1) + 1, 100):
        d.line([px(x0, z), px(x1, z)], fill=(40, 44, 56))
        d.text(px(x0, z), str(z), fill=(120, 120, 140))
    for P in (A, B):
        for i in range(0, len(P) - 1, 2):
            z = zone_of(P, P.s[i])
            w = 3
            d.line([px(*P.p[i]), px(*P.p[i + 1])], fill=COL.get(z, (200, 200, 200)), width=w)
    for i in range(0, len(S) - 1, 2):
        d.line([px(*S.p[i]), px(*S.p[i + 1])], fill=(255, 255, 255), width=2)
    for P, nm in ((A, "A"), (B, "B"), (S, "S")):
        for k, v in P.marks.items():
            if k != "end":
                q = P.p[P.i(v)]
                d.text(px(*q), k, fill=(255, 255, 0))
    img.save(out)
    print("saved", out, img.size)


if __name__ == "__main__":
    main(*(sys.argv[1:2] or ["layout.png"]))
