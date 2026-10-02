"""racemap core: a sparse voxel world, material registry, shape helpers, noise, and the
build-script emitter that turns the finished world into boxes + point lists.

Coordinates follow the game: x east, y up, z south.  Everything is integers.
"""
import json
import math
import os

import numpy as np

OX, OZ, OY = 4096, 4096, 1024          # key offsets (coordinates may be negative)


def kenc(x, y, z):
    return (x + OX) | ((z + OZ) << 14) | ((y + OY) << 28)


def kdec(k):
    return (k & 0x3FFF) - OX, (k >> 28) - OY, ((k >> 14) & 0x3FFF) - OZ


# --------------------------------------------------------------------------- materials
MAT = {}


def mat(key, blocks, weights=None):
    """Register a material: one block name, or a list (mixed per cell; weights repeat entries)."""
    if isinstance(blocks, str):
        blocks = [blocks]
    if weights:
        lst = []
        for b, w in zip(blocks, weights):
            lst += [b] * int(w)
    else:
        lst = list(blocks)
    MAT[key] = lst
    return key


# stairs / roof pieces: rotation says which side is TALL (n,w,s,e = 0..3, upside down 8..11)
def stair_rot(facing, flip=False):
    up = "nwse"
    inv = "senw"
    return (8 + inv.index(facing)) if flip else up.index(facing)


AXIS_ROT = {"y": 0, "z": 4, "x": 5}


