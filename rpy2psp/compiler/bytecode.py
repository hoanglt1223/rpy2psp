"""VN bytecode: opcode table, assembler, binary writer, reader, and disassembler.

The format is specified in docs/dev/bytecode.md. All integers are little-endian.
"""

import hashlib
import struct

MAGIC = b"VNB1"
VERSION = 2
NONE16 = 0xFFFF
NONE32 = 0xFFFFFFFF

# Operand codes: b u8, h u16, i i32, f f32, d f64, s string id (u32), v symbol (u16),
# a code address (u32), S u8 count + string ids, M u8 count + (string id, address) pairs.
OPCODES = [
    ("NOP", ""), ("PUSH_NONE", ""), ("PUSH_TRUE", ""), ("PUSH_FALSE", ""),
    ("PUSH_INT", "i"), ("PUSH_FLOAT", "d"), ("PUSH_STR", "s"),
    ("LOAD", "v"), ("STORE", "v"), ("STORE_DEFAULT", "v"), ("POP", ""), ("DUP", ""),
    ("BUILD_LIST", "h"), ("BUILD_TUPLE", "h"), ("BUILD_DICT", "h"), ("BUILD_SET", "h"),
    ("SUBSCR", ""), ("SLICE", ""), ("STORE_SUBSCR", ""),
    ("BINARY", "b"), ("UNARY", "b"), ("COMPARE", "b"),
    ("JUMP", "a"), ("JUMP_IF_FALSE", "a"), ("JUMP_IF_TRUE", "a"),
    ("JUMP_IF_FALSE_OR_POP", "a"), ("JUMP_IF_TRUE_OR_POP", "a"),
    ("CALL_FN", "bb"), ("CALL_METHOD", "bb"),
    ("CALL", "a"), ("CALL_DYNAMIC", ""), ("JUMP_DYNAMIC", ""), ("RETURN", ""), ("BIND", "v"),
    ("SAY", "hshb"), ("SAY_DYNAMIC", "hhb"), ("SAY_ATTRS", "hbS"), ("MENU", "M"),
    ("PAUSE", ""), ("WITH", "h"), ("SHOW", "h"), ("SHOW_DYNAMIC", "h"), ("SCENE", "h"), ("HIDE", "h"),
    ("PLAY", "h"), ("STOP", "bf"), ("VOICE", "h"), ("VOICE_SUSTAIN", ""),
    ("WINDOW", "bh"), ("NVL", "bh"), ("MOVIE", "h"),
]
OP = {name: code for code, (name, _) in enumerate(OPCODES)}
OPERANDS = {code: fmt for code, (_, fmt) in enumerate(OPCODES)}

BINOPS = ["ADD", "SUB", "MUL", "DIV", "FLOORDIV", "MOD"]
UNOPS = ["NEG", "POS", "NOT"]
CMPOPS = ["EQ", "NE", "LT", "LE", "GT", "GE", "IN", "NOTIN", "IS", "ISNOT"]

# Builtin functions for CALL_FN: (name, min args, max args)
FUNCTIONS = [
    ("len", 1, 1), ("str", 0, 1), ("int", 0, 1), ("float", 0, 1), ("bool", 0, 1),
    ("min", 1, 8), ("max", 1, 8), ("abs", 1, 1), ("round", 1, 2), ("set", 0, 1), ("list", 0, 1),
    ("renpy.random.randint", 2, 2), ("renpy.random.choice", 1, 1), ("renpy.random.random", 0, 0),
    ("renpy.input", 3, 3), ("renpy.notify", 1, 1), ("renpy.full_restart", 0, 0),
    ("renpy.block_rollback", 0, 0), ("renpy.save_persistent", 0, 0), ("renpy.force_autosave", 0, 0),
    ("renpy.has_label", 1, 1), ("renpy.showing", 1, 1), ("renpy.music.set_volume", 3, 3),
    ("menu_set_add", 2, 2), ("tuple", 0, 1),
]
FN = {name: i for i, (name, _, _) in enumerate(FUNCTIONS)}
METHODS = ["strip", "lower", "upper", "capitalize", "title", "add", "append", "remove", "discard",
           "pop", "get", "keys", "values", "items", "count", "index", "startswith", "endswith",
           "replace", "split", "join"]
