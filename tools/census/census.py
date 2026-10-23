"""Ren'Py feature census CLI.

    python -I census.py --manifest games.json --games-dir <scratch>/games \
        --subset subset.json --json out.json --markdown out.md

or, for ad-hoc folders:  python -I census.py --subset subset.json <game-dir>...

A game directory is the folder that holds the game's .rpy files (normally the
project's `game/` folder or its parent). Parse only; no game code is executed.
"""

import argparse
import fnmatch
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyzer import GameCensus  # noqa: E402


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def match_any(key, patterns):
    return any(fnmatch.fnmatchcase(key, p) for p in patterns)


def blockers(game, subset):
    out = {}
    for key, rec in game["features"].items():
        if match_any(key, subset["ignored"]) or match_any(key, subset["supported"]):
            continue
        out[key] = rec
    return out


def run_games(args):
    games = []
    if args.manifest:
        for g in load_json(args.manifest)["games"]:
            base = os.path.join(args.games_dir, g["name"])
            root = os.path.join(base, g.get("root", ""))
            files = None
            listing = os.path.join(args.games_dir, g["name"] + ".files.txt")
            if os.path.exists(listing):
                prefix = g.get("root", "").strip("/")
                with open(listing, encoding="utf-8") as f:
                    files = [p.strip() for p in f if p.strip().startswith(prefix)]
            c = GameCensus(g["name"], root, files).run().to_dict()
            c["meta"] = g
            games.append(c)
    for d in args.dirs:
        c = GameCensus(os.path.basename(os.path.normpath(d)), d).run().to_dict()
        c["meta"] = {"name": c["name"]}
        games.append(c)
    return games


def coverage(games, subset):
    for g in games:
        g["blockers"] = blockers(g, subset)
        g["covered"] = not g["blockers"]
    by_key = {}
    for g in games:
        for k in g["blockers"]:
            by_key.setdefault(k, []).append(g["name"])
    solo = {}
    for g in games:
        if len(g["blockers"]) == 1:
            solo.setdefault(next(iter(g["blockers"])), []).append(g["name"])
    return by_key, solo


FAMILIES = [
    ("custom screens", ("screen:", "py:call:renpy.call_screen", "py:call:renpy.show_screen",
                        "py:call:renpy.hide_screen", "py:call:renpy.curry")),
    ("Python logic", ("py:stmt:", "py:call:user-func", "py:method:", "py:expr:", "py:attr:",
                      "py:assign:", "py:early", "py:unparsable", "py:call:<complex>")),
    ("transitions", ("trans:",)),
    ("ATL", ("atl:", "transform:", "at:")),
    ("text tags", ("text:",)),
    ("Ren'Py API", ("py:call:renpy.",)),
]


def family(key):
    for name, prefixes in FAMILIES:
        if key.startswith(prefixes):
            return name
    if key.startswith(("py:call:", "image:call:", "stmt:layeredimage")):
        return "displayables/classes"
    return "other"


def scenarios(games, subset):
    """Cumulative what-if coverage for the optional extensions in subset.json."""
    gate = [g for g in games if not g["meta"].get("supplementary")]
    jam = [g for g in games if g["meta"].get("supplementary")]
    supported = list(subset["supported"])
    rows = []
    steps = [("Core subset (supported list)", [])] + [(k, v) for k, v in subset.get("scenarios", {}).items()]
    for name, extra in steps:
        supported += extra
        trial = {"supported": supported, "ignored": subset["ignored"]}
        cov = lambda gs: [g["name"] for g in gs if not blockers(g, trial)]
        rows.append((name, cov(gate), len(gate), cov(jam), len(jam)))
    return rows


def feature_matrix(games):
    keys = sorted({k for g in games for k in g["features"]})
    rows = []
    for k in keys:
        users = [g for g in games if k in g["features"]]
        rows.append((k, len(users), sum(g["features"][k]["count"] for g in users)))
    return rows


