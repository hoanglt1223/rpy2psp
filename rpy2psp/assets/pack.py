"""Asset pack (`data.pak`): converted assets grouped by scene, with an index.

Layout and entry formats: docs/dev/pack-format.md. `build_pack` converts every
asset in manifest.json; `verify_pack` decodes every entry back on the PC and
compares it with a fresh conversion of the source (images: PSNR and alpha
error, glyphs: exact match, audio: SNR after alignment).
"""

import struct
from dataclasses import dataclass, field

import numpy as np

from . import audio, fonts, images, movies
from .images import AssetError

MAGIC = b"VNPK"
VERSION = 1
ALIGN = 64
KIND = {"image": 0, "music": 1, "sfx": 2, "voice": 3, "font": 4, "movie": 5}
FONT_ROLES = {"text": ("gui.text_font", "style.default.font"),
              "name": ("gui.name_text_font", "gui.text_font", "style.default.font"),
              "interface": ("gui.interface_text_font", "gui.choice_button_text_font", "gui.text_font",
                            "style.default.font")}
DEFAULT_FONT = "DejaVuSans.ttf"


@dataclass
class PackOptions:
    font_dirs: list = field(default_factory=list)
    font_size: int = 14
    name_font_size: int = 14
    outline: int = 1
    pcm_limit: float = 1.0
    fallback_font: str = None


# ---- entry encoders -----------------------------------------------------------------------

def texture_entry(tex):
    head = struct.pack("<HHHHBBH", tex.width, tex.height, tex.source_size[0], tex.source_size[1], tex.fmt,
                       len(tex.tiles), 0) + b"\0" * 4
    table_len = 24 * len(tex.tiles)
    offset = _align(16 + table_len, 16)
    table, body = bytearray(), bytearray()
    for t in tex.tiles:
        table += struct.pack("<HHHHBBHHHII", t["x"], t["y"], t["w"], t["h"], t["tex_w"].bit_length() - 1,
                             t["tex_h"].bit_length() - 1, t["bufw"], t["rows"], 0, offset + len(body),
                             len(t["data"]))
        body += t["data"]
        body += b"\0" * (_align(len(body), 16) - len(body))
    return b"TEX1" + head[:12] + bytes(table) + b"\0" * (offset - 16 - table_len) + bytes(body)


def read_texture_entry(blob):
    if blob[:4] != b"TEX1":
        raise AssetError("not a TEX1 entry")
    w, h, sw, sh, fmt, n, _ = struct.unpack_from("<HHHHBBH", blob, 4)
    tiles = []
    for i in range(n):
        x, y, tw, th, lw, lh, bufw, rows, _, off, size = struct.unpack_from("<HHHHBBHHHII", blob, 16 + 24 * i)
        tiles.append({"x": x, "y": y, "w": tw, "h": th, "tex_w": 1 << lw, "tex_h": 1 << lh, "bufw": bufw,
                      "rows": rows, "data": blob[off:off + size]})
    return images.Texture(fmt, w, h, (sw, sh), tiles)


def font_entry(atlas):
    head = struct.pack("<HhhhBBHBBH", atlas.size, atlas.ascent, -atlas.descent, atlas.line_height, atlas.fmt,
                       len(atlas.pages), len(atlas.glyphs), atlas.outline, 0, 0)
    glyphs = bytearray()
    for cp in sorted(atlas.glyphs):
        g = atlas.glyphs[cp]
        glyphs += struct.pack("<IHHHHhhHBB", cp, g["x"], g["y"], g["w"], g["h"], g["bx"], g["by"],
                              int(round(g["advance"] * 64)), g["page"], 0)
    pages_off = _align(4 + len(head) + 12 * len(atlas.pages) + len(glyphs), 16)
    table, body = bytearray(), bytearray()
    for data, rows in atlas.pages:
        table += struct.pack("<HHII", fonts.PAGE, rows, pages_off + len(body), len(data))
        body += data
        body += b"\0" * (_align(len(body), 16) - len(body))
    pre = b"FNT1" + head + bytes(table) + bytes(glyphs)
    return pre + b"\0" * (pages_off - len(pre)) + bytes(body)


