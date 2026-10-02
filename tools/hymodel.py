"""Turns a Hytale .blockymodel into textured quads.

A model is a tree of nodes. Each node has a pivot (position, relative to its parent), a rotation
(quaternion), and maybe a shape: a box (size in model units, 32 units = 1 block) or a flat quad.
Every face of a shape says where its picture sits in the texture (textureLayout: pixel offset,
mirror flags, quarter turns).

Model space: x east, y up, z south, origin at the bottom centre of the block, 32 units per block.
Output positions are in BLOCK units with the block's own corner at 0,0,0 (x and z shifted by 0.5).
"""
import json
import numpy as np


class Conv:
    """The conventions the files do not spell out. Calibrated against the game's own icons."""
    # Rotate the whole model half a turn about Y (i.e. which side is 'front').
    front_flip = False
    # Sign of the quaternion rotation (+1: standard, -1: inverse).
    qsign = 1
    # Which way a face's `angle` turns its picture, in quarter turns per 90 degrees.
    angle_dir = 1
    # Rotate the face pictures of top/bottom by a quarter turn (top's 'up' edge is north or something else).
    top_rot = 0
    # Whether the file's 'front' is the -Z face (and 'back' +Z), and whether 'right' is the -X face.
    fb_swap = False
    lr_swap = False
    # A child's position is measured from its parent's shape centre (pivot + offset), not its pivot.
    child_center = True
    # Debug: node indices (depth-first order) whose front/back faces are swapped.
    swap_nodes = frozenset()
    # Debug: node indices whose own rotation is inverted.
    inv_nodes = frozenset()
    # How a child's rotation and position combine with its parent's: named variants for calibration.
    compose = "PL"      # PL, LP, PLPi, PiL, LPi, PLiP...
    pos_frame = "P"     # P (parent rotation), Pi (its inverse), I (none)
    # A mirrored axis makes the picture extend BACKWARDS from the layout offset (not forwards).
    mirror_shift = True
    # Reflect the finished model along these axes (a left-handed model space would need this).
    reflect = (1, 1, 1)
    # A node's own rotation is NOT combined with its parent's (only positions follow the parent).
    no_inherit = False
    # A child's position is scaled by its parent's stretch.
    stretch_children = False
    # Stretch acts along the parent-frame axes (after rotation) instead of the node's own axes.
    stretch_world = False
    # Compose a child's rotation as local * parent instead of parent * local.
    compose_swap = False
    # Apply a face's mirror flags before its quarter turns (instead of after).
    mirror_first = False
    # Undo the picture mirroring that a negative stretch causes (keep pictures upright).
    cancel_neg = False
    # A shape's offset is multiplied by its stretch (False: the offset is in plain model units).
    stretch_offset = False


DEFAULT = Conv()


def quat_matrix(q, sign=1):
    """sign: 1, -1 (inverse), or a tuple of three signs applied to the x, y, z parts."""
    x, y, z, w = q["x"], q["y"], q["z"], q["w"]
    n = (x * x + y * y + z * z + w * w) ** 0.5 or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    if isinstance(sign, tuple):
        x, y, z = x * sign[0], y * sign[1], z * sign[2]
    elif sign < 0:
        x, y, z = -x, -y, -z
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def _corners(face, hx, hy, hz):
    """Four corners TL, TR, BR, BL of a box face as seen from outside, and the picture size (w, h) in
    units. front is +Z (the Conv can turn the whole model)."""
    if face == "front":
        return [(-hx, hy, hz), (hx, hy, hz), (hx, -hy, hz), (-hx, -hy, hz)], (2 * hx, 2 * hy)
    if face == "back":
        return [(hx, hy, -hz), (-hx, hy, -hz), (-hx, -hy, -hz), (hx, -hy, -hz)], (2 * hx, 2 * hy)
    if face == "right":     # +X
        return [(hx, hy, hz), (hx, hy, -hz), (hx, -hy, -hz), (hx, -hy, hz)], (2 * hz, 2 * hy)
    if face == "left":      # -X
        return [(-hx, hy, -hz), (-hx, hy, hz), (-hx, -hy, hz), (-hx, -hy, -hz)], (2 * hz, 2 * hy)
    if face == "top":
        return [(-hx, hy, -hz), (hx, hy, -hz), (hx, hy, hz), (-hx, hy, hz)], (2 * hx, 2 * hz)
    if face == "bottom":
        return [(-hx, -hy, hz), (hx, -hy, hz), (hx, -hy, -hz), (-hx, -hy, -hz)], (2 * hx, 2 * hz)
    raise ValueError(face)


