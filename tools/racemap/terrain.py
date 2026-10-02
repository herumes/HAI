"""racemap terrain: heightfields around the road, filled into the world as a thin skin with skirts."""
import math

import numpy as np

from core import fbm, smoothstep
from path import raster_band


class Grid:
    def __init__(self, x0, x1, z0, z1):
        self.x0, self.x1, self.z0, self.z1 = int(x0), int(x1), int(z0), int(z1)
        self.nx, self.nz = self.x1 - self.x0 + 1, self.z1 - self.z0 + 1
        self.X, self.Z = np.meshgrid(np.arange(self.x0, self.x1 + 1), np.arange(self.z0, self.z1 + 1), indexing="ij")

    def inside(self, cx, cz):
        return (cx >= self.x0) & (cx <= self.x1) & (cz >= self.z0) & (cz <= self.z1)

    def idx(self, cx, cz):
        return cx - self.x0, cz - self.z0


_OFFS = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, 1.4142), (1, -1, 1.4142), (-1, 1, 1.4142), (-1, -1, 1.4142)]


def _shift_slices(dx, dz, nx, nz):
    # destination slice and source slice so that dst[i] = src[i - d]
    def sl(d, n):
        if d >= 0:
            return slice(d, n), slice(0, n - d)
        return slice(0, n + d), slice(-d, n)
    (dxs, sxs), (dzs, szs) = sl(dx, nx), sl(dz, nz)
    return (dxs, dzs), (sxs, szs)


def centre_field(g, path, i0, i1, radius, hw=0.0):
    """Distance (in blocks) to the path centre line [i0,i1) up to `radius`, the nearest sample's road height
    and its path position s.  hw > 0 paints the road footprint as distance 0 (distance then measured from
    the road's edge)."""
    big = 1e9
    d = np.full((g.nx, g.nz), big)
    Tn = np.zeros((g.nx, g.nz))
    Sn = np.zeros((g.nx, g.nz))
    if hw > 0:
        cx, cz, si, uu = raster_band(path, i0, i1, -hw, hw)
    else:
        cx = np.floor(path.p[i0:i1, 0] + 0.5).astype(np.int64)
        cz = np.floor(path.p[i0:i1, 1] + 0.5).astype(np.int64)
        si = np.arange(i0, i1)
    ok = g.inside(cx, cz)
    cx, cz, si = cx[ok], cz[ok], si[ok]
    ix, iz = g.idx(cx, cz)
    d[ix, iz] = 0.0
    Tn[ix, iz] = path.T[si]
    Sn[ix, iz] = path.s[si]
    steps = int(radius) + 2
    for _ in range(steps):
        changed = False
        for dx, dz, w in _OFFS:
            dst, src = _shift_slices(dx, dz, g.nx, g.nz)
            cand = d[src] + w
            m = cand < d[dst]
            if m.any():
                changed = True
                d[dst] = np.where(m, cand, d[dst])
                Tn[dst] = np.where(m, Tn[src], Tn[dst])
                Sn[dst] = np.where(m, Sn[src], Sn[dst])
        if not changed:
            break
    d[d > radius] = big
    return d, Tn, Sn


def ridge_field(g, path, i0, i1, slope, hw=0.0, steps=None, extra_seed=None):
    """Max-plus propagation: each road cell is a peak of height T and the surface falls off at `slope` blocks
    per block in every direction (a mountain that hugs the road).  Returns the height field (float)."""
    if hw > 0:
        cx, cz, si, uu = raster_band(path, i0, i1, -hw, hw)
    else:
        cx = np.floor(path.p[i0:i1, 0] + 0.5).astype(np.int64)
        cz = np.floor(path.p[i0:i1, 1] + 0.5).astype(np.int64)
        si = np.arange(i0, i1)
    ok = g.inside(cx, cz)
    cx, cz, si = cx[ok], cz[ok], si[ok]
    H = np.full((g.nx, g.nz), -1e6)
    ix, iz = g.idx(cx, cz)
    np.maximum.at(H, (ix, iz), path.T[si].astype(float))
    if extra_seed is not None:
        H = np.maximum(H, extra_seed)
    tmax = float(max(1.0, np.nanmax(np.where(H > -1e5, H, -1e6))))
    n = steps or int(tmax / max(0.05, slope)) + 3
    for _ in range(n):
        changed = False
        for dx, dz, w in _OFFS:
            dst, src = _shift_slices(dx, dz, g.nx, g.nz)
            cand = H[src] - slope * w
            m = cand > H[dst]
            if m.any():
                changed = True
                H[dst] = np.where(m, cand, H[dst])
        if not changed:
            break
    return H


def carve(H, d, Tn, d0, d1, shoulder=-1, noise=None):
    """Blend the landform H toward the road level (Tn + shoulder) within d0 (full) .. d1 (none) of the road."""
    dd = d if noise is None else d + noise
    w = 1.0 - smoothstep(d0, d1, dd)
    return H * (1 - w) + (Tn + shoulder) * w


