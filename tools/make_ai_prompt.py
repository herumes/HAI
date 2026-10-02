#!/usr/bin/env python3
"""Builds ai_prefab_prompt.md: the one prompt to paste into a strong AI so it can
design a Hytale build as a build script (see buildscript.hpp for the format).

Sources, in order of trust:
  * block_ids.txt      - the game's own block registry, written by the mod
                         (id<TAB>name). This is what the game will accept.
  * hytale_blocks.json - the hytaleguide.net scrape. Only ~825 placeable blocks,
                         so it is used as a cross-check, not as the list.
  * prefabs/*.json     - names inside real exported prefabs, all known-good.

Run:  python tools/make_ai_prompt.py
Then the Build tab's "Copy prompt" button reads the file next to the DLL.
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)

# Top-level name prefixes that are never something you build with: debug and
# spawner blocks, food, potions, tools, portals, workbenches...
SKIP_TOP = {
    "Debug", "Filter", "Spawn", "Spawner", "Template", "Ingredient", "PortalKey", "Tool", "Survival", "Portal",
    "Editor", "Test", "Hub", "Block", "Alchemy", "Vehicle", "Recipe", "Rail", "Objective", "Forgotten", "Lobby",
    "Empty", "Unknown", "ExamplePrefabPlacementBlock", "Instance", "Launchpad", "Tinkering", "Bandage",
    "Decorative", "Arcade", "Greenscreen", "Barrier", "Container", "Stalactite", "Geyzer", "Coop", "Leave",
    "Prefab", "Potion", "Food", "Bench", "Trap", "Halloween", "Winter", "Kweebec", "Tree", "Fluid",
    "Flag", "Lightsource",
}
# Plant families that are farming, seeds or dev leftovers rather than scenery.
SKIP_PLANT = {"Crop", "Seeds", "Sapling", "Test", "Lavender", "Sunflower", "Hay"}


def load_registry(path):
    """Every name in block_ids.txt. NOT keyed by id: the file merges the game's
    registry with names learned while building, and the two disagree on some ids
    (1238 is both Rock_Stone_Brick and Wood_Softwood_Roof). Both names are real,
    so a dict keyed by id silently drops real blocks."""
    names = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip(chr(10) + chr(13)).split(chr(9), 1)
            if len(parts) == 2:
                names.append(parts[1])
    return names


def plain_names(reg):
    """Plain names, plus the base of every state name (Wood_Oak_Trunk is only ever
    seen as the base of '*Wood_Oak_Trunk_State_Definitions_Stripped')."""
    names = set()
    for n in reg:
        if n.startswith("*"):
            m = re.match(r"\*(.*?)_State_Definitions_", n)
            if m:
                names.add(m.group(1))
        else:
            names.add(n)
    return names


def usable(name):
    top = name.split("_")[0]
    if top in SKIP_TOP or top == "Unknown":
        return False
    if top == "Plant" and name.split("_")[1] in SKIP_PLANT:
        return False
    if "Test" in name.split("_") or "Debug" in name:
        return False
    if name.startswith("Furniture_") and ("Windows_1X2_Test" in name or "Windows_2X2_Test" in name):
        return False
    return True


# How many leading tokens name a "family". Everything after is the piece.
DEPTH = {"Rock": 2, "Soil": 2, "Wood": 2, "Metal": 2, "Cloth": 2, "Build": 2, "Plant": 2, "Furniture": 2,
         "Deco": 2, "Prototype": 3, "Rubble": 1, "Ore": 1, "WinterHoliday": 1}


def families(names):
    fam = collections.defaultdict(set)
    for n in names:
        toks = n.split("_")
        top = toks[0]
        d = min(DEPTH.get(top, 2), len(toks))
        key = "_".join(toks[:d])
        fam[key].add("_".join(toks[d:]) or ".")
    return fam


def factor_variants(pieces):
    """Rock_Sandstone has Brick_Stairs and Red_Brick_Stairs and White_Brick_Stairs.
    Pull 'Red' and 'White' out as variants of the whole family.

    Returns (base pieces, {variant: pieces it has, without the variant word})."""
    pieces = set(pieces)
    first = collections.Counter(p.split("_")[0] for p in pieces if "_" in p)
    variants = {}
    for tok, n in first.items():
        if n < 5 or tok in ("Brick", "Cobble", "Smooth", "Roof", "Pillar", "Pipe", "Stalactite", "Stalactites"):
            continue
        stripped = {p[len(tok) + 1:] for p in pieces if p.startswith(tok + "_")}
        rest = pieces - {p for p in pieces if p.startswith(tok + "_")} - {tok}
        # Only a variant if what follows the word is what the plain family has.
        if len(stripped & rest) * 2 >= len(stripped) and len(stripped & rest) >= 4:
            variants[tok] = stripped | ({"."} if tok in pieces else set())
    if not variants:
        return pieces, {}
    base = set(pieces)
    for tok in variants:
        base = {p for p in base if not p.startswith(tok + "_") and p != tok}
    return base, variants


class Node:
    __slots__ = ("kids", "end")

    def __init__(self):
        self.kids = {}
        self.end = False


def trie(pieces):
    root = Node()
    for p in pieces:
        node = root
        if p == ".":
            node.end = True
            continue
        for tok in p.split("_"):
            node = node.kids.setdefault(tok, Node())
        node.end = True
    return root


def render(node):
    """Brace expansion of a trie. '' is 'the stem itself'."""
    items = []
    if node.end:
        items.append("")
    for tok in sorted(node.kids):
        kid = node.kids[tok]
        text = "_" + tok
        # follow a chain with no branching and no end
        while len(kid.kids) == 1 and not kid.end:
            (t2, k2), = kid.kids.items()
            text += "_" + t2
            kid = k2
        text += render(kid)
        items.append(text)
    if not items:
        return ""
    if len(items) == 1:
        return items[0]
    return "{" + ",".join(items) + "}"


def expr(pieces):
    r = render(trie(pieces))
    # a bare family with a single piece is just "_piece"
    return r


def compress(fams, keys, max_delta=4):
    """Group families that have (nearly) the same pieces."""
    struct = {k: factor_variants(fams[k]) for k in keys}
    left = sorted(keys)
    out = []
    while left:
        counts = collections.Counter(tuple(sorted(struct[k][0])) for k in left)
        base_set, _ = counts.most_common(1)[0]
        base = set(base_set)
        members, rest = [], []
        limit = max_delta if len(base) > 6 else 1
        for k in left:
            s = struct[k][0]
            add, miss = s - base, base - s
            if len(add) + len(miss) <= limit:
                members.append((k, sorted(add), sorted(miss), struct[k][1]))
            else:
                rest.append(k)
        out.append((base_set, members))
        left = rest
    return out


def family_term(k, add, miss, variants):
    t = k
    if variants:
        t += "(+" + ",".join(sorted(variants)) + ")"
    if add:
        t += " +" + ",".join(add)
    if miss:
        t += " -" + ",".join(miss)
    return t


def fmt_group(base_set, members):
    terms = [family_term(k, a, m, v) for k, a, m, v in members]
    e = expr(base_set)
    if len(members) == 1:
        return terms[0] + ": " + (e if e else "(just itself)")
    return " | ".join(terms) + ": " + (e if e else "(just itself)")


def variant_notes(members):
    """A variant that lacks pieces the family has needs saying, or the AI will
    invent Rock_Runic_Blue_Cobble."""
    lines = []
    for k, add, miss, variants in members:
        for v, pieces in sorted(variants.items()):
            pass
    return lines


def catalog(names, scrape_ids):
    fams = families(names)
    by_top = collections.defaultdict(list)
    for k in fams:
        by_top[k.split("_")[0]].append(k)
    order = ["Rock", "Soil", "Wood", "Metal", "Cloth", "Build", "Rubble", "Ore", "Prototype", "Furniture", "Deco",
             "Plant", "WinterHoliday"]
    lines = []
    for top in order:
        if top not in by_top:
            continue
        lines.append(f"## {top}")
        for base_set, members in compress(fams, by_top[top]):
            lines.append(fmt_group(base_set, members))
            # variants are written next to their family, exactly
            for k, add, miss, variants in members:
                own = (set(base_set) | set(add)) - set(miss)
                for v, pieces in sorted(variants.items()):
                    if pieces == own:
                        continue          # the same pieces: "(+Red,White)" already says it
                    lines.append(f"  {k}_{v} has only: " + (expr(pieces) or "(just itself)"))
        lines.append("")
    return chr(10).join(lines).rstrip() + chr(10)


# ---------------------------------------------------------------------------------------------
# The race server's custom blocks (tools/race_blocks.json)
# ---------------------------------------------------------------------------------------------
def _norm(name):
    return name.lower().replace("_", "")


def load_race(catalog_path, known_names, names_file):
    """The catalog entries, each with the name the game really uses when we know it.

    The zip the catalog comes from has models and pictures but no definitions, so a name is a guess
    from a folder or file name. Anything the registry (block_ids.txt, learned in the game) or a
    --race-names list has under the same letters, ignoring case and underscores, wins."""
    with open(catalog_path, encoding="utf-8") as f:
        cat = json.load(f)
    real = {}
    for n in known_names:
        real.setdefault(_norm(n), n)
    listed = set()
    if names_file and os.path.exists(names_file):
        with open(names_file, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                n = line.split(chr(9))[-1].strip()
                listed.add(n)
                real[_norm(n)] = n                       # the list beats the registry
    out = []
    for e in cat["blocks"]:
        e = dict(e)
        e["guess"] = e["name"]
        hit = real.get(_norm(e["name"]))
        e["verified"] = hit is not None
        if hit:
            e["name"] = hit
        out.append(e)
    unmatched = sorted(n for n in listed if _norm(n) not in {_norm(e["name"]) for e in out})
    return out, unmatched


RACE_CATS = [("road", "Road and ground"), ("curb", "Kerbs and bumps"), ("marking", "Flat road markings"),
             ("barrier", "Barriers and walls"), ("prop", "Props and buildings"), ("roof", "Roofs")]


LUMP_DESC = {
    "startgrid": "flat start-grid slot tiles, each marked with its number (Kart_Start_Grid is slot 1; the suffixed ones carry that digit)",
    "skid": "flat tyre-mark tiles for run-off areas and braking zones (plain diagonal, straight pair, curve, crossing, fading end)",
}


def race_catalog(entries):
    lines = []
    done = set()
    for cat, title in RACE_CATS:
        rows = [e for e in entries if e.get("cat") == cat]
        if not rows:
            continue
        lines.append("### " + title)
        for e in rows:
            lump = e.get("lump")
            if lump:
                if lump in done:
                    continue
                done.add(lump)
                names = [x["name"] for x in entries if x.get("lump") == lump]
                lines.append("- %s: %s" % (", ".join("`%s`" % n for n in names), LUMP_DESC.get(lump, e["desc"])))
                continue
            text = "- `%s`: %s" % (e["name"], e["desc"])
            if e.get("rot"):
                text += ". Orientation: " + e["rot"]
            lines.append(text)
        lines.append("")
    return chr(10).join(lines).rstrip() + chr(10)


def example_block_names(example_json):
    """Every block name a script uses: literal names in block/edge/line/barrier/verge/under (palette keys excluded)."""
    data = json.loads(example_json)
    keys = set(data.get("palette", {}))
    found = set()

    def take(v):
        if isinstance(v, str):
            if v not in keys:
                found.add(v)
        elif isinstance(v, list):
            for x in v:
                take(x)

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ("block", "edge", "line", "barrier", "verge", "under", "cap", "gable_fill", "only"):
                    take(v)
                else:
                    walk(v)
        elif isinstance(o, list):
            for x in o:
                walk(x)

    walk(data.get("ops", []))
    for v in data.get("palette", {}).values():
        take(v if not isinstance(v, dict) else v.get("block"))
    return found



# ---------------------------------------------------------------------------------------------
# Server mods with their own item definitions (tools/server_mods/*.zip), e.g. NoCube's neon blocks
# ---------------------------------------------------------------------------------------------
MOD_HINTS = {
    "NoCube_Neon": "flat, unlit solid colours: they are not affected by light or shadow and carry no texture. Use them for signs, "
                   "glowing trims and light strips, logos and pixel art, arcade / cyberpunk / space looks and track-edge accents; "
                   "not for natural materials",
}


def mod_blocks():
    """[{name, description, items: [(id, colour)]}] for every server mod zip found."""
    import hyrace
    out = []
    for z in hyrace.find_mod_zips():
        _, items, man = hyrace.read_mod_zip(z)
        rows = []
        for k in sorted(items):
            bt = items[k].get("BlockType")
            if not bt:
                continue
            col = (bt.get("Light") or {}).get("Color") or bt.get("ParticleColor") or ""
            rows.append((k, col))
        if rows:
            out.append({"name": man.get("Name") or os.path.basename(z), "description": man.get("Description", ""), "items": rows})
    return out


def mod_section(mods):
    if not mods:
        return ""
    lines = ["# SERVER MOD BLOCKS", "",
             "My server also has these extra blocks (names exact). They exist nowhere else, so use them when the build calls for them.", ""]
    for m in mods:
        names = [n for n, _ in m["items"]]
        common = os.path.commonprefix(names)
        hint = next((v for k, v in MOD_HINTS.items() if common.startswith(k)), "")
        lines.append("## " + m["name"] + ("" if not m["description"] else " - " + m["description"]))
        if hint:
            lines.append("Note: " + hint + ".")
        lines.append(", ".join("`%s`%s" % (n, " (%s)" % c if c else "") for n, c in m["items"]))
        lines.append("")
    return chr(10).join(lines) + chr(10)



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", default=os.path.join(ROOT, "x64", "Release", "block_ids.txt"))
    ap.add_argument("--scrape", default=os.environ.get("HYTALE_BLOCKS_JSON", ""))
    ap.add_argument("--template", default=os.path.join(HERE, "ai_prefab_prompt.template.md"))
    ap.add_argument("--out", nargs="*", default=[os.path.join(ROOT, "x64", "Release", "ai_prefab_prompt.md"),
                                                 os.path.join(HERE, "ai_prefab_prompt.md")])
    ap.add_argument("--example", default=os.path.join(HERE, "example_cottage.json"))
    ap.add_argument("--names-out", nargs="*", default=[os.path.join(ROOT, "x64", "Release", "ai_prefab_blocks.txt"),
                                                       os.path.join(HERE, "ai_prefab_blocks.txt")])
    ap.add_argument("--race-catalog", default=os.path.join(HERE, "race_blocks.json"))
    ap.add_argument("--race-template", default=os.path.join(HERE, "ai_prefab_race.template.md"))
    ap.add_argument("--race-example", default=os.path.join(HERE, "example_race.json"))
    ap.add_argument("--race-names", default=os.path.join(HERE, "race_names.txt"),
                    help="the real names of the server's custom blocks, one per line (optional)")
    ap.add_argument("--race-out", nargs="*", default=[os.path.join(ROOT, "x64", "Release", "ai_prefab_prompt_race.md"),
                                                      os.path.join(HERE, "ai_prefab_prompt_race.md")])
    ap.add_argument("--catalog-only", action="store_true")
    args = ap.parse_args()

    reg = load_registry(args.registry)
    all_names = plain_names(reg)

    # Names inside real exports are good by construction.
    prefab_dir = os.path.join(ROOT, "x64", "Release", "prefabs")
    real = set()
    if os.path.isdir(prefab_dir):
        for fn in os.listdir(prefab_dir):
            if fn.endswith(".json") and os.path.getsize(os.path.join(prefab_dir, fn)) < 20_000_000:
                try:
                    txt = open(os.path.join(prefab_dir, fn), encoding="utf-8").read()
                except OSError:
                    continue
                for n in re.findall(r'"name":\s*"([^"]+)"', txt):
                    m = re.match(r"\*(.*?)_State_Definitions_", n)
                    real.add(m.group(1) if m else n.lstrip("*"))
    real.discard("Empty")
    not_in_reg = sorted(real - all_names)

    scrape_ids = set()
    scrape_path = args.scrape
    if not scrape_path:
        guess = r"C:\Users\chris\OneDrive\Desktop\GitHub\shuttleai\ShuttleAI-API2\hytale_blocks.json"
        scrape_path = guess if os.path.exists(guess) else ""
    if scrape_path and os.path.exists(scrape_path):
        scrape_ids = {it["id"] for it in json.load(open(scrape_path, encoding="utf-8"))["items"]}

    names = {n for n in (all_names | real) if usable(n)}
    cat = catalog(names, scrape_ids)

    sys.stderr.write(f"registry: {len(reg)} entries, {len(all_names)} plain names\n")
    sys.stderr.write(f"usable for building: {len(names)}\n")
    if scrape_ids:
        sys.stderr.write(f"scrape: {len(scrape_ids)} ids, {len(scrape_ids & all_names)} in the registry; "
                         f"not in registry: {sorted(scrape_ids - all_names)}\n")
    if not_in_reg:
        sys.stderr.write(f"names in real prefabs but not the registry: {not_in_reg}\n")
    sys.stderr.write(f"catalog: {len(cat)} chars (~{len(cat) // 3} tokens)\n")

    if args.catalog_only:
        sys.stdout.write(cat)
        return

    with open(args.template, encoding="utf-8") as f:
        tpl = f.read()
    with open(args.example, encoding="utf-8") as f:
        example = f.read().strip()

    # Every block the prompt itself names (in the text and the example) must be a
    # real one, or the prompt would teach the AI a wrong name.
    every_real = all_names | real | scrape_ids
    mods = mod_blocks()
    mod_text = mod_section(mods)
    for m in mods:
        every_real = every_real | {n for n, _ in m["items"]}
        sys.stderr.write(f"server mod: {m['name']}: {len(m['items'])} blocks\n")
    mentioned = set()
    for m in re.finditer(r"\b(?:Rock|Wood|Soil|Cloth|Metal|Build|Plant|Deco|Furniture)_[A-Za-z0-9_]*", tpl + example):
        if not m.group(0).endswith("_"):
            mentioned.add(m.group(0))
    bad = sorted(n for n in mentioned if n not in every_real)
    if bad:
        sys.stderr.write(f"ERROR: the template or example names blocks that do not exist: {bad}\n")
        sys.exit(1)

    # The race server's own blocks: a second prompt that teaches them, and their names join the list
    # the mod validates replies against.
    race_entries, race_unmatched = [], []
    if os.path.exists(args.race_catalog):
        race_entries, race_unmatched = load_race(args.race_catalog, all_names | real, args.race_names)
        race_names = {e["name"] for e in race_entries}
        with open(args.race_example, encoding="utf-8") as f:
            race_example = f.read().strip()
        # the example may only use blocks that exist: the game's, or the race catalog's
        guess_names = {e["guess"]: e["name"] for e in race_entries}
        for n in sorted(example_block_names(race_example)):
            if n in guess_names and guess_names[n] != n:
                race_example = race_example.replace('"%s"' % n, '"%s"' % guess_names[n])
            elif n not in every_real and n not in race_names and n.lower() != "air":
                sys.stderr.write(f"ERROR: the race example uses a block that does not exist: {n}\n")
                sys.exit(1)
        every_real = every_real | race_names
        verified = sum(1 for e in race_entries if e["verified"])
        sys.stderr.write(f"race blocks: {len(race_entries)} ({verified} confirmed by the registry or --race-names, "
                         f"{len(race_entries) - verified} guessed from file names)\n")
        if race_unmatched:
            sys.stderr.write(f"race names given that match no catalog block (add them to tools/race_blocks.json): {race_unmatched}\n")

    prompt = (tpl.replace("{{RACE_SECTION}}", mod_text).replace("{{CATALOG}}", cat).replace("{{EXAMPLE}}", example)
              .replace("{{BLOCK_COUNT}}", str(len(names))))
    race_prompt = None
    if race_entries:
        with open(args.race_template, encoding="utf-8") as f:
            rtpl = f.read()
        section = mod_text + (rtpl.replace("{{RACE_CATALOG}}", race_catalog(race_entries))
                              .replace("{{RACE_EXAMPLE}}", race_example)).rstrip() + chr(10) + chr(10)
        race_prompt = (tpl.replace("{{RACE_SECTION}}", section).replace("{{CATALOG}}", cat)
                       .replace("{{EXAMPLE}}", example).replace("{{BLOCK_COUNT}}", str(len(names))))

    # The validator in the mod checks a script's names against this list, and it is
    # the same list the prompt was made from, so the two can never disagree. It is
    # deliberately wider than the catalog: anything the game really has is accepted.
    valid = sorted(n for n in every_real if not n.startswith("Unknown") and n != "Empty")
    for out in args.out:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(prompt)
        sys.stderr.write(f"wrote {out} ({len(prompt)} chars, ~{len(prompt) // 3} tokens)\n")
    if race_prompt:
        for out in args.race_out:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "w", encoding="utf-8", newline="\n") as f:
                f.write(race_prompt)
            sys.stderr.write(f"wrote {out} ({len(race_prompt)} chars, ~{len(race_prompt) // 3} tokens)\n")
    for out in args.names_out:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write("# every block name the game accepts, for checking AI-made builds\n")
            f.write("\n".join(valid) + "\n")
        sys.stderr.write(f"wrote {out} ({len(valid)} names)\n")


if __name__ == "__main__":
    main()
