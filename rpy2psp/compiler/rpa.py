"""Reader for Ren'Py .rpa archives (RPA-1.0, RPA-2.0, RPA-3.0).

Format reference: rpatool by Shiz (WTFPL). The index is a zlib-compressed
pickle of {name: [(offset, length[, prefix])]}; it is loaded with an
unpickler that refuses every global except `_codecs.encode` and `bytes`, which
Python 3 uses to pickle bytes at protocol 2. No archive content is executed.
"""

import io
import os
import pickle
import zlib


class RpaError(Exception):
    pass


def _codecs_encode(s, encoding="utf-8"):
    if not isinstance(s, str) or encoding not in ("latin1", "latin-1", "utf-8"):
        raise pickle.UnpicklingError("unexpected _codecs.encode arguments")
    return s.encode(encoding)


class _IndexUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if (module, name) == ("_codecs", "encode"):
            return _codecs_encode
        if module in ("builtins", "__builtin__") and name == "bytes":
            return bytes
        raise pickle.UnpicklingError(f"global {module}.{name} is not allowed in an .rpa index")


def load_index(data):
    try:
        raw = zlib.decompress(data)
    except zlib.error as e:
        raise RpaError(f"index is not zlib data: {e}") from e
    try:
        index = _IndexUnpickler(io.BytesIO(raw), encoding="bytes").load()
    except (pickle.UnpicklingError, EOFError, ValueError, TypeError) as e:
        raise RpaError(f"bad index pickle: {e}") from e
    if not isinstance(index, dict):
        raise RpaError("index is not a dict")
    out = {}
    for k, v in index.items():
        name = k.decode("utf-8") if isinstance(k, bytes) else k
        if not isinstance(name, str) or not isinstance(v, list):
            raise RpaError("malformed index entry")
        parts = []
        for t in v:
            if not isinstance(t, tuple) or len(t) not in (2, 3):
                raise RpaError(f"malformed index entry for {name}")
            off, ln = int(t[0]), int(t[1])
            prefix = t[2] if len(t) == 3 else b""
            if isinstance(prefix, str):
                prefix = prefix.encode("latin1")
            parts.append((off, ln, prefix))
        out[name.replace("\\", "/")] = parts
    return out


class RpaArchive:
    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        header = self._f.readline(200)
        try:
            self.index = self._read_index(header)
        except Exception:
            self._f.close()
            raise

    def _read_index(self, header):
        if header.startswith(b"RPA-3.0 "):
            parts = header.split()
            offset, key = int(parts[1], 16), int(parts[2], 16)
            self._f.seek(offset)
            index = load_index(self._f.read())
            return {n: [(o ^ key, l ^ key, p) for o, l, p in v] for n, v in index.items()}
        if header.startswith(b"RPA-2.0 "):
            offset = int(header.split()[1], 16)
            self._f.seek(offset)
            return load_index(self._f.read())
        rpi = os.path.splitext(self.path)[0] + ".rpi"
        if self.path.lower().endswith(".rpa") and os.path.exists(rpi):
            with open(rpi, "rb") as f:
                return load_index(f.read())
        raise RpaError(f"{os.path.basename(self.path)}: unknown archive header {header[:16]!r}")

    def names(self):
        return sorted(self.index)

    def read(self, name):
        out = []
        size = os.fstat(self._f.fileno()).st_size
        for off, ln, prefix in self.index[name]:
            if off < 0 or ln < 0 or off + ln > size:
                raise RpaError(f"{name}: entry outside the archive")
            self._f.seek(off)
            out.append(prefix + self._f.read(ln - len(prefix)))
        return b"".join(out)

    def close(self):
        self._f.close()


def write_rpa3(path, files, key=0x42424242):
    """Write an RPA-3.0 archive from {name: bytes}. Used by tests to build fixtures."""
    with open(path, "wb") as f:
        f.write(b"RPA-3.0 %016x %08x\n" % (0, key))
        index = {}
        for name, data in files.items():
            off = f.tell()
            f.write(data)
            index[name] = [(off ^ key, len(data) ^ key, b"")]
        idx_off = f.tell()
        f.write(zlib.compress(pickle.dumps(index, 2)))
        f.seek(0)
        f.write(b"RPA-3.0 %016x %08x\n" % (idx_off, key))