METHOD = {name: i for i, name in enumerate(METHODS)}

WINDOW_MODES = ["show", "hide", "auto", "auto-show", "auto-hide"]
NVL_MODES = ["clear", "show", "hide"]
SAY_INTERACT = 1


class Label:
    __slots__ = ("addr", "name")

    def __init__(self, name=None):
        self.addr = None
        self.name = name


class Assembler:
    def __init__(self, strings):
        self.strings = strings
        self.code = bytearray()
        self.fixups = []  # (offset, Label)
        self.lines = []   # (addr, where) for the debug line table

    def here(self):
        return len(self.code)

    def place(self, label):
        label.addr = len(self.code)

    def mark(self, where):
        if not self.lines or self.lines[-1][1] != where:
            self.lines.append((len(self.code), where))

    def emit(self, name, *args):
        code = OP[name]
        fmt = OPERANDS[code]
        self.code.append(code)
        ai = 0
        for c in fmt:
            v = args[ai]
            ai += 1
            if c == "b":
                self.code += struct.pack("<B", v)
            elif c == "h" or c == "v":
                self.code += struct.pack("<H", NONE16 if v is None else v)
            elif c == "i":
                self.code += struct.pack("<i", v)
            elif c == "f":
                self.code += struct.pack("<f", v)
            elif c == "d":
                self.code += struct.pack("<d", v)
            elif c == "s":
                self.code += struct.pack("<I", NONE32 if v is None else self.strings.intern(v))
            elif c == "a":
                self.fixups.append((len(self.code), v))
                self.code += b"\0\0\0\0"
            elif c == "S":
                self.code += struct.pack("<B", len(v))
                for s in v:
                    self.code += struct.pack("<I", self.strings.intern(s))
            elif c == "M":
                self.code += struct.pack("<B", len(v))
                for text, lab in v:
                    self.code += struct.pack("<I", self.strings.intern(text))
                    self.fixups.append((len(self.code), lab))
                    self.code += b"\0\0\0\0"
        if ai != len(args):
            raise ValueError(f"{name}: expected {ai} operands, got {len(args)}")

    def resolve(self):
        for off, lab in self.fixups:
            if lab.addr is None:
                raise ValueError(f"unplaced label {lab.name}")
            self.code[off:off + 4] = struct.pack("<I", lab.addr)
        self.fixups.clear()


class StringTable:
    def __init__(self):
        self.ids = {}
        self.items = []

    def intern(self, s):
        if not isinstance(s, str):
            raise TypeError(f"string table entry must be str, got {type(s).__name__}")
        i = self.ids.get(s)
        if i is None:
            i = self.ids[s] = len(self.items)
            self.items.append(s)
        return i


def decode_ops(code, strings=None):
    """Yield (addr, name, operands) for every instruction."""
    pos = 0
    n = len(code)
    while pos < n:
        addr = pos
        op = code[pos]
        pos += 1
        if op >= len(OPCODES):
            raise ValueError(f"bad opcode {op} at {addr}")
        name, fmt = OPCODES[op]
        vals = []
        for c in fmt:
            if c == "b":
                vals.append(code[pos]); pos += 1
            elif c in "hv":
                vals.append(struct.unpack_from("<H", code, pos)[0]); pos += 2
            elif c == "i":
                vals.append(struct.unpack_from("<i", code, pos)[0]); pos += 4
            elif c == "f":
                vals.append(struct.unpack_from("<f", code, pos)[0]); pos += 4
            elif c == "d":
                vals.append(struct.unpack_from("<d", code, pos)[0]); pos += 8
            elif c in "sa":
                vals.append(struct.unpack_from("<I", code, pos)[0]); pos += 4
            elif c == "S":
                cnt = code[pos]; pos += 1
                lst = list(struct.unpack_from(f"<{cnt}I", code, pos)); pos += 4 * cnt
                vals.append(lst)
            elif c == "M":
                cnt = code[pos]; pos += 1
                lst = []
                for _ in range(cnt):
                    lst.append(struct.unpack_from("<II", code, pos)); pos += 8
                vals.append(lst)
        yield addr, name, vals