def markdown(games, subset, by_key, solo):
    L = []
    for label, group in (("Gate corpus", [g for g in games if not g["meta"].get("supplementary")]),
                         ("Supplementary jam corpus", [g for g in games if g["meta"].get("supplementary")])):
        if group:
            covered = [g["name"] for g in group if g["covered"]]
            L.append(f"{label}: {len(covered)} of {len(group)} covered ({', '.join(covered) or 'none'})\n")
    L.append("| Game | Era | License | Files | Say lines | Labels | Languages | Covered | Blocking families | Blocking features |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for g in games:
        s, m = g["stats"], g["meta"]
        top = sorted(g["blockers"].items(), key=lambda kv: -kv[1]["count"])
        bl = ", ".join(f"`{k}`x{v['count']}" for k, v in top[:6]) + (f" (+{len(top) - 6} more)" if len(top) > 6 else "")
        fams = sorted({family(k) for k in g["blockers"]})
        L.append(f"| {g['name']}{' (jam)' if m.get('supplementary') else ''} | {m.get('era', '?')} | "
                 f"{m.get('license', '?')} | {s.get('files', 0)} | {s.get('say', 0)} | "
                 f"{s.get('labels', 0)} | {', '.join(g['languages']) or '-'} | "
                 f"{'yes' if g['covered'] else 'no'} | {', '.join(fams) or '-'} | {bl or '-'} |")
    L.append("\n### Cumulative what-if coverage\n")
    L.append("| Step | Gate corpus covered | Jam corpus covered |")
    L.append("|---|---|---|")
    for name, g_cov, g_n, j_cov, j_n in scenarios(games, subset):
        L.append(f"| {name} | {len(g_cov)}/{g_n}: {', '.join(g_cov)} | {len(j_cov)}/{j_n}: {', '.join(j_cov) or '-'} |")
    L.append("\n### Blocking features by number of games\n")
    L.append("| Feature | Games blocked | Sole blocker for | Example |")
    L.append("|---|---|---|---|")
    for k, names in sorted(by_key.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        ex = next(g["features"][k]["first"] for g in games if g["name"] == names[0])
        L.append(f"| `{k}` | {len(names)} ({', '.join(names)}) | {', '.join(solo.get(k, [])) or '-'} | {ex} |")
    L.append("\n### Feature frequency (games using, total occurrences)\n")
    L.append("| Feature | Games | Count | In subset |")
    L.append("|---|---|---|---|")
    for k, n, total in feature_matrix(games):
        tag = "ignored" if match_any(k, subset["ignored"]) else ("yes" if match_any(k, subset["supported"]) else "no")
        L.append(f"| `{k}` | {n} | {total} | {tag} |")
    L.append("\n### Asset formats (files in the game folder)\n")
    exts = sorted({e for g in games for e in g["assets"]})
    L.append("| Game | " + " | ".join(exts) + " |")
    L.append("|---|" + "---|" * len(exts))
    for g in games:
        L.append(f"| {g['name']} | " + " | ".join(str(g["assets"].get(e, "")) for e in exts) + " |")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", help="game directories to scan")
    ap.add_argument("--manifest")
    ap.add_argument("--games-dir", default=".")
    ap.add_argument("--subset", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "subset.json"))
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    args = ap.parse_args()
    if not args.manifest and not args.dirs:
        ap.error("give --manifest or game directories")
    subset = load_json(args.subset)
    games = run_games(args)
    by_key, solo = coverage(games, subset)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(games, f, ensure_ascii=False, indent=1)
    md = markdown(games, subset, by_key, solo)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(md)
    gate = [g for g in games if not g["meta"].get("supplementary")]
    print(f"gate corpus: {sum(g['covered'] for g in gate)}/{len(gate)} games fully inside the subset")
    for g in games:
        print(f"  {g['name']:16} {'covered' if g['covered'] else 'blocked by ' + str(len(g['blockers'])) + ' features'}")


if __name__ == "__main__":
    main()
