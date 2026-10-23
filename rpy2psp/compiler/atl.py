"""ATL (animation and transformation language) subset: parser and compiler.

`parse_atl` turns ATL lines into nodes. `AtlCompiler` turns nodes into a flat
instruction list (see docs/dev/bytecode.md, "Transforms"), evaluating every
property value at build time; values that depend on runtime state are errors.
"""

from dataclasses import dataclass, field

from .lexer import LexError, LineLexer


class CompileError(Exception):
    def __init__(self, where, msg, feature=None):
        super().__init__(f"{where}: {msg}")
        self.where = where
        self.msg = msg
        self.feature = feature

WARPERS = {"linear": 0, "ease": 1, "easein": 2, "easeout": 3}

# Property ids written to the transform table. Pair properties expand to two ids.
PROPS = {
    "xpos": 0, "ypos": 1, "xanchor": 2, "yanchor": 3, "xalign": 4, "yalign": 5,
    "xoffset": 6, "yoffset": 7, "zoom": 8, "xzoom": 9, "yzoom": 10, "alpha": 11,
    "rotate": 12, "additive": 13, "xsize": 14, "ysize": 15, "crop": 16,
    "xcenter": 17, "ycenter": 18,
}
PAIRS = {"pos": ("xpos", "ypos"), "anchor": ("xanchor", "yanchor"), "align": ("xalign", "yalign"),
         "offset": ("xoffset", "yoffset"), "size": ("xsize", "ysize"), "xysize": ("xsize", "ysize"),
         "xycenter": ("xcenter", "ycenter")}
KNOWN_UNSUPPORTED = set("""
rotate_pad transform_anchor nearest around alignaround angle radius crop_relative
corner1 corner2 fit subpixel delay events xtile ytile xpan ypan matrixcolor
matrixtransform matrixanchor blur zpos zzoom perspective point_to orientation
xrotate yrotate zrotate mesh mesh_pad shader blend fps show_cancels_hide maxsize
xfill yfill xmaximum ymaximum xminimum yminimum knot clockwise counterclockwise circles
""".split())
# Button events (idle, hover, ...) never fire for story images; accepted and ignored like in Ren'Py.
EVENTS = {"start", "show", "hide", "replace", "replaced", "update", "idle", "hover", "selected_idle",
          "selected_hover", "insensitive"}
ALL_PROPS = set(PROPS) | set(PAIRS)
ANCHOR_WORDS = {"left": 0.0, "top": 0.0, "center": 0.5, "right": 1.0, "bottom": 1.0}


@dataclass
class AtlNode:
    where: str


@dataclass
class Interp(AtlNode):
    warper: str = None
    duration: str = None
    props: list = field(default_factory=list)       # (name, expr)
    exprs: list = field(default_factory=list)       # (expr, with_expr) children or includes


@dataclass
class APause(AtlNode):
    duration: str = "0"


@dataclass
class Repeat(AtlNode):
    count: str = None


@dataclass
class Block(AtlNode):
    stmts: list = field(default_factory=list)


@dataclass
class Parallel(AtlNode):
    blocks: list = field(default_factory=list)


@dataclass
class Choice(AtlNode):
    choices: list = field(default_factory=list)     # (weight expr, Block)


@dataclass
class On(AtlNode):
    events: list = field(default_factory=list)
    block: Block = None


@dataclass
class Time(AtlNode):
    time: str = "0"


@dataclass
class AtlEvent(AtlNode):
    name: str = ""


@dataclass
class AtlUnsupported(AtlNode):
    feature: str = ""


def parse_atl(lines):
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        lx = LineLexer(ln)
        try:
            out.extend(_parse_line(ln, lx, lines, i))
        except LexError as e:
            out.append(AtlUnsupported(ln.where, f"atl:parse-error: {e}"))
        # merge consecutive parallel/choice siblings
        if out and isinstance(out[-1], (Parallel, Choice)) and len(out) > 1 and type(out[-2]) is type(out[-1]):
            last = out.pop()
            if isinstance(last, Parallel):
                out[-1].blocks.extend(last.blocks)
            else:
                out[-1].choices.extend(last.choices)
        i += 1
    return Block(lines[0].where if lines else "?", out)


def _parse_line(ln, lx, lines, i):
    w = ln.where
    word_pos = lx.pos
    word = lx.word()
    if word == "block":
        _colon(lx)
        return [parse_atl(ln.children)]
    if word == "parallel":
        _colon(lx)
        return [Parallel(w, [parse_atl(ln.children)])]
    if word == "choice":
        weight = lx.simple_expression() or "1.0"
        _colon(lx)
        return [Choice(w, [(weight, parse_atl(ln.children))])]
    if word == "on":
        events = []
        while True:
            n = lx.word()
            if n is None:
                break
            events.append(n)
            if not lx.match(","):
                break
        _colon(lx)
        return [On(w, events, parse_atl(ln.children))]
    if word in ("contains", "function", "animation", "warp"):
        return [AtlUnsupported(w, "atl:" + word)]
    if word == "time":
        return [Time(w, lx.rest())]
    if word == "event":
        return [AtlEvent(w, lx.word() or "")]
    lx.pos = word_pos
    out = []
    while not lx.eol():
        out.append(_statement(ln, lx))
        if not lx.match(","):
            break
    if not lx.eol():
        raise LexError(f"unexpected text in ATL: {lx.text[lx.pos:][:40]!r}")
    return out


def _colon(lx):
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':'")