# ---- container ----------------------------------------------------------------------

def _pack_strings(items):
    blob = bytearray()
    offs = []
    for s in items:
        offs.append(len(blob))
        blob += s.encode("utf-8") + b"\0"
    head = struct.pack("<I", len(items)) + b"".join(struct.pack("<I", o) for o in offs)
    return head + bytes(blob)


def _u16(v):
    return struct.pack("<H", NONE16 if v is None else v)


def _u32(v):
    return struct.pack("<I", NONE32 if v is None else v)


def _rgba(c):
    return bytes(c) if c is not None else b"\0\0\0\0"


def encode_atl(instrs, strings):
    """Serialize compiled ATL instructions (see bytecode.md, Transforms)."""
    out = bytearray()
    for ins in instrs:
        kind = ins[0]
        if kind in ("set", "interp"):
            if kind == "set":
                out += b"\x01"
            else:
                out += b"\x02" + struct.pack("<Bf", ins[1], ins[2])
            props = ins[-1]
            out += struct.pack("<B", len(props))
            for pid, comps in props:
                out += struct.pack("<BB", pid, len(comps))
                for k, v in comps:
                    out += struct.pack("<Bf", k, v)
        elif kind == "pause":
            out += b"\x03" + struct.pack("<f", ins[1])
        elif kind == "repeat":
            out += b"\x04" + struct.pack("<i", ins[1])
        elif kind == "block":
            body = encode_atl(ins[1], strings)
            out += b"\x05" + struct.pack("<I", len(body)) + body
        elif kind == "parallel":
            out += b"\x06" + struct.pack("<B", len(ins[1]))
            for blk in ins[1]:
                body = encode_atl(blk, strings)
                out += struct.pack("<I", len(body)) + body
        elif kind == "choice":
            out += b"\x07" + struct.pack("<B", len(ins[1]))
            for weight, blk in ins[1]:
                body = encode_atl(blk, strings)
                out += struct.pack("<fI", weight, len(body)) + body
        elif kind == "on":
            body = encode_atl(ins[2], strings)
            out += b"\x08" + struct.pack("<B", len(ins[1]))
            for e in ins[1]:
                out += _u32(strings.intern(e))
            out += struct.pack("<I", len(body)) + body
        elif kind == "time":
            out += b"\x09" + struct.pack("<f", ins[1])
        elif kind == "child":
            out += b"\x0a" + _u16(ins[1]) + _u16(ins[2])
        elif kind == "event":
            out += b"\x0b" + _u32(strings.intern(ins[1]))
        else:
            raise ValueError(f"unknown ATL instruction {kind}")
    return bytes(out)


