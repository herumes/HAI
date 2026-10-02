#!/usr/bin/env python3
"""Builds the block pack the 3D prefab preview draws with, from Hytale's own Assets.zip.

    python tools/build_preview_pack.py [Assets.zip] [--install]

Everything comes from the game files: each block's definition (which model or which per-side textures),
its .blockymodel geometry, and its texture. The result is ONE script file, pack.js, that the viewer
loads next to preview.html (a script tag works from a local file, where fetch and canvas reads would not):

    window.HYPACK = {
        atlas: "data:image/png;base64,...",      every texture in the pack, packed into one image
        tex:   [[x, y, w, h, alphaClass], ...],  where each texture sits in it
        meshes:[...],                            model geometry, shared between blocks
        recs:  [...],                            what a block looks like (meshes + textures + flags)
        blocks:{ "Rock_Stone_Brick": 12, ... },  block name -> rec, for plain names AND state variants
    }

The game's files are not redistributed by this repository: the pack is generated on your machine from
your install and is git-ignored.
"""
import argparse
import base64
import io
import json
import math
import os
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import hyblock          # noqa: E402
import hymodel          # noqa: E402
import hyrace           # noqa: E402
from hyassets import Assets, STATE_SEP  # noqa: E402

PAD = 4                 # pixels of wrapped border around every texture in the atlas
POS_SCALE = 1024        # block units -> int
UV_SCALE = 16           # texture pixels -> int
MAX_VARIANTS = 6

OPACITY = {"Solid": 0, None: 0, "Transparent": 1, "Cutout": 1, "Semitransparent": 2}
RANDOM_ROT = {None: 0, "None": 0, "YawStep90": 1, "YawStep1": 2, "YawStep1XZ": 2, "YawPitchRollStep1": 3}


# ---------------------------------------------------------------------------------------------
# Textures
# ---------------------------------------------------------------------------------------------
class Textures:
    """Loads, tints and registers every distinct picture the blocks use."""

    def __init__(self, assets):
        self.A = assets
        self.raw = {}
        self.keys = {}        # key -> id
        self.images = []      # id -> RGBA uint8 array

    def load(self, path):
        if path not in self.raw:
            try:
                im = Image.open(io.BytesIO(self.A.read(path))).convert("RGBA")
            except KeyError:
                im = Image.new("RGBA", (32, 32), (255, 0, 255, 255))
                print("  missing texture", path, file=sys.stderr)
            self.raw[path] = np.asarray(im, np.float32)
        return self.raw[path]

    def get(self, path, tint=None, mask=None, mask_tint=None):
        """A texture id. `tint` multiplies the colour; `mask` is an overlay (the grass fringe on a grass
        block's sides) tinted with `mask_tint` and composited over the picture."""
        key = (path, tint, mask, mask_tint)
        if key in self.keys:
            return self.keys[key]
        img = self.load(path).copy()
        if mask:
            m = self.load(mask)
            a = m[..., 3:4] / 255.0
            ov = m[..., :3] * (np.array(mask_tint or (255, 255, 255), np.float32) / 255.0)
            if m.shape[:2] == img.shape[:2]:
                img[..., :3] = img[..., :3] * (1 - a) + ov * a
        if tint:
            img[..., :3] *= np.array(tint, np.float32) / 255.0
        arr = np.clip(img + 0.5, 0, 255).astype(np.uint8)
        self.keys[key] = len(self.images)
        self.images.append(arr)
        return self.keys[key]

    def size(self, tid):
        h, w = self.images[tid].shape[:2]
        return w, h

    def alpha_class(self, tid):
        a = self.images[tid][..., 3]
        if a.min() == 255:
            return 0          # fully opaque
        return 1 if np.all((a == 0) | (a == 255)) else 2   # cut-out / partly see-through


