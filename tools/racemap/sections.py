"""python sections.py -> cross-sections of every zone's road into out/sec_*.png (stitched into one sheet)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build, render
from PIL import Image, ImageDraw
ctx = build.main(sys.argv[1].split(",") if len(sys.argv) > 1 else [])
A, B, W = ctx.A, ctx.B, ctx.W
m, mb = A.marks, B.marks
jobs = [("stadium", A, 150), ("meadows", A, m["meadows"] + 200), ("canyon", A, m["canyon"] + 120), ("bridge", A, m["canyon"] + 0.5 * (m["crystal"] - m["canyon"])),
        ("crystal", A, m["crystal"] + 150), ("abyss-ramp", A, m["abyss"] + 30), ("frost", A, m["frost"] + 200),
        ("runway", B, 100), ("spiral", B, mb["volcano"] + 120), ("rainbow", B, mb["rainbow"] + 120), ("slide", B, mb["end"] - 120)]
tiles = []
for nm, P, s in jobs:
    T = int(P.Tat(s))
    out = os.path.join(build.OUT_DIR, "sec_%s.png" % nm)
    render.section(W, P, s, out, umax=18, y0=T - 4, y1=T + 9, scale=14, vwidth=1)
    im = Image.open(out).convert("RGB")
    d = ImageDraw.Draw(im)
    d.text((4, 2), "%s s=%d T=%d" % (nm, s, T), fill=(255, 255, 0))
    tiles.append(im)
W_ = max(t.width for t in tiles)
H_ = sum(t.height + 4 for t in tiles)
sheet = Image.new("RGB", (W_, H_), (0, 0, 0))
y = 0
for t in tiles:
    sheet.paste(t, (0, y)); y += t.height + 4
sheet.save(os.path.join(build.OUT_DIR, "sections.png"))
print(sheet.size)
