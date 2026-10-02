# Server mods with their own item definitions

Drop a server mod's zip here (or leave `NoCube_Neon_Blocks_*.zip` in Downloads) and it is picked up by
`python tools\build_preview_pack.py --install` (3D preview) and `python tools\make_ai_prompt.py` (AI prompts and the
block-name list). Unlike the race mod (`tools\race_blocks`), these zips contain `Server/Item/Items/*.json`, so the block
names are exact and nothing is guessed.

Supported so far: **NoCube's Neon Blocks** - 17 solid colours, draw type `GizmoCube` (an unlit cube: the preview draws it
at full brightness with only a hint of face shading so the shape stays readable).
