"""Voxel sculpting kit: signed-distance-ish fields on a numpy grid, exported as merged box ops."""
import numpy as np
import lib


NX, NY, NZ = 90, 70, 100
X, Y, Z = np.meshgrid(np.arange(NX, dtype=np.float32), np.arange(NY, dtype=np.float32), np.arange(NZ, dtype=np.float32), indexing="ij")


def ell(c, r):
    """approximate signed distance to an ellipsoid (negative inside)."""
    n = np.sqrt(((X - c[0]) / r[0]) ** 2 + ((Y - c[1]) / r[1]) ** 2 + ((Z - c[2]) / r[2]) ** 2)
    return (n - 1.0) * min(r)


def cap(a, b, rad):
    """capsule from a to b."""
    a = np.array(a, np.float32)
    b = np.array(b, np.float32)
    ab = b - a
    px, py, pz = X - a[0], Y - a[1], Z - a[2]
    t = np.clip((px * ab[0] + py * ab[1] + pz * ab[2]) / float(ab @ ab), 0, 1)
    dx, dy, dz = px - t * ab[0], py - t * ab[1], pz - t * ab[2]
    return np.sqrt(dx * dx + dy * dy + dz * dz) - rad


def tcap(a, b, ra, rb):
    """tapered capsule."""
    a = np.array(a, np.float32)
    b = np.array(b, np.float32)
    ab = b - a
    px, py, pz = X - a[0], Y - a[1], Z - a[2]
    t = np.clip((px * ab[0] + py * ab[1] + pz * ab[2]) / float(ab @ ab), 0, 1)
    dx, dy, dz = px - t * ab[0], py - t * ab[1], pz - t * ab[2]
    return np.sqrt(dx * dx + dy * dy + dz * dz) - (ra + (rb - ra) * t)


def box_sdf(c, h):
    q = np.maximum(np.abs(X - c[0]) - h[0], np.maximum(np.abs(Y - c[1]) - h[1], np.abs(Z - c[2]) - h[2]))
    return q


def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b * (1 - h) + a * h - k * h * (1 - h)


def union(*fs, k=0):
    r = fs[0]
    for f in fs[1:]:
        r = smin(r, f, k) if k else np.minimum(r, f)
    return r


def vnoise(scale, seed):
    """smooth value noise 0..1 over the grid."""
    rng = np.random.RandomState(seed)
    gx, gy, gz = int(NX / scale) + 3, int(NY / scale) + 3, int(NZ / scale) + 3
    g = rng.rand(gx, gy, gz).astype(np.float32)
    fx, fy, fz = X / scale, Y / scale, Z / scale
    ix, iy, iz = fx.astype(int), fy.astype(int), fz.astype(int)
    tx, ty, tz = fx - ix, fy - iy, fz - iz
    tx, ty, tz = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty), tz * tz * (3 - 2 * tz)
    out = 0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (tx if dx else 1 - tx) * (ty if dy else 1 - ty) * (tz if dz else 1 - tz)
                out = out + w * g[ix + dx, iy + dy, iz + dz]
    return out


class Vox:
    """a set of cells with a class (palette key) each."""

    def __init__(self):
        self.cells = {}

    def put(self, mask, cls):
        idx = np.argwhere(mask)
        for (x, y, z) in idx:
            self.cells[(int(x), int(y), int(z))] = cls

    def put_classes(self, mask, clsmap):
        """clsmap: array of class names (object) same shape as grid."""
        idx = np.argwhere(mask)
        for (x, y, z) in idx:
            self.cells[(int(x), int(y), int(z))] = clsmap[x, y, z]

    def carve(self, mask):
        for (x, y, z) in np.argwhere(mask):
            self.cells.pop((int(x), int(y), int(z)), None)

    def emit(self, xmax=None, minbox=5):
        """greedy box decomposition per class; big boxes become box ops, the rest are pooled into point lists."""
        by = {}
        for (x, y, z), c in self.cells.items():
            if xmax is not None and x > xmax:
                continue
            by.setdefault(c, set()).add((x, y, z))
        nb = 0
        for cls, S in by.items():
            S = set(S)
            order = sorted(S, key=lambda p: (p[1], p[2], p[0]))
            used = set()
            small = []
            for (x, y, z) in order:
                if (x, y, z) in used:
                    continue
                x2 = x
                while (x2 + 1, y, z) in S and (x2 + 1, y, z) not in used:
                    x2 += 1
                z2 = z
                while all(((xx, y, z2 + 1) in S and (xx, y, z2 + 1) not in used) for xx in range(x, x2 + 1)):
                    z2 += 1
                y2 = y
                while all(((xx, y2 + 1, zz) in S and (xx, y2 + 1, zz) not in used) for xx in range(x, x2 + 1) for zz in range(z, z2 + 1)):
                    y2 += 1
                cells = [(xx, yy, zz) for xx in range(x, x2 + 1) for yy in range(y, y2 + 1) for zz in range(z, z2 + 1)]
                for c in cells:
                    used.add(c)
                if len(cells) >= minbox:
                    lib.ops.append({"op": "box", "from": [x, y, z], "to": [x2, y2, z2], "block": cls})
                    nb += 1
                else:
                    small.extend(cells)
            for i in range(0, len(small), 400):
                chunk = small[i:i + 400]
                lib.ops.append({"op": "set", "points": [list(c) for c in chunk], "block": cls})
                nb += 1
        return nb
