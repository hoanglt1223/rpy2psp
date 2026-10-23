"""Game discovery and the .rpy statement parser.

`load_game` builds a read-only view over a game directory and its .rpa
archives; `read_scripts` turns every script (.rpy preferred, otherwise .rpyc
through the restricted decoder) into statement nodes. Parse only: Python
fragments are kept as text and parsed later with `ast.parse`, never executed.
"""

import os
import unicodedata
from dataclasses import dataclass, field

from . import atl as atl_mod
from .lexer import LexError, Line, LineLexer, block_source, decode_source, parse_source
from .rpa import RpaArchive, RpaError

SKIP_DIRS = {".git", "renpy", "cache", "saves", "__pycache__"}
UI_FILES = {"gui.rpy", "screens.rpy", "options.rpy"}


from .atl import CompileError  # noqa: E402  (single error type shared by all compiler modules)


# ---- game file system ------------------------------------------------------

class Game:
    """Files of one game: loose files under `root` plus entries of .rpa archives."""

    def __init__(self, root, listed=()):
        self.root = root
        self.listed = set(listed)  # files known from a listing but not downloaded (census corpus)
        self.loose = {}
        self.archived = {}
        self.archives = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
            for fn in sorted(filenames):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace("\\", "/")
                self.loose[rel] = full
        for rel in sorted(self.loose):
            if rel.lower().endswith(".rpa"):
                try:
                    arc = RpaArchive(self.loose[rel])
                except RpaError as e:
                    raise CompileError(rel, str(e), "rpa") from e
                self.archives.append(arc)
                for name in arc.names():
                    self.archived.setdefault(name, arc)

    def files(self):
        return sorted(set(self.loose) | set(self.archived) | self.listed)

    def resolve(self, rel):
        """Exact name, else Ren'Py's case-insensitive match; None when absent."""
        if rel in self.loose or rel in self.archived or rel in self.listed:
            return rel
        if not hasattr(self, "_lower"):
            self._lower = {unicodedata.normalize("NFC", f.lower()): f for f in self.files()}
        return self._lower.get(unicodedata.normalize("NFC", rel.lower()))

    def exists(self, rel):
        return self.resolve(rel) is not None

    def read(self, rel):
        rel = self.resolve(rel) or rel
        if rel in self.loose:
            with open(self.loose[rel], "rb") as f:
                return f.read()
        if rel in self.archived:
            return self.archived[rel].read(rel)
        raise FileNotFoundError(rel)

    def close(self):
        for a in self.archives:
            a.close()


def find_game_root(path):
    sub = os.path.join(path, "game")
    return sub if os.path.isdir(sub) else path


def load_game(path, listed=()):
    return Game(find_game_root(path), listed)


# ---- statement nodes ---------------------------------------------------------

@dataclass
class Node:
    where: str


@dataclass
class Say(Node):
    who: str = None
    who_is_string: bool = False
    attrs: list = field(default_factory=list)
    temp_attrs: list = field(default_factory=list)
    what: str = ""
    interact: bool = True
    ident: str = None
    args: str = None
    with_: str = None


@dataclass
class Label(Node):
    name: str = ""
    params: str = None
    hide: bool = False
    block: list = field(default_factory=list)


@dataclass
class Jump(Node):
    target: str = ""
    expression: bool = False


@dataclass
class Call(Node):
    target: str = ""
    expression: bool = False
    args: str = None
    from_: str = None


@dataclass
class Return(Node):
    expr: str = None


@dataclass
class Pass(Node):
    pass


@dataclass
class MenuItem:
    where: str
    text: str
    cond: str = None
    args: str = None
    block: list = field(default_factory=list)


@dataclass
class Menu(Node):
    name: str = None
    args: str = None
    set_expr: str = None
    caption: list = field(default_factory=list)
    items: list = field(default_factory=list)


@dataclass
class If(Node):
    entries: list = field(default_factory=list)  # (cond or None, block, where)


@dataclass
class While(Node):
    cond: str = ""
    block: list = field(default_factory=list)


@dataclass
class Python(Node):
    code: str = ""
    inline: bool = False
    early: bool = False
    hide: bool = False
    store: str = "store"
    init: bool = False
    priority: int = 0


@dataclass
class Init(Node):
    priority: int = 0
    block: list = field(default_factory=list)


@dataclass
class Define(Node):
    name: str = ""
    index: str = None
    op: str = "="
    expr: str = ""
    priority: int = 0
    default: bool = False


