"""Image recipes -> PSP textures (DXT1/DXT5 in the GE's native block order).

A recipe from manifest.json is rendered at the game's resolution with Pillow,
scaled into PSP screen space, split into tiles of at most 512x512 (the GE
limit), and compressed with etcpak. The GE reads DXT as a row-major grid of
4x4 blocks and has no swizzled DXT mode, so blocks are only reordered into the
PSP block layout (indices first, then colours; DXT5 alpha after the colour
block; see PPSSPP GPU/Common/TextureDecoder.h). `decode_texture` is an
independent decoder with the GE's RGB565 expansion, used for verification.
"""

import io
import math

import etcpak
import numpy as np
from PIL import Image

GE_DXT1 = 8
GE_DXT5 = 10
MAX_TEX = 512


class AssetError(Exception):
    pass


# ---- recipes ------------------------------------------------------------------------

def render_recipe(game, recipe):
    """Render a manifest image recipe to an RGBA Pillow image (source pixels)."""
    op = recipe["op"]
    if op == "file":
        try:
            with Image.open(io.BytesIO(game.read(recipe["path"]))) as im:
                im.load()
                return im.convert("RGBA")
        except FileNotFoundError:
            raise AssetError(f"image file {recipe['path']!r} not found") from None
        except OSError as e:
            raise AssetError(f"cannot decode {recipe['path']!r}: {e}") from None
    if op == "solid":
        return Image.new("RGBA", (1, 1), tuple(recipe["color"]))
    if op == "null":
        return Image.new("RGBA", tuple(max(1, v) for v in recipe["size"]), (0, 0, 0, 0))
    if op == "composite":
        out = Image.new("RGBA", tuple(recipe["size"]), (0, 0, 0, 0))
        for x, y, part in recipe["parts"]:
            img = render_recipe(game, part)
            if part["op"] == "solid":
                img = img.resize(out.size)
            _paste(out, img, x, y)
        return out
    child = render_recipe(game, recipe["child"]) if "child" in recipe else None
    if op == "scale":
        return _resize(child, tuple(max(1, int(v)) for v in recipe["size"]))
    if op == "factor":
        fx, fy = recipe["factor"]
        return _resize(child, (max(1, int(child.width * fx)), max(1, int(child.height * fy))))
    if op == "crop":
        x, y, w, h = recipe["rect"]
        return child.crop((x, y, x + w, y + h))
    if op == "flip":
        if recipe.get("horizontal"):
            child = child.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        if recipe.get("vertical"):
            child = child.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        return child
    if op in ("grayscale", "sepia", "matrix"):
        if op == "grayscale":
            m = [0.299, 0.587, 0.114, 0, 0] * 3 + [0, 0, 0, 1, 0]
        elif op == "sepia":
            m = [0.393, 0.769, 0.189, 0, 0, 0.349, 0.686, 0.168, 0, 0, 0.272, 0.534, 0.131, 0, 0, 0, 0, 0, 1, 0]
        else:
            m = recipe["matrix"]
        return apply_matrix(child, m)
    if op == "alpha":
        a = np.asarray(child, dtype=np.float32).copy()
        a[..., 3] *= recipe["alpha"]
        return Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8), "RGBA")
    if op == "frame":
        return child
    raise AssetError(f"unknown image recipe op {op!r}")


def _paste(dst, img, x, y):
    layer = Image.new("RGBA", dst.size, (0, 0, 0, 0))
    layer.paste(img, (x, y))
    dst.alpha_composite(layer)


def apply_matrix(img, m):
    """Ren'Py im.MatrixColor: 4x5 matrix on 0..1 RGBA values (offsets in the 5th column)."""
    a = np.asarray(img, dtype=np.float32) / 255.0
    mat = np.asarray(m, dtype=np.float32).reshape(4, 5)
    out = a @ mat[:, :4].T + mat[:, 4]
    return Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA")


def _resize(img, size):
    """Resize with premultiplied alpha so transparent edges do not darken."""
    if img.size == size:
        return img
    a = np.asarray(img, dtype=np.float32)
    alpha = a[..., 3:4] / 255.0
    pre = np.concatenate([a[..., :3] * alpha, a[..., 3:4]], axis=-1)
    chans = [Image.fromarray(pre[..., i]).resize(size, Image.Resampling.LANCZOS) for i in range(4)]
    r = np.stack([np.asarray(c, dtype=np.float32) for c in chans], axis=-1)
    al = np.clip(r[..., 3:4], 0, 255)
    rgb = np.where(al > 0, r[..., :3] * 255.0 / np.maximum(al, 1e-6), 0)
    out = np.concatenate([rgb, al], axis=-1)
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


