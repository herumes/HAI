"""Developer tool: checks the model conventions against the game's own item icons.

The icons (Common/Icons/ItemsGenerated) are the game's isometric renders of each block. This renders the
same block with tools/hyblock.py, at the icon's camera, and measures how well the two agree. Used to
settle the things the .blockymodel files do not say (which side is 'front', which way a picture turns).

    python tools/hycalibrate.py <Assets.zip> [--sheet out.png] [names...]
"""
import argparse
import io
import itertools
import math
import sys

import numpy as np
from PIL import Image

import hymodel
from hyassets import Assets
from hyblock import block_geom

_tex_cache = {}


def texture(A, path):
    if path not in _tex_cache:
        try:
            im = Image.open(io.BytesIO(A.read(path))).convert("RGBA")
        except KeyError:
            im = Image.new("RGBA", (32, 32), (255, 0, 255, 255))
        _tex_cache[path] = np.asarray(im)
    return _tex_cache[path]


def rot_matrix(rotation):
    """Prefab rotation number -> matrix: yaw (r % 4 quarter turns about Y) after pitch (r // 4 about X)."""
    yaw, pitch = rotation % 4, (rotation // 4) % 4

    def ry(k):
        a = math.radians(90 * k)
        c, s = round(math.cos(a)), round(math.sin(a))
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], float)

    def rx(k):
        a = math.radians(90 * k)
        c, s = round(math.cos(a)), round(math.sin(a))
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], float)
    return ry(yaw) @ rx(pitch)


def camera(yaw_deg, pitch_deg):
    th, ph = math.radians(yaw_deg), math.radians(pitch_deg)
    d = np.array([math.sin(th) * math.cos(ph), math.sin(ph), math.cos(th) * math.cos(ph)])
    r = np.array([math.cos(th), 0.0, -math.sin(th)])
    u = np.cross(d, r)
    return d, r, u


def euler_camera(rx, ry, rz, order="ZXY", signs=(1, 1, 1), depth_sign=1):
    """A camera from the icon's Euler angles (degrees). Returns (toward-camera, right, up)."""
    def R(axis, deg):
        a = math.radians(deg)
        c, s = math.cos(a), math.sin(a)
        if axis == "X":
            return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
        if axis == "Y":
            return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
    ang = {"X": rx * signs[0], "Y": ry * signs[1], "Z": rz * signs[2]}
    M = np.eye(3)
    for ax in order:            # first letter is applied last, like a product read left to right
        M = M @ R(ax, ang[ax])
    return (M[2] * depth_sign, M[0], M[1])


def project(pts, cam, scale, ox, oy):
    d, r, u = cam
    c = np.array([0.5, 0.5, 0.5])
    out = []
    for p in pts:
        v = np.array(p) - c
        out.append((ox + scale * float(v @ r), oy - scale * float(v @ u), float(v @ d)))
    return out


