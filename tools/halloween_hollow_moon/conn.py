import json, sys
from collections import deque

d = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "prefab_all.json"))
cells = {(b["x"], b["y"], b["z"]): b["name"] for b in d["blocks"]}
seen = set()
comps = []
N6 = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
for c in cells:
    if c in seen:
        continue
    q = deque([c])
    seen.add(c)
    comp = [c]
    while q:
        x, y, z = q.popleft()
        for dx, dy, dz in N6:
            n = (x + dx, y + dy, z + dz)
            if n in cells and n not in seen:
                seen.add(n)
                q.append(n)
                comp.append(n)
    comps.append(comp)
comps.sort(key=len, reverse=True)
print("components", len(comps), "main", len(comps[0]))
ground = 0
for comp in comps[1:]:
    low = min(c[1] for c in comp)
    names = {}
    for c in comp:
        names[cells[c]] = names.get(cells[c], 0) + 1
    print(len(comp), "cells, lowest y", low, "at", comp[0], sorted(names.items(), key=lambda kv: -kv[1])[:2])