def build_container(prog):
    """Serialize a compiled Program (see compile.py) into the .vnb byte layout."""
    st = prog.strings
    secs = []

    code = bytes(prog.asm.code)
    secs.append((b"CODE", code))

    syms = bytearray(struct.pack("<I", len(prog.symbols)))
    for name, kind in prog.symbol_list():
        syms += _u32(st.intern(name)) + struct.pack("<BBH", kind, 0, 0)

    labels = bytearray(struct.pack("<I", len(prog.labels)))
    for name, info in sorted(prog.labels.items()):
        labels += _u32(st.intern(name)) + _u32(info.entry.addr)

    chars = bytearray(struct.pack("<I", len(prog.characters)))
    for c in prog.characters:
        chars += (_u32(None if c.name is None else st.intern(c.name)) + _rgba(c.who_color) + _rgba(c.what_color)
                  + struct.pack("<BB", c.kind, c.flags) + _u32(None if c.image is None else st.intern(c.image))
                  + _u16(c.side_image) + _u32(st.intern(c.what_prefix)) + _u32(st.intern(c.what_suffix))
                  + _u32(st.intern(c.who_prefix)) + _u32(st.intern(c.who_suffix)) + _u16(c.ctc))

    images = bytearray(struct.pack("<I", len(prog.images)))
    for name in prog.image_order:
        images += _u32(st.intern(" ".join(name))) + _u16(prog.images[name])

    disps = bytearray(struct.pack("<I", len(prog.displayables)))
    for d in prog.displayables:
        disps += struct.pack("<B", d.kind) + _u16(d.asset) + _rgba(d.color) + struct.pack(
            "<ff", d.width, d.height) + _u16(d.transform) + _u16(d.child) + _u32(
            None if d.text is None else st.intern(d.text)) + struct.pack("<4f", *d.borders)

    trs = bytearray(struct.pack("<I", len(prog.transforms)))
    blobs = [encode_atl(t, st) for t in prog.transforms]
    for b in blobs:
        trs += struct.pack("<I", len(b)) + b

    trans = bytearray(struct.pack("<I", len(prog.transitions)))
    for t in prog.transitions:
        trans += struct.pack("<BB", t.kind, t.warper) + struct.pack("<6f", *t.params) + _rgba(t.color) + \
            _u16(t.enter) + _u16(t.leave) + struct.pack("<BB", t.enter_warper, t.leave_warper)

    shows = bytearray(struct.pack("<I", len(prog.shows)))
    for s in prog.shows:
        shows += (_u32(st.intern(s.tag)) + struct.pack("<B", len(s.attrs))
                  + b"".join(_u32(st.intern(a)) for a in s.attrs)
                  + struct.pack("<B", len(s.at)) + b"".join(_u16(t) for t in s.at)
                  + _u32(None if s.as_ is None else st.intern(s.as_))
                  + struct.pack("<B", len(s.behind)) + b"".join(_u32(st.intern(b)) for b in s.behind)
                  + struct.pack("<iB", s.zorder or 0, 1 if s.zorder is not None else 0)
                  + _u32(st.intern(s.layer)) + _u16(s.atl) + _u16(s.displayable))

    audio = bytearray(struct.pack("<I", len(prog.audio)))
    for a in prog.audio:
        audio += struct.pack("<BB", a.channel, a.flags) + struct.pack("<fff", a.fadein, a.fadeout, a.volume) + \
            struct.pack("<B", len(a.assets)) + b"".join(_u16(x) for x in a.assets)

    chans = bytearray(struct.pack("<I", len(prog.channels)))
    for ch in prog.channels:
        chans += _u32(st.intern(ch.name)) + struct.pack("<BB", ch.mixer, 1 if ch.loop else 0)

    assets = bytearray(struct.pack("<I", len(prog.assets)))
    for key, kind in prog.asset_list():
        assets += _u32(st.intern(key)) + struct.pack("<B", kind)

    head = struct.pack("<IIIIff", prog.init_entry.addr, prog.defaults_entry.addr, prog.start_entry.addr,
                       prog.flags, float(prog.width), float(prog.height))
    secs.insert(0, (b"HEAD", head))
    secs += [(b"SYMS", bytes(syms)), (b"LABL", bytes(labels)), (b"CHRS", bytes(chars)),
             (b"IMGS", bytes(images)), (b"DISP", bytes(disps)), (b"TRFM", bytes(trs)),
             (b"TRAN", bytes(trans)), (b"SHOW", bytes(shows)), (b"AUDI", bytes(audio)),
             (b"CHAN", bytes(chans)), (b"ASET", bytes(assets))]
    meta = [prog.config.get(k) for k in ("name", "version")]
    secs.append((b"META", b"".join(_u32(st.intern(str(v)) if v else None) for v in meta)))
    secs.append((b"STRS", _pack_strings(st.items)))

    header_len = 12 + 12 * len(secs)
    out = bytearray(MAGIC + struct.pack("<HHI", VERSION, 0, len(secs)))
    offset = header_len
    table = bytearray()
    body = bytearray()
    for sid, data in secs:
        pad = (-len(body)) % 4
        body += b"\0" * pad
        table += sid + struct.pack("<II", header_len + len(body), len(data))
        body += data
    out += table + body
    return bytes(out)