class World:
    def __init__(self):
        self.d = {}
        self.spec_ids = {}
        self.specs = []
        self.zcount = {}
        self.surf = {}             # road surface cells as stamped: key -> spec id (restored at the end if terrain/props overwrote them)
        self._zone = None
        self.ticks = []
        self._last = 0

    # ---- bookkeeping
    def sid(self, m, rot=None):
        if m not in MAT:
            raise KeyError("material not registered: %s" % m)
        k = (m, rot)
        s = self.spec_ids.get(k)
        if s is None:
            s = len(self.specs)
            self.spec_ids[k] = s
            self.specs.append(k)
        return s

    class _Zone:
        def __init__(self, w, name):
            self.w, self.name = w, name

        def __enter__(self):
            self.before = len(self.w.d)
            self.w._last = self.before
            self.w._zone = self.name
            return self

        def __exit__(self, *a):
            self.w.zcount[self.name] = self.w.zcount.get(self.name, 0) + len(self.w.d) - self.before
            self.w._zone = None

    def zone(self, name):
        return World._Zone(self, name)

    def tick(self, label):
        n = len(self.d)
        self.ticks.append((self._zone, label, n - self._last))
        self._last = n

    # ---- single cells
    def put(self, x, y, z, m, rot=None, keep=False):
        k = kenc(int(x), int(y), int(z))
        if keep and k in self.d:
            return
        self.d[k] = self.sid(m, rot)

    def get(self, x, y, z):
        s = self.d.get(kenc(int(x), int(y), int(z)))
        return None if s is None else self.specs[s]

    def has(self, x, y, z):
        return kenc(int(x), int(y), int(z)) in self.d

    def erase(self, x, y, z):
        self.d.pop(kenc(int(x), int(y), int(z)), None)

    # ---- bulk (numpy arrays of equal shape)
    def put_arr(self, X, Y, Z, m, rot=None, keep=False):
        X = np.asarray(X, dtype=np.int64).ravel()
        Y = np.asarray(Y, dtype=np.int64).ravel()
        Z = np.asarray(Z, dtype=np.int64).ravel()
        if X.size == 0:
            return
        keys = ((X + OX) | ((Z + OZ) << 14) | ((Y + OY) << 28)).tolist()
        s = self.sid(m, rot)
        if keep:
            d = self.d
            for k in keys:
                if k not in d:
                    d[k] = s
        else:
            self.d.update(dict.fromkeys(keys, s))

    def erase_arr(self, X, Y, Z):
        X = np.asarray(X, dtype=np.int64).ravel()
        Y = np.asarray(Y, dtype=np.int64).ravel()
        Z = np.asarray(Z, dtype=np.int64).ravel()
        keys = ((X + OX) | ((Z + OZ) << 14) | ((Y + OY) << 28)).tolist()
        d = self.d
        for k in keys:
            d.pop(k, None)

    # ---- shapes
    def box(self, a, b, m, rot=None, mode="fill", keep=False):
        x0, x1 = sorted((int(a[0]), int(b[0])))
        y0, y1 = sorted((int(a[1]), int(b[1])))
        z0, z1 = sorted((int(a[2]), int(b[2])))
        if (x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 8_000_000:
            raise ValueError("box too big")
        X, Y, Z = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1), np.arange(z0, z1 + 1), indexing="ij")
        if mode != "fill":
            ex = (X == x0) | (X == x1)
            ey = (Y == y0) | (Y == y1)
            ez = (Z == z0) | (Z == z1)
            if mode == "shell":
                sel = ex | ey | ez
            elif mode == "walls":
                sel = ex | ez
            elif mode == "floor":
                sel = Y == y0
            elif mode == "ceiling":
                sel = Y == y1
            elif mode == "frame":
                sel = (ex.astype(int) + ey + ez) >= 2
            else:
                raise ValueError(mode)
            X, Y, Z = X[sel], Y[sel], Z[sel]
        self.put_arr(X, Y, Z, m, rot, keep)

    def line(self, a, b, m, rot=None, keep=False, rad=0.0):
        dx, dy, dz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        n = int(max(abs(dx), abs(dy), abs(dz)))
        t = np.linspace(0.0, 1.0, n + 1) if n else np.zeros(1)
        X = np.floor(a[0] + dx * t + 0.5).astype(np.int64)
        Y = np.floor(a[1] + dy * t + 0.5).astype(np.int64)
        Z = np.floor(a[2] + dz * t + 0.5).astype(np.int64)
        if rad > 0:
            r = int(math.ceil(rad))
            ox, oy, oz = np.meshgrid(*(np.arange(-r, r + 1),) * 3, indexing="ij")
            sel = (ox * ox + oy * oy + oz * oz) <= rad * rad + 0.25
            ox, oy, oz = ox[sel], oy[sel], oz[sel]
            X = (X[:, None] + ox[None, :]).ravel()
            Y = (Y[:, None] + oy[None, :]).ravel()
            Z = (Z[:, None] + oz[None, :]).ravel()
        self.put_arr(X, Y, Z, m, rot, keep)

    def disc(self, cx, y, cz, r, m, rot=None, keep=False, ring=False):
        R = int(math.ceil(r)) + 1
        ox, oz = np.meshgrid(np.arange(-R, R + 1), np.arange(-R, R + 1), indexing="ij")
        d = np.sqrt(ox * ox + oz * oz)
        sel = d <= r + 0.5
        if ring:
            sel &= d > r - 0.5
        self.put_arr(cx + ox[sel], np.full(sel.sum(), y), cz + oz[sel], m, rot, keep)

    def cyl(self, cx, cz, y0, y1, r, m, rot=None, mode="fill", keep=False):
        """y0..y1 inclusive.  mode: fill, wall (hollow tube), shell (tube + caps)."""
        R = int(math.ceil(r)) + 1
        ox, oz = np.meshgrid(np.arange(-R, R + 1), np.arange(-R, R + 1), indexing="ij")
        d = np.sqrt(ox * ox + oz * oz)
        inside = d <= r + 0.5
        ring = inside & (d > r - 0.5)
        for y in range(int(y0), int(y1) + 1):
            if mode == "fill":
                sel = inside
            elif mode == "wall":
                sel = ring
            else:
                sel = inside if (y == y0 or y == y1) else ring
            self.put_arr(cx + ox[sel], np.full(sel.sum(), y), cz + oz[sel], m, rot, keep)

    def ellipsoid(self, cx, cy, cz, rx, ry, rz, m, mode="fill", part="all", keep=False, rot=None):
        Rx, Ry, Rz = int(math.ceil(rx)) + 1, int(math.ceil(ry)) + 1, int(math.ceil(rz)) + 1
        ox, oy, oz = np.meshgrid(np.arange(-Rx, Rx + 1), np.arange(-Ry, Ry + 1), np.arange(-Rz, Rz + 1), indexing="ij")
        nrm = (ox / (rx + 0.5)) ** 2 + (oy / (ry + 0.5)) ** 2 + (oz / (rz + 0.5)) ** 2
        sel = nrm <= 1.0
        if mode == "shell":
            inner = (ox / max(0.5, rx - 0.5)) ** 2 + (oy / max(0.5, ry - 0.5)) ** 2 + (oz / max(0.5, rz - 0.5)) ** 2
            sel &= inner > 1.0
        if part == "upper":
            sel &= oy >= 0
        elif part == "lower":
            sel &= oy <= 0
        self.put_arr(cx + ox[sel], cy + oy[sel], cz + oz[sel], m, rot, keep)

    def sphere(self, c, r, m, **kw):
        self.ellipsoid(c[0], c[1], c[2], r, r, r, m, **kw)

    # ---- queries used by decorators
    def top_y(self, x, z, y_from=400, y_to=-2):
        for y in range(y_from, y_to, -1):
            if kenc(x, y, z) in self.d:
                return y
        return None


