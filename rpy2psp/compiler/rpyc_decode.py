"""Restricted decoder for compiled Ren'Py scripts (.rpyc).

A .rpyc holds a zlib-compressed pickle of Ren'Py AST objects. Unpickling it
normally constructs Ren'Py classes; here `find_class` maps every `renpy.*`
class to an inert stand-in that only stores its state, and refuses all other
globals. The recovered statements are rendered back to Ren'Py source lines and
parsed by the normal .rpy parser. The approach follows unrpyc (MIT,
CensoredUsername); no code is copied from it.
"""

import io
import pickle
import re
import struct
import zlib

from .lexer import Line

RPYC2_HEADER = b"RENPY RPC2"


class RpycError(Exception):
    pass


# ---- inert stand-ins ---------------------------------------------------------

class Inert:
    _qualname = "?"

    def __init__(self, *args, **kwargs):
        self._args = args

    def __setstate__(self, state):
        if isinstance(state, tuple) and len(state) == 2 and all(s is None or isinstance(s, dict) for s in state):
            for part in state:
                if part:
                    self.__dict__.update(part)
        elif isinstance(state, dict):
            self.__dict__.update(state)
        else:
            self._state = state

    def __repr__(self):
        return f"<{self._qualname}>"


class InertList(list):
    def __setstate__(self, state):
        if isinstance(state, dict):
            self.__dict__.update(state)


class InertDict(dict):
    def __init__(self, *args, **kwargs):
        super().__init__()

    def __setstate__(self, state):
        if isinstance(state, dict):
            self.__dict__.update(state)


class InertSet(set):
    def __setstate__(self, state):
        if isinstance(state, tuple) and state and isinstance(state[0], dict):
            self.update(state[0].keys())
        elif isinstance(state, (set, frozenset, list)):
            self.update(state)


class PyExpr(str):
    def __new__(cls, s, filename=None, linenumber=0, *rest):
        obj = super().__new__(cls, s)
        obj.filename = filename
        obj.linenumber = linenumber
        return obj


def _reconstructor(cls, base, state):
    if not isinstance(cls, type) or not issubclass(cls, (Inert, InertList, InertDict, InertSet)):
        raise pickle.UnpicklingError("refusing to reconstruct a non-inert class")
    if base is object or base is None:
        return cls.__new__(cls)
    return base.__new__(cls, state) if base in (list, dict, set) else cls.__new__(cls)


def _codecs_encode(s, encoding="utf-8"):
    if not isinstance(s, str):
        raise pickle.UnpicklingError("unexpected _codecs.encode arguments")
    return s.encode(encoding if encoding in ("latin1", "latin-1", "utf-8") else "latin1")


SAFE_BUILTINS = {"set": set, "frozenset": frozenset, "object": object, "list": list, "dict": dict,
                 "tuple": tuple, "bytes": bytes, "str": str, "unicode": str, "int": int, "long": int,
                 "float": float, "bool": bool, "bytearray": bytearray}


class RestrictedUnpickler(pickle.Unpickler):
    def __init__(self, f):
        super().__init__(f, encoding="utf-8", errors="surrogateescape")
        self._classes = {}

    def find_class(self, module, name):
        if module in ("builtins", "__builtin__") and name in SAFE_BUILTINS:
            return SAFE_BUILTINS[name]
        if module in ("copy_reg", "copyreg") and name == "_reconstructor":
            return _reconstructor
        if module == "_codecs" and name == "encode":
            return _codecs_encode
        if module == "collections" and name in ("OrderedDict", "defaultdict"):
            return InertDict
        if module == "renpy" or module.startswith(("renpy.", "store.", "store")):
            if name == "PyExpr":
                return PyExpr
            key = module + "." + name
            if key not in self._classes:
                if name.startswith("Revertable") and name.endswith("List"):
                    base = InertList
                elif name.startswith("Revertable") and "Dict" in name:
                    base = InertDict
                elif name.startswith("Revertable") and name.endswith("Set"):
                    base = InertSet
                else:
                    base = Inert
                self._classes[key] = type(name, (base,), {"_qualname": key})
            return self._classes[key]
        raise pickle.UnpicklingError(f"global {module}.{name} is not allowed in a .rpyc")


def read_slot(data, slot):
    if data[:len(RPYC2_HEADER)] != RPYC2_HEADER:
        return zlib.decompress(data) if slot == 1 else None
    pos = len(RPYC2_HEADER)
    while pos + 12 <= len(data):
        s, start, length = struct.unpack("<III", data[pos:pos + 12])
        if s == slot:
            return zlib.decompress(data[start:start + length])
        if s == 0:
            return None
        pos += 12
    return None


