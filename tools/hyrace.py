"""The race server's custom blocks, laid over the game's own assets.

BlockTextures.zip holds the server mod's models (Blocks/<Name>/<Name>.blockymodel) and pictures
(BlockTextures/*.png) but no block definitions, so the definitions come from tools/race_blocks.json: one
entry per block saying which model and picture it uses (or which vanilla shape it borrows). Each entry
becomes a virtual item that hyassets.Assets serves like one from Assets.zip, so the rest of the pipeline
(block_geom, the pack builder, the calibration renderer) needs no special case.

    catalog entry kinds
      model   own .blockymodel + one picture            {"model": "Kart_Cone", "texture": "Kart_Cone"}
      stairs  vanilla stair shapes with a new picture   {"texture": "Kart_Asphalt_Stairs"}
      cube    six plain sides                           {"all": "kart_grass"} or {"up": .., "sides": ..}
      like    a vanilla block with new pictures         {"parent": "Rock_Stone_Brick_Roof_Shallow", "texture": ..}
"""
import io
import json
import os
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG = os.path.join(HERE, "race_blocks.json")
PACK_DIR = os.path.join(HERE, "race_blocks")

STAIRS_PARENT = "Rock_Stone_Stairs"      # its BlockType already carries every stair state


def find_zip(name="BlockTextures.zip"):
    """tools/race_blocks/<zip> first, then the Downloads folder."""
    cands = [os.path.join(PACK_DIR, name)]
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    cands.append(os.path.join(home, "Downloads", name))
    for c in cands:
        if os.path.exists(c):
            return c
    return None


MODS_DIR = os.path.join(HERE, "server_mods")


def find_mod_zips():
    """Server mods that ship their own item definitions (Server/Item/Items/*.json): tools/server_mods/*.zip,
    plus NoCube_Neon_Blocks_*.zip left in Downloads."""
    import glob
    found = {}
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    for pat in (os.path.join(MODS_DIR, "*.zip"), os.path.join(home, "Downloads", "NoCube_Neon_Blocks_*.zip")):
        for z in sorted(glob.glob(pat)):
            found.setdefault(os.path.basename(z).lower(), z)       # a copy in tools/server_mods wins
    out = []
    for z in found.values():
        try:
            with zipfile.ZipFile(z) as zf:
                if any(n.startswith("Server/Item/Items/") and n.endswith(".json") for n in zf.namelist()):
                    out.append(z)
        except zipfile.BadZipFile:
            pass
    return out


def read_mod_zip(path):
    """(files, items, manifest) of a mod with real definitions. Items are keyed by file name, as in Assets.zip."""
    files, items, manifest = {}, {}, {}
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            if n.endswith("/"):
                continue
            if n.startswith("Common/"):
                files[n] = z.read(n)
            elif n.startswith("Server/Item/Items/") and n.endswith(".json"):
                items[n.rsplit("/", 1)[-1][:-5]] = json.loads(z.read(n).decode("utf-8"))
            elif n == "manifest.json":
                manifest = json.loads(z.read(n).decode("utf-8"))
    return files, items, manifest


def load_catalog(path=CATALOG):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_zip(path):
    """{'Common/BlockTextures/x.png': bytes, 'Common/Blocks/X/X.blockymodel': bytes}"""
    files = {}
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            if n.endswith("/"):
                continue
            files["Common/" + n] = z.read(n)
    return files


def _tex(name):
    return "BlockTextures/%s.png" % name


def find_model(files, name):
    """'Kart_Cone' -> 'Blocks/Kart_Cone/Kart_Cone.blockymodel' (the folder need not match: kart_roof_shallow_corner_left)."""
    want = "/" + name + ".blockymodel"
    for p in files:
        if p.startswith("Common/Blocks/") and p.endswith(want) and p.count("/") == 3:
            return p[len("Common/"):]
    raise KeyError("no model called %s in the pack" % name)


def _baked(name, face):
    return "BlockTextures/_baked/%s_%s.png" % (name, face)


