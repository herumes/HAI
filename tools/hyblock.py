"""One registry block name -> the textured faces that draw it (unrotated, block units).

A cube block has one texture per side (All / Sides / UpDown / Up / Down / North ...). A model block is a
.blockymodel plus one texture. Rotation, culling and atlas placement happen later; this only says what
the block looks like when it is not turned.
"""
import os
import numpy as np

import hymodel
from hyassets import Assets

# cube face: (normal, corners TL TR BR BL seen from outside)
CUBE_FACES = {
    "Up":    ((0, 1, 0),  [(0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)]),
    "Down":  ((0, -1, 0), [(0, 0, 1), (1, 0, 1), (1, 0, 0), (0, 0, 0)]),
    "North": ((0, 0, -1), [(1, 1, 0), (0, 1, 0), (0, 0, 0), (1, 0, 0)]),
    "South": ((0, 0, 1),  [(0, 1, 1), (1, 1, 1), (1, 0, 1), (0, 0, 1)]),
    "East":  ((1, 0, 0),  [(1, 1, 1), (1, 1, 0), (1, 0, 0), (1, 0, 1)]),
    "West":  ((-1, 0, 0), [(0, 1, 0), (0, 1, 1), (0, 0, 1), (0, 0, 0)]),
}
SIDES = ("North", "South", "East", "West")


def _first_hex(v):
    if isinstance(v, list) and v:
        v = v[0]
    if isinstance(v, str) and v.startswith("#"):
        h = v[1:]
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        try:
            return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
        except ValueError:
            return None
    return None


class Face:
    """One drawable face. `tex` is a path under Common/; uv is in texture pixels."""
    __slots__ = ("pts", "uv", "normal", "tex", "tint", "flat", "double", "bright", "face", "side_mask", "node")

    def __init__(self, pts, uv, normal, tex, tint, flat, double, bright, face, side_mask=None, node=""):
        self.pts, self.uv, self.normal, self.tex, self.tint = pts, uv, normal, tex, tint
        self.flat, self.double, self.bright, self.face, self.side_mask = flat, double, bright, face, side_mask
        self.node = node


class BlockGeom:
    def __init__(self):
        self.faces = []
        self.opacity = "Solid"        # Solid / Transparent / Cutout / Semitransparent
        self.is_cube = False          # a full cube: hides the faces it touches
        self.variants = 1
        self.random_rotation = None
        self.light = None
        self.scale = 1.0
        self.draw = "Cube"
        self.unlit = False            # drawn at full brightness whatever the light (GizmoCube: the neon blocks)
        self.cube_faces = []          # the faces of the cube part (unit cube, one texture per side)
        self.model_faces = []         # the faces of the model part
        self.model_key = None         # which model + scale they came from (for sharing geometry)
        self.weights = []


def _cube_textures(bt, variant):
    tex_list = bt.get("Textures") or []
    if not tex_list:
        return {}
    entry = tex_list[variant % len(tex_list)]
    out = {}
    if "All" in entry:
        for f in CUBE_FACES:
            out[f] = entry["All"]
    if "Sides" in entry:
        for f in SIDES:
            out[f] = entry["Sides"]
    if "UpDown" in entry:
        out["Up"] = out["Down"] = entry["UpDown"]
    for f in CUBE_FACES:
        if f in entry:
            out[f] = entry[f]
    return out


def _tint_for(bt, face):
    key = "Tint" + face
    if key in bt:
        return _first_hex(bt[key])
    if face in ("Up", "Down") + SIDES and "Tint" in bt:
        # a whole-block tint is only meaningful when the block asks for a biome colour
        return _first_hex(bt["Tint"])
    return None


def block_geom(A, name, conv=hymodel.DEFAULT, variant=0, model_cache={}):
    bt = A.block_type(name)
    if bt is None:
        return None
    g = BlockGeom()
    g.draw = bt.get("DrawType", "Cube")
    g.opacity = bt.get("Opacity", "Solid")
    g.random_rotation = bt.get("RandomRotation")
    g.light = bt.get("Light")
    g.scale = float(bt.get("CustomModelScale", 1.0))
    g.variants = max(len(bt.get("Textures") or []), len(bt.get("CustomModelTexture") or []), 1)
    g.weights = [t.get("Weight", 1) or 1 for t in (bt.get("Textures") or bt.get("CustomModelTexture") or [{}])]
    if g.draw == "Empty":
        return g

    if g.draw == "GizmoCube":
        # An unlit, alpha-blended cube overlay (NoCube's neon blocks): six plain sides, always full brightness.
        g.unlit = True
        g.draw = "Cube"
        g.opacity = "Solid"
        bt = {k: v for k, v in bt.items() if k != "CustomModel"}

    if g.draw in ("Cube", "CubeWithModel"):
        texes = _cube_textures(bt, variant)
        mask = bt.get("TextureSideMask")
        full = True
        for face, (normal, corners) in CUBE_FACES.items():
            path = texes.get(face)
            if not path:
                full = False
                continue
            uv = [(0, 0), (32, 0), (32, 32), (0, 32)]
            if face in ("Up", "Down") and conv.top_rot:
                k = conv.top_rot % 4
                uv = [uv[(i + k) % 4] for i in range(4)]
            masked = bool(mask and face in SIDES)
            fc = Face([tuple(map(float, c)) for c in corners], uv, normal, "Common/" + path,
                      None if masked else _tint_for(bt, face), False, False, False, face,
                      ("Common/" + mask) if masked else None)
            if masked:
                fc.node = _first_hex(bt.get("TintUp") or bt.get("Tint"))   # the overlay's colour
            g.faces.append(fc)
            g.cube_faces.append(fc)
        g.is_cube = full and g.draw == "Cube" and g.opacity in ("Solid", None)

    if g.draw in ("Model", "CubeWithModel") and bt.get("CustomModel"):
        mpath = "Common/" + bt["CustomModel"]
        if not A.has(mpath):
            return g
        key = (mpath, conv.front_flip, str(conv.qsign), conv.angle_dir, conv.fb_swap, conv.lr_swap, conv.top_rot, conv.child_center, conv.stretch_offset, conv.mirror_first, conv.cancel_neg, conv.compose_swap, conv.no_inherit, conv.stretch_children, conv.stretch_world, str(conv.reflect), conv.mirror_shift, conv.compose, conv.pos_frame, tuple(sorted(conv.swap_nodes)), tuple(sorted(conv.inv_nodes)), g.scale)
        if key not in model_cache:
            import json
            model = json.loads(A.read(mpath).decode("utf-8"))
            model_cache[key] = hymodel.model_quads(model, conv, g.scale)
        tex_list = bt.get("CustomModelTexture") or []
        tpath = None
        if tex_list:
            tpath = "Common/" + tex_list[variant % len(tex_list)]["Texture"]
        tint = _first_hex(bt.get("Tint"))
        g.model_key = key
        for q in model_cache[key]:
            if tpath is None:
                continue
            fc = Face(q.pts, q.uv, q.normal, tpath, tint, q.flat, q.double, q.bright, q.face, None, q.node)
            g.faces.append(fc)
            g.model_faces.append(fc)
    return g