def read_container(data):
    """Return {section id: bytes} after validating the header."""
    if data[:4] != MAGIC:
        raise ValueError("not a VNB file")
    version, _, count = struct.unpack_from("<HHI", data, 4)
    if version != VERSION:
        raise ValueError(f"unsupported VNB version {version}")
    out = {}
    for i in range(count):
        sid = bytes(data[12 + 12 * i:16 + 12 * i]).decode("ascii")
        off, size = struct.unpack_from("<II", data, 16 + 12 * i)
        out[sid] = bytes(data[off:off + size])
    return out


def read_strings(blob):
    count = struct.unpack_from("<I", blob, 0)[0]
    offs = struct.unpack_from(f"<{count}I", blob, 4)
    base = 4 + 4 * count
    out = []
    for o in offs:
        end = blob.index(b"\0", base + o)
        out.append(blob[base + o:end].decode("utf-8"))
    return out


def disassemble(code, strings, symbols, label_names=None):
    """Human-readable listing used by golden tests and `rpy2psp disasm`."""
    label_names = label_names or {}
    lines = []
    for addr, name, vals in decode_ops(code):
        fmt = OPERANDS[OP[name]]
        parts = []
        for c, v in zip(fmt, vals):
            if c == "s":
                parts.append("None" if v == NONE32 else repr(strings[v]))
            elif c == "v":
                parts.append(symbols[v])
            elif c == "a":
                parts.append(f"@{v}" + (f"<{label_names[v]}>" if v in label_names else ""))
            elif c in "fd":
                parts.append(f"{v:g}")
            elif c == "S":
                parts.append("[" + ", ".join(repr(strings[s]) for s in v) + "]")
            elif c == "M":
                parts.append("[" + ", ".join(f"{strings[s]!r}->@{a}" for s, a in v) + "]")
            elif c == "h":
                parts.append("-" if v == NONE16 else str(v))
            else:
                parts.append(str(v))
        if name == "BINARY":
            parts = [BINOPS[vals[0]]]
        elif name == "UNARY":
            parts = [UNOPS[vals[0]]]
        elif name == "COMPARE":
            parts = [CMPOPS[vals[0]]]
        elif name == "CALL_FN":
            parts = [FUNCTIONS[vals[0]][0], str(vals[1])]
        elif name == "CALL_METHOD":
            parts = [METHODS[vals[0]], str(vals[1])]
        prefix = f"{label_names[addr]}:\n" if addr in label_names else ""
        lines.append(f"{prefix}{addr:6d}  {name} {' '.join(parts)}".rstrip())
    return "\n".join(lines) + "\n"


def script_hash(data):
    return hashlib.sha1(data).hexdigest()[:16]


def disassemble_container(data):
    secs = read_container(data)
    strings = read_strings(secs["STRS"])
    syms_blob = secs["SYMS"]
    n = struct.unpack_from("<I", syms_blob, 0)[0]
    symbols = [strings[struct.unpack_from("<I", syms_blob, 4 + 8 * i)[0]] for i in range(n)]
    lab_blob = secs["LABL"]
    n = struct.unpack_from("<I", lab_blob, 0)[0]
    labels = {}
    for i in range(n):
        sid, addr = struct.unpack_from("<II", lab_blob, 4 + 8 * i)
        labels[addr] = strings[sid]
    init, defaults, start = struct.unpack_from("<III", secs["HEAD"], 0)
    labels.setdefault(init, "__init__")
    labels.setdefault(defaults, "__defaults__")
    return disassemble(secs["CODE"], strings, symbols, labels)