def read_font_entry(blob):
    if blob[:4] != b"FNT1":
        raise AssetError("not a FNT1 entry")
    size, asc, desc, lh, fmt, npages, nglyphs, outline, _, _ = struct.unpack_from("<HhhhBBHBBH", blob, 4)
    pos = 4 + 16
    pages = []
    for _ in range(npages):
        _, rows, off, ln = struct.unpack_from("<HHII", blob, pos)
        pages.append((blob[off:off + ln], rows))
        pos += 12
    glyphs = {}
    for _ in range(nglyphs):
        cp, x, y, w, h, bx, by, adv, page, _ = struct.unpack_from("<IHHHHhhHBB", blob, pos)
        glyphs[cp] = {"page": page, "x": x, "y": y, "w": w, "h": h, "bx": bx, "by": by, "advance": adv / 64}
        pos += 20
    return fonts.GlyphAtlas(size, fmt, pages, glyphs, asc, -desc, lh, outline)


def _align(n, a=ALIGN):
    return (n + a - 1) // a * a


# ---- building --------------------------------------------------------------------------------

def font_plan(manifest, options):
    """(entry key, font file name, size) for each role and {font=} face."""
    cfg = manifest.get("fonts", {})
    out = []
    for role, keys in FONT_ROLES.items():
        name = next((cfg[k] for k in keys if cfg.get(k)), DEFAULT_FONT)
        size = options.name_font_size if role == "name" else options.font_size
        out.append((f"font/{role}", name, size))
    for face in manifest.get("text_fonts", []):
        out.append((f"font/face/{face}", face, options.font_size))
    return out


def build_fonts(game, manifest, options, language):
    chars = manifest["glyphs"]
    outline = options.outline if manifest.get("config", {}).get("outlines") else 0
    built = {}
    result = []
    for key, name, size in font_plan(manifest, options):
        label, data = fonts.find_font(game, name, language, options.font_dirs)
        cov = fonts.coverage_of(data)
        missing = sorted({c for c in chars if ord(c) not in cov and c not in "\n\r\t" and ord(c) >= 32})
        if missing and options.fallback_font:
            # characters the main font lacks come from the fallback font (same size)
            _, fb = fonts.find_font(game, options.fallback_font, language, options.font_dirs)
            fbcov = fonts.coverage_of(fb)
            still = [c for c in missing if ord(c) not in fbcov]
            if still:
                raise AssetError(f"{len(still)} characters are in neither {name} nor the fallback font: "
                                 f"{''.join(still[:30])}")
        elif missing:
            raise AssetError(f"font {name} lacks {len(missing)} characters the story uses: {''.join(missing[:30])} "
                             f"(pass --fallback-font)")
        cache_key = (label, size, outline)
        if cache_key not in built:
            main = fonts.build_atlas(data, size, [c for c in chars if c not in missing], outline)
            sources = [(data, set(main.glyphs))]
            atlas = main
            if missing:
                extra = fonts.build_atlas(fb, size, missing, outline)
                atlas = merge_atlases(main, extra)
                sources.append((fb, set(extra.glyphs)))
            built[cache_key] = (atlas, sources)
        result.append((key, built[cache_key], label))
    return result


def merge_atlases(a, b):
    """Append b's pages and glyphs to a (pages stay separate)."""
    base = len(a.pages)
    glyphs = dict(a.glyphs)
    for cp, g in b.glyphs.items():
        glyphs[cp] = dict(g, page=g["page"] + base)
    return fonts.GlyphAtlas(a.size, a.fmt, a.pages + b.pages, glyphs, a.ascent, a.descent, a.line_height, a.outline)


def convert_asset(game, key, spec, scale, options):
    kind = spec["kind"]
    if kind == "image":
        img = images.render_recipe(game, spec["recipe"])
        tex, _ = images.convert_image(img, scale)
        return texture_entry(tex)
    if kind in ("music", "sfx", "voice"):
        try:
            data = game.read(spec["path"])
        except FileNotFoundError:
            raise AssetError(f"audio file {spec['path']!r} not found") from None
        frames, rate = audio.decode_source(data, spec["path"])
        if kind == "music":
            mp3, pcm = audio.encode_mp3(frames, rate)
            return audio.music_entry(mp3, len(pcm), pcm.shape[1])
        blob, _, _ = audio.convert_effect(frames, rate, options.pcm_limit)
        return blob
    if kind == "movie":
        return movies.convert_movie(game.read(spec["path"]), spec["path"])
    raise AssetError(f"unknown asset kind {kind!r}")