def _statement(ln, lx):
    w = ln.where
    old = lx.pos
    word = lx.word()
    if word == "pause":
        return APause(w, lx.simple_expression() or "0")
    if word == "repeat":
        return Repeat(w, lx.simple_expression())
    node = Interp(w)
    if word in WARPERS or (word is not None and (word.startswith("ease") or word == "instant")):
        node.warper = word
        node.duration = lx.simple_expression()
        if node.duration is None:
            raise LexError(f"expected duration after {word}")
    else:
        lx.pos = old
        num = lx.float()
        if num is not None:
            return APause(w, num)
    while not lx.eol() and not lx.at(","):
        save = lx.pos
        name = lx.word()
        if name in ALL_PROPS or name in KNOWN_UNSUPPORTED:
            val = lx.simple_expression() if name not in ("clockwise", "counterclockwise") else "True"
            node.props.append((name, val))
            continue
        lx.pos = save
        expr = lx.simple_expression()
        if expr is None:
            raise LexError(f"unexpected text in ATL: {lx.text[lx.pos:][:40]!r}")
        with_ = None
        if lx.keyword("with"):
            with_ = lx.simple_expression()
        node.exprs.append((expr, with_))
    return node


# ---- compilation -----------------------------------------------------------------

class AtlCompileError(CompileError):
    def __init__(self, where, msg):
        super().__init__(where, msg, "atl")


def encode_value(v, where, prop):
    """Property value -> list of (kind, float); kind 0 fraction/float, 1 pixels (int), 2 None."""
    if isinstance(v, str) and v in ANCHOR_WORDS:
        return [(0, ANCHOR_WORDS[v])]
    if isinstance(v, bool):
        return [(0, 1.0 if v else 0.0)]
    if v is None:
        return [(2, 0.0)]
    if isinstance(v, int):
        return [(1, float(v))]
    if isinstance(v, float):
        return [(0, v)]
    if isinstance(v, (tuple, list)):
        out = []
        for x in v:
            out.extend(encode_value(x, where, prop))
        return out
    raise AtlCompileError(where, f"ATL property {prop} needs a number, got {type(v).__name__}")


class AtlCompiler:
    """Compiles ATL nodes. `ev(expr, where, params)` evaluates an expression at build time.
    `child(expr_value, where)` turns a displayable value into a displayable id.
    `include(value)` returns compiled instructions when the value is a transform, else None."""

    def __init__(self, ev, child, include, transition):
        self.ev = ev
        self.child = child
        self.include = include
        self.transition = transition

    def compile(self, block, params=None):
        return self._block(block, params or {})

    def _block(self, block, params):
        out = []
        for st in block.stmts:
            out.extend(self._stmt(st, params))
        return out

    def _stmt(self, st, params):
        w = st.where
        if isinstance(st, AtlUnsupported):
            raise AtlCompileError(w, f"unsupported ATL: {st.feature}")
        if isinstance(st, Block):
            return [("block", self._block(st, params))]
        if isinstance(st, Parallel):
            return [("parallel", [self._block(b, params) for b in st.blocks])]
        if isinstance(st, Choice):
            return [("choice", [(float(self.ev(wt, w, params)), self._block(b, params)) for wt, b in st.choices])]
        if isinstance(st, On):
            bad = [e for e in st.events if e not in EVENTS]
            if bad:
                raise AtlCompileError(w, f"unsupported ATL event {bad[0]!r}")
            return [("on", list(st.events), self._block(st.block, params))]
        if isinstance(st, Time):
            return [("time", float(self.ev(st.time, w, params)))]
        if isinstance(st, AtlEvent):
            return [("event", st.name)]
        if isinstance(st, APause):
            return [("pause", float(self.ev(st.duration, w, params)))]
        if isinstance(st, Repeat):
            n = -1 if st.count is None else int(self.ev(st.count, w, params))
            return [("repeat", n)]
        return self._interp(st, params)

    def _interp(self, st, params):
        w = st.where
        out = []
        props = []
        for name, expr in st.props:
            if name in KNOWN_UNSUPPORTED:
                raise AtlCompileError(w, f"unsupported ATL property {name!r}")
            value = self.ev(expr, w, params)
            if name in PAIRS:
                if not isinstance(value, (tuple, list)) or len(value) != 2:
                    raise AtlCompileError(w, f"ATL property {name} needs a pair")
                a, b = PAIRS[name]
                props.append((PROPS[a], encode_value(value[0], w, a)))
                props.append((PROPS[b], encode_value(value[1], w, b)))
            elif name == "crop":
                if value is not None and (not isinstance(value, (tuple, list)) or len(value) != 4):
                    raise AtlCompileError(w, "crop needs (x, y, w, h)")
                props.append((PROPS[name], encode_value(value, w, name)))
            else:
                props.append((PROPS[name], encode_value(value, w, name)))
        for expr, with_ in st.exprs:
            value = self.ev(expr, w, params)
            inc = self.include(value, w)
            if inc is not None:
                if with_:
                    raise AtlCompileError(w, "'with' after a transform include")
                out.append(("block", inc))
                continue
            trans = self.transition(self.ev(with_, w, params), w) if with_ else None
            out.append(("child", self.child(value, w), trans))
        if st.warper is not None:
            if st.warper not in WARPERS:
                raise AtlCompileError(w, f"unsupported ATL warper {st.warper!r}")
            dur = float(self.ev(st.duration, w, params))
            out.insert(0, ("interp", WARPERS[st.warper], dur, props))
        elif props:
            out.insert(0, ("set", props))
        return out