def screen_scale(source_size):
    w, h = source_size
    return min(480.0 / w, 272.0 / h)


def to_screen_space(img, scale):
    w = max(1, int(round(img.width * scale)))
    h = max(1, int(round(img.height * scale)))
    return _resize(img, (w, h))


# ---- DXT in PSP layout ---------------------------------------------------------------

def _pow2(n):
    return 1 << max(3, math.ceil(math.log2(max(n, 1))))


def _dds_to_psp(blocks, fmt):
    b = np.frombuffer(blocks, dtype=np.uint8)
    if fmt == GE_DXT1:
        b = b.reshape(-1, 8)
        return np.concatenate([b[:, 4:8], b[:, 0:4]], axis=1).tobytes()
    b = b.reshape(-1, 16)
    color = b[:, 8:16]
    color_psp = np.concatenate([color[:, 4:8], color[:, 0:4]], axis=1)
    return np.concatenate([color_psp, b[:, 2:8], b[:, 0:2]], axis=1).tobytes()


def encode_tile(rgba, fmt):
    """rgba: HxWx4 uint8 with W, H multiples of 4 -> PSP-layout DXT blocks."""
    h, w, _ = rgba.shape
    # The GE expands RGB565 by shifting (31 -> 248) without bit replication; scale the input
    # so the encoder's endpoint choice targets what the GE will output.
    pre = rgba.astype(np.float32)
    pre[..., :3] = np.minimum(pre[..., :3] * (255.0 / 248.0), 255.0)
    data = np.ascontiguousarray(np.round(pre).astype(np.uint8)).tobytes()
    raw = etcpak.compress_bc1(data, w, h) if fmt == GE_DXT1 else etcpak.compress_bc3(data, w, h)
    return _dds_to_psp(raw, fmt)