def build_pack(game, manifest, options=None, log=None):
    """Return (pack bytes, report dict)."""
    options = options or PackOptions()
    scale = images.screen_scale(manifest["source_size"])
    order = []
    seen = set()
    for scene in manifest.get("scenes", {}).values():
        for key in scene:
            if key not in seen:
                seen.add(key)
                order.append(key)
    order += [k for k in manifest["assets"] if k not in seen]
    entries = []
    errors = []
    sizes = {}
    for key in order:
        spec = manifest["assets"][key]
        try:
            blob = convert_asset(game, key, spec, scale, options)
        except AssetError as e:
            errors.append(f"{key}: {e}")
            continue
        entries.append((key, KIND[spec["kind"]], blob))
        sizes[spec["kind"]] = sizes.get(spec["kind"], 0) + len(blob)
        if log:
            log(f"{key}: {len(blob)} bytes")
    try:
        for key, (atlas, _), label in build_fonts(game, manifest, options, manifest.get("language")):
            blob = font_entry(atlas)
            entries.append((key, KIND["font"], blob))
            sizes["font"] = sizes.get("font", 0) + len(blob)
    except AssetError as e:
        errors.append(f"fonts: {e}")
    if errors:
        raise AssetError("asset conversion failed:\n  " + "\n  ".join(errors))
    data = write_pack(entries, manifest.get("scenes", {}))
    return data, {"entries": len(entries), "bytes": len(data), "by_kind": sizes}


