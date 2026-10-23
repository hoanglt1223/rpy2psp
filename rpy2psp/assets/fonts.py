"""Glyph atlases for the PSP: every character the story can display, per font and size.

Glyphs are rendered with FreeType (through Pillow), packed on shelves into
512x512 pages, quantised to 4-bit coverage, and stored as swizzled GE indexed
textures:

- T4 (GU_PSM_T4): one 4-bit coverage value per pixel; the engine supplies a
  16-entry CLUT that maps coverage to its text colour.
- T8 (GU_PSM_T8) when the game uses outlines: low nibble = fill coverage,
  high nibble = outline coverage (the fill dilated by the outline width). The
  engine draws twice with two 256-entry CLUTs, outline colour then text colour.
"""

import io
import os
import unicodedata

import numpy as np
from fontTools.ttLib import TTFont
from PIL import ImageFont

from .images import AssetError, swizzle, unswizzle

GE_T4 = 4
GE_T8 = 5
PAGE = 512
PAD = 1


def find_font(game, name, language=None, font_dirs=()):
    """Locate a font named by the game; returns (label, bytes). Raises AssetError with the searched places."""
    cands = []
    if language:
        cands += [f"tl/{language}/{name}", f"tl/{language}/fonts/{name}"]
    cands += [name, "fonts/" + name]
    for c in cands:
        rel = game.resolve(c)
        if rel is not None and rel not in game.listed:
            return rel, game.read(rel)
    for d in font_dirs:
        for c in (name, os.path.splitext(name)[0] + ".woff2", os.path.splitext(name)[0] + ".otf"):
            p = os.path.join(d, c)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return p, f.read()
    raise AssetError(f"font {name!r} not found in the game or in --font-dir {list(font_dirs)}; "
                     f"Ren'Py ships it with the SDK (renpy/common, sdk-fonts)")


class GlyphAtlas:
    def __init__(self, size, fmt, pages, glyphs, ascent, descent, line_height, outline):
        self.size = size
        self.fmt = fmt
        self.pages = pages          # list of swizzled page bytes
        self.glyphs = glyphs        # codepoint -> dict(page, x, y, w, h, bx, by, advance)
        self.ascent = ascent
        self.descent = descent
        self.line_height = line_height
        self.outline = outline


def coverage_of(font_bytes):
    f = TTFont(io.BytesIO(font_bytes), fontNumber=0, lazy=True)
    return set(f.getBestCmap() or {})


def render_glyph(font, ch, outline):
    """Return (coverage uint8 array HxW, outline array or None, bx, by, advance)."""
    mask, offset = font.getmask2(ch, mode="L")
    w, h = mask.size
    cov = np.asarray(mask, dtype=np.uint8).reshape(h, w) if w and h else np.zeros((0, 0), np.uint8)
    advance = font.getlength(ch)
    bx, top = offset
    ascent = font.getmetrics()[0]
    by = ascent - top  # distance from baseline up to the bitmap top
    if outline and cov.size:
        pad = outline
        big = np.pad(cov, pad)
        dil = big.copy()
        for dy in range(-pad, pad + 1):
            for dx in range(-pad, pad + 1):
                if dx * dx + dy * dy <= pad * pad:
                    dil = np.maximum(dil, np.roll(np.roll(big, dy, 0), dx, 1))
        return big, dil, bx - pad, by + pad, advance
    return cov, None, bx, by, advance


def quantise(a):
    return ((a.astype(np.uint16) * 15 + 127) // 255).astype(np.uint8)


def build_atlas(font_bytes, size, chars, outline=0):
    font = ImageFont.truetype(io.BytesIO(font_bytes), size)
    ascent, descent = font.getmetrics()
    fmt = GE_T8 if outline else GE_T4
    pages = [np.zeros((PAGE, PAGE), np.uint8)]
    glyphs = {}
    x = y = shelf = 0
    for ch in sorted(set(chars), key=ord):
        if ch in "\n\r\t" or unicodedata.category(ch) in ("Cc", "Zl", "Zp"):
            continue
        cov, out, bx, by, adv = render_glyph(font, ch, outline)
        h, w = cov.shape
        if w > PAGE or h > PAGE:
            raise AssetError(f"glyph {ch!r} at {size}px is larger than a page")
        if x + w + PAD > PAGE:
            x, y, shelf = 0, y + shelf + PAD, 0
        if y + h + PAD > PAGE:
            pages.append(np.zeros((PAGE, PAGE), np.uint8))
            x = y = shelf = 0
        q = quantise(cov)
        if out is not None:
            q = q | (quantise(out) << 4)
        if w and h:
            pages[-1][y:y + h, x:x + w] = q
        glyphs[ord(ch)] = {"page": len(pages) - 1, "x": x, "y": y, "w": w, "h": h, "bx": bx, "by": by,
                           "advance": adv}
        x += w + PAD
        shelf = max(shelf, h)
    used_h = [PAGE] * len(pages)
    if pages:
        last_h = y + shelf + PAD
        used_h[-1] = max(8, (last_h + 7) // 8 * 8)
    packed = []
    for page, ph in zip(pages, used_h):
        page = page[:ph]
        if fmt == GE_T4:
            raw = (page[:, 0::2] | (page[:, 1::2] << 4)).astype(np.uint8)
            packed.append((swizzle(raw.tobytes(), PAGE // 2, ph), ph))
        else:
            packed.append((swizzle(page.tobytes(), PAGE, ph), ph))
    return GlyphAtlas(size, fmt, packed, glyphs, ascent, descent, ascent + descent, outline)


def page_pixels(atlas, index):
    """Unswizzle and unpack one page back to a 2-D array of 4-bit (or packed 8-bit) values."""
    data, ph = atlas.pages[index]
    if atlas.fmt == GE_T4:
        raw = np.frombuffer(unswizzle(data, PAGE // 2, ph), np.uint8).reshape(ph, PAGE // 2)
        out = np.zeros((ph, PAGE), np.uint8)
        out[:, 0::2] = raw & 15
        out[:, 1::2] = raw >> 4
        return out
    return np.frombuffer(unswizzle(data, PAGE, ph), np.uint8).reshape(ph, PAGE)


def verify_atlas(font_bytes, atlas):
    """Re-render every glyph and compare with the unswizzled atlas. Returns the number of mismatches."""
    font = ImageFont.truetype(io.BytesIO(font_bytes), atlas.size)
    pages = [page_pixels(atlas, i) for i in range(len(atlas.pages))]
    bad = 0
    for cp, g in atlas.glyphs.items():
        cov, out, _, _, _ = render_glyph(font, chr(cp), atlas.outline)
        q = quantise(cov)
        if out is not None:
            q = q | (quantise(out) << 4)
        got = pages[g["page"]][g["y"]:g["y"] + g["h"], g["x"]:g["x"] + g["w"]]
        if got.shape != q.shape or not np.array_equal(got, q):
            bad += 1
    return bad
