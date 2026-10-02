"""Road styles shared by the zones."""
from core import mat
from path import RoadStyle

for _c in ["Black", "Blue", "Cyan", "Gray", "Green", "Orange", "Pink", "Purple", "Red", "White", "Yellow"]:
    mat("wstair_" + _c.lower(), "Cloth_Block_Wool_%s_Stairs" % _c)
    mat("whalf_" + _c.lower(), "Cloth_Block_Wool_%s_Half" % _c)


def road_style(hw=7.0, surface="asphalt_mix", stairs="asphalt_stair", line="line_white", dashed=True, edge_line=True,
               curb=("curb_red", "curb_white"), curb_period=2.0, wall=("clay_white", "clay_red"), wall_period=6.0, wall_h=2,
               deck=None, piers=None, top_fn=None, curb_w=1.0, wall_w=1.0, no_wall=False, wall_top=None):
    def surf(s, u):
        a = abs(u)
        if edge_line and hw - 1.2 < a < hw - 0.2:
            return line
        if dashed and a < 0.6 and int(s // 5) % 2 == 0:
            return line
        return surface

    def curb_fn(s, u):
        return curb[int(s // curb_period) % len(curb)]

    def wall_fn(s, u, h):
        w = wall[int(s // wall_period) % len(wall)]
        if h == wall_h - 1 and wall_top:
            return wall_top
        return w

    return RoadStyle(surf, stairs, curb=curb_fn, wall=None if no_wall else wall_fn, hw=hw, curb_w=curb_w,
                     wall_w=wall_w, wall_h=wall_h, deck=deck, piers=piers, top_fn=top_fn)