def write_pack(entries, scenes):
    names = bytearray()
    name_off = {}
    for key, _, _ in entries:
        name_off[key] = len(names)
        names += key.encode("utf-8") + b"\0"
    body = bytearray()
    index = []
    offset = _align(32)
    dedupe = {}
    for key, kind, blob in entries:
        if blob in dedupe:
            index.append((name_off[key], kind, *dedupe[blob]))
            continue
        pos = offset + len(body)
        body += blob
        body += b"\0" * (_align(len(body)) - len(body))
        dedupe[blob] = (pos, len(blob))
        index.append((name_off[key], kind, pos, len(blob)))
    entry_index = {key: i for i, (key, _, _) in enumerate(entries)}
    scene_rows, scene_lists = [], bytearray()
    for name, keys in scenes.items():
        name_off.setdefault("scene:" + name, len(names))
        names += name.encode("utf-8") + b"\0"
        ids = [entry_index[k] for k in keys if k in entry_index]
        scene_rows.append((name_off["scene:" + name], len(scene_lists) // 4, len(ids)))
        scene_lists += b"".join(struct.pack("<I", i) for i in ids)
    index_off = offset + len(body)
    idx = b"".join(struct.pack("<IB3xII", n, k, o, s) for n, k, o, s in index)
    scene_off = index_off + len(idx)
    scn = b"".join(struct.pack("<III", *r) for r in scene_rows) + bytes(scene_lists)
    names_off = scene_off + len(scn)
    header = MAGIC + struct.pack("<HHIIIIII", VERSION, 0, len(entries), index_off, scene_off, len(scene_rows),
                                 names_off, len(names))
    return header + b"\0" * (offset - len(header)) + bytes(body) + idx + scn + bytes(names)


def read_pack(data):
    if data[:4] != MAGIC:
        raise AssetError("not a VNPK file")
    version, _, count, index_off, scene_off, nscenes, names_off, names_len = struct.unpack_from("<HHIIIIII", data, 4)
    if version != VERSION:
        raise AssetError(f"unsupported pack version {version}")

    def name_at(o):
        end = data.index(b"\0", names_off + o)
        return data[names_off + o:end].decode("utf-8")
    entries = {}
    order = []
    for i in range(count):
        n, k, o, s = struct.unpack_from("<IB3xII", data, index_off + 16 * i)
        key = name_at(n)
        entries[key] = (k, data[o:o + s])
        order.append(key)
    scenes = {}
    lists = scene_off + 12 * nscenes
    for i in range(nscenes):
        n, first, cnt = struct.unpack_from("<III", data, scene_off + 12 * i)
        ids = struct.unpack_from(f"<{cnt}I", data, lists + 4 * first)
        scenes[name_at(n)] = [order[j] for j in ids]
    return entries, scenes


# ---- verification ---------------------------------------------------------------------------------

def verify_pack(data, game, manifest, options=None):
    """Decode every entry and compare it with the source. Returns a per-kind summary and failures."""
    options = options or PackOptions()
    entries, scenes = read_pack(data)
    scale = images.screen_scale(manifest["source_size"])
    summary = {"image": [], "music": [], "sfx": [], "font": []}
    failures = []
    for key, spec in manifest["assets"].items():
        if key not in entries:
            failures.append(f"{key}: missing from the pack")
            continue
        kind, blob = entries[key]
        if spec["kind"] == "image":
            tex = read_texture_entry(blob)
            ref = images.to_screen_space(images.render_recipe(game, spec["recipe"]), scale)
            m = images.compare(ref, images.decode_texture(tex))
            m["key"] = key
            summary["image"].append(m)
            if m["psnr"] < 20 or m["alpha_mean"] > 4:
                failures.append(f"{key}: PSNR {m['psnr']:.1f} dB, alpha error {m['alpha_mean']:.2f}")
        elif spec["kind"] == "music":
            mp3, length, delay, chans = audio.read_music_entry(blob)
            version, layer, br, sr = audio.check_mp3_header(mp3)
            frames, rate = audio.decode_source(game.read(spec["path"]), spec["path"])
            ref = audio.to_int16(audio.resample(frames, rate, 44100))
            m = audio.verify_mp3(mp3, ref)
            m.update(key=key, header=(version, layer, br, sr))
            summary["music"].append(m)
            # A silent intro defeats the cross-correlation, so the stored delay is only
            # checked against it when the measured lag is meaningful.
            if ((version, layer, br, sr) != (1, 3, 128, 44100) or abs(m["duration_diff"]) > 0.1 or m["snr"] < 10
                    or length != len(ref) or chans != ref.shape[1] or m["lag"] not in (0, delay)):
                failures.append(f"{key}: MP3 check failed {m}, header length {length} delay {delay}")
        elif spec["kind"] in ("sfx", "voice"):
            frames, rate = audio.decode_source(game.read(spec["path"]), spec["path"])
            _, ref, codec = audio.convert_effect(frames, rate, options.pcm_limit)
            dec, _ = audio.decode_effect(blob)
            snr = audio.snr_db(ref, dec)
            summary["sfx"].append({"key": key, "codec": codec, "snr": snr})
            if (codec == audio.CODEC_PCM16 and not np.array_equal(ref, dec)) or snr < 20:
                failures.append(f"{key}: audio SNR {snr:.1f} dB")
    for key, (_, sources), _ in build_fonts(game, manifest, options, manifest.get("language")):
        if key not in entries:
            failures.append(f"{key}: missing from the pack")
            continue
        stored = read_font_entry(entries[key][1])
        bad = 0
        for font_bytes, cps in sources:
            subset = {cp: g for cp, g in stored.glyphs.items() if cp in cps}
            bad += fonts.verify_atlas(font_bytes, fonts.GlyphAtlas(stored.size, stored.fmt, stored.pages, subset,
                                                                   stored.ascent, stored.descent,
                                                                   stored.line_height, stored.outline))
        missing = [c for c in manifest["glyphs"] if ord(c) >= 32 and ord(c) not in stored.glyphs]
        summary["font"].append({"key": key, "glyphs": len(stored.glyphs), "pages": len(stored.pages),
                                "mismatches": bad, "missing": len(missing)})
        if bad or missing:
            failures.append(f"{key}: {bad} glyph mismatches, {len(missing)} missing glyphs")
    return summary, failures