def item_for(entry, files, shapes):
    """(virtual item JSON, extra baked picture files) for one catalog entry."""
    kind = entry.get("kind", "model")
    extra = {}
    if kind in ("model", "shape"):
        texs = [entry["texture"]] if isinstance(entry["texture"], str) else entry["texture"]
        if kind == "shape":
            model = shapes[entry["shape"]]
        else:
            model = find_model(files, entry["model"])
        if kind == "model" and entry.get("bake", True) and not entry.get("states") and is_unit_cube(files, model):
            # A model that is exactly one plain block is better as a real cube: neighbours hide its faces.
            faces = bake_cube(files, model, "Common/" + _tex(texs[0]))
            tex = {"Weight": 1}
            for f, png in faces.items():
                key = _baked(entry["name"], f)
                extra["Common/" + key] = png
                tex[f] = key
            return {"BlockType": {"DrawType": "Cube", "Material": "Solid", "Textures": [tex]}}, extra
        bt = {
            "DrawType": "Model",
            "Material": "Solid",
            "Opacity": "Transparent",
            "CustomModel": model,
            "CustomModelTexture": [{"Texture": _tex(t), "Weight": 1} for t in texs],
        }
        if entry.get("scale"):
            bt["CustomModelScale"] = entry["scale"]
        if entry.get("states"):
            bt["State"] = {"Definitions": {
                st: {"CustomModel": find_model(files, m)} for st, m in entry["states"].items()}}
        return {"BlockType": bt}, extra
    if kind == "stairs":
        return {"Parent": STAIRS_PARENT, "BlockType": {
            "CustomModelTexture": [{"Texture": _tex(entry["texture"]), "Weight": 1}]}}, extra
    if kind == "like":
        return {"Parent": entry["parent"], "BlockType": {
            "CustomModelTexture": [{"Texture": _tex(entry["texture"]), "Weight": 1}]}}, extra
    if kind == "cube":
        tex = {}
        if "all" in entry:
            tex["All"] = _tex(entry["all"])
        if "sides" in entry:
            tex["Sides"] = _tex(entry["sides"])
        if "up" in entry:
            tex["Up"] = _tex(entry["up"])
        if "down" in entry:
            tex["Down"] = _tex(entry["down"])
        tex["Weight"] = 1
        return {"BlockType": {"DrawType": "Cube", "Material": "Solid", "Textures": [tex]}}, extra
    raise ValueError("unknown kind %r in %r" % (kind, entry.get("name")))


def is_unit_cube(files, model_path):
    """True when a model is exactly one full block (six faces on the boundary), so it may hide its neighbours' faces."""
    import hymodel
    try:
        m = json.loads(files["Common/" + model_path].decode("utf-8"))
    except KeyError:
        return False
    qs = hymodel.model_quads(m)
    if len(qs) != 6:
        return False
    pts = np.array([p for q in qs for p in q.pts])
    return bool(np.allclose(pts.min(0), 0, atol=0.01) and np.allclose(pts.max(0), 1, atol=0.01))


def bake_cube(files, model_path, tex_path):
    """Cuts the six pictures of a one-block model out of its texture: {'Up': png bytes, ...}.

    Each cube face is sampled through the corners the model gives it, so mirrored and turned pictures,
    and pictures that wrap past the texture's edge, come out exactly as the model draws them.
    """
    from PIL import Image
    import hymodel
    from hyblock import CUBE_FACES
    tex = np.asarray(Image.open(io.BytesIO(files[tex_path])).convert("RGBA"))
    th, tw = tex.shape[:2]
    quads = hymodel.model_quads(json.loads(files["Common/" + model_path].decode("utf-8")))
    out = {}
    n = 32
    for face, (normal, corners) in CUBE_FACES.items():
        q = next(q for q in quads if tuple(int(round(v)) for v in q.normal) == normal)
        uv = []
        for c in corners:                              # TL TR BR BL seen from outside
            k = min(range(4), key=lambda i: sum((q.pts[i][a] - c[a]) ** 2 for a in range(3)))
            uv.append(q.uv[k])
        tl, tr, br, bl = (np.array(u, float) for u in uv)
        img = np.zeros((n, n, 4), np.uint8)
        for y in range(n):
            for x in range(n):
                fx, fy = (x + 0.5) / n, (y + 0.5) / n
                p = tl + (tr - tl) * fx + (bl - tl) * fy
                img[y, x] = tex[int(np.floor(p[1])) % th, int(np.floor(p[0])) % tw]
        buf = io.BytesIO()
        Image.fromarray(img, "RGBA").save(buf, format="PNG")
        out[face] = buf.getvalue()
    return out


def overlay(zip_path=None, catalog_path=CATALOG, mods=True, race=True):
    """(files, items, catalog, mods) ready for Assets.add_overlay, or None when nothing is around.

    `catalog` is the race catalog (None without BlockTextures.zip); `mods` lists
    {"zip", "name", "items"} for every server mod with its own definitions."""
    zip_path = (zip_path or find_zip()) if race else None
    files, items, cat = {}, {}, None
    if zip_path:
        cat = load_catalog(catalog_path)
        files = read_zip(zip_path)
        shapes = cat.get("shapes", {})
        for e in cat["blocks"]:
            item, extra = item_for(e, files, shapes)
            files.update(extra)
            items[e["name"]] = item
    mod_info = []
    if mods:
        for z in find_mod_zips():
            f, it, man = read_mod_zip(z)
            files.update(f)
            # only things that are blocks; an item with no BlockType is not placeable
            blocks = {k: v for k, v in it.items() if "BlockType" in v}
            items.update(blocks)
            mod_info.append({"zip": z, "name": man.get("Name") or os.path.basename(z), "items": sorted(blocks),
                             "description": man.get("Description", "")})
    if not files and not items:
        return None
    return files, items, cat, mod_info