def load_rpyc(data):
    last = None
    for slot in (1, 2):
        try:
            raw = read_slot(data, slot)
        except zlib.error as e:
            last = e
            continue
        if raw is None:
            continue
        try:
            payload = RestrictedUnpickler(io.BytesIO(raw)).load()
        except (pickle.UnpicklingError, EOFError, ValueError, TypeError, AttributeError) as e:
            last = e
            continue
        if isinstance(payload, tuple) and len(payload) == 2:
            return payload
        last = "unexpected payload shape"
    raise RpycError(f"cannot decode .rpyc: {last}")


# ---- rendering back to source lines -------------------------------------------

def _cls(node):
    return getattr(node, "_qualname", type(node).__name__).rsplit(".", 1)[-1]


def _src(x):
    if x is None:
        return None
    state = getattr(x, "_state", None)
    if isinstance(state, tuple) and len(state) >= 2:  # PyCode
        return str(state[1])
    if hasattr(x, "source"):
        return str(x.source)
    return str(x)


def encode_say_string(s):
    s = s.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')
    s = re.sub(r"(?<= ) ", "\\ ", s)
    return '"' + s + '"'


class Renderer:
    def __init__(self, rel):
        self.rel = rel

    def where(self, node):
        ln = getattr(node, "linenumber", None)
        if ln is None:
            loc = getattr(node, "loc", None)
            ln = loc[1] if isinstance(loc, tuple) and len(loc) > 1 else 0
        return int(ln or 0)

    def line(self, node, text, children=(), raw=None):
        n = self.where(node)
        return Line(self.rel, n, n, 0, text, list(children), raw)

    def block(self, nodes):
        out = []
        for n in nodes or []:
            out.extend(self.node(n))
        return out

    def node(self, n):
        kind = _cls(n)
        fn = getattr(self, "r_" + kind, None)
        if fn is None:
            return [self.line(n, f"rpyc_unknown_{kind}")]
        return fn(n)

    # statements
    def r_Say(self, n):
        parts = []
        if getattr(n, "who", None):
            parts.append(str(n.who))
        if getattr(n, "attributes", None):
            parts.extend(n.attributes)
        if getattr(n, "temporary_attributes", None):
            parts.append("@")
            parts.extend(n.temporary_attributes)
        parts.append(encode_say_string(str(n.what)))
        if not getattr(n, "interact", True):
            parts.append("nointeract")
        if getattr(n, "explicit_identifier", False) and getattr(n, "identifier", None):
            parts += ["id", n.identifier]
        if getattr(n, "arguments", None) is not None:
            parts.append(self.arguments(n.arguments))
        if getattr(n, "with_", None):
            parts += ["with", str(n.with_)]
        return [self.line(n, " ".join(parts))]

    def r_TranslateSay(self, n):
        say = self.r_Say(n)
        if getattr(n, "language", None) is None:
            return say
        return [self.line(n, f"translate {n.language} {n.identifier}:", say)]

    def r_Label(self, n):
        params = self.parameters(getattr(n, "parameters", None))
        hide = " hide" if getattr(n, "hide", False) else ""
        return [self.line(n, f"label {n.name}{params}{hide}:", self.block(n.block))]

    def r_Init(self, n):
        return [self.line(n, f"init {n.priority}:", self.block(n.block))]

    def r_Python(self, n, early=False):
        code = _src(n.code)
        store = getattr(n, "store", "store")
        hide = getattr(n, "hide", False)
        if "\n" not in code.strip() and not early and not hide and store == "store":
            return [self.line(n, "$ " + code.strip())]
        head = "python" + (" early" if early else "") + (" hide" if hide else "")
        if store != "store":
            head += " in " + store[len("store."):]
        return [self.line(n, head + ":", raw=code)]

    def r_EarlyPython(self, n):
        return self.r_Python(n, early=True)

    def _store_name(self, n, varname):
        store = getattr(n, "store", "store")
        return varname if store == "store" else store[len("store."):] + "." + varname

    def r_Define(self, n):
        name = self._store_name(n, n.varname)
        index = f"[{_src(n.index)}]" if getattr(n, "index", None) is not None else ""
        op = getattr(n, "operator", "=")
        return [self.line(n, f"define {name}{index} {op} {_src(n.code)}")]

    def r_Default(self, n):
        return [self.line(n, f"default {self._store_name(n, n.varname)} = {_src(n.code)}")]

    def r_Image(self, n):
        name = " ".join(n.imgname)
        if getattr(n, "atl", None) is not None:
            return [self.line(n, f"image {name}:", self.atl(n.atl))]
        return [self.line(n, f"image {name} = {_src(n.code)}")]

    def r_Transform(self, n):
        name = self._store_name(n, n.varname)
        return [self.line(n, f"transform {name}{self.parameters(getattr(n, 'parameters', None))}:",
                          self.atl(n.atl))]

    def imspec(self, kind, imspec, layer=None):
        if imspec is None:
            return kind + (f" onlayer {layer}" if layer else "")
        expression = tag = zorder = None
        behind = []
        if len(imspec) == 3:
            name, at_list, layer2 = imspec
        elif len(imspec) == 6:
            name, expression, tag, at_list, layer2, zorder = imspec
        else:
            name, expression, tag, at_list, layer2, zorder, behind = imspec[:7]
        layer = layer or layer2
        parts = [kind]
        if expression is not None:
            parts += ["expression", str(expression)]
        else:
            parts += [encode_say_string(c) if " " in c or not re.match(r"^[-\w]+$", c) else c for c in name]
        if tag:
            parts += ["as", tag]
        if at_list:
            parts += ["at", ", ".join(str(a) for a in at_list)]
        if behind:
            parts += ["behind", ", ".join(behind)]
        if layer and layer != "master":
            parts += ["onlayer", layer]
        if zorder is not None:
            parts += ["zorder", str(zorder)]
        return " ".join(parts)

    def _show(self, n, kind, layer=None):
        text = self.imspec(kind, n.imspec, layer)
        atl = getattr(n, "atl", None)
        if atl is not None:
            return [self.line(n, text + ":", self.atl(atl))]
        return [self.line(n, text)]

    def r_Show(self, n):
        return self._show(n, "show")

    def r_Scene(self, n):
        layer = getattr(n, "layer", None)
        return self._show(n, "scene", layer if layer != "master" else None)

    def r_Hide(self, n):
        return [self.line(n, self.imspec("hide", n.imspec))]

    def r_With(self, n):
        if getattr(n, "paired", None) is not None:
            return [self.line(n, "with None")]
        return [self.line(n, f"with {n.expr}")]

    def r_Jump(self, n):
        if getattr(n, "expression", False):
            return [self.line(n, f"jump expression {n.target}")]
        return [self.line(n, f"jump {n.target}")]

    def r_Call(self, n):
        args = getattr(n, "arguments", None)
        if getattr(n, "expression", False):
            text = f"call expression {n.label}" + (f" pass {self.arguments(args)}" if args else "")
        else:
            text = f"call {n.label}" + (self.arguments(args) if args else "")
        return [self.line(n, text)]

    def r_Return(self, n):
        e = getattr(n, "expression", None)
        return [self.line(n, "return" + (f" {e}" if e else ""))]

    def r_Pass(self, n):
        return [self.line(n, "pass")]

    def r_Menu(self, n):
        children = []
        if getattr(n, "set", None):
            children.append(self.line(n, f"set {n.set}"))
        item_args = getattr(n, "item_arguments", None) or []
        for i, (label, cond, block) in enumerate(n.items):
            text = encode_say_string(str(label))
            if block is None:
                children.append(self.line(n, text))
                continue
            args = item_args[i] if i < len(item_args) else None
            if args is not None:
                text += " " + self.arguments(args)
            if cond is not None and str(cond) != "True":
                text += f" if {cond}"
            first = block[0] if block else n
            children.append(Line(self.rel, self.where(first), self.where(first), 0, text + ":", self.block(block)))
        head = "menu"
        if getattr(n, "arguments", None) is not None:
            head += self.arguments(n.arguments)
        return [self.line(n, head + ":", children)]

    def r_If(self, n):
        out = []
        for i, (cond, block) in enumerate(n.entries):
            if i > 0 and str(cond) == "True" and i == len(n.entries) - 1:
                text = "else:"
            else:
                text = ("if " if i == 0 else "elif ") + str(cond) + ":"
            first = block[0] if block else n
            out.append(Line(self.rel, max(self.where(first) - 1, 0), 0, 0, text, self.block(block)))
        out[0].lineno = self.where(n)
        return out

    def r_While(self, n):
        return [self.line(n, f"while {n.condition}:", self.block(n.block))]

    def r_UserStatement(self, n):
        children = self.block(getattr(n, "block", None) or []) if isinstance(getattr(n, "block", None), list) and \
            all(not isinstance(b, (str, tuple)) for b in n.block) else []
        return [self.line(n, str(n.line), children)]

    def r_Translate(self, n):
        if getattr(n, "language", None) is None:
            return self.block(n.block)
        return [self.line(n, f"translate {n.language} {n.identifier}:", self.block(n.block))]

    def r_EndTranslate(self, n):
        return []

    def r_TranslateString(self, n):
        kids = [self.line(n, "old " + encode_say_string(str(n.old))),
                self.line(n, "new " + encode_say_string(str(n.new)))]
        return [self.line(n, f"translate {n.language} strings:", kids)]

    def r_TranslatePython(self, n):
        return [self.line(n, f"translate {n.language} python:", raw=_src(n.code))]

    def r_TranslateBlock(self, n):
        return [self.line(n, f"translate {n.language} style rpyc_block")]

    r_TranslateEarlyBlock = r_TranslateBlock

    def r_Screen(self, n):
        name = getattr(n.screen, "name", "screen")
        return [self.line(n, f"screen {name}:")]

    def r_Style(self, n):
        return [self.line(n, f"style {getattr(n, 'style_name', 'x')}")]

    def r_Testcase(self, n):
        return [self.line(n, "testcase rpyc")]

    def r_RPY(self, n):
        return [self.line(n, "rpy " + " ".join(getattr(n, "rest", ())))]

    def r_Camera(self, n):
        return [self.line(n, "camera")]

    def r_ShowLayer(self, n):
        return [self.line(n, f"show layer {getattr(n, 'layer', 'master')}")]

    # parameters and arguments
    def parameters(self, p):
        if p is None:
            return ""
        params = getattr(p, "parameters", None)
        out = []
        if isinstance(params, dict):
            for prm in params.values():
                kind = getattr(prm, "kind", 1)
                name = prm.name
                default = getattr(prm, "default", None)
                if kind == 2:
                    out.append("*" + name)
                elif kind == 4:
                    out.append("**" + name)
                else:
                    out.append(name + (f"={default}" if default is not None else ""))
        elif isinstance(params, list):
            for name, default in params:
                out.append(name + (f"={default}" if default is not None else ""))
            if getattr(p, "extrapos", None):
                out.append("*" + p.extrapos)
            if getattr(p, "extrakw", None):
                out.append("**" + p.extrakw)
        return "(" + ", ".join(out) + ")"

    def arguments(self, a):
        out = []
        starred = getattr(a, "starred_indexes", None) or set()
        dstarred = getattr(a, "doublestarred_indexes", None) or set()
        for i, (name, expr) in enumerate(getattr(a, "arguments", [])):
            if i in starred:
                out.append("*" + str(expr))
            elif i in dstarred:
                out.append("**" + str(expr))
            elif name:
                out.append(f"{name}={expr}")
            else:
                out.append(str(expr))
        if getattr(a, "extrapos", None):
            out.append("*" + str(a.extrapos))
        if getattr(a, "extrakw", None):
            out.append("**" + str(a.extrakw))
        return "(" + ", ".join(out) + ")"

    # ATL
    def atl(self, block):
        out = []
        for st in getattr(block, "statements", []):
            out.extend(self.atl_stmt(st))
        return out

    def _aline(self, st, text, children=()):
        loc = getattr(st, "loc", None)
        n = loc[1] if isinstance(loc, tuple) and len(loc) > 1 else 0
        return Line(self.rel, n, n, 0, text, list(children))

    def atl_stmt(self, st):
        kind = _cls(st)
        if kind == "RawBlock":
            return [self._aline(st, "block:", self.atl(st))]
        if kind == "RawMultipurpose":
            parts = []
            warper = getattr(st, "warper", None)
            if warper == "pause":
                return [self._aline(st, f"pause {st.duration}")]
            if getattr(st, "warp_function", None):
                return [self._aline(st, f"warp {st.warp_function} {st.duration}")]
            if warper:
                parts += [warper, str(st.duration)]
            for name, expr in getattr(st, "properties", []):
                parts += [name, str(expr)]
            for expr, with_ in getattr(st, "expressions", []):
                parts.append(str(expr))
                if with_:
                    parts += ["with", str(with_)]
            for name, exprs in getattr(st, "splines", []) or []:
                parts += [name, str(exprs[-1])] + [x for e in exprs[:-1] for x in ("knot", str(e))]
            if getattr(st, "revolution", None):
                parts.append(st.revolution)
            return [self._aline(st, " ".join(parts))] if parts else []
        if kind == "RawRepeat":
            r = getattr(st, "repeats", None)
            return [self._aline(st, "repeat" + (f" {r}" if r else ""))]
        if kind == "RawParallel":
            return [self._aline(st, "parallel:", self.atl(b)) for b in st.blocks]
        if kind == "RawChoice":
            return [self._aline(st, f"choice {c}:", self.atl(b)) for c, b in st.choices]
        if kind == "RawOn":
            return [self._aline(st, f"on {name}:", self.atl(b)) for name, b in st.handlers.items()]
        if kind == "RawTime":
            return [self._aline(st, f"time {st.time}")]
        if kind == "RawEvent":
            return [self._aline(st, f"event {st.name}")]
        if kind == "RawFunction":
            return [self._aline(st, f"function {st.expr}")]
        if kind in ("RawContainsExpr", "RawChild"):
            return [self._aline(st, "contains rpyc")]
        return [self._aline(st, f"atl_unknown_{kind}")]


def decode_rpyc(data, rel):
    """Decode a .rpyc into Line trees ready for the .rpy statement parser."""
    _, stmts = load_rpyc(data)
    if not isinstance(stmts, list):
        raise RpycError("statement list missing")
    return Renderer(rel).block(stmts)