@dataclass
class Image(Node):
    name: tuple = ()
    expr: str = None
    atl: object = None
    priority: int = 500


@dataclass
class Transform(Node):
    name: str = ""
    params: str = None
    atl: object = None
    priority: int = 0


@dataclass
class Show(Node):
    kind: str = "show"           # show, scene, hide
    name: tuple = None           # image name components; None for bare scene / expression
    expression: str = None
    text: str = None             # show text "..."
    at_list: list = field(default_factory=list)
    as_: str = None
    behind: list = field(default_factory=list)
    layer: str = None
    zorder: str = None
    with_: str = None
    atl: object = None


@dataclass
class With(Node):
    expr: str = ""


@dataclass
class Play(Node):
    kind: str = "play"           # play, queue, stop
    channel: str = ""
    file: str = None
    fadein: str = None
    fadeout: str = None
    loop: bool = None
    if_changed: bool = False
    volume: str = None


@dataclass
class Voice(Node):
    expr: str = None
    sustain: bool = False


@dataclass
class Pause(Node):
    expr: str = None


@dataclass
class Window(Node):
    mode: str = "show"           # show, hide, auto, auto-show, auto-hide
    trans: str = None


@dataclass
class Nvl(Node):
    mode: str = "clear"
    trans: str = None


@dataclass
class ScreenUse(Node):
    how: str = "show"
    name: str = ""


@dataclass
class Translate(Node):
    language: str = None
    ident: str = ""
    block: list = field(default_factory=list)


@dataclass
class TranslateStrings(Node):
    language: str = None
    pairs: list = field(default_factory=list)  # (old, new, where)


@dataclass
class TranslatePython(Node):
    language: str = None
    code: str = ""


@dataclass
class ScreenDef(Node):
    name: str = ""


@dataclass
class Ignored(Node):
    kind: str = ""


@dataclass
class Unsupported(Node):
    feature: str = ""
    text: str = ""


@dataclass
class ScriptFile:
    rel: str
    nodes: list
    ui: bool
    tl_language: str = None
    source: str = ".rpy"


# ---- parser --------------------------------------------------------------------

class ParseContext:
    def __init__(self, rel, src_lines):
        self.rel = rel
        self.src_lines = src_lines
        self.global_label = None
        self.init_offset = 0
        self.monologue = '"'  # default "double" monologue delimiter is a blank line
        self.monologue = "\n\n"
        self.in_init = False


def _priority(lx):
    p = lx.integer()
    return int(p) if p else 0


def _code_of(ln, ctx):
    if ln.raw is not None:
        return ln.raw
    return block_source(ctx.src_lines, ln)


def parse_block(lines, ctx):
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        try:
            lx = LineLexer(ln, ctx.monologue)
            if lx.keyword("if"):
                node = If(ln.where)
                node.entries.append((_cond(lx, ln), parse_block(ln.children, ctx), ln.where))
                while i + 1 < len(lines):
                    nx = lines[i + 1]
                    nlx = LineLexer(nx)
                    if nlx.keyword("elif"):
                        node.entries.append((_cond(nlx, nx), parse_block(nx.children, ctx), nx.where))
                    elif nlx.keyword("else"):
                        if not nlx.match(":") or not nlx.eol():
                            raise LexError("expected ':' after else")
                        node.entries.append((None, parse_block(nx.children, ctx), nx.where))
                        i += 1
                        break
                    else:
                        break
                    i += 1
                out.append(node)
            else:
                res = parse_statement(ln, lx, ctx)
                if isinstance(res, list):
                    out.extend(res)
                elif res is not None:
                    out.append(res)
        except LexError as e:
            out.append(Unsupported(ln.where, "parse-error", f"{e}: {ln.text[:60]}"))
        i += 1
    return out


def _cond(lx, ln):
    text = lx.rest()
    if not text.endswith(":"):
        raise LexError("expected ':' at end of condition")
    return text[:-1].strip()


def _expect_block(ln, what):
    if not ln.text.endswith(":"):
        raise LexError(f"expected ':' after {what}")


SAY_CLAUSES = ("nointeract", "id", "with")


def parse_statement(ln, lx, ctx):
    w = ln.where
    if lx.match(r"\$"):
        return Python(w, code=lx.rest(), inline=True)
    old = lx.pos
    word = lx.word()
    if word is not None:
        handler = STATEMENTS.get(word)
        if handler is not None:
            res = handler(ln, lx, ctx)
            if res is not NOT_THIS:
                return res
        lx.pos = old
    say = parse_say(ln, lx, ctx)
    if say is not None:
        return say
    first = ln.text.split()[0] if ln.text.split() else ln.text
    return Unsupported(w, "stmt:unknown:" + first, ln.text[:60])