def render(A, name, conv, rotation=0, yaw=45, pitch=30, scale=None, ox=None, oy=None, size=64, ss=4,
           shade=False, variant=0, cam=None, only=None, only_idx=None):
    g = block_geom(A, name, conv, variant)
    if g is None:
        return None
    S = size * ss
    cam = cam if cam is not None else camera(yaw, pitch)
    scale = (scale or 0.72 * size) * ss
    ox = (size / 2 if ox is None else ox) * ss
    oy = (size / 2 if oy is None else oy) * ss
    M = rot_matrix(rotation) if rotation else np.eye(3)
    img = np.zeros((S, S, 4), np.float32)
    zbuf = np.full((S, S), -1e9, np.float32)
    ys, xs = np.mgrid[0:S, 0:S]
    for fi, f in enumerate(g.faces):
        if only is not None and not only(f):
            continue
        if only_idx is not None and fi not in only_idx:
            continue
        tex = texture(A, f.tex)
        th, tw = tex.shape[:2]
        pts = [tuple(M @ (np.array(p) - 0.5) + 0.5) for p in f.pts]
        sp = project(pts, cam, scale, ox, oy)
        n = M @ np.array(f.normal, float)
        light = 1.0
        if shade and not f.bright:
            nn = n / (np.linalg.norm(n) or 1.0)
            # The game's face shading: top 1.0, bottom 0.5, north/south 0.8, east/west 0.6, smoothly in between.
            light = 0.6 * nn[0] ** 2 + 0.8 * nn[2] ** 2 + (1.0 if nn[1] > 0 else 0.5) * nn[1] ** 2
        for tri in ((0, 1, 2), (0, 2, 3)):
            a, b, c = (sp[i] for i in tri)
            ua, ub, uc = (f.uv[i] for i in tri)
            minx = max(0, int(math.floor(min(a[0], b[0], c[0]))))
            maxx = min(S - 1, int(math.ceil(max(a[0], b[0], c[0]))))
            miny = max(0, int(math.floor(min(a[1], b[1], c[1]))))
            maxy = min(S - 1, int(math.ceil(max(a[1], b[1], c[1]))))
            if minx > maxx or miny > maxy:
                continue
            den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(den) < 1e-9:
                continue
            px = xs[miny:maxy + 1, minx:maxx + 1] + 0.5
            py = ys[miny:maxy + 1, minx:maxx + 1] + 0.5
            w0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / den
            w1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            z = w0 * a[2] + w1 * b[2] + w2 * c[2]
            u = w0 * ua[0] + w1 * ub[0] + w2 * uc[0]
            v = w0 * ua[1] + w1 * ub[1] + w2 * uc[1]
            tx = np.floor(u).astype(int) % tw       # the game samples with wrap-around (repeat)
            ty = np.floor(v).astype(int) % th
            col = tex[ty, tx].astype(np.float32)
            if f.tint:
                col[..., :3] *= np.array(f.tint, np.float32) / 255.0
            ok = inside & (col[..., 3] > 127) & (z > zbuf[miny:maxy + 1, minx:maxx + 1])
            zbuf[miny:maxy + 1, minx:maxx + 1][ok] = z[ok]
            col[..., :3] *= light
            col[..., 3] = 255
            region = img[miny:maxy + 1, minx:maxx + 1]
            region[ok] = col[ok]
    small = img.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
    # alpha averaged, colour weighted by alpha
    a = small[..., 3:4]
    rgb = img[..., :3].reshape(size, ss, size, ss, 3).sum(axis=(1, 3)) / np.maximum(1, (img[..., 3] > 0).reshape(size, ss, size, ss).sum(axis=(1, 3)))[..., None]
    return np.concatenate([rgb, a], axis=-1)


def icon(A, name):
    try:
        return np.asarray(Image.open(io.BytesIO(A.read("Common/Icons/ItemsGenerated/%s.png" % name))).convert("RGBA"), np.float32)
    except KeyError:
        return None


def iou(a, b):
    ma, mb = a[..., 3] > 127, b[..., 3] > 127
    inter = (ma & mb).sum()
    union = (ma | mb).sum()
    return inter / union if union else 0.0


def ncc(a, b):
    ma, mb = (a[..., 3] > 127) & (b[..., 3] > 127), None
    if ma.sum() < 20:
        return 0.0
    ga = a[..., :3].mean(axis=-1)[ma]
    gb = b[..., :3].mean(axis=-1)[ma]
    ga, gb = ga - ga.mean(), gb - gb.mean()
    den = math.sqrt((ga * ga).sum() * (gb * gb).sum())
    return float((ga * gb).sum() / den) if den > 1e-6 else 0.0


