# RACE TRACKS (THIS SERVER'S CUSTOM BLOCKS)

I play on a server with a kart-racing mod and its own blocks. Everything above still applies; when I ask for a race track, circuit or kart course, build it with the blocks below (names exact) and lay the road with the `road` op, never with boxes of asphalt. A track is flat racing surface first, scenery second.

## Layout rules
- **Surface:** the road is full `Kart_Asphalt` at `y=-1`, flush with the ground (it replaces the top ground layer, so a kart drives on from the grass). Everything that decorates it - curbs, lines, markings, barriers - sits on layer `y=0`; the `road` op already does that. Scenery also goes at `y=0`.
- **Sizes** (blocks): circuit road 9-13 wide, pit lane and narrow sections 5-7; corner radius at least the road width, hairpins at least 7; straights 25+; no sharp right angles, give every corner a `radius`. A lap of 150-400 blocks is a good circuit; include a long start straight, 4-8 corners of different radius, at least one S-bend and one slow corner. Elevation: change `y` gently (at most 1 block of rise per 6 blocks of road); a bridge or hill is just waypoints with different `y`, add `"under":"Rock_Stone","depth":4` so it is not floating. Keep 4 blocks of clear air above the road.
- **Markings are flat tiles 1/32 block thick** that lie ON the road surface, i.e. in layer `y=0` over the asphalt, each in its own cell: set them after the `road` op. Their picture already includes the asphalt behind it.
- **Rotation of the custom blocks:** `rot` 0 is how the model file lies; `rot` N turns it N quarter turns anticlockwise seen from above (north -> west -> south -> east). The list says how each directional block sits at `rot` 0; blocks not described are symmetric. The `road` op turns rails, curbs and centre lines to follow the road by itself.
- If a named block does not exist I will tell you; do not invent names. Everything in the list is placeable one block per cell like any other block.

## `road` op (use it for every stretch of road, pit lane, path, bridge)
`{"op":"road","points":[[x,y,z],...],"width":9,"block":"Kart_Asphalt", ...}` - a road of `width` cells along a line through the waypoints; `y` of each waypoint sets the height there (the height changes smoothly between them). Extra fields, all optional:
- `closed:true` joins the last waypoint back to the first (a lap). `curve`: `round` (default: straight between waypoints, every corner rounded by `radius`, default = width), `spline` (a smooth curve through every waypoint, for organic bends) or `sharp`.
- `edge` (block or list of blocks, cycled every `stripe` cells, default 3), `edge_width` (default 1): kerbs on the outermost road cells, put on top of the road (`edge_mode:"same"` replaces the road cell instead).
- `line` (block), `line_width` (1), `dash:[on,off]`: a centre line on top of the road.
- `barrier` (block or list), `barrier_gap` (cells left free between road and barrier, default 0), `barrier_sides` (`both`, `left`, `right`, relative to the direction the waypoints run): a one-cell ring beside the road, standing on layer `y+1`. `verge` (block) fills the gap and the barrier cell at road level first (needed beside raised roads, so the barrier does not float).
- `under` (block) and `depth` (default 1): support blocks below the road. `auto_rot:false` leaves rails and curbs unturned (for cones, tyre walls). Put the centre line on a half-block coordinate (`[x,y,z+0.5]`) to get an even width.
- Cells of later ops overwrite this one, so lay roads first, then the start line, grid and scenery.
Two roads that cross or join simply share their cells. A pit lane is a second, narrower road that leaves the main straight and rejoins it further on.

## Typical circuit checklist
1. One closed `road` for the lap, with `edge` curbs (`["Kart_Curb_Red","Kart_Curb_Yellow"]`), a dashed `line`, and `barrier` on both sides (`Kart_Armco` or `Kart_Barrier_Metal`, with `barrier_gap` 1-3 of run-off), tyre walls (`Kart_Tire_Wall` with `auto_rot:false`) on the outside of the fast corners.
2. A start/finish line: a 2-cell `box` of `Kart_Finish_Checker` across the whole road at `y=0` (or `Finish_Line` blocks as a gantry base), and a start grid behind it: two columns, one slot every 3 cells, numbered with `Kart_Start_Grid` (1) and `Kart_Start_Grid_N`.
3. A pit lane with `Kart_Pit_Box` tiles and, behind it, `Red_Workwall` garages (3 wide each, picture faces north at rot 0 - use `rot` to face the lane).
4. Spectator side: `Kart_Catch_Fence`, `Kart_Billboard` and `Flag` along the start straight, `Kart_Floodlight` posts, `Kart_Podium_*` and `Kart_Trophy` near the finish. The neon blocks (listed above) make bright start gantries, tunnel and track-edge lights.
5. Fill the infield with terrain blocks (grass, trees) only if I ask; otherwise leave it.

## The custom blocks
{{RACE_CATALOG}}

# WORKED RACE EXAMPLE (valid, ~3,800 blocks; a closed circuit with pit lane, grid and podium)

```json
{{RACE_EXAMPLE}}
```