NOT_THIS = object()


def parse_say(ln, lx, ctx):
    w = ln.where
    lx.pos = 0
    who = None
    who_is_string = False
    attrs, temp = [], []
    what = _say_text(lx)
    if what is not None and not lx.eol() and (lx.at('"') or lx.at("'") or lx.at("`")):
        # "who" "what"
        who = _quote(what[0])
        who_is_string = True
        what = _say_text(lx)
    elif what is None:
        lx.pos = 0
        who = lx.simple_expression(operator=False)
        if who is None:
            return None
        while True:
            if lx.match("@"):
                while True:
                    a = lx.image_name_component()
                    if a is None:
                        break
                    temp.append(a)
                break
            a = lx.image_name_component()
            if a is None:
                break
            attrs.append(a)
        what = _say_text(lx)
        if what is None:
            return None
    interact, ident, args, with_ = True, None, None, None
    while not lx.eol():
        if lx.keyword("nointeract"):
            interact = False
        elif lx.keyword("id"):
            ident = lx.word()
        elif lx.keyword("with"):
            with_ = lx.simple_expression()
        elif lx.at("("):
            args = lx.delimited()
        else:
            raise LexError(f"unexpected text in say statement: {lx.text[lx.pos:][:40]!r}")
    if ln.children:
        raise LexError("say statement does not take a block")
    nodes = [Say(w, who=who, who_is_string=who_is_string, attrs=attrs, temp_attrs=temp, what=t,
                 interact=interact, ident=ident, args=args, with_=with_) for t in what]
    return nodes if len(nodes) > 1 else nodes[0]


def _say_text(lx):
    t = lx.triple_string()
    if t is not None:
        return t
    s = lx.string()
    return None if s is None else [s]


def _quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# -- individual statements -----------------------------------------------------

def st_label(ln, lx, ctx):
    name = lx.label_name(ctx.global_label)
    if name is None:
        raise LexError("expected label name")
    params = lx.delimited() if lx.at("(") else None
    hide = lx.keyword("hide")
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':' after label")
    if "." not in name:
        ctx.global_label = name
    return Label(ln.where, name=name, params=params, hide=hide, block=parse_block(ln.children, ctx))


def st_jump(ln, lx, ctx):
    if lx.keyword("expression"):
        return Jump(ln.where, target=lx.rest(), expression=True)
    name = lx.label_name(ctx.global_label)
    if name is None:
        raise LexError("expected label name after jump")
    lx.expect_eol("jump")
    return Jump(ln.where, target=name)


def st_call(ln, lx, ctx):
    old = lx.pos
    if lx.keyword("screen"):
        return ScreenUse(ln.where, how="call", name=lx.word() or "")
    lx.pos = old
    node = Call(ln.where)
    if lx.keyword("expression"):
        node.expression = True
        node.target = lx.simple_expression()
        if lx.keyword("pass"):
            node.args = lx.delimited()
    else:
        node.target = lx.label_name(ctx.global_label)
        if node.target is None:
            raise LexError("expected label name after call")
        if lx.at("("):
            node.args = lx.delimited()
    if lx.keyword("from"):
        node.from_ = lx.label_name(ctx.global_label)
    lx.expect_eol("call")
    return node


def st_return(ln, lx, ctx):
    rest = lx.rest()
    return Return(ln.where, expr=rest or None)


def st_pass(ln, lx, ctx):
    lx.expect_eol("pass")
    return Pass(ln.where)


def st_menu(ln, lx, ctx):
    node = Menu(ln.where)
    node.name = lx.label_name(ctx.global_label)
    if lx.at("("):
        node.args = lx.delimited()
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':' after menu")
    for ch in ln.children:
        clx = LineLexer(ch, ctx.monologue)
        if clx.keyword("set"):
            node.set_expr = clx.rest()
            continue
        if ch.text.endswith(":"):
            text = clx.string()
            if text is None:
                raise LexError(f"expected menu choice string at {ch.where}")
            item = MenuItem(ch.where, text)
            if clx.at("("):
                item.args = clx.delimited()
            if clx.keyword("if"):
                item.cond = _cond(clx, ch)
            elif not clx.match(":") or not clx.eol():
                raise LexError(f"unexpected text in menu choice at {ch.where}")
            item.block = parse_block(ch.children, ctx)
            node.items.append(item)
        else:
            cap = parse_say(ch, clx, ctx)
            if cap is None:
                raise LexError(f"expected menu caption or choice at {ch.where}")
            for s in cap if isinstance(cap, list) else [cap]:
                s.interact = False
                node.caption.append(s)
    return node


