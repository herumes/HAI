import json, math, random, sys

PART = "A"


class StampList(list):
    def append(self, d):
        if isinstance(d, dict) and "_p" not in d:
            d["_p"] = PART
        super().append(d)


ops = StampList()
pal = {}


def box(f, t, b, mode=None, **kw):
    d = {"op": "box", "from": list(f), "to": list(t), "block": b}
    if mode:
        d["mode"] = mode
    d.update(kw)
    ops.append(d)


def setb(at, b, **kw):
    d = {"op": "set", "at": [int(round(v)) for v in at], "block": b}
    d.update(kw)
    ops.append(d)


def pts(points, b, **kw):
    pl = []
    for p in points:
        t = [int(round(v)) for v in p]
        if t not in pl:
            pl.append(t)
    if not pl:
        return
    d = {"op": "set", "points": pl, "block": b}
    d.update(kw)
    ops.append(d)


def line(f, t, b, **kw):
    d = {"op": "line", "from": [int(round(v)) for v in f], "to": [int(round(v)) for v in t], "block": b}
    d.update(kw)
    ops.append(d)


def cyl(c, r, h, b, mode="fill"):
    ops.append({"op": "cylinder", "center": [int(v) for v in c], "radius": r, "height": h, "block": b, "mode": mode})


def sph(c, r, b, mode="fill", part=None):
    d = {"op": "sphere", "center": [round(v, 1) if v != int(v) else int(v) for v in c], "block": b, "mode": mode}
    if isinstance(r, (list, tuple)):
        d["radii"] = list(r)
    else:
        d["radius"] = r
    if part:
        d["part"] = part
    ops.append(d)


def stairs(f, dr, steps, width, b, **kw):
    d = {"op": "stairs", "from": list(f), "dir": dr, "steps": steps, "width": width, "block": b}
    d.update(kw)
    ops.append(d)


def scatter(f, t, b, where, chance, seed, only=None):
    d = {"op": "scatter", "from": list(f), "to": list(t), "block": b, "where": where, "chance": chance, "seed": seed}
    if only:
        d["only"] = only
    ops.append(d)


def bres(a, b):
    n = max(abs(b[0] - a[0]), abs(b[1] - a[1]), abs(b[2] - a[2]))
    out = []
    for i in range(n + 1):
        t = i / n if n else 0
        c = (round(a[0] + (b[0] - a[0]) * t), round(a[1] + (b[1] - a[1]) * t), round(a[2] + (b[2] - a[2]) * t))
        if c not in out:
            out.append(c)
    return out


def thick(cells, r):
    out = set()
    R = int(math.ceil(r))
    for (x, y, z) in cells:
        for dx in range(-R, R + 1):
            for dy in range(-R, R + 1):
                for dz in range(-R, R + 1):
                    if dx * dx + dy * dy + dz * dz <= r * r + 0.01:
                        out.add((x + dx, y + dy, z + dz))
    return sorted(out)


def write(path, name, part=None):
    pick = [o for o in ops if part is None or o.get("_p") in part]
    if part and len(part) > 1:
        pick = sorted(pick, key=lambda o: part.index(o["_p"]))
    sel = [dict((k, v) for k, v in o.items() if k != "_p") for o in pick]
    used = set()
    for o in sel:
        for key in ("block", "cap", "gable_fill"):
            v = o.get(key)
            for x in (v if isinstance(v, list) else [v]):
                used.add(x)
    pp = {k: v for k, v in pal.items() if k in used}
    nl = chr(10)
    q = chr(34)
    txt = "{" + nl + "  " + q + "name" + q + ": " + json.dumps(name) + "," + nl + "  " + q + "palette" + q + ": {" + nl
    txt += ("," + nl).join("    %s: %s" % (json.dumps(k), json.dumps(v)) for k, v in pp.items())
    txt += nl + "  }," + nl + "  " + q + "ops" + q + ": [" + nl
    txt += ("," + nl).join("    " + json.dumps(o, separators=(",", ":")) for o in sel)
    txt += nl + "  ]" + nl + "}" + nl
    json.loads(txt)
    with open(path, "w", newline=nl) as f:
        f.write(txt)
    print(part, len(sel), "ops", len(txt), "bytes")