# --------------------------------------------------------------------------- noise
def _hash2(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 1442695041) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFF) / 65535.0


def vnoise(X, Z, scale, seed=0):
    """Smooth value noise in 0..1 for float arrays X, Z."""
    fx, fz = X / scale, Z / scale
    ix, iz = np.floor(fx).astype(np.int64), np.floor(fz).astype(np.int64)
    tx, tz = fx - ix, fz - iz
    tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
    a, b = _hash2(ix, iz, seed), _hash2(ix + 1, iz, seed)
    c, d = _hash2(ix, iz + 1, seed), _hash2(ix + 1, iz + 1, seed)
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz


def fbm(X, Z, scale, octaves=4, seed=0):
    tot, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        tot = tot + amp * vnoise(X, Z, scale / (2 ** o), seed + o * 101)
        norm += amp
        amp *= 0.5
    return tot / norm


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# --------------------------------------------------------------------------- emitting
def _rects(cells):
    """cells: sorted list of (z, x) -> list of (z0, z1, x0, x1) rectangles."""
    runs = {}
    cur = None
    for z, x in cells:
        if cur and cur[0] == z and cur[2] + 1 == x:
            cur[2] = x
        else:
            if cur:
                runs.setdefault(cur[0], []).append((cur[1], cur[2]))
            cur = [z, x, x]
    if cur:
        runs.setdefault(cur[0], []).append((cur[1], cur[2]))
    out = []
    open_ = {}
    for z in sorted(runs):
        nxt = {}
        for (x0, x1) in runs[z]:
            o = open_.pop((x0, x1), None)
            if o is not None and o[1] == z - 1:
                o[1] = z
                nxt[(x0, x1)] = o
            else:
                if o is not None:
                    out.append((o[0], o[1], x0, x1))
                nxt[(x0, x1)] = [z, z]
        for (x0, x1), o in open_.items():
            out.append((o[0], o[1], x0, x1))
        open_ = nxt
    for (x0, x1), o in open_.items():
        out.append((o[0], o[1], x0, x1))
    return out


