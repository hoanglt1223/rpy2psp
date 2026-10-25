"""PSP EBOOT.PBP and PARAM.SFO reading and writing, and the XMB images.

A PBP is a 40-byte header (magic, version, eight section offsets) followed by
the sections PARAM.SFO, ICON0.PNG, ICON1.PMF, PIC0.PNG, PIC1.PNG, SND0.AT3,
DATA.PSP (the engine executable) and DATA.PSAR. `rpy2psp build` takes the
engine's own EBOOT, keeps its DATA.PSP and SFO entries, and writes a new one
with the game's title, ICON0 (144x80) and PIC1 (480x272).
"""

import io
import struct

from PIL import Image, ImageOps

PBP_MAGIC = b"\0PBP"
PBP_VERSION = 0x00010000
SECTIONS = ("PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG", "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR")
SFO_MAGIC = b"\0PSF"
SFO_VERSION = 0x00000101
FMT_UTF8, FMT_INT32 = 0x0204, 0x0404
TITLE_MAX = 128          # bytes of UTF-8, NUL included, as the XMB reserves it
ICON0_SIZE = (144, 80)
PIC1_SIZE = (480, 272)


class EbootError(Exception):
    pass


def read_pbp(data):
    """Returns {section name: bytes} for a PBP file."""
    if len(data) < 40 or data[:4] != PBP_MAGIC:
        raise EbootError("not a PBP file (bad magic)")
    offsets = list(struct.unpack_from("<8I", data, 8)) + [len(data)]
    out = {}
    for i, name in enumerate(SECTIONS):
        start, end = offsets[i], offsets[i + 1]
        if not 40 <= start <= end <= len(data):
            raise EbootError(f"PBP section {name} has a bad offset")
        out[name] = data[start:end]
    return out


def write_pbp(sections):
    unknown = set(sections) - set(SECTIONS)
    if unknown:
        raise EbootError(f"unknown PBP sections: {sorted(unknown)}")
    body, offsets, pos = [], [], 40
    for name in SECTIONS:
        blob = sections.get(name) or b""
        offsets.append(pos)
        body.append(blob)
        pos += len(blob)
    return PBP_MAGIC + struct.pack("<I8I", PBP_VERSION, *offsets) + b"".join(body)


def read_sfo(data):
    """Returns an ordered {key: str | int}."""
    if len(data) < 20 or data[:4] != SFO_MAGIC:
        raise EbootError("not a PARAM.SFO (bad magic)")
    _, key_start, data_start, count = struct.unpack_from("<IIII", data, 4)
    if 20 + 16 * count > len(data) or key_start > len(data) or data_start > len(data):
        raise EbootError("PARAM.SFO is truncated")
    out = {}
    for i in range(count):
        key_off, fmt, length, _max_len, data_off = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        k0 = key_start + key_off
        k1 = data.find(b"\0", k0)
        if k1 < 0:
            raise EbootError("PARAM.SFO key table is not terminated")
        key = data[k0:k1].decode("ascii")
        raw = data[data_start + data_off:data_start + data_off + length]
        if len(raw) != length:
            raise EbootError(f"PARAM.SFO value {key} is truncated")
        if fmt == FMT_INT32:
            out[key] = struct.unpack("<I", raw)[0]
        else:
            out[key] = raw.rstrip(b"\0").decode("utf-8")
    return out


def _align4(n):
    return (n + 3) & ~3


def write_sfo(entries):
    """Keys are written in sorted order, as Sony's tools do."""
    keys = sorted(entries)
    key_table = b""
    key_offsets = []
    for k in keys:
        key_offsets.append(len(key_table))
        key_table += k.encode("ascii") + b"\0"
    key_table += b"\0" * (_align4(len(key_table)) - len(key_table))
    index, values = b"", b""
    for k, key_off in zip(keys, key_offsets):
        v = entries[k]
        if isinstance(v, int):
            raw, fmt, max_len = struct.pack("<I", v), FMT_INT32, 4
        else:
            raw = str(v).encode("utf-8") + b"\0"
            fmt = FMT_UTF8
            max_len = TITLE_MAX if k == "TITLE" else _align4(len(raw))
            if len(raw) > max_len:
                raise EbootError(f"PARAM.SFO value {k} is longer than {max_len - 1} bytes")
        index += struct.pack("<HHIII", key_off, fmt, len(raw), max_len, len(values))
        values += raw + b"\0" * (max_len - len(raw))
    key_start = 20 + len(index)
    data_start = key_start + len(key_table)
    return SFO_MAGIC + struct.pack("<IIII", SFO_VERSION, key_start, data_start, len(keys)) + index + key_table + values


def clip_title(title):
    """Cuts a title to what the XMB stores, on a UTF-8 character boundary."""
    raw = title.strip().encode("utf-8")[:TITLE_MAX - 1]
    return raw.decode("utf-8", errors="ignore")


# ---- XMB images ----------------------------------------------------------------------

def _png(img):
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def _open(data):
    try:
        # Pillow opens the largest size of an .ico and the first frame of an animation.
        with Image.open(io.BytesIO(data)) as im:
            im.load()
            return im.convert("RGBA")
    except (OSError, ValueError) as e:
        raise EbootError(f"cannot decode image: {e}") from None


def icon0_from(data, kind):
    """ICON0 (144x80). A square window icon is fitted with transparent sides; a
    menu background is cropped to fill."""
    img = _open(data)
    if kind == "background":
        out = ImageOps.fit(img, ICON0_SIZE, Image.LANCZOS)
    else:
        img = ImageOps.contain(img, ICON0_SIZE, Image.LANCZOS)
        out = Image.new("RGBA", ICON0_SIZE, (0, 0, 0, 0))
        out.alpha_composite(img, ((ICON0_SIZE[0] - img.width) // 2, (ICON0_SIZE[1] - img.height) // 2))
    return _png(out)


def pic1_from(data):
    """PIC1 (480x272), the XMB background: the menu image cropped to fill, opaque."""
    img = ImageOps.fit(_open(data), PIC1_SIZE, Image.LANCZOS)
    out = Image.new("RGB", PIC1_SIZE, (0, 0, 0))
    out.paste(img, mask=img.getchannel("A"))
    return _png(out)


def make_eboot(engine_pbp, title, icon0=None, pic1=None):
    """A new EBOOT from the engine's: same executable and SFO entries, new title and images."""
    sections = read_pbp(engine_pbp)
    if not sections["DATA.PSP"]:
        raise EbootError("the engine EBOOT has no DATA.PSP executable")
    sfo = read_sfo(sections["PARAM.SFO"])
    sfo["TITLE"] = clip_title(title) or "vnpsp"
    sections["PARAM.SFO"] = write_sfo(sfo)
    sections["ICON0.PNG"] = icon0 or b""
    sections["PIC1.PNG"] = pic1 or b""
    return write_pbp(sections)
