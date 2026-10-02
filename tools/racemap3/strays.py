"""Finds stray blocks: cells with no solid face-neighbour at all (floating single blocks).  Intentional ones (stars, fireflies, bubbles, light
trails, embers, plants) are listed separately by material so a human can judge them; everything else is reported with coordinates.

    python strays.py [--cached]
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import check3  # noqa: E402
from core import MAT, OX, OY, OZ  # noqa: E402

DECOR = ("rb3_star", "sb_star", "trail", "jg_glow", "jg_glow2", "dk_ember", "at_glow4", "lit_white", "lit_cyan", "lit_yellowlight", "co_flare", "kelp",
         "cbush", "cmodel", "starfish", "jg_fern", "jg_flower", "flowers", "jg_vine", "cd_sprinkle", "dk_smoke", "sb_disk", "sb_vein", "cd_marsh", "n_cyan",
         "n_white", "n_yellow", "n_magenta", "n_pink", "n_orange", "n_lime", "kp_glow", "aurora", "kp_pole", "k_flood", "k_cone", "k_flag", "k_billboard")


def main(argv):
    ctx = check3.load_ctx("--cached" in argv)
    W = ctx.W
    keys = np.fromiter(W.d.keys(), dtype=np.int64, count=len(W.d))
    vals = np.fromiter(W.d.values(), dtype=np.int32, count=len(W.d))
    ks = set(keys.tolist())
    step = [1, -1, 1 << 14, -(1 << 14), 1 << 28, -(1 << 28)]
    iso = [i for i, k in enumerate(keys.tolist()) if not any((k + d) in ks for d in step)]
    by = collections.Counter(W.specs[vals[i]][0] for i in iso)
    print("%d floating single blocks of %d" % (len(iso), len(keys)))
    bad = collections.defaultdict(list)
    for i in iso:
        m = W.specs[vals[i]][0]
        if not m.startswith(DECOR):
            k = int(keys[i])
            bad[m].append(((k & 0x3FFF) - OX, (k >> 28) - OY, ((k >> 14) & 0x3FFF) - OZ))
    print("intentional-looking:", dict(sorted(((m, n) for m, n in by.items() if m.startswith(DECOR)), key=lambda kv: -kv[1])[:12]))
    print("other materials:")
    for m, lst in sorted(bad.items(), key=lambda kv: -len(kv[1]))[:25]:
        print("   %-22s %5d   e.g. %s" % (m, len(lst), lst[:3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