def pack_atlas(tex, max_width=4096):
    """Shelf-packs the textures (each with a wrapped border) into one image. Returns (image, rects)."""
    order = sorted(range(len(tex.images)), key=lambda i: (-tex.images[i].shape[0], -tex.images[i].shape[1]))
    rects = [None] * len(tex.images)
    x = y = shelf = 0
    for i in order:
        h, w = tex.images[i].shape[:2]
        pw, ph = w + 2 * PAD, h + 2 * PAD
        if x + pw > max_width:
            x, y, shelf = 0, y + shelf, 0
        rects[i] = (x + PAD, y + PAD, w, h)
        x += pw
        shelf = max(shelf, ph)
    height = 1
    while height < y + shelf:
        height *= 2
    atlas = np.zeros((height, max_width, 4), np.uint8)
    for i in range(len(tex.images)):
        px, py, w, h = rects[i]
        padded = np.pad(tex.images[i], ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
        atlas[py - PAD:py + h + PAD, px - PAD:px + w + PAD] = padded
        # anything fully transparent keeps its colour out of the border bleed
    return atlas, rects


# ---------------------------------------------------------------------------------------------
# Geometry: wrapping texture coordinates
# ---------------------------------------------------------------------------------------------
# The game samples textures with wrap-around: a face at offset (32, 32) on a 32x32 picture shows the
# whole picture. A texture atlas cannot wrap, so a face is cut along the seams and every piece is
# moved back inside the picture.
def _clip(poly, axis, value, keep_low):
    """Sutherland-Hodgman against u<=value / u>=value (axis 3) or v (axis 4). Vertices: (x, y, z, u, v)."""
    out = []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        da = (value - a[axis]) if keep_low else (a[axis] - value)
        db = (value - b[axis]) if keep_low else (b[axis] - value)
        if da >= 0:
            out.append(a)
        if (da >= 0) != (db >= 0):
            t = (value - a[axis]) / (b[axis] - a[axis])
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(5)))
    return out


def wrap_polygon(pts, uv, W, H):
    """Splits a face at texture seams and shifts each piece into [0, W] x [0, H]. Returns polygons."""
    stack = [[(p[0], p[1], p[2], q[0], q[1]) for p, q in zip(pts, uv)]]
    done = []
    eps = 1e-4
    guard = 0
    while stack and guard < 200:
        guard += 1
        poly = stack.pop()
        if len(poly) < 3:
            continue
        us = [v[3] for v in poly]
        vs = [v[4] for v in poly]
        umin, umax, vmin, vmax = min(us), max(us), min(vs), max(vs)
        bu = (math.floor((umin + eps) / W) + 1) * W
        if bu < umax - eps:
            stack.append(_clip(poly, 3, bu, True))
            stack.append(_clip(poly, 3, bu, False))
            continue
        bv = (math.floor((vmin + eps) / H) + 1) * H
        if bv < vmax - eps:
            stack.append(_clip(poly, 4, bv, True))
            stack.append(_clip(poly, 4, bv, False))
            continue
        su = -math.floor((umin + eps) / W) * W
        sv = -math.floor((vmin + eps) / H) * H
        poly = [(v[0], v[1], v[2], v[3] + su, v[4] + sv) for v in poly]
        area = 0.0
        for i in range(len(poly)):
            a, b = poly[i], poly[(i + 1) % len(poly)]
            area += a[3] * b[4] - b[3] * a[4]
        if abs(area) > 1e-3:
            done.append(poly)
    return done


def encode_mesh(faces, W, H):
    """Faces -> a flat, quantised list of polygons: [flags, nx, ny, nz, n, (x, y, z, u, v) * n, ...]."""
    out = []
    for f in faces:
        flags = (1 if f.flat else 0) | (2 if f.double else 0) | (4 if f.bright else 0)
        n = f.normal
        for poly in wrap_polygon(f.pts, f.uv, W, H):
            row = [flags, int(round(n[0] * 100)), int(round(n[1] * 100)), int(round(n[2] * 100)), len(poly)]
            for v in poly:
                row += [int(round(v[0] * POS_SCALE)), int(round(v[1] * POS_SCALE)), int(round(v[2] * POS_SCALE)),
                        int(round(v[3] * UV_SCALE)), int(round(v[4] * UV_SCALE))]
            out.append(row)
    return out


