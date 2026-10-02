# The race server's custom blocks

`BlockTextures.zip` (the server mod's models and pictures) is not committed; put your copy in this folder
(or leave it in Downloads - `tools\hyrace.py` looks in both). It holds 37 `.blockymodel` files and 55 pictures but
**no block definitions**, so nothing in it says what the blocks are called or which picture goes on which model.
`tools\race_blocks.json` is that missing definition list, worked out like this:

* The game's own cache (`%APPDATA%\Hytale\UserData\CachedAssets`) proves the zip is what the server sends, but the
  block definitions themselves only travel as packets, so they are not on disk.
* Names are **guesses** from the model folder (`Kart_Asphalt`) or, where there is no model, the picture file
  (`Kart_Start_Grid_3`). Each entry says which in `conf` (`model` is the stronger guess).
* Model -> picture pairs were settled by rendering every model with every candidate picture and keeping the one that
  is drawn correctly (for example `Kart_Curb_Red` uses `Kart_Curb_Red_Rounded.png` and `Kart_Curb_Red_Block` the plain
  one; the two signs differ only in the chevron direction). Pictures without a model are plain cubes, vanilla stair
  shapes (`Kart_Asphalt_Stairs`, `Kart_Concrete_Stairs`), the vanilla quarter-block (`Kart_Asphalt_Quarter`), or flat
  decals drawn on the `kart_paint_quad` model (a single square 1/32 above the floor).
* Models that are exactly one plain block (asphalt, concrete, curb blocks, the chequer block...) are cut out of their
  texture into six 32x32 faces so they behave like real cubes in the preview (neighbours hide each other's faces).
* `workbench.png` is byte-identical to `work.png`, `sign_left.png` an older `Rotatesign_left.png`, `wheel2.png` a vehicle
  picture: none is used.

## If a name is wrong

Edit the `name` in `tools\race_blocks.json`, or make `tools\race_names.txt` (one real name per line - copy them from the
game's inventory / creative list, or from a `block_ids.txt` taken on the server) and run:

    python tools\make_ai_prompt.py        # prompts + the name list the Build tab checks against
    python tools\build_preview_pack.py --install   # 3D preview pack

A name in `race_names.txt` (or in `block_ids.txt`) that matches a catalog entry ignoring case and underscores replaces
the guess everywhere, and the generator reports how many names are confirmed and which are still guesses.

## What the models say about their placement

`rot` 0 is how the model file lies; `rot` N turns it N quarter turns anticlockwise seen from above (north -> west ->
south -> east). Rails, curbs and mesh panels run east-west at `rot` 0. Flat decal pictures are laid on the floor with
north at the top of the picture, so `Kart_Painted_Line` runs north-south and `Kart_Arrow_Straight` points north at `rot` 0.
Not verified in the game: whether the blocks are placed one layer above the road (what the decals and curbs
assume) and the facing of each model - both come from the model files only.
