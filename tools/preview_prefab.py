#!/usr/bin/env python3
"""Opens a prefab in the 3D preview (tools/preview/preview.html).

    python tools/preview_prefab.py path/to/prefab.json        # writes prefab.js and opens the page
    python tools/preview_prefab.py path/to/prefab.json --no-open

The page can also be opened by itself and a prefab dropped onto it. This only saves the dragging: a page
opened from disk cannot read files by itself, so the prefab is written next to it as a script.
"""
import json
import os
import sys
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
PREVIEW = os.path.join(HERE, "preview")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    src = args[0]
    with open(src, encoding="utf-8") as f:
        data = json.load(f)
    # Only what the page needs. Many exported prefabs are the whole bounding box with air in every empty
    # cell (the dragon statue is 877,000 cells for about 50,000 blocks), which a browser should not parse.
    air = {"", "empty", "air", "void", "none"}
    blocks = []
    for b in data.get("blocks", []):
        name = b.get("name", "")
        if name.lower() in air:
            continue
        row = {"x": b["x"], "y": b["y"], "z": b["z"], "name": name}
        if b.get("rotation") is not None:
            row["rotation"] = b["rotation"]
        blocks.append(row)
    data = {"__name": os.path.splitext(os.path.basename(src))[0], "anchorX": data.get("anchorX", 0),
            "anchorY": data.get("anchorY", 0), "anchorZ": data.get("anchorZ", 0), "blocks": blocks}
    print("%d blocks" % len(blocks))
    os.makedirs(PREVIEW, exist_ok=True)
    with open(os.path.join(PREVIEW, "prefab.js"), "w", encoding="utf-8", newline="\n") as f:
        f.write("window.HYPREFAB=")
        json.dump(data, f, separators=(",", ":"))
        f.write(";\n")
    if not os.path.exists(os.path.join(PREVIEW, "pack.js")):
        print("pack.js is missing: run  python tools/build_preview_pack.py \"<path to Assets.zip>\"  once first.")
    page = os.path.join(PREVIEW, "preview.html")
    print("wrote", os.path.join(PREVIEW, "prefab.js"))
    if "--no-open" not in sys.argv:
        webbrowser.open("file:///" + page.replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