def _uv_corners(layout, w, h, conv):
    """Texture pixel coordinates for TL, TR, BR, BL of a face, from its layout entry."""
    ox, oy = layout["offset"]["x"], layout["offset"]["y"]
    ang = int(layout.get("angle", 0)) % 360
    quarter = (ang // 90) * conv.angle_dir
    # The picture's own rectangle. Turned a quarter, it occupies h wide by w tall.
    rw, rh = (w, h) if quarter % 2 == 0 else (h, w)
    rect = [(ox, oy), (ox + rw, oy), (ox + rw, oy + rh), (ox, oy + rh)]   # TL TR BR BL of the rectangle
    m = layout.get("mirror", {})

    def mirror(uv):
        if m.get("x"):
            uv = [uv[1], uv[0], uv[3], uv[2]]
        if m.get("y"):
            uv = [uv[3], uv[2], uv[1], uv[0]]
        return uv

    if conv.mirror_shift:
        # Same picture, but a mirrored axis runs from the offset in the negative direction.
        dx = -rw if m.get("x") else 0
        dy = -rh if m.get("y") else 0
        rect = [(x + dx, y + dy) for x, y in rect]
    k = quarter % 4
    if conv.mirror_first:
        rect = mirror(rect)
        return [rect[(i + k) % 4] for i in range(4)]
    # Walk the face's corners around the rectangle, starting one corner further per quarter turn.
    uv = [rect[(i + k) % 4] for i in range(4)]
    return mirror(uv)


class Quad:
    __slots__ = ("pts", "uv", "normal", "flat", "double", "bright", "face", "node")

    def __init__(self, pts, uv, normal, flat, double, bright, face, node=""):
        self.pts, self.uv, self.normal = pts, uv, normal
        self.flat, self.double, self.bright, self.face, self.node = flat, double, bright, face, node


def model_quads(model, conv=DEFAULT, scale=1.0):
    """All visible faces of a model as Quads in block units."""
    out = []
    flip = np.diag([-1.0, 1.0, -1.0]) if conv.front_flip else np.eye(3)

    counter = [0]

    def visit(node, parent_R, parent_t, parent_st=np.ones(3)):
        my_index = counter[0]
        counter[0] += 1
        p = node["position"]
        R_local = quat_matrix(node["orientation"], conv.qsign)
        if my_index in conv.inv_nodes:
            R_local = R_local.T
        pos_v = np.array([p["x"], p["y"], p["z"]])
        if conv.stretch_children:
            pos_v = pos_v * parent_st
        pf = {"P": parent_R, "Pi": parent_R.T, "I": np.eye(3)}[conv.pos_frame]
        t = parent_t + pf @ pos_v
        Pi = parent_R.T
        variants = {"PL": parent_R @ R_local, "LP": R_local @ parent_R, "PLPi": parent_R @ R_local @ Pi,
                    "PiL": Pi @ R_local, "LPi": R_local @ Pi, "PLiP": parent_R @ R_local.T @ parent_R,
                    "PLi": parent_R @ R_local.T, "PiLP": Pi @ R_local @ parent_R}
        R = R_local if conv.no_inherit else variants[conv.compose]
        if conv.compose_swap: R = variants["LP"]
        shape = node.get("shape") or {}
        typ = shape.get("type")
        if typ in ("box", "quad") and shape.get("visible", True):
            off = np.array([shape["offset"]["x"], shape["offset"]["y"], shape["offset"]["z"]])
            st = np.array([shape["stretch"]["x"], shape["stretch"]["y"], shape["stretch"]["z"]])
            size = shape["settings"]["size"]
            layout = shape.get("textureLayout", {})
            shading = shape.get("shadingMode") or "standard"
            double = bool(shape.get("doubleSided"))
            if typ == "box":
                hx, hy, hz = size["x"] / 2.0, size["y"] / 2.0, size["z"] / 2.0
                # The layout's face NAME picks the picture; the geometry it lands on depends on the convention.
                geo = {"front": "front", "back": "back", "left": "left", "right": "right", "top": "top", "bottom": "bottom"}
                if conv.fb_swap != (my_index in conv.swap_nodes):
                    geo["front"], geo["back"] = "back", "front"
                if conv.lr_swap:
                    geo["left"], geo["right"] = "right", "left"
                faces = [(f, _corners(geo[f], hx, hy, hz)) for f in ("front", "back", "left", "right", "top", "bottom")
                         if f in layout]
            else:
                faces = [("front", _quad_corners(size, shape["settings"].get("normal", "+Z")))]
            for name, (corners, (fw, fh)) in faces:
                lay = layout.get(name)
                if lay is None:
                    continue
                if name in ("top", "bottom") and conv.top_rot:
                    k = conv.top_rot % 4
                    corners = [corners[(i + k) % 4] for i in range(4)]
                uv = _uv_corners(lay, int(round(fw)), int(round(fh)), conv)
                if conv.cancel_neg:
                    flip_u = flip_v = False
                    if name in ("front", "back", "top", "bottom"):
                        flip_u = st[0] < 0
                    else:
                        flip_u = st[2] < 0
                    if name in ("top", "bottom"):
                        flip_v = st[2] < 0
                    else:
                        flip_v = st[1] < 0
                    if flip_u:
                        uv = [uv[1], uv[0], uv[3], uv[2]]
                    if flip_v:
                        uv = [uv[3], uv[2], uv[1], uv[0]]
                pts = []
                offv = st * off if conv.stretch_offset else off
                for c in corners:
                    if conv.stretch_world:
                        v = t + R @ offv + parent_R @ (st * (R_local @ np.array(c)))
                    else:
                        v = offv + st * np.array(c)
                        v = t + R @ v
                    v = flip @ v
                    v = v * np.array(conv.reflect)
                    pts.append(v)
                a, b, c2 = pts[0], pts[1], pts[2]
                n = np.cross(b - a, c2 - b)
                ln = np.linalg.norm(n)
                n = n / ln if ln > 1e-9 else np.array([0.0, 1.0, 0.0])
                # A mirrored stretch flips the winding; the normal should still point outward from the box.
                centre = (flip @ (t + R @ offv)) * np.array(conv.reflect)
                mid = sum(pts) / 4.0
                if typ == "box" and np.dot(n, mid - centre) < 0:
                    n = -n
                q = Quad(
                    [((v[0] / 32.0) * scale + 0.5, (v[1] / 32.0) * scale, (v[2] / 32.0) * scale + 0.5) for v in pts],
                    uv, tuple(float(x) for x in n), shading == "flat", double, shading == "fullbright", name, node.get("name", ""))
                out.append(q)
        child_t = t
        if conv.child_center and shape.get("offset") is not None:
            off2 = np.array([shape["offset"]["x"], shape["offset"]["y"], shape["offset"]["z"]])
            st2 = np.array([shape["stretch"]["x"], shape["stretch"]["y"], shape["stretch"]["z"]])
            child_t = t + R @ (st2 * off2 if conv.stretch_offset else off2)
        my_st = np.array([shape["stretch"]["x"], shape["stretch"]["y"], shape["stretch"]["z"]]) if shape.get("stretch") else np.ones(3)
        for ch in node.get("children", []):
            visit(ch, R, child_t, my_st)

    for n in model["nodes"]:
        visit(n, np.eye(3), np.zeros(3))
    return out


def _quad_corners(size, normal):
    """A flat quad of size (x wide, y tall) facing `normal`, centred on the node. +Z is like a box front."""
    hw, hh = size["x"] / 2.0, size["y"] / 2.0
    base = [(-hw, hh, 0), (hw, hh, 0), (hw, -hh, 0), (-hw, -hh, 0)]   # facing +Z
    n = normal.strip()
    sign = -1 if n.startswith("-") else 1
    axis = n[-1].upper()
    pts = []
    for (x, y, z) in base:
        if axis == "Z":
            pts.append((x, y, 0) if sign > 0 else (-x, y, 0))
        elif axis == "X":
            pts.append((0, y, -x) if sign > 0 else (0, y, x))
        else:   # Y: lies flat; picture 'up' is toward -Z
            pts.append((x, 0, -y) if sign > 0 else (x, 0, y))
    return pts, (size["x"], size["y"])


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)