# ---------------------------------------------------------------------------------------------
# Blocks
# ---------------------------------------------------------------------------------------------
# Things that have a model for the inventory or the hand but are never placed in a world.
NOT_PLACED = ("Potion", "Food", "Ingredient", "Tool", "Template", "Recipe", "Objective", "Bandage", "Decorative_Potion",
              "Container", "Weapon", "Armor", "Debug", "Editor", "Test")


def all_block_names(A):
    names = []
    for item_id in sorted(A.raw):
        if item_id.split("_")[0] in NOT_PLACED:
            continue
        it = A.item(item_id)
        bt = it.get("BlockType") if it else None
        if not bt:
            continue
        names.append(item_id)
        for st in ((bt.get("State") or {}).get("Definitions") or {}):
            names.append("*" + item_id + STATE_SEP + st)
    return names


def build(assets_path, out_dir, verbose=True, race_zip="auto", mods=True):
    t0 = time.time()
    A = Assets(assets_path)
    if race_zip or mods:
        # The race server's own blocks (tools/race_blocks.json + its BlockTextures.zip) go in the same pack.
        ov = hyrace.overlay(None if race_zip in ("auto", None) else race_zip, mods=mods, race=bool(race_zip))
        if ov:
            A.add_overlay(ov[0], ov[1])
            if verbose:
                print("server blocks: %d (race catalog %s; mods: %s)" % (
                    len(ov[1]), "yes" if ov[2] else "no", ", ".join("%s x%d" % (m["name"], len(m["items"])) for m in ov[3]) or "none"))
        elif verbose:
            print("no BlockTextures.zip or server mod zips found (tools/race_blocks, tools/server_mods, Downloads): left out")
    conv = hymodel.Conv()
    tex = Textures(A)
    meshes, mesh_index = [], {}
    recs, rec_index = [], {}
    blocks = {}
    skipped = 0

    def mesh_for(g, tid):
        W, H = tex.size(tid)
        key = (g.model_key, W, H)
        if key not in mesh_index:
            mesh_index[key] = len(meshes)
            meshes.append(encode_mesh(g.model_faces, W, H))
        return mesh_index[key]

    # Texture 0 is the "missing" picture, shown for any block the pack does not know.
    miss = np.zeros((8, 8, 4), np.uint8)
    miss[..., 3] = 255
    for yy in range(8):
        for xx in range(8):
            miss[yy, xx, :3] = (255, 0, 255) if (xx // 4 + yy // 4) % 2 == 0 else (30, 0, 30)
    tex.keys[("missing",)] = 0
    tex.images.append(miss)

    names = all_block_names(A)
    for name in names:
        try:
            base = hyblock.block_geom(A, name, conv, 0)
        except Exception as e:                       # a broken definition must not stop the pack
            if verbose:
                print("  skipped", name, e, file=sys.stderr)
            skipped += 1
            continue
        if base is None or base.draw == "Empty" or not base.faces:
            skipped += 1
            continue
        variants = []
        for v in range(min(base.variants, MAX_VARIANTS)):
            g = base if v == 0 else hyblock.block_geom(A, name, conv, v)
            var = {}
            if g.cube_faces:
                order = ["Up", "Down", "North", "South", "East", "West"]
                by = {f.face: f for f in g.cube_faces}
                ids = []
                for face in order:
                    f = by.get(face)
                    if f is None:
                        ids.append(-1)
                    else:
                        ids.append(tex.get(f.tex, f.tint, f.side_mask, f.node if f.side_mask else None))
                var["c"] = ids
            if g.model_faces:
                f0 = g.model_faces[0]
                tid = tex.get(f0.tex, f0.tint)
                var["t"] = tid
                var["m"] = mesh_for(g, tid)
            w = g.weights[v] if v < len(g.weights) else 1
            if w != 1:
                var["w"] = w
            variants.append(var)
        if not variants or all(not v for v in variants):
            skipped += 1
            continue
        rec = {
            "o": OPACITY.get(base.opacity, 1),
            "s": 1 if base.is_cube else 0,
            "r": RANDOM_ROT.get(base.random_rotation, 2 if base.random_rotation else 0),
            "v": variants,
        }
        if base.light:
            rec["l"] = 1
        if base.unlit:
            rec["u"] = 1
        key = json.dumps(rec, sort_keys=True)
        if key not in rec_index:
            rec_index[key] = len(recs)
            recs.append(rec)
        blocks[name] = rec_index[key]

    # A ground tile for the viewer: the grass top, tinted.
    grass_bt = A.block_type("Soil_Grass")
    ground_tid = tex.get("Common/BlockTextures/Soil_Grass_GS.png", (103, 182, 45))

    atlas, rects = pack_atlas(tex)
    texinfo = [[r[0], r[1], r[2], r[3], tex.alpha_class(i)] for i, r in enumerate(rects)]
    buf = io.BytesIO()
    Image.fromarray(atlas, "RGBA").save(buf, format="PNG", optimize=True)
    atlas_png = buf.getvalue()

    pack = {
        "v": 1,
        "aw": int(atlas.shape[1]), "ah": int(atlas.shape[0]),
        "ps": POS_SCALE, "us": UV_SCALE,
        "tex": texinfo, "meshes": meshes, "recs": recs, "blocks": blocks,
        "ground": ground_tid, "missing": 0,
    }
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "pack.js")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("window.HYPACK=")
        json.dump(pack, f, separators=(",", ":"))
        f.write(";\nwindow.HYPACK.atlas=\"data:image/png;base64,")
        f.write(base64.b64encode(atlas_png).decode("ascii"))
        f.write("\";\n")
    if verbose:
        print("blocks: %d  (skipped %d)" % (len(blocks), skipped))
        print("distinct block records: %d, meshes: %d, textures: %d" % (len(recs), len(meshes), len(tex.images)))
        print("atlas: %dx%d, png %.1f MB" % (atlas.shape[1], atlas.shape[0], len(atlas_png) / 1e6))
        print("wrote %s (%.1f MB) in %.0fs" % (path, os.path.getsize(path) / 1e6, time.time() - t0))
    return path


