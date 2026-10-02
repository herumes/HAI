"""Reads block definitions out of Hytale's Assets.zip.

An item's JSON may name a "Parent" and only state what differs, so a block is the parent chain merged
(child wins, dictionaries merge, lists are replaced). A state variant such as
'*Wood_Softwood_Roof_State_Definitions_Corner_Left' is the base block with
BlockType.State.Definitions.Corner_Left laid over it.
"""
import json
import re
import zipfile

STATE_SEP = "_State_Definitions_"


def deep_merge(base, over):
    """Child over parent: dicts merge key by key, everything else is replaced."""
    out = dict(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def split_name(name):
    """'*Base_State_Definitions_X' -> ('Base', 'X'); a plain name -> (name, None)."""
    n = name[1:] if name.startswith("*") else name
    if STATE_SEP in n:
        base, state = n.split(STATE_SEP, 1)
        return base, state
    return n, None


class Assets:
    def __init__(self, zip_path):
        self.zip = zipfile.ZipFile(zip_path)
        self.names = set(self.zip.namelist())
        self.raw = {}
        for n in self.names:
            if n.startswith("Server/Item/Items/") and n.endswith(".json"):
                self.raw[n.rsplit("/", 1)[-1][:-5]] = n
        self._cache = {}
        self._parsed = {}
        self.extra_files = {}     # published path -> bytes, from add_overlay (a server's own assets)
        self.extra_items = {}     # item id -> item JSON (virtual: the server's definitions are not in any zip)

    def add_overlay(self, files, items):
        """Lays a server's custom assets over the game's: extra files, and block definitions written by hand."""
        self.extra_files.update(files)
        self.names.update(files)
        for k, v in items.items():
            self.extra_items[k] = v
            self.raw[k] = None
            self._cache.pop(k, None)

    def read(self, path):
        if path in self.extra_files:
            return self.extra_files[path]
        return self.zip.read(path)

    def has(self, path):
        return path in self.names

    def item(self, item_id):
        """The item JSON with its Parent chain merged in, or None."""
        if item_id in self._cache:
            return self._cache[item_id]
        path = self.raw.get(item_id)
        if not path and item_id not in self.extra_items:
            self._cache[item_id] = None
            return None
        d = self.extra_items.get(item_id) or self._parsed.get(item_id)
        if d is None:
            d = json.loads(self.read(path).decode("utf-8"))
            self._parsed[item_id] = d
        merged = d
        parent = d.get("Parent")
        if parent and parent != item_id:
            p = self.item(parent)
            if p:
                merged = deep_merge(p, d)
        self._cache[item_id] = merged
        return merged

    def block_type(self, name):
        """The merged BlockType for a registry name (state variants included), or None."""
        base, state = split_name(name)
        it = self.item(base)
        if not it or "BlockType" not in it:
            return None
        bt = it["BlockType"]
        if state:
            over = (bt.get("State", {}) or {}).get("Definitions", {}).get(state)
            if over is None:
                return None
            bt = deep_merge({k: v for k, v in bt.items() if k != "State"}, over)
        return bt