def st_while(ln, lx, ctx):
    return While(ln.where, cond=_cond(lx, ln), block=parse_block(ln.children, ctx))


def st_elif(ln, lx, ctx):
    raise LexError("elif without if")


def st_else(ln, lx, ctx):
    raise LexError("else without if")


def _python_header(lx):
    early = lx.keyword("early")
    hide = lx.keyword("hide")
    store = "store"
    if lx.keyword("in"):
        store = "store." + (lx.dotted_name() or "")
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':' after python")
    return early, hide, store


def st_python(ln, lx, ctx):
    early, hide, store = _python_header(lx)
    return Python(ln.where, code=_code_of(ln, ctx), early=early, hide=hide, store=store, init=early)


def st_init(ln, lx, ctx):
    if lx.keyword("offset"):
        if not lx.match("="):
            raise LexError("expected '=' after init offset")
        ctx.init_offset = int(lx.integer() or 0)
        return Ignored(ln.where, "init-offset")
    prio = _priority(lx) + ctx.init_offset
    if lx.keyword("python"):
        early, hide, store = _python_header(lx)
        return Python(ln.where, code=_code_of(ln, ctx), early=early, hide=hide, store=store, init=True,
                      priority=prio)
    if lx.keyword("label"):
        return Unsupported(ln.where, "stmt:init-label", ln.text[:60])
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':' after init")
    was = ctx.in_init
    ctx.in_init = True
    try:
        block = parse_block(ln.children, ctx)
    finally:
        ctx.in_init = was
    return Init(ln.where, priority=prio, block=block)


def _define(ln, lx, ctx, default):
    prio = _priority(lx) + ctx.init_offset
    name = lx.dotted_name()
    if name is None:
        raise LexError("expected variable name")
    index = None
    if not default and lx.at("["):
        index = lx.delimited()[1:-1]
    op = lx.match(r"\+=|\|=|=")
    if op is None or (default and op != "="):
        raise LexError("expected '='")
    expr = lx.rest()
    if not expr:
        raise LexError("expected expression")
    return Define(ln.where, name=name, index=index, op=op, expr=expr, priority=prio, default=default)


def st_define(ln, lx, ctx):
    return _define(ln, lx, ctx, False)


def st_default(ln, lx, ctx):
    return _define(ln, lx, ctx, True)


def st_image(ln, lx, ctx):
    name = []
    while True:
        c = lx.image_name_component()
        if c is None:
            break
        name.append(c)
    if not name:
        raise LexError("expected image name")
    node = Image(ln.where, name=tuple(name), priority=500 + ctx.init_offset)
    if lx.match("="):
        node.expr = lx.rest()
        if ln.children:
            raise LexError("image statement with '=' does not take a block")
    elif lx.match(":"):
        lx.expect_eol("image")
        node.atl = atl_mod.parse_atl(ln.children)
    else:
        raise LexError("expected '=' or ':' after image name")
    return node


def st_transform(ln, lx, ctx):
    prio = _priority(lx) + ctx.init_offset
    name = lx.dotted_name()
    if name is None:
        raise LexError("expected transform name")
    params = lx.delimited() if lx.at("(") else None
    if not lx.match(":") or not lx.eol():
        raise LexError("expected ':' after transform")
    return Transform(ln.where, name=name, params=params, atl=atl_mod.parse_atl(ln.children), priority=prio)


