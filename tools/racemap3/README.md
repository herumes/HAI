# racemap3 - generator for "Universe Grand Prix - Cosmic Odyssey v3"

A second, completely different race world, compiled by code into ONE build script (the format the Build tab's "Make a build
with an AI" section reads). It reuses the proven parts of `../racemap` (voxel world + emitter, road stamping with the server's
Kart_* blocks, props, terrain skins, lettering) and adds a new layout, new zones and new checks. Nothing here needs the game.

    python build3.py                  all zones -> out/universe_grand_prix_cosmic_odyssey_v3.json   (~2.5 min)
    python build3.py candy --ticks    one zone, with a block count per step (no JSON)
    python check3.py                  build everything + verify (roads, joins, flights, sub lane, water), caches out/world.pkl
    python check3.py --cached --emit  re-run the checks on the cached world and write the JSON from that verified world
    python info3.py [water_volume]    out/map_info3.md: zone lengths, lap estimate, trigger coordinates, water
    python view3.py cached iso x0 x1 z0 z1 [az el scale name y0 y1]   oblique PNG into out/ (also: top, sec)
    python view3.py candy iso ...     same but builds the named zone(s) first
    python textcheck.py [--cached] [--bad]   audit EVERY sign: missing glyph blocks, blocks in front of it, text wider than its board,
                                     diagonal signs; writes out/text_report.txt and contact sheets out/text_sheet_N.png (what a reader sees)
    python strays.py [--cached]       floating single blocks that are not intentional decoration
    python floodtest3.py [--analyse]  flood fill of the Atlantis channel (volume + leaks)
    python layout3.py                 the five centre lines, lengths, min radii and out/layout.png (schematic)

Expand the result the way the game does (raise the limit - the map is 3.9M blocks, the default is 600,000):

    bs_cli.exe out/universe_grand_prix_cosmic_odyssey_v3.json --max 6000000 --materials      (~10 s, ~1.3 GB peak)

## Layout (layout3.py)

A clockwise ring about 3500 x 2650 blocks (x -1540..1950, z -730..1920, y up to 291; the finish line is at 0, -1, 0).
Five fillet polylines (a full circle is a regular polygon with the fillet radius = the circle radius):

| path | what | mode |
|---|---|---|
| A  | Nova Station -> Candy Comet Hills -> Neon Metropolis -> Skyport Tower spiral (+100 blocks) | kart |
| SA | Stratos Cloudlands flight (whale tunnel, thunderstorm, rainbow arches, cloud palace, armada) | plane |
| B  | Golden Dunes (runway, pyramid hall) -> Lagoon dive -> Atlantis (sub) -> surface -> Ancient Jungle -> Dread Keep -> Aurora Glacier | kart + sub |
| SB | Cosmic Gateway flight (ring station, moon, asteroid belt, gas giant, black hole, wormhole) | plane |
| C  | Celestial Ribbon (landing strip, floating road in space) -> Star Slide onto the start straight | kart |

Heights are set in `build3.set_heights` (linear keys + moving average, ends preserved). Rises are at most 1 block per 6 blocks
of road; flight altitudes are `layout3.set_altitude` keys. `common3.find_straight` picks the straight for the pyramid.

## Files

* common3.py   shared imports, `stamp` (registers the head-room corridor), `Land` (terrain helper), `gate` (moves arches to an
               axis-aligned stretch, adds legs when the road is raised), flight helpers (`Clear`, `frame3`, `hoop3`), `torus`, `put_text`.
* build3.py    context, height profiles, `restore_roads`, `clear_corridor` (3 free blocks over every kart road, only Kart decals and
               neon on layer road+1), zone loop, emit.  Speeds used for the lap estimate: `SPEED`.
* z_*.py       one file per zone (station candy city tower skya dunes atlantis jungle keep glacier skyb ribbon core).
* check3.py    the verification suite; floodtest3.py; info3.py; view3.py; layout3.py.

## What the checks guarantee (and what they do not)

Verified offline on the finished world: no holes in any kart road (every cell with |u| <= 6.4 holds a road block), 3 free blocks
above every road, step spacing >= 6 blocks and 1-block steps only, the segment joins line up in position and heading, both flight
lines are clear within 6 blocks (except the pad and the runway threshold themselves), the sub lane is clear within 4 blocks, the
Atlantis channel is watertight by flood fill (about 270k water blocks), height cap 291, and the real C++ expander accepts the script
with exactly the same block count as the generator's world.

NOT verified (never run in the game): whether the kart climbs 1-block stair steps, the facing of every directional Kart piece
(rotations follow the server's table in the prompt), whether the mod's transform triggers fit the gate spots, the speeds behind the
lap estimate (kart 45 / plane 75 / sub 38 blocks per second are guesses), and the water fill itself.

## Lessons recorded while building it

* A road that overlaps itself in plan (spiral, overpass) must be stamped in separate calls (`stamp(..., chunk=)` or separate ranges):
  the raster keeps one sample per cell.  The Skyport spiral has 37 blocks between turns so only the launch pad crosses it.
* Flood-test solidity: neon / glow blocks are SOLID (only Plant_ and Deco_ let water through) - treating them as air made a fake leak.
* Never use falling-block-prone blocks for skins (no Soil_Sand / gravel tops): the Dunes use sandstone and clay.
* The seabed road is still stamped and kept clear (hoops have their bottom arc cut with `ymin`), so nothing in the lane blocks anyone.
* Smooth height profiles with odd reflection padding, otherwise the ends move by one block and the joins stop matching.
* Wide terrain boxes: size the Land box from the mountain, not just the road (the Glacier was cut off at the box edge once).

## Signs (lettering) - rules the audit enforces

* Every sign is registered when drawn (`common3.lsign / put_text / put_bitmap / ftext`); `textcheck.py` fails on missing glyph blocks,
  blocks within 24 blocks in front, text wider than the plate behind it, and signs on a diagonal.
* `common3.gate` moves an arch to a stretch where the road runs along an axis (`axis_s`), widens the board (or shrinks the scale)
  so the text always fits, and gives raised arches legs.  Anything else that writes lettering across the road uses `axis_s` too.
* Reserve the space in front of a sign BEFORE placing trees / towers / planets (see `rects` in z_city, `placed` in z_jungle / z_ribbon).
* `kit.lsign` rounded `u0 + c` half-to-even, which doubled/dropped columns of odd-width text ("gibberish"); `common3.lsign` fixes it.
* Item boxes (floating Mario-Kart cubes over the road) are decoration only and are OFF (`common3.ITEM_BOXES`).
