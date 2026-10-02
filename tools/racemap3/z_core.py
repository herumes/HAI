"""Zone 12: the Cosmic Compass - a colossal armillary sculpture floating over the empty heart of the map, visible from every zone:
a glowing sun core, three tilted neon rings with nodes and little orbiting moons."""
import math

import numpy as np

from common3 import *  # noqa: F401,F403

mat("co_sun", "Build_Lightsource_YellowLight")
mat("co_sun2", ["Build_Lightsource_Orange", "Build_Lightsource_Yellow", "Build_Lightsource_YellowLight"])
mat("co_flare", "Build_Lightsource_Orange")
mat("co_a", "NoCube_Neon_Block_Cyan")
mat("co_b", "NoCube_Neon_Block_Magenta")
mat("co_c", "NoCube_Neon_Block_Yellow")
mat("co_d", "NoCube_Neon_Block_White")
MOONS = ["clay_cyan", "clay_pink", "clay_orange", "clay_green", "clay_purple", "clay_yellow", "clay_blue", "clay_red"]
CENTER = (450, 150, 950)


def build(ctx):
    W, rng = ctx.W, ctx.rng
    cx, cy, cz = CENTER
    # the sun
    W.ellipsoid(cx, cy, cz, 36, 36, 36, "co_sun", mode="shell")
    W.ellipsoid(cx, cy, cz, 29, 29, 29, "co_sun2", mode="shell")
    for k in range(60):
        v = rng.normal(size=3)
        v /= np.linalg.norm(v)
        for t in range(36, int(rng.integers(50, 66))):
            p = np.array(CENTER) + v * t
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "co_flare")
    W.tick("sun")
    rings = [(190, (0.0, 1.0, 0.0), "co_a", 6), (225, (0.42, 1.0, 0.0), "co_b", 8), (262, (-0.3, 1.0, 0.45), "co_c", 10)]
    for (R, nrm, m_, nodes) in rings:
        torus(W, CENTER, nrm, R, 1.5, m_, step=0.5)
        n = np.array(nrm, float)
        n /= np.linalg.norm(n)
        up = np.array([0, 1.0, 0]) if abs(n[1]) < 0.95 else np.array([1.0, 0, 0])
        a = np.cross(n, up)
        a /= np.linalg.norm(a)
        b = np.cross(n, a)
        for k in range(nodes):
            th = k * 2 * math.pi / nodes + R * 0.01
            p = np.array(CENTER) + R * (math.cos(th) * a + math.sin(th) * b)
            W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), 5, 5, 5, "co_d", mode="shell")
            W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), 3, 3, 3, "lit_yellowlight")
        for k in range(10):
            th = rng.uniform(0, 2 * math.pi)
            p = np.array(CENTER) + R * (math.cos(th) * a + math.sin(th) * b)
            r = int(rng.integers(4, 8))
            W.ellipsoid(int(p[0]), int(p[1]), int(p[2]), r, r, r, MOONS[int(rng.integers(0, 8))], mode="shell")
    W.tick("rings")
    # four struts tying the outer ring to a hub ring so the sculpture reads as one machine
    for k in range(4):
        th = k * math.pi / 2 + math.pi / 4
        for t in np.arange(40, 188, 0.8):
            p = np.array(CENTER) + t * np.array([math.cos(th), 0.0, math.sin(th)])
            W.put(int(round(p[0])), int(round(p[1])), int(round(p[2])), "co_d" if int(t) % 8 else "co_a", keep=True)
    # a ground-level beacon column under the sun (a tall pole with rings) so it is findable from the track
    for y in range(-1, cy - 38):
        W.put(cx, y, cz, "co_d")
        W.put(cx + 1, y, cz, "co_d")
    for y in range(10, cy - 38, 25):
        hoop(W, (cx, y, cz), (0, 1, 0), 9, "co_a" if (y // 25) % 2 else "co_b", thick=1)
    W.cyl(cx, cz, -1, 3, 14, "b_greydark")
    W.cyl(cx, cz, 4, 4, 14, "co_a", mode="wall")