def _image_spec(ln, lx, ctx, kind):
    node = Show(ln.where, kind=kind)
    if lx.keyword("expression"):
        node.expression = lx.simple_expression(image=True)
    elif lx.keyword("layer"):
        return Unsupported(ln.where, kind + ":layer", ln.text[:60])
    else:
        name = []
        while True:
            if name == ["text"] and lx.at("_("):
                break
            c = lx.image_name_component()
            if c is not None:
                name.append(c)
                continue
            s = lx.string()
            if s is not None:
                name.append(s)
                continue
            break
        if name == ["text"] and kind == "show" and lx.at("_("):
            lx.match("_")
            inner = LineLexer(Line(ln.file, ln.lineno, ln.lineno, 0, lx.delimited()[1:-1].strip()))
            text = inner.string()
            if text is None or not inner.eol():
                raise LexError("show text _() needs a single string")
            name.append(text)
        if name and name[0] == "text" and len(name) == 2 and kind == "show":
            node.text = name[1]
            node.name = ("text",)
        elif name:
            node.name = tuple(name)
    while not lx.eol():
        if lx.keyword("at"):
            node.at_list = _split_commas(lx.simple_expression(comma=True, image=True) or "")
        elif lx.keyword("as"):
            node.as_ = lx.image_name_component() or lx.word()
        elif lx.keyword("behind"):
            while True:
                b = lx.image_name_component()
                if b is None:
                    break
                node.behind.append(b)
                if not lx.match(","):
                    break
        elif lx.keyword("onlayer"):
            node.layer = lx.word()
        elif lx.keyword("zorder"):
            node.zorder = lx.simple_expression(image=True)
        elif lx.keyword("with"):
            node.with_ = lx.simple_expression(image=True)
        elif lx.match(":"):
            lx.expect_eol(kind)
            node.atl = atl_mod.parse_atl(ln.children)
        else:
            raise LexError(f"unexpected text in {kind} statement: {lx.text[lx.pos:][:40]!r}")
    if node.name is None and node.expression is None and kind != "scene":
        raise LexError(f"expected image name after {kind}")
    return node


def _split_commas(text):
    out, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def st_show(ln, lx, ctx):
    old = lx.pos
    if lx.keyword("screen"):
        return ScreenUse(ln.where, how="show", name=lx.word() or "")
    lx.pos = old
    return _image_spec(ln, lx, ctx, "show")


def st_scene(ln, lx, ctx):
    return _image_spec(ln, lx, ctx, "scene")


def st_hide(ln, lx, ctx):
    old = lx.pos
    if lx.keyword("screen"):
        return ScreenUse(ln.where, how="hide", name=lx.word() or "")
    lx.pos = old
    return _image_spec(ln, lx, ctx, "hide")


def st_with(ln, lx, ctx):
    expr = lx.rest()
    if not expr:
        raise LexError("expected transition after with")
    return With(ln.where, expr=expr)


def _audio(ln, lx, ctx, kind):
    channel = lx.name()
    if channel is None:
        return NOT_THIS
    node = Play(ln.where, kind=kind, channel=channel)
    if kind != "stop":
        node.file = lx.simple_expression()
        if node.file is None:
            raise LexError(f"expected file after {kind} {channel}")
    while not lx.eol():
        if lx.keyword("fadein"):
            node.fadein = lx.simple_expression()
        elif lx.keyword("fadeout"):
            node.fadeout = lx.simple_expression()
        elif lx.keyword("loop"):
            node.loop = True
        elif lx.keyword("noloop"):
            node.loop = False
        elif lx.keyword("if_changed"):
            node.if_changed = True
        elif lx.keyword("volume"):
            node.volume = lx.simple_expression()
        elif lx.keyword("synchro_start"):
            return Unsupported(ln.where, "audio:synchro_start", ln.text[:60])
        else:
            raise LexError(f"unexpected text in {kind} statement: {lx.text[lx.pos:][:40]!r}")
    return node


def st_play(ln, lx, ctx):
    return _audio(ln, lx, ctx, "play")


def st_queue(ln, lx, ctx):
    return _audio(ln, lx, ctx, "queue")


def st_stop(ln, lx, ctx):
    return _audio(ln, lx, ctx, "stop")


def st_voice(ln, lx, ctx):
    if lx.keyword("sustain"):
        lx.expect_eol("voice sustain")
        return Voice(ln.where, sustain=True)
    expr = lx.simple_expression()
    if expr is None:
        return NOT_THIS
    lx.expect_eol("voice")
    return Voice(ln.where, expr=expr)


def st_pause(ln, lx, ctx):
    if lx.eol():
        return Pause(ln.where)
    expr = lx.simple_expression()
    if expr is None:
        return NOT_THIS
    lx.expect_eol("pause")
    return Pause(ln.where, expr=expr)


def st_window(ln, lx, ctx):
    for mode in ("show", "hide", "auto"):
        if lx.keyword(mode):
            if mode == "auto":
                if lx.keyword("show"):
                    mode = "auto-show"
                elif lx.keyword("hide"):
                    mode = "auto-hide"
            return Window(ln.where, mode=mode, trans=lx.rest() or None)
    return NOT_THIS


def st_nvl(ln, lx, ctx):
    for mode in ("clear", "show", "hide"):
        if lx.keyword(mode):
            return Nvl(ln.where, mode=mode, trans=lx.rest() or None)
    return NOT_THIS