def decode_blocks(data, fmt, width, rows):
    """Decode PSP-layout DXT blocks exactly as the GE does (PPSSPP's decoder semantics)."""
    bs = 8 if fmt == GE_DXT1 else 16
    bw, bh = width // 4, rows // 4
    b = np.frombuffer(data, dtype=np.uint8)[:bw * bh * bs].reshape(bh, bw, bs).astype(np.int32)
    lines = b[..., 0:4]
    c1 = b[..., 4] | (b[..., 5] << 8)
    c2 = b[..., 6] | (b[..., 7] << 8)

    def rgb(c):
        return np.stack([(c >> 8) & 0xF8, (c >> 3) & 0xFC, (c << 3) & 0xF8], axis=-1)
    r1, r2 = rgb(c1), rgb(c2)
    four = (c1 > c2)[..., None]
    col2 = np.where(four, (2 * r1 + r2) // 3, (r1 + r2) // 2)
    col3 = np.where(four, (2 * r2 + r1) // 3, 0)
    palette = np.stack([r1, r2, col2, col3], axis=-2)  # bh, bw, 4, 3
    alpha_pal = np.full((bh, bw, 4), 255, np.int32)
    if fmt == GE_DXT1:
        alpha_pal[..., 3] = np.where(c1 > c2, 255, 0)
    idx = np.zeros((bh, bw, 4, 4), np.int32)
    for y in range(4):
        for x in range(4):
            idx[..., y, x] = (lines[..., y] >> (2 * x)) & 3
    rgbout = np.take_along_axis(palette[:, :, None, None, :, :], idx[..., None, None], axis=4)[..., 0, :]
    if fmt == GE_DXT1:
        aout = np.take_along_axis(alpha_pal[:, :, None, None, :], idx[..., None], axis=4)[..., 0]
    else:
        a1 = b[..., 14]
        a2 = b[..., 15]
        b64 = b.astype(np.int64)
        bits = (b64[..., 8] | (b64[..., 9] << 8) | (b64[..., 10] << 16) | (b64[..., 11] << 24)
                | (b64[..., 12] << 32) | (b64[..., 13] << 40))
        apal = np.zeros((bh, bw, 8), np.int32)
        apal[..., 0], apal[..., 1] = a1, a2
        gt = a1 > a2
        for n in range(1, 7):
            lerp8 = ((a1 * ((7 - n) << 8)) // 7 + (a2 * (n << 8)) // 7 + 31) >> 8
            if n <= 4:
                lerp6 = ((a1 * ((5 - n) << 8)) // 5 + (a2 * (n << 8)) // 5 + 31) >> 8
            else:
                lerp6 = 0 if n == 5 else 255
            apal[..., n + 1] = np.where(gt, lerp8, lerp6)
        aidx = np.zeros((bh, bw, 4, 4), np.int64)
        for y in range(4):
            for x in range(4):
                aidx[..., y, x] = (bits >> (12 * y + 3 * x)) & 7
        aout = np.take_along_axis(apal[:, :, None, None, :], aidx[..., None].astype(np.int32), axis=4)[..., 0]
    out = np.concatenate([rgbout, aout[..., None]], axis=-1)  # bh, bw, 4, 4, 4
    return out.transpose(0, 2, 1, 3, 4).reshape(rows, width, 4).astype(np.uint8)


class Texture:
    """A converted image: tiles of DXT data plus placement metadata."""

    def __init__(self, fmt, width, height, source_size, tiles):
        self.fmt = fmt
        self.width = width          # used size in PSP pixels
        self.height = height
        self.source_size = source_size
        self.tiles = tiles          # list of dict(x, y, w, h, tex_w, tex_h, bufw, rows, data)


def convert_image(img, scale, force_fmt=None):
    """Scale an RGBA image into PSP space and encode it as DXT tiles."""
    small = to_screen_space(img, scale)
    a = np.asarray(small, dtype=np.uint8)
    opaque = bool((a[..., 3] == 255).all())
    fmt = force_fmt or (GE_DXT1 if opaque else GE_DXT5)
    tiles = []
    for ty in range(0, small.height, MAX_TEX):
        for tx in range(0, small.width, MAX_TEX):
            tile = a[ty:ty + MAX_TEX, tx:tx + MAX_TEX]
            h, w = tile.shape[:2]
            tex_w = _pow2(w)
            rows = (h + 3) // 4 * 4
            # pad by edge replication so filtering at the border does not pull in black
            padded = np.pad(tile, ((0, rows - h), (0, tex_w - w), (0, 0)), mode="edge")
            if not opaque:
                padded[h:, :, 3] = 0
                padded[:, w:, 3] = 0
            tiles.append({"x": tx, "y": ty, "w": w, "h": h, "tex_w": tex_w, "tex_h": _pow2(h), "bufw": tex_w,
                          "rows": rows, "data": encode_tile(padded, fmt)})
    return Texture(fmt, small.width, small.height, img.size, tiles), small


def decode_texture(tex):
    """Reassemble the decoded tiles into one RGBA array of the used size."""
    out = np.zeros((tex.height, tex.width, 4), np.uint8)
    for t in tex.tiles:
        px = decode_blocks(t["data"], tex.fmt, t["bufw"], t["rows"])
        out[t["y"]:t["y"] + t["h"], t["x"]:t["x"] + t["w"]] = px[:t["h"], :t["w"]]
    return out


def compare(reference, decoded):
    """PSNR (dB) of the displayed (alpha-premultiplied) colour and mean/max absolute alpha error."""
    ref = np.asarray(reference, dtype=np.float64)
    dec = decoded.astype(np.float64)
    pr = ref[..., :3] * ref[..., 3:4] / 255.0
    pd = dec[..., :3] * dec[..., 3:4] / 255.0
    mse = ((pr - pd) ** 2).mean()
    psnr = float("inf") if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
    aerr = np.abs(ref[..., 3] - dec[..., 3])
    return {"psnr": psnr, "alpha_mean": float(aerr.mean()), "alpha_max": float(aerr.max())}


# ---- swizzling (uncompressed textures such as glyph atlases) ------------------------------

def swizzle(data, width_bytes, height):
    """GE swizzle: 16-byte x 8-row blocks stored contiguously, row-major."""
    if width_bytes % 16 or height % 8:
        raise ValueError("swizzle needs width in bytes % 16 == 0 and height % 8 == 0")
    a = np.frombuffer(data, dtype=np.uint8).reshape(height // 8, 8, width_bytes // 16, 16)
    return a.transpose(0, 2, 1, 3).tobytes()


def unswizzle(data, width_bytes, height):
    a = np.frombuffer(data, dtype=np.uint8).reshape(height // 8, width_bytes // 16, 8, 16)
    return a.transpose(0, 2, 1, 3).tobytes()