def skin(W, g, Ht, top_ids, side_ids, mats, mask=None, ground=-1, bottom_floor=0, diag=True):
    """Fill the heightfield.  Ht: int array (top block y per column, ground = -1 means 'surface at ground level').
    top_ids / side_ids: int arrays of indices into `mats` (list of material keys).  mask: bool array of columns to place.
    Columns at ground level get a single top block; taller columns get skirts down to their lowest 4-neighbour."""
    Ht = np.asarray(Ht).astype(np.int64)
    pad = np.pad(Ht, 1, mode="constant", constant_values=ground)
    nb = np.minimum(np.minimum(pad[2:, 1:-1], pad[:-2, 1:-1]), np.minimum(pad[1:-1, 2:], pad[1:-1, :-2]))
    if diag:
        nb = np.minimum(nb, np.minimum(np.minimum(pad[2:, 2:], pad[2:, :-2]), np.minimum(pad[:-2, 2:], pad[:-2, :-2])))
    bottom = np.minimum(Ht, np.maximum(bottom_floor, nb + 1))
    bottom = np.where(Ht <= ground, Ht, bottom)
    if mask is None:
        mask = Ht >= ground
    else:
        mask = mask & (Ht >= ground)
    ix, iz = np.nonzero(mask)
    if ix.size == 0:
        return 0
    x = g.x0 + ix
    z = g.z0 + iz
    top = Ht[ix, iz]
    bot = bottom[ix, iz]
    tid = top_ids[ix, iz]
    sid = side_ids[ix, iz] if not callable(side_ids) else None
    n = 0
    # top blocks
    for k in np.unique(tid):
        sel = tid == k
        W.put_arr(x[sel], top[sel], z[sel], mats[int(k)])
        n += int(sel.sum())
    # skirts: columns from bot..top-1
    hgt = top - bot
    maxh = int(hgt.max()) if hgt.size else 0
    if callable(side_ids):
        for dy in range(1, maxh + 1):
            sel = hgt >= dy
            if not sel.any():
                break
            yy = (top - dy)[sel]
            ids = side_ids(yy, x[sel], z[sel])
            for k in np.unique(ids):
                q = ids == k
                W.put_arr(x[sel][q], yy[q], z[sel][q], mats[int(k)])
                n += int(q.sum())
        return n
    for k in np.unique(sid):
        selk = sid == k
        for dy in range(1, maxh + 1):
            sel = selk & (hgt >= dy)
            if not sel.any():
                break
            W.put_arr(x[sel], (top - dy)[sel], z[sel], mats[int(k)])
            n += int(sel.sum())
    return n


def slope_of(H):
    gx, gz = np.gradient(H.astype(float))
    return np.hypot(gx, gz)


def pick(mask_list, default=0):
    """mask_list: [(bool array, index), ...] later entries win.  Returns an int index array."""
    out = np.full(mask_list[0][0].shape, default, dtype=np.int64)
    for m, k in mask_list:
        out = np.where(m, k, out)
    return out


def floating_skin(W, x0, z0, top, bot, top_mat, under_mat, side_mat, top_layers=2, sub_mat=None):
    """A floating slab given per-column top and bottom heights (int arrays, top = -9999 outside the shape).
    Top surface, underside and rim are all closed (skirts toward the neighbours)."""
    NONE_T, NONE_B = -9999, 9999
    inside = top > NONE_T
    top = np.where(inside, top, NONE_T)
    bot = np.where(inside, np.minimum(bot, top), NONE_B)
    pt = np.pad(top, 1, constant_values=NONE_T)
    pb = np.pad(bot, 1, constant_values=NONE_B)
    nbt = pt[1:-1, 1:-1].copy()
    nbb = pb[1:-1, 1:-1].copy()
    nbt_min = np.full(top.shape, 10 ** 6)
    nbb_max = np.full(top.shape, -10 ** 6)
    for dx in (-1, 0, 1):
        for dz in (-1, 0, 1):
            if dx == 0 and dz == 0:
                continue
            sx = slice(1 + dx, pt.shape[0] - 1 + dx)
            sz = slice(1 + dz, pt.shape[1] - 1 + dz)
            nbt_min = np.minimum(nbt_min, pt[sx, sz])
            nbb_max = np.maximum(nbb_max, pb[sx, sz])
    ix, iz = np.nonzero(inside)
    x = x0 + ix
    z = z0 + iz
    t = top[ix, iz]
    b = bot[ix, iz]
    lo_top = np.maximum(b, nbt_min[ix, iz] + 1)       # top skirt from here up to t
    hi_bot = np.minimum(t, nbb_max[ix, iz] - 1)       # underside skirt from b up to here
    lo_top = np.minimum(lo_top, t)
    hi_bot = np.maximum(hi_bot, b)
    # top surface + grass/dirt layers
    W.put_arr(x, t, z, top_mat)
    n = len(x)
    maxh = int((t - lo_top).max()) if n else 0
    for dy in range(1, maxh + 1):
        sel = (t - dy) >= lo_top
        if not sel.any():
            break
        W.put_arr(x[sel], (t - dy)[sel], z[sel], under_mat if dy < top_layers else side_mat)
    # underside + skirts upward
    maxh = int((hi_bot - b).max()) if n else 0
    for dy in range(0, maxh + 1):
        sel = (b + dy) <= hi_bot
        if not sel.any():
            break
        # do not overwrite the top layer
        sel &= (b + dy) < t
        W.put_arr(x[sel], (b + dy)[sel], z[sel], sub_mat or side_mat)
    return n