def st_translate(ln, lx, ctx):
    lang = lx.word()
    if lang is None:
        raise LexError("expected language after translate")
    language = None if lang == "None" else lang
    if lx.keyword("strings"):
        lx.match(":")
        node = TranslateStrings(ln.where, language=language)
        old = None
        for ch in ln.children:
            clx = LineLexer(ch)
            if clx.keyword("old"):
                old = clx.string()
            elif clx.keyword("new"):
                new = clx.string()
                if old is None or new is None:
                    raise LexError(f"bad old/new pair at {ch.where}")
                node.pairs.append((old, new, ch.where))
                old = None
        return node
    if lx.keyword("python"):
        return TranslatePython(ln.where, language=language, code=_code_of(ln, ctx))
    if lx.keyword("style") or lx.keyword("early"):
        return Ignored(ln.where, "translate-style")
    ident = lx.word()
    if ident is None or not lx.match(":"):
        raise LexError("expected translation identifier")
    return Translate(ln.where, language=language, ident=ident, block=parse_block(ln.children, ctx))


def st_screen(ln, lx, ctx):
    return ScreenDef(ln.where, name=lx.word() or "")


def st_style(ln, lx, ctx):
    return Ignored(ln.where, "style")


def st_testcase(ln, lx, ctx):
    return Ignored(ln.where, "testcase")


def st_rpy(ln, lx, ctx):
    if lx.keyword("monologue"):
        mode = lx.word()
        ctx.monologue = {"double": "\n\n", "single": "\n", "none": None}.get(mode, ctx.monologue)
    return Ignored(ln.where, "rpy")


def st_unsupported(feature):
    def handler(ln, lx, ctx):
        return Unsupported(ln.where, feature, ln.text[:60])
    return handler


STATEMENTS = {
    "label": st_label, "jump": st_jump, "call": st_call, "return": st_return, "pass": st_pass,
    "menu": st_menu, "while": st_while, "elif": st_elif, "else": st_else, "python": st_python,
    "init": st_init, "define": st_define, "default": st_default, "image": st_image,
    "transform": st_transform, "show": st_show, "scene": st_scene, "hide": st_hide, "with": st_with,
    "play": st_play, "queue": st_queue, "stop": st_stop, "voice": st_voice, "pause": st_pause,
    "window": st_window, "nvl": st_nvl, "translate": st_translate, "screen": st_screen,
    "style": st_style, "testcase": st_testcase, "testsuite": st_testcase, "rpy": st_rpy,
    "layeredimage": st_unsupported("stmt:layeredimage"), "camera": st_unsupported("stmt:camera"),
}


def parse_rpy_text(src, rel):
    ctx = ParseContext(rel, src.split("\n"))
    return parse_block(parse_source(src, rel), ctx)


def parse_lines(tree, rel, src_lines=()):
    ctx = ParseContext(rel, list(src_lines))
    return parse_block(tree, ctx)


# ---- script discovery ------------------------------------------------------------

def tl_language(rel):
    parts = rel.split("/")
    if len(parts) > 2 and parts[0] == "tl":
        return parts[1]
    return None


def read_scripts(game, language=None):
    """Parse every script of `game`. Translation folders other than `language` are skipped."""
    from .rpyc_decode import RpycError, decode_rpyc

    files = game.files()
    stems = {}
    for rel in files:
        low = rel.lower()
        if low.endswith(".rpy"):
            stems.setdefault(rel[:-4], {})[".rpy"] = rel
        elif low.endswith(".rpyc"):
            stems.setdefault(rel[:-5], {})[".rpyc"] = rel
    out, errors = [], []
    for stem in sorted(stems):
        lang = tl_language(stem + ".rpy")
        if lang is not None and lang != language:
            continue
        variants = stems[stem]
        rel = variants.get(".rpy") or variants[".rpyc"]
        ui = os.path.basename(rel).rsplit(".", 1)[0] + ".rpy" in UI_FILES
        if rel.endswith(".rpy"):
            src = decode_source(game.read(rel))
            nodes = parse_rpy_text(src, rel)
            out.append(ScriptFile(rel, nodes, ui, lang, ".rpy"))
        else:
            try:
                tree = decode_rpyc(game.read(rel), rel)
            except RpycError as e:
                errors.append(CompileError(rel, str(e), "rpyc"))
                continue
            out.append(ScriptFile(rel, parse_lines(tree, rel), ui, lang, ".rpyc"))
    return out, errors
