"""python shots.py  -> iso views of the main landmarks into out/ (builds the whole world first)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from viewer import run

run([], [
    ("shot_start", (-110, 60, -45, 60), 25, 18, 4.0),
    ("shot_meadows", (90, 420, -150, 110), 20, 40, 2.4),
    ("shot_castle", (340, 450, -10, 110), 200, 24, 6.0),
    ("shot_canyon", (520, 780, 60, 500), 200, 38, 2.6),
    ("shot_crystal", (480, 730, 430, 760), 160, 38, 2.6),
    ("shot_abyss", (-20, 560, 600, 790), 10, 40, 2.4),
    ("shot_frost", (-400, 60, 400, 780), 20, 36, 2.2),
    ("shot_sky", (-60, 630, 190, 700), 25, 28, 1.5),
    ("shot_volcano", (-690, -70, 145, 560), 25, 34, 1.7),
    ("shot_rainbow", (-740, -150, -190, 340), 25, 28, 1.7),
])
