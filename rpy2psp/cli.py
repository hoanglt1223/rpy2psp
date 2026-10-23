"""Command line: `rpy2psp compile|disasm|coverage ...` (see README)."""

import argparse
import json
import os
import sys
from collections import Counter

from .compiler.bytecode import disassemble_container
from .compiler.compile import compile_game


def write_outputs(comp, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    data = comp.bytecode()
    with open(os.path.join(out_dir, "script.vnb"), "wb") as f:
        f.write(data)
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(comp.manifest(), f, ensure_ascii=False, indent=1)
    return data


def report(comp, stream):
    for w in comp.warnings:
        print("warning:", w, file=stream)
    for e in comp.errors:
        tag = f" [{e.feature}]" if e.feature else ""
        print(f"error: {e.where}: {e.msg}{tag}", file=stream)


def cmd_compile(args):
    comp = compile_game(args.game, args.language)
    report(comp, sys.stderr)
    if comp.errors:
        print(f"{len(comp.errors)} unsupported construct(s); nothing written.", file=sys.stderr)
        return 1
    data = write_outputs(comp, args.out)
    print(f"wrote {os.path.join(args.out, 'script.vnb')} ({len(data)} bytes), "
          f"{len(comp.labels)} labels, {len(comp.assets)} assets")
    return 0


def cmd_disasm(args):
    with open(args.file, "rb") as f:
        sys.stdout.write(disassemble_container(f.read()))
    return 0


def _pack_options(args):
    from .assets.pack import PackOptions
    return PackOptions(font_dirs=args.font_dir or [], font_size=args.font_size, name_font_size=args.font_size,
                       pcm_limit=args.pcm_limit, fallback_font=args.fallback_font)


def _load_manifest(args):
    path = args.manifest or os.path.join(args.out, "manifest.json")
    if not os.path.exists(path):
        raise SystemExit(f"{path} not found; run `rpy2psp compile` first")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cmd_pack(args):
    from .assets.images import AssetError
    from .assets.pack import build_pack
    from .compiler.reader import load_game
    manifest = _load_manifest(args)
    game = load_game(args.game)
    try:
        data, report_ = build_pack(game, manifest, _pack_options(args))
    except AssetError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        game.close()
    path = os.path.join(args.out, "data.pak")
    with open(path, "wb") as f:
        f.write(data)
    kinds = ", ".join(f"{k} {v / 1024:.0f} KiB" for k, v in sorted(report_["by_kind"].items()))
    print(f"wrote {path}: {report_['entries']} entries, {len(data) / 1048576:.2f} MiB ({kinds})")
    return 0


def cmd_verify(args):
    from .assets.pack import verify_pack
    from .compiler.reader import load_game
    manifest = _load_manifest(args)
    with open(os.path.join(args.out, "data.pak"), "rb") as f:
        data = f.read()
    game = load_game(args.game)
    try:
        summary, failures = verify_pack(data, game, manifest, _pack_options(args))
    finally:
        game.close()
    imgs = summary["image"]
    if imgs:
        print(f"images: {len(imgs)}, PSNR min {min(m['psnr'] for m in imgs):.1f} dB, "
              f"mean {sum(min(m['psnr'], 99) for m in imgs) / len(imgs):.1f} dB, "
              f"alpha error max mean {max(m['alpha_mean'] for m in imgs):.2f}")
    for m in summary["music"]:
        print(f"music {m['key']}: MPEG-{m['header'][0]} layer {m['header'][1]} {m['header'][2]} kbps "
              f"{m['header'][3]} Hz, SNR {m['snr']:.1f} dB, duration diff {m['duration_diff'] * 1000:.0f} ms")
    for m in summary["sfx"]:
        print(f"sound {m['key']}: {'PCM16' if m['codec'] == 0 else 'IMA ADPCM'}, SNR {m['snr']:.1f} dB")
    for m in summary["font"]:
        print(f"font {m['key']}: {m['glyphs']} glyphs on {m['pages']} page(s), {m['mismatches']} mismatches, "
              f"{m['missing']} missing")
    for f_ in failures:
        print("FAIL", f_)
    return 1 if failures else 0


def coverage_rows(games, games_dir):
    rows = []
    for g in games:
        root = os.path.join(games_dir, g["name"], g.get("root", ""))
        listed = []
        listing = os.path.join(games_dir, g["name"] + ".files.txt")
        if os.path.exists(listing):
            prefix = g.get("root", "").strip("/")
            prefix = prefix + "/" if prefix else ""
            with open(listing, encoding="utf-8") as f:
                listed = [p.strip()[len(prefix):] for p in f if p.strip().startswith(prefix)]
        try:
            comp = compile_game(root, None, listed)
        except Exception as e:  # noqa: BLE001 - one broken game must not stop the corpus report
            rows.append((g["name"], None, Counter({f"crash:{type(e).__name__}": 1}), str(e)))
            continue
        feats = Counter(e.feature or "other" for e in comp.errors)
        rows.append((g["name"], len(comp.errors), feats, ""))
    return rows


def cmd_coverage(args):
    with open(args.manifest, encoding="utf-8") as f:
        games = [g for g in json.load(f)["games"] if args.all or not g.get("supplementary")]
    rows = coverage_rows(games, args.games_dir)
    covered = [r[0] for r in rows if r[1] == 0]
    construct_ok = [r[0] for r in rows if r[1] is not None and
                    not [k for k in r[2] if not k.endswith("file-missing")]]
    features = Counter()
    for _, _, feats, _ in rows:
        for k in feats:
            features[k] += 1
    lines = [f"Compiled with zero errors: {len(covered)} of {len(rows)} ({', '.join(covered) or 'none'})",
             f"No unsupported constructs (ignoring files missing from the checkout): {len(construct_ok)} of "
             f"{len(rows)} ({', '.join(construct_ok) or 'none'})", "",
             "| Game | Errors | Top error features |", "|---|---|---|"]
    for name, count, feats, note in rows:
        top = ", ".join(f"`{k}` x{v}" for k, v in feats.most_common(5)) or "-"
        lines.append(f"| {name} | {'crash' if count is None else count} | {top}{' ' + note[:60] if note else ''} |")
    lines += ["", "| Error feature | Games |", "|---|---|"]
    lines += [f"| `{k}` | {v} |" for k, v in features.most_common()]
    text = "\n".join(lines) + "\n"
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(text)
    print(text)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="rpy2psp", description="Convert Ren'Py visual novels to PSP homebrew.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("compile", help="compile scripts to script.vnb + manifest.json")
    p.add_argument("game", help="game folder (the one holding game/, or game/ itself)")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--language", help="translation compiled in (tl/<language>)")
    p.set_defaults(fn=cmd_compile)
    for name, fn, text in (("pack", cmd_pack, "convert the assets in manifest.json into data.pak"),
                           ("verify", cmd_verify, "decode data.pak back and compare it with the sources")):
        p = sub.add_parser(name, help=text)
        p.add_argument("game")
        p.add_argument("-o", "--out", required=True, help="folder with manifest.json (data.pak goes here)")
        p.add_argument("--manifest")
        p.add_argument("--font-dir", action="append", help="extra folder to search for fonts (repeatable)")
        p.add_argument("--fallback-font", help="font for characters the game font lacks")
        p.add_argument("--font-size", type=int, default=14)
        p.add_argument("--pcm-limit", type=float, default=1.0, help="effects up to this many seconds stay PCM")
        p.set_defaults(fn=fn)
    p = sub.add_parser("disasm", help="print a readable listing of script.vnb")
    p.add_argument("file")
    p.set_defaults(fn=cmd_disasm)
    p = sub.add_parser("coverage", help="compile every game of a census manifest and report errors")
    p.add_argument("--manifest", required=True)
    p.add_argument("--games-dir", required=True)
    p.add_argument("--markdown")
    p.add_argument("--all", action="store_true", help="include supplementary (jam) games")
    p.set_defaults(fn=cmd_coverage)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
