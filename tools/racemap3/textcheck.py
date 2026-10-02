"""Audit of every sign (lettering made of blocks) in the finished world.  A sign is registered when it is drawn
(common3.register_text via lsign / put_text / put_bitmap / ftext), so this sees exactly what the generator meant to write.

For each sign it reports
  missing      glyph blocks that are no longer the lettering material (overwritten by a later op, terrain, corridor clearing ...)
  hidden       glyph blocks with something solid between them and the reader (up to 24 blocks in front): buildings, trees, rails
  board        for signs meant to sit on a plate: how much of the text box has a block behind it (text wider than its board)
  diagonal     the sign faces a direction that is not a compass axis (staircased letters)
and renders contact sheets of what a reader really sees, so a human can check legibility at a glance.

    python textcheck.py                 build + audit, writes out/text_report.txt and out/text_sheet_N.png
    python textcheck.py --cached        reuse out/world.pkl + out/texts.pkl (written by check3.py)
    python textcheck.py --bad           only the signs with problems on the sheets
Exit code 1 when any sign fails, so it can sit in a build pipeline.
"""
import collections
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402,F401

import build3  # noqa: E402
import check3  # noqa: E402
from core import MAT  # noqa: E402

LOOK = 24            # how far in front of a sign blocks count as "covering" it
OCC_FAIL = 0.04      # more than this fraction of glyph blocks hidden = fail
COVER_FAIL = 0.98    # the board behind the text must cover this much of the text box


def key_of(W, x, y, z):
    g = W.get(x, y, z)
    return None if g is None else g[0]


