W, H = 38, 24
SP = []


def span(r, a, b):
    SP.append((r, a, b))


# hat cone tilted back
for r, (a, b) in enumerate([(9, 10), (9, 11), (10, 12), (10, 13), (11, 14), (11, 15), (12, 16)]):
    span(r, a, b)
span(7, 8, 23)     # brim
span(8, 9, 22)
# head + nose, chin
for r, (a, b) in zip(range(9, 13), [(14, 21), (14, 22), (14, 20), (15, 19)]):
    span(r, a, b)
# hair streaming back
for r, (a, b) in zip(range(9, 14), [(9, 13), (7, 13), (5, 13), (6, 12), (8, 12)]):
    span(r, a, b)
# body and robe
for r, (a, b) in zip(range(13, 16), [(13, 20), (8, 21), (5, 22)]):
    span(r, a, b)
span(16, 15, 17)
span(16, 19, 21)
# arm
for (r, a, b) in [(12, 19, 21), (13, 21, 23), (14, 23, 25), (15, 24, 26)]:
    span(r, a, b)
# broom pole
span(17, 2, 24)
span(16, 23, 36)
span(17, 23, 34)
span(15, 30, 36)
# bristles
for r, (a, b) in zip(range(12, 23), [(0, 0), (0, 1), (0, 3), (0, 5), (0, 6), (0, 22), (0, 6), (0, 5), (0, 3), (0, 1), (0, 0)]):
    pass
for r, (a, b) in zip(range(13, 22), [(0, 1), (0, 3), (0, 5), (0, 7), (0, 8), (0, 7), (0, 5), (0, 3), (0, 1)]):
    span(r + 1, a, b)
# cat
for r, (a, b) in zip(range(11, 16), [(28, 31), (28, 32), (27, 33), (28, 33), (28, 33)]):
    span(r, a, b)
for r, (a, b) in zip(range(8, 11), [(32, 32), (31, 34), (31, 35)]):
    span(r, a, b)
span(8, 35, 35)


def grid():
    g = [["." for _ in range(W)] for _ in range(H)]
    for (r, a, b) in SP:
        for c in range(a, b + 1):
            if 0 <= r < H and 0 <= c < W:
                g[r][c] = "#"
    return g


def cells(ox, oy, z):
    g = grid()
    out = []
    for r in range(H):
        for c in range(W):
            if g[r][c] == "#":
                out.append((ox + c, oy + (H - 1 - r), z))
    return out


def small(scale=0.8, thr=0.42):
    from PIL import Image
    g = grid()
    img = Image.new("L", (W, H), 0)
    for r in range(H):
        for c in range(W):
            if g[r][c] == "#":
                img.putpixel((c, r), 255)
    w2, h2 = int(round(W * scale)), int(round(H * scale))
    img = img.resize((w2, h2), Image.BOX)
    return [[("#" if img.getpixel((c, r)) > 255 * thr else ".") for c in range(w2)] for r in range(h2)]


if __name__ == "__main__":
    for row in grid():
        print("".join(row))