def emit(world, name, known_names, small=5, max_ops=185000, out_path=None, verbose=True):
    """Turn the world into a build script (dict).  Boxes for chunky pieces, point lists for the rest."""
    keys = np.fromiter(world.d.keys(), dtype=np.int64, count=len(world.d))
    vals = np.fromiter(world.d.values(), dtype=np.int32, count=len(world.d))
    X = (keys & 0x3FFF) - OX
    Z = ((keys >> 14) & 0x3FFF) - OZ
    Y = (keys >> 28) - OY
    if verbose:
        print("cells:", len(keys), " x[%d..%d] y[%d..%d] z[%d..%d]" % (X.min(), X.max(), Y.min(), Y.max(), Z.min(), Z.max()))
    if max(abs(int(X.min())), abs(int(X.max())), abs(int(Z.min())), abs(int(Z.max()))) > 3990:
        raise ValueError("build passes the 4000 block coordinate limit")

    # palette: one entry per material that is actually used
    used = sorted({world.specs[s][0] for s in np.unique(vals).tolist()})
    bad = sorted({b for m in used for b in MAT[m] if b not in known_names})
    if bad:
        raise ValueError("unknown block names: %s" % bad)
    pal_key = {m: "m%d" % i for i, m in enumerate(used)}
    palette = {pal_key[m]: (MAT[m][0] if len(set(MAT[m])) == 1 else MAT[m]) for m in used}

    order = np.lexsort((X, Z, Y, vals))
    X, Y, Z, V = X[order], Y[order], Z[order], vals[order]
    # group boundaries by (val, y)
    change = np.nonzero((V[1:] != V[:-1]) | (Y[1:] != Y[:-1]))[0] + 1
    starts = np.concatenate(([0], change))
    ends = np.concatenate((change, [len(V)]))

    def run(small_):
        boxes, pts = [], {}
        per_spec_layers = {}
        for a, b in zip(starts.tolist(), ends.tolist()):
            per_spec_layers.setdefault(int(V[a]), []).append((int(Y[a]), a, b))
        for sid_, layers in per_spec_layers.items():
            prisms = {}
            for (y, a, b) in layers:
                cells = list(zip(Z[a:b].tolist(), X[a:b].tolist()))
                rects = _rects(cells)
                seen = set()
                for (z0, z1, x0, x1) in rects:
                    k = (x0, x1, z0, z1)
                    p = prisms.get(k)
                    if p is not None and p[1] == y - 1:
                        p[1] = y
                    else:
                        if p is not None:
                            _flush(sid_, k, p, boxes, pts, small_)
                        prisms[k] = [y, y]
                    seen.add(k)
                # close prisms that were not extended this layer
                for k in [k for k, p in prisms.items() if p[1] < y]:
                    _flush(sid_, k, prisms.pop(k), boxes, pts, small_)
            for k, p in prisms.items():
                _flush(sid_, k, p, boxes, pts, small_)
        return boxes, pts

    def _flush(sid_, k, p, boxes, pts, small_):
        x0, x1, z0, z1 = k
        y0, y1 = p
        n = (x1 - x0 + 1) * (z1 - z0 + 1) * (y1 - y0 + 1)
        if n >= small_:
            boxes.append((sid_, x0, y0, z0, x1, y1, z1))
        else:
            lst = pts.setdefault(sid_, [])
            for y in range(y0, y1 + 1):
                for z in range(z0, z1 + 1):
                    for x in range(x0, x1 + 1):
                        lst.append([x, y, z])

    boxes, pts = run(small)
    while len(boxes) + len(pts) > max_ops and small < 60:
        small *= 2
        boxes, pts = run(small)
    if verbose:
        print("boxes:", len(boxes), " point-ops:", len(pts), " points:", sum(len(v) for v in pts.values()), " small<", small)

    ops = []
    spec_rot = lambda sid_: world.specs[sid_][1]
    for (sid_, x0, y0, z0, x1, y1, z1) in boxes:
        m, rot = world.specs[sid_]
        op = {"op": "box", "from": [x0, y0, z0], "to": [x1, y1, z1], "block": pal_key[m]}
        if rot is not None:
            op["rot"] = rot
        ops.append(op)
    for sid_, lst in pts.items():
        m, rot = world.specs[sid_]
        # split long lists so a line of the file stays a sane size
        for i in range(0, len(lst), 4000):
            op = {"op": "set", "points": lst[i:i + 4000], "block": pal_key[m]}
            if rot is not None:
                op["rot"] = rot
            ops.append(op)
    script = {"name": name, "palette": palette, "ops": ops}
    if out_path:
        with open(out_path, "w", encoding="utf8", newline="\n") as f:
            f.write('{"name":%s,\n"palette":{\n' % json.dumps(name))
            f.write(",\n".join('%s:%s' % (json.dumps(k), json.dumps(v, separators=(",", ":"))) for k, v in palette.items()))
            f.write('},\n"ops":[\n')
            f.write(",\n".join(json.dumps(o, separators=(",", ":")) for o in ops))
            f.write("\n]}\n")
        if verbose:
            print("wrote", out_path, "%.1f MB" % (os.path.getsize(out_path) / 1e6), " ops:", len(ops))
    return script


def load_known_names(path):
    names = set()
    with open(path, encoding="utf8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("*"):
                line = line[1:]
            i = line.find("_State_Definitions_")
            if i >= 0:
                line = line[:i]
            names.add(line)
    return names