def audit_one(W, t):
    vx, vz = t["view"][0], t["view"][2]
    rx, rz = t["right"][0], t["right"][2]
    cells = t["cells"]
    miss, occ, occ_by, first_occ = 0, 0, collections.Counter(), None
    for (x, y, z, m) in cells:
        if key_of(W, x, y, z) != m:
            miss += 1
        for k in range(1, LOOK + 1):
            q = (int(round(x + vx * k)), y, int(round(z + vz * k)))
            if W.has(*q):
                occ += 1
                occ_by[key_of(W, *q)] += 1
                if first_occ is None:
                    first_occ = (q, (x, y, z))
                break
    n = len(cells)
    us = [x * rx + z * rz for (x, y, z, m) in cells]
    ys = [y for (x, y, z, m) in cells]
    u0, u1, y0, y1 = min(us), max(us), min(ys), max(ys)
    bx, bz = cells[0][0], cells[0][2]
    ub = bx * rx + bz * rz
    tot = cov = 0
    for du in range(int(math.floor(u0 - ub)), int(math.ceil(u1 - ub)) + 1):
        for yy in range(y0, y1 + 1):
            px = bx + rx * du - vx
            pz = bz + rz * du - vz
            tot += 1
            if W.has(int(round(px)), yy, int(round(pz))):
                cov += 1
    ax = abs(vx) > 0.97 or abs(vz) > 0.97
    mid = cells[len(cells) // 2]
    return dict(label=t["label"], zone=t["zone"], kind=t["kind"], n=n, missing=miss, occluded=occ, occ_by=occ_by, covered=cov / float(max(1, tot)),
                width=int(u1 - u0 + 1), height=y1 - y0 + 1, at=(mid[0], mid[1], mid[2]), first_occ=first_occ, diagonal=not ax, plate=t["kind"] in ("lsign", "plane"))


def verdict(a):
    bad = []
    if a["missing"]:
        bad.append("MISSING %d/%d glyph blocks" % (a["missing"], a["n"]))
    if a["occluded"] > OCC_FAIL * a["n"]:
        bad.append("HIDDEN %d/%d glyph blocks by %s (e.g. block %s in front of glyph %s)" % (a["occluded"], a["n"], dict(a["occ_by"].most_common(3)), a["first_occ"][0], a["first_occ"][1]))
    if a["plate"] and a["covered"] < COVER_FAIL:
        bad.append("TEXT WIDER THAN ITS BOARD (%.0f%% of the text box has a board behind it)" % (100 * a["covered"]))
    if a["diagonal"]:
        bad.append("DIAGONAL sign (staircased letters)")
    return bad


def render_sheet(W, texts, out_prefix, per_sheet=12, px=6):
    from PIL import Image, ImageDraw
    from render import color_of
    cache = {}

    def col(k):
        if k not in cache:
            cache[k] = color_of(MAT[k][0])
        return cache[k]
    tiles = []
    for t, a in texts:
        vx, vz = t["view"][0], t["view"][2]
        rx, rz = t["right"][0], t["right"][2]
        cells = t["cells"]
        us = [x * rx + z * rz for (x, y, z, m) in cells]
        ys = [y for (x, y, z, m) in cells]
        bx, bz = cells[0][0], cells[0][2]
        ub = bx * rx + bz * rz
        du0, du1 = int(math.floor(min(us) - ub)) - 6, int(math.ceil(max(us) - ub)) + 6
        y0, y1 = min(ys) - 5, max(ys) + 5
        w, h = (du1 - du0 + 1), (y1 - y0 + 1)
        img = Image.new("RGB", (w * px, h * px + 14), (30, 34, 46))
        d = ImageDraw.Draw(img)
        for iu, du in enumerate(range(du0, du1 + 1)):
            for iy, yy in enumerate(range(y1, y0 - 1, -1)):
                c = None
                for k in range(LOOK + 6, -4, -1):                 # from the reader's side toward the board: the first solid block wins
                    q = (int(round(bx + rx * du + vx * k)), yy, int(round(bz + rz * du + vz * k)))
                    kk = key_of(W, *q)
                    if kk is not None:
                        c = col(kk)
                        break
                if c is not None:
                    d.rectangle([iu * px, iy * px + 14, iu * px + px - 1, iy * px + px - 1 + 14], fill=c)
        bad = verdict(a)
        d.text((2, 1), ("FAIL " if bad else "ok   ") + "%s [%s] %dx%d" % (a["label"][:28], a["zone"], a["width"], a["height"]),
               fill=(255, 120, 120) if bad else (150, 230, 160))
        tiles.append(img)
    sheets = []
    for s0 in range(0, len(tiles), per_sheet):
        grp = tiles[s0:s0 + per_sheet]
        colw = max(i.width for i in grp)
        ncol = max(1, min(3, 1900 // (colw + 8)))
        rows = [grp[i:i + ncol] for i in range(0, len(grp), ncol)]
        H = sum(max(i.height for i in r) + 8 for r in rows)
        sheet = Image.new("RGB", (ncol * (colw + 8), H), (12, 14, 20))
        y = 0
        for r in rows:
            x = 0
            for i in r:
                sheet.paste(i, (x, y))
                x += colw + 8
            y += max(i.height for i in r) + 8
        path = "%s_%d.png" % (out_prefix, len(sheets) + 1)
        sheet.save(path)
        sheets.append(path)
    return sheets


def main(argv):
    ctx = check3.load_ctx("--cached" in argv)
    W = ctx.W
    texts = getattr(W, "texts", [])
    print("%d signs registered" % len(texts))
    res = [(t, audit_one(W, t)) for t in texts]
    lines, nbad = [], 0
    for t, a in res:
        bad = verdict(a)
        nbad += bool(bad)
        lines.append("%-4s %-30s zone=%-9s at=%-22s %3dx%-3d cells=%-4d covered=%3.0f%%  %s" % (
            "FAIL" if bad else "ok", a["label"][:30], a["zone"], str(a["at"]), a["width"], a["height"], a["n"], 100 * a["covered"], "; ".join(bad)))
    rep = "\n".join(lines) + "\n\n%d signs, %d with problems\n" % (len(res), nbad)
    out = os.path.join(build3.OUT_DIR, "text_report.txt")
    open(out, "w", encoding="utf8").write(rep)
    print(rep)
    sel = [(t, a) for t, a in res if verdict(a)] if "--bad" in argv else res
    if sel:
        for p in render_sheet(W, sel, os.path.join(build3.OUT_DIR, "text_sheet" + ("_bad" if "--bad" in argv else ""))):
            print("wrote", p)
    return 1 if nbad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