def save_sheet(images, path, cols=8, cell=192):
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell, rows * cell), (54, 58, 70, 255))
    for i, im in enumerate(images):
        arr = np.clip(im, 0, 255).astype(np.uint8)
        pil = Image.fromarray(arr, "RGBA").resize((cell, cell), Image.NEAREST)
        sheet.paste(pil, ((i % cols) * cell, (i // cols) * cell), pil)
    sheet.save(path)


def best_shift(icon_img, r, rad=14):
    """Slides the render over the icon and returns (score, dx, dy) at the best alignment by mask overlap."""
    ma = icon_img[..., 3] > 127
    mb = r[..., 3] > 127
    best = (-1.0, 0, 0)
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            sh = np.roll(np.roll(mb, dy, axis=0), dx, axis=1)
            inter = (ma & sh).sum()
            union = (ma | sh).sum()
            sc = inter / union if union else 0
            if sc > best[0]:
                best = (sc, dx, dy)
    return best


def shifted(r, dx, dy):
    return np.roll(np.roll(r, dy, axis=0), dx, axis=1)


ICON_SCALE = 62.7
DEFAULT_ICON_SCALE = 0.606


def icon_cam(A, name, depth_sign=1):
    it = A.item(name) or {}
    ip = it.get("IconProperties") or {}
    rot = ip.get("Rotation", [22.5, 45, 22.5])
    scale = ip.get("Scale", DEFAULT_ICON_SCALE) * ICON_SCALE
    return H_euler(rot, depth_sign), scale


def H_euler(rot, depth_sign=1):
    return euler_camera(rot[0], rot[1], rot[2], "YXZ", (1, 1, 1), depth_sign)


def compare(A, name, conv, depth_sign=1, ss=2, rotation=0, variant=0):
    """(score, iou, ncc, render, icon) of a block against its icon, or None."""
    ic = icon(A, name)
    if ic is None:
        return None
    cam, scale = icon_cam(A, name, depth_sign)
    r = render(A, name, conv, rotation, cam=cam, scale=scale, ox=32, oy=32, size=64, ss=ss, variant=variant, shade=True)
    if r is None:
        return None
    sc, dx, dy = best_shift(ic, r)
    r = shifted(r, dx, dy)
    n = ncc(ic, r)
    return 0.5 * sc + 0.5 * max(0.0, n), sc, n, r, ic


# ---------------------------------------------------------------------------------------------
# Regression check
# ---------------------------------------------------------------------------------------------
# Blocks that cover every rule the geometry code depends on (cube faces, stairs, slabs, stretched and
# rotated parts, mirrored faces, nested parts, textured furniture). Each must stay close to the game's icon.
CURATED = [
    "Rock_Stone_Brick", "Rock_Stone_Cobble", "Wood_Softwood_Planks", "Wood_Oak_Trunk_Full", "Wood_Village_Wall_White_Full",
    "Rock_Stone_Brick_Stairs", "Rock_Marble_Cobble_Stairs", "Cloth_Block_Wool_Green_Half",
    "Rock_Stone_Brick_Half", "Soil_Clay_Smooth_Red_Stairs", "Rock_Marble_Cobble_Roof", "Wood_Darkwood_Roof_Hollow",
    "Wood_Softwood_Beam", "Furniture_Village_Chest_Small", "Furniture_Village_Table", "Furniture_Village_Door",
    "Furniture_Village_Window", "Furniture_Tavern_Barrel", "Furniture_Village_Bench", "Furniture_Kweebec_Lantern",
    "Rock_Stone_Brick_Pillar_Middle", "Prototype_Window_Single",
]


def check(assets_path, minimum=0.87, per_block=0.60, verbose=True):
    """Mean agreement with the icons must stay above `minimum`, and no curated block below `per_block`."""
    A = Assets(assets_path)
    conv = hymodel.Conv()
    scores = []
    worst = []
    for name in CURATED:
        r = compare(A, name, conv, 1, ss=2)
        if r is None:
            print("no icon or block for", name)
            continue
        scores.append(r[0])
        worst.append((r[0], name, r[1], r[2]))
    worst.sort()
    mean = sum(scores) / max(1, len(scores))
    if verbose:
        for s, n, i, c in worst[:6]:
            print("  %.2f  (silhouette %.2f, texture %.2f)  %s" % (s, i, c, n))
        print("mean agreement with the game's icons: %.3f over %d blocks (needs %.2f)" % (mean, len(scores), minimum))
    return mean >= minimum and all(w[0] >= per_block for w in worst)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets")
    ap.add_argument("--check", action="store_true", help="the regression check (exit 1 on failure)")
    ap.add_argument("--sheet", help="write a comparison sheet of the given blocks (game icon, then mine)")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    if a.check or not a.names:
        sys.exit(0 if check(a.assets) else 1)
    A_ = Assets(a.assets)
    conv_ = hymodel.Conv()
    imgs_ = []
    for n_ in a.names:
        r_ = compare(A_, n_, conv_, 1, ss=3)
        if r_:
            print("%-40s score %.2f (silhouette %.2f, texture %.2f)" % (n_, r_[0], r_[1], r_[2]))
            imgs_ += [r_[4], r_[3]]
    if a.sheet and imgs_:
        save_sheet(imgs_, a.sheet, cols=4, cell=256)