def find_assets():
    """Where the launcher puts Assets.zip, if it is there."""
    cands = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        cands.append(os.path.join(appdata, "Hytale", "install", "release", "package", "game", "latest", "Assets.zip"))
    for c in cands:
        if os.path.exists(c):
            return c
    return None


VIEWER_FILES = ("preview.html", "viewer.js", "mesher.js", "collide.js", "pack.js", "bigview.html", "bigview.js", "bigcore.js")


def install(src_dir, dest):
    """Copies the viewer and its pack to where the mod looks for them (a 'preview' folder next to the DLL)."""
    import shutil
    os.makedirs(dest, exist_ok=True)
    for name in VIEWER_FILES:
        shutil.copyfile(os.path.join(src_dir, name), os.path.join(dest, name))
    print("installed the viewer to", dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets", nargs="?", help="path to Hytale's Assets.zip (found automatically when omitted)")
    ap.add_argument("--race-zip", default="auto", metavar="ZIP",
                    help="the race server's BlockTextures.zip (default: tools/race_blocks/ or Downloads); 'none' leaves the custom blocks out")
    ap.add_argument("--no-mods", action="store_true", help="leave out server mods with their own definitions (tools/server_mods)")
    ap.add_argument("--out", default=os.path.join(HERE, "preview"), help="folder for pack.js")
    ap.add_argument("--install", nargs="?", const=os.path.join(os.path.dirname(HERE), "x64", "Release", "preview"),
                    metavar="DIR", help="also copy the viewer to DIR (default x64/Release/preview, next to the DLL)")
    a = ap.parse_args()
    assets = a.assets or find_assets()
    if not assets or not os.path.exists(assets):
        print("Assets.zip not found. Pass its path: python tools/build_preview_pack.py <path to Assets.zip>")
        return 2
    print("reading", assets)
    build(assets, a.out, race_zip=None if a.race_zip == 'none' else a.race_zip, mods=not a.no_mods)
    if a.install:
        install(a.out, a.install)
    return 0


if __name__ == "__main__":
    sys.exit(main())
