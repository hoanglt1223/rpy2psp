"""Statement compiler: Ren'Py statement nodes -> VN bytecode program + asset manifest.

Pipeline: read scripts -> collect definitions -> run init-time definitions at
build time in Ren'Py priority order -> compile the story flow -> serialize.
Every construct outside the supported subset is reported as a CompileError
with file and line; compilation continues so one run lists all of them.
"""

import ast
import hashlib
import io
import json
import os
import re
import unicodedata
from dataclasses import dataclass, field

from . import atl as atl_mod
from .bytecode import (FN, NVL_MODES, SAY_INTERACT, WINDOW_MODES, Assembler, Label as AsmLabel,
                       StringTable, build_container)
from .expr import ExprCompiler, ExprError, NotStatic, StaticEvaluator, dotted, parse_code, parse_expr
from .reader import (Call, CompileError, Define, If, Ignored, Image, Init, Jump, Label, Menu, Nvl, Pass,
                     Pause, Play, Python, Return, Say, ScreenDef, ScreenUse, Show, Transform, Translate,
                     TranslatePython, TranslateStrings, Unsupported, Voice, While, Window, With,
                     load_game, parse_rpy_text, read_scripts)
from .translate import TranslationIds, translatable

IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".webp", ".avif")
AUDIO_EXTS = (".wav", ".mp2", ".mp3", ".ogg", ".opus", ".flac")
CFG_ROOTS = {"config", "gui", "build", "style", "theme", "layout", "_preferences", "preferences",
             "_game_menu_screen", "_confirm_quit", "iap", "achievement", "updater", "mouse_visible"}
TEXT_TAGS = {"b", "i", "u", "s", "color", "size", "font", "w", "p", "nw", "fast", "cps", "a", "vspace",
             "space", "alpha"}
TAG_RE = re.compile(r"\{\{|\{(/?)([^}=\s]*)(?:=([^}]*))?\}")
INTERP_FIELD_RE = re.compile(r"^\s*([A-Za-z_][\w.]*)((?:\[[^\[\]]+\])*)(?:![rstiulcq]+)*(?::[^\[\]]*)?\s*$")


def interpolations(s):
    """Bodies of [field] interpolations in a Ren'Py string; `[[` is an escaped bracket."""
    out, i, n = [], 0, len(s)
    while i < n:
        if s.startswith("[[", i):
            i += 2
            continue
        if s[i] != "[":
            i += 1
            continue
        depth, j = 0, i
        while j < n:
            if s[j] == "[":
                depth += 1
            elif s[j] == "]":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append(s[i + 1:j])
        i = j + 1
    return out
CROPMOVE_MODES = ["wiperight", "wipeleft", "wipeup", "wipedown", "slideright", "slideleft", "slideup",
                  "slidedown", "slideawayright", "slideawayleft", "slideawayup", "slideawaydown"]
PUSH_MODES = ["pushright", "pushleft", "pushup", "pushdown"]
SPECIAL_SCREENS = set("""
say input choice quick_menu navigation main_menu game_menu about save load file_slots preferences
pref history help keyboard_help mouse_help gamepad_help confirm skip_indicator notify nvl
nvl_dialogue bubble ctc window side_image yesno_prompt file_picker
""".split())

# Transition kinds (TRAN section)
T_DISSOLVE, T_FADE, T_PIXELLATE, T_MOVE, T_CROPMOVE, T_PUSHMOVE, T_SHAKE, T_PAUSE = range(1, 9)
# Displayable kinds (DISP section)
D_TEXTURE, D_SOLID, D_NULL, D_FRAME, D_ATL, D_TEXT, D_TRANSFORMED = range(7)
# Asset kinds (ASET section)
A_IMAGE, A_MUSIC, A_SFX, A_VOICE, A_FONT, A_MOVIE = range(6)
CHAR_CENTERED, CHAR_EXTEND, CHAR_DYNAMIC_NAME = 1, 2, 4

PRELUDE = '''
init -1400:
    transform reset:
        xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0 alpha 1.0 zoom 1.0 xzoom 1.0 yzoom 1.0 rotate None
    transform default:
        xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0
    transform left:
        xpos 0.0 xanchor 0.0 ypos 1.0 yanchor 1.0
    transform right:
        xpos 1.0 xanchor 1.0 ypos 1.0 yanchor 1.0
    transform center:
        xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0
    transform truecenter:
        xpos 0.5 xanchor 0.5 ypos 0.5 yanchor 0.5
    transform topleft:
        xpos 0.0 xanchor 0.0 ypos 0.0 yanchor 0.0
    transform topright:
        xpos 1.0 xanchor 1.0 ypos 0.0 yanchor 0.0
    transform top:
        xpos 0.5 xanchor 0.5 ypos 0.0 yanchor 0.0
    transform offscreenleft:
        xpos 0.0 xanchor 1.0 ypos 1.0 yanchor 1.0
    transform offscreenright:
        xpos 1.0 xanchor 0.0 ypos 1.0 yanchor 1.0
    transform _moveleft:
        xpos 0.0 xanchor 1.0
    transform _moveright:
        xpos 1.0 xanchor 0.0
    transform _movetop:
        ypos 0.0 yanchor 1.0
    transform _movebottom:
        ypos 1.0 yanchor 0.0
    image black = Solid("#000")
    define fade = Fade(.5, 0, .5)
    define dissolve = Dissolve(0.5)
    define pixellate = Pixellate(1.0, 5)
    define vpunch = Move((0, 10), (0, -10), .10, bounce=True, repeat=True, delay=.275)
    define hpunch = Move((15, 0), (-15, 0), .10, bounce=True, repeat=True, delay=.275)
init -1400 python:
    define.move_transitions("move", 0.5)
    define.move_transitions("ease", 0.5, _warper.ease, _warper.easein, _warper.easeout)
'''


# ---- static values ------------------------------------------------------------------

@dataclass
class Character:
    name: str = None
    who_color: tuple = None
    what_color: tuple = None
    kind: int = 0
    flags: int = 0
    image: str = None
    side_image: int = None
    what_prefix: str = ""
    what_suffix: str = ""
    who_prefix: str = ""
    who_suffix: str = ""
    ctc: int = None


@dataclass
class Disp:
    kind: int
    asset: int = None
    color: tuple = None
    width: float = 0.0
    height: float = 0.0
    transform: int = None
    child: int = None
    text: str = None
    borders: tuple = (0.0, 0.0, 0.0, 0.0)


@dataclass
class Transition:
    kind: int
    warper: int = 0
    params: tuple = (0.0,) * 6
    color: tuple = None
    enter: int = None
    leave: int = None
    enter_warper: int = 0
    leave_warper: int = 0


@dataclass
class ShowRec:
    tag: str
    attrs: list = field(default_factory=list)
    at: list = field(default_factory=list)
    as_: str = None
    behind: list = field(default_factory=list)
    zorder: int = None
    layer: str = "master"
    atl: int = None
    displayable: int = None


@dataclass
class AudioRec:
    channel: int
    flags: int = 0
    fadein: float = 0.0
    fadeout: float = -1.0
    volume: float = 1.0
    assets: list = field(default_factory=list)


@dataclass
class Channel:
    name: str
    mixer: int
    loop: bool


class CharV:
    def __init__(self, index):
        self.index = index


class TransV:
    def __init__(self, index):
        self.index = index


class TransformV:
    """A transform: compiled instructions, or an ATL definition with parameters."""

    def __init__(self, instrs=None, atl=None, params=None, where=None, name=None):
        self.instrs = instrs
        self.atl = atl
        self.params = params or []
        self.where = where
        self.name = name


class DispV:
    def __init__(self, recipe, props=None):
        self.recipe = recipe
        self.props = props or []


class WarperV:
    def __init__(self, name):
        self.name = name


class MarkerV:
    """A Ren'Py name we know but do not support (reported on use)."""

    def __init__(self, name, why):
        self.name = name
        self.why = why


class ImageNameV:
    def __init__(self, name):
        self.name = tuple(name)


@dataclass
class LabelInfo:
    name: str
    where: str
    params: list
    entry: AsmLabel
    bound: AsmLabel


def color(value, where):
    if isinstance(value, (tuple, list)) and len(value) in (3, 4) and all(isinstance(v, int) for v in value):
        c = list(value) + [255] * (4 - len(value))
        return tuple(max(0, min(255, int(v))) for v in c)
    if not isinstance(value, str) or not value.startswith("#"):
        raise CompileError(where, f"expected a colour like '#rrggbb', got {value!r}")
    h = value[1:]
    if len(h) in (3, 4):
        h = "".join(c * 2 for c in h)
    if len(h) == 6:
        h += "ff"
    if len(h) != 8 or not re.fullmatch(r"[0-9a-fA-F]{8}", h):
        raise CompileError(where, f"bad colour {value!r}")
    return tuple(int(h[i:i + 2], 16) for i in range(0, 8, 2))


def recipe_key(recipe):
    return hashlib.sha1(json.dumps(recipe, sort_keys=True).encode()).hexdigest()[:16]


# ---- the compiler ------------------------------------------------------------------

class Compiler:
    def __init__(self, game, language=None):
        self.game = game
        self.language = language
        self.errors = []
        self.warnings = []
        self.strings = StringTable()
        self.asm = Assembler(self.strings)
        self.symbols = {}
        self.persistent = set()
        self.labels = {}
        self.characters = []
        self.images = {}
        self.image_order = []
        self.displayables = []
        self.disp_keys = {}
        self.transforms = []
        self.transform_ids = {}
        self.transitions = []
        self.shows = []
        self.audio = []
        self.channels = [Channel("music", 0, True), Channel("sound", 1, False), Channel("voice", 2, False),
                         Channel("audio", 1, False)]
        self.assets = {}
        self.asset_index = {}
        self.static = {}
        self.assigned = set()
        self.config = {"screen_width": None, "screen_height": None, "fonts": {}, "outlines": False}
        self.tl_blocks = {}
        self.tl_strings = {}
        self.tl_used = set()
        self.scene_assets = {}
        self.texts = set()
        self.text_fonts = set()
        self.current_scene = None
        self.current_store = None
        self.flags = 0
        self.ui_file = False
        self.tl_ids = TranslationIds()
        self.image_size_cache = {}
        self.disp_recipes = {}
        self._builtins()

    # -- bookkeeping ------------------------------------------------------------
    def error(self, where, msg, feature=None):
        self.errors.append(CompileError(where, msg, feature))

    def warn(self, where, msg):
        self.warnings.append(f"{where}: {msg}")

    def sym(self, name):
        if name.startswith("store."):
            name = name[len("store."):]
        i = self.symbols.get(name)
        if i is None:
            if len(self.symbols) >= 0xFFFF:
                raise CompileError("?", "too many variables")
            i = self.symbols[name] = len(self.symbols)
            if name.startswith("persistent."):
                self.persistent.add(name)
        return i

    def symbol_list(self):
        return [(n, 1 if n in self.persistent else 0) for n, _ in sorted(self.symbols.items(), key=lambda x: x[1])]

    def asset(self, key, kind, recipe):
        if key not in self.asset_index:
            self.asset_index[key] = len(self.asset_index)
            self.assets[key] = dict(recipe, kind=["image", "music", "sfx", "voice", "font", "movie"][kind])
        if self.current_scene is not None:
            self.scene_assets.setdefault(self.current_scene, set()).add(key)
        return self.asset_index[key]

    def asset_list(self):
        kinds = {"image": A_IMAGE, "music": A_MUSIC, "sfx": A_SFX, "voice": A_VOICE, "font": A_FONT,
                 "movie": A_MOVIE}
        return [(k, kinds[self.assets[k]["kind"]]) for k, _ in sorted(self.asset_index.items(), key=lambda x: x[1])]

    def tr(self, s):
        return self.tl_strings.get(s, s)

    def _builtins(self):
        self.characters = [Character(), Character(flags=CHAR_CENTERED), Character(kind=1),
                           Character(flags=CHAR_EXTEND), Character(flags=CHAR_CENTERED)]
        self.static.update({"narrator": CharV(0), "adv_narrator": CharV(0), "_narrator": CharV(0),
                            "centered": CharV(1), "vcentered": CharV(4), "nvl_narrator": CharV(2),
                            "extend": CharV(3), "adv": CharV(0), "nvl": CharV(2), "None": None})
        for w in ("linear", "ease", "easein", "easeout"):
            self.static["_warper." + w] = WarperV(w)
        for name in ("irisin", "irisout", "zoomin", "zoomout", "zoominout", "blinds", "squares", "swing"):
            self.static[name] = MarkerV(name, f"transition {name} is not supported")
        for i, m in enumerate(CROPMOVE_MODES):
            self.static[m] = TransV(self.add_transition(Transition(T_CROPMOVE, params=(1.0, float(i), 0, 0, 0, 0))))
        for i, m in enumerate(PUSH_MODES):
            self.static[m] = TransV(self.add_transition(Transition(T_PUSHMOVE, params=(1.0, float(i), 0, 0, 0, 0))))

    def add_transition(self, t):
        self.transitions.append(t)
        return len(self.transitions) - 1

    # -- top level ----------------------------------------------------------------
    def compile(self):
        prelude = parse_rpy_text(PRELUDE, "<prelude>")
        files, read_errors = read_scripts(self.game, self.language)
        self.errors.extend(read_errors)
        self.files = files
        all_files = [("<prelude>", prelude, False)] + [(f.rel, f.nodes, f.ui) for f in files]
        for rel, nodes, ui in all_files:
            self.ui_file = ui
            self.collect(nodes, rel)
        self.ui_file = False
        self.scan_images_dir()
        self.scan_audio_dir()
        self.init_entry = AsmLabel("__init__")
        self.defaults_entry = AsmLabel("__defaults__")
        self.asm.place(self.init_entry)
        self.run_init(all_files)
        self.asm.emit("RETURN")
        self.asm.place(self.defaults_entry)
        for where, name, node in self.defaults:
            self.compile_default(where, name, node)
        self.asm.emit("RETURN")
        for f in files:
            if f.ui or f.tl_language is not None:
                continue
            self.current_file = f.rel
            self.tl_ids.start_file()
            self.flow(f.nodes, top=True)
            self.asm.emit("RETURN")
        for ident in sorted(set(self.tl_blocks) - self.tl_used):
            self.warn("tl", f"translation {ident!r} does not match any dialogue")
        if "start" not in self.labels:
            self.error(files[0].rel if files else "?", "the game has no 'start' label")
            self.start_entry = self.init_entry
        else:
            self.start_entry = self.labels["start"].entry
        try:
            self.asm.resolve()
        except ValueError as e:
            self.error("?", f"internal: {e}")
        return self

    # -- pass 1: collect labels, definitions, translations, assigned names ---------
    def collect(self, nodes, rel, prio=None):
        for n in nodes:
            if isinstance(n, Label):
                if n.name in self.labels:
                    self.error(n.where, f"label {n.name!r} is defined twice (also at {self.labels[n.name].where})")
                else:
                    params = self.label_params(n)
                    entry = AsmLabel(n.name)
                    self.labels[n.name] = LabelInfo(n.name, n.where, params, entry,
                                                    AsmLabel(n.name + "$bound") if params else entry)
                    for p, _ in params:
                        self.assigned.add(p)
                self.collect(n.block, rel)
            elif isinstance(n, Init):
                self.collect(n.block, rel, n.priority)
            elif isinstance(n, Define):
                self.assigned.add(n.name)
            elif isinstance(n, Python):
                self.collect_python_names(n)
            elif isinstance(n, Menu):
                if n.name:
                    self.labels.setdefault(n.name, LabelInfo(n.name, n.where, [], AsmLabel(n.name),
                                                             AsmLabel(n.name)))
                for s in n.caption:
                    pass
                for it in n.items:
                    self.collect(it.block, rel)
            elif isinstance(n, If):
                for _, block, _ in n.entries:
                    self.collect(block, rel)
            elif isinstance(n, While):
                self.collect(n.block, rel)
            elif isinstance(n, Call) and n.from_:
                if n.from_ not in self.labels:
                    self.labels[n.from_] = LabelInfo(n.from_, n.where, [], AsmLabel(n.from_), AsmLabel(n.from_))
            elif isinstance(n, Translate):
                if n.language is not None and n.language == self.language:
                    if n.ident in self.tl_blocks:
                        self.error(n.where, f"translation {n.ident!r} for {n.language} is defined twice")
                    self.tl_blocks[n.ident] = n.block
                    self.collect(n.block, rel)
            elif isinstance(n, TranslateStrings):
                if n.language is not None and n.language == self.language:
                    for old, new, _ in n.pairs:
                        self.tl_strings[old] = new
            elif isinstance(n, ScreenDef):
                self.static.setdefault("screen:" + n.name, True)
        self.assigned.add("_return")

    def label_params(self, n):
        if not n.params:
            return []
        try:
            fn = ast.parse(f"def f{n.params}: pass").body[0]
        except SyntaxError:
            self.error(n.where, f"bad label parameters {n.params}")
            return []
        a = fn.args
        if a.vararg or a.kwarg or a.kwonlyargs or a.posonlyargs:
            self.error(n.where, "label parameters with *, ** or keyword-only markers are not supported",
                       "label:params")
        names = [x.arg for x in a.args]
        defaults = [None] * (len(names) - len(a.defaults)) + list(a.defaults)
        return list(zip(names, defaults))

    def collect_python_names(self, n):
        try:
            stmts = parse_code(n.code, n.where)
        except ExprError:
            return
        prefix = "" if n.store == "store" else n.store[len("store."):] + "."
        for node in ast.walk(ast.Module(body=stmts, type_ignores=[])):
            targets = []
            if isinstance(node, ast.Assign):
                targets = node.targets
            elif isinstance(node, (ast.AugAssign, ast.AnnAssign)):
                targets = [node.target]
            for t in targets:
                d = dotted(t)
                if d and not d.split(".")[0] in CFG_ROOTS:
                    self.assigned.add(prefix + d if isinstance(t, ast.Name) else d)

    # -- images directory ---------------------------------------------------------
    def scan_images_dir(self):
        for rel in self.game.files():
            if not rel.startswith("images/") or not rel.lower().endswith(IMAGE_EXTS):
                continue
            base = os.path.splitext(os.path.basename(rel))[0].lower().partition("@")[0]
            name = tuple(base.split())
            if name and name not in self.images:
                self.define_image(name, DispV({"op": "file", "path": rel}), "<images>")

    def scan_audio_dir(self):
        for rel in self.game.files():
            if rel.startswith("audio/") and rel.lower().endswith(AUDIO_EXTS):
                base = os.path.splitext(os.path.basename(rel))[0].lower()
                self.static.setdefault("audio." + base, rel)

    def define_image(self, name, value, where):
        try:
            did = self.displayable(value, where)
        except CompileError as e:
            self.errors.append(e)
            return
        if name not in self.images:
            self.image_order.append(name)
        self.images[name] = did

    # -- pass 2: init-time definitions ----------------------------------------------
    def run_init(self, all_files):
        items = []
        seq = 0

        def gather(nodes, rel, ui, prio=None):
            nonlocal seq
            for n in nodes:
                if isinstance(n, Init):
                    gather(n.block, rel, ui, n.priority)
                    continue
                if isinstance(n, Label):
                    gather(n.block, rel, ui)
                    continue
                p = None
                if isinstance(n, Define):
                    p = n.priority if prio is None else prio
                elif isinstance(n, (Image, Transform)):
                    p = n.priority if prio is None else prio
                elif isinstance(n, Python) and (n.init or prio is not None):
                    if n.early and not self.all_cfg(n):
                        self.error(n.where, "python early blocks are not supported", "py:early-block")
                        continue
                    p = n.priority if prio is None else prio
                elif isinstance(n, TranslatePython) and n.language == self.language and n.language:
                    p = 1000
                elif isinstance(n, (If, While)) and prio is not None:
                    self.error(n.where, "control flow inside an init block is not supported")
                if p is not None:
                    items.append((p, seq, n, rel, ui))
                    seq += 1
        for rel, nodes, ui in all_files:
            gather(nodes, rel, ui)
        items.sort(key=lambda x: (x[0], x[1]))
        self.defaults = []
        for prio, _, n, rel, ui in items:
            self.ui_file = ui
            before = len(self.errors)
            try:
                if isinstance(n, Define):
                    self.init_define(n)
                elif isinstance(n, Image):
                    self.init_image(n)
                elif isinstance(n, Transform):
                    self.static[n.name] = self.transform_def(n)
                elif isinstance(n, (Python, TranslatePython)):
                    self.init_python(n)
            except CompileError as e:
                self.errors.append(e)
            except ExprError as e:
                self.errors.append(CompileError(e.where, e.msg, e.feature))
            except NotStatic as e:
                self.error(n.where, f"value must be known at build time: {e}", "static:unknown")
            if ui and len(self.errors) > before:
                for e in self.errors[before:]:
                    self.warn(e.where, "ignored in template UI file: " + e.msg)
                del self.errors[before:]
        self.ui_file = False

    def all_cfg(self, n):
        try:
            stmts = parse_code(n.code, n.where)
        except ExprError:
            return False
        return all(self.is_cfg_stmt(s) for s in stmts)

    def is_cfg_stmt(self, st):
        if isinstance(st, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
            targets = st.targets if isinstance(st, ast.Assign) else [st.target]
            return all((dotted(t) or "?").split(".")[0] in CFG_ROOTS for t in targets)
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Call):
            fn = dotted(st.value.func) or ""
            return fn.split(".")[0] in CFG_ROOTS
        return isinstance(st, ast.Pass) or (isinstance(st, ast.Expr) and isinstance(st.value, ast.Constant))

    # config values the build cares about
    def set_config(self, name, value_node, where):
        try:
            value = self.static_eval(value_node, where)
        except (NotStatic, CompileError, ExprError):
            return
        self.static[name] = value
        if name in ("config.screen_width", "config.screen_height") and isinstance(value, int):
            self.config[name.split(".")[1]] = value
        elif name.endswith("_font") or name.endswith(".font"):
            if isinstance(value, str):
                key = name if self.language is None or not self._in_tl else name
                self.config["fonts"][key] = value
        elif "outlines" in name and value:
            self.config["outlines"] = True
        elif name in ("config.name", "config.window_title", "config.version"):
            self.config[name.split(".")[1]] = value if isinstance(value, str) else None
    _in_tl = False

    def init_define(self, n):
        root = n.name.split(".")[0]
        if root in CFG_ROOTS:
            if n.op == "=" and n.index is None:
                self.set_config(n.name, parse_expr(n.expr, n.where), n.where)
            return
        if n.default:
            self.defaults.append((n.where, n.name, parse_expr(n.expr, n.where)))
            return
        node = parse_expr(n.expr, n.where)
        if n.index is not None or n.op != "=":
            self.runtime_define(n, node)
            return
        try:
            value = self.static_eval(node, n.where)
        except NotStatic:
            self.runtime_define(n, node)
            return
        self.bind_static(n.name, value, n.where)

    def bind_static(self, name, value, where):
        self.static[name] = value
        if self.is_runtime_value(value):
            try:
                self.expr_compiler().const(self._runtime_form(value), where)
            except ExprError as e:
                raise CompileError(e.where, e.msg) from None
            self.asm.emit("STORE", self.sym(name))

    def is_runtime_value(self, v):
        if v is None or isinstance(v, (bool, int, float, str)):
            return True
        if isinstance(v, (tuple, list)):
            return all(self.is_runtime_value(x) for x in v)
        if isinstance(v, dict):
            return all(self.is_runtime_value(k) and self.is_runtime_value(x) for k, x in v.items())
        if isinstance(v, set):
            return all(self.is_runtime_value(x) for x in v)
        return False

    def _runtime_form(self, v):
        if isinstance(v, set):
            return sorted(v, key=repr)
        return v

    def runtime_define(self, n, node):
        ec = self.expr_compiler()
        target = self.sym(n.name)
        if n.index is not None:
            self.asm.emit("LOAD", target)
            ec.compile(parse_expr(n.index, n.where), n.where)
            ec.compile(node, n.where)
            self.asm.emit("STORE_SUBSCR")
            return
        if n.op == "=":
            ec.compile(node, n.where)
        elif n.op == "+=":
            self.asm.emit("LOAD", target)
            ec.compile(node, n.where)
            self.asm.emit("BINARY", 0)
        else:
            self.error(n.where, f"define with {n.op} is not supported", "define:augmented")
            return
        self.asm.emit("STORE", target)

    def compile_default(self, where, name, node):
        try:
            if name.split(".")[0] in CFG_ROOTS:
                return
            self.expr_compiler().compile(node, where)
            self.asm.emit("STORE_DEFAULT", self.sym(name))
        except ExprError as e:
            self.error(e.where, e.msg, e.feature)

    def init_image(self, n):
        if n.atl is not None:
            instrs = self.compile_atl(n.atl, {}, n.where)
            tid = self.transform_id(instrs)
            self.define_image(n.name, ("atl", tid), n.where)
            return
        node = parse_expr(n.expr, n.where)
        try:
            value = self.static_eval(node, n.where)
        except NotStatic as e:
            raise CompileError(n.where, f"image {' '.join(n.name)!r} is not a build-time displayable ({e})",
                               "image:expr") from None
        self.define_image(n.name, value, n.where)

    def transform_def(self, n):
        params = []
        if n.params:
            try:
                fn = ast.parse(f"def f{n.params}: pass").body[0]
            except SyntaxError:
                raise CompileError(n.where, f"bad transform parameters {n.params}") from None
            a = fn.args
            names = [x.arg for x in a.args + a.kwonlyargs]
            defaults = [None] * (len(a.args) - len(a.defaults)) + list(a.defaults) + list(a.kw_defaults)
            params = list(zip(names, defaults))
            for nm, dflt in params:
                if nm in ("old_widget", "new_widget", "child"):
                    raise CompileError(n.where, f"transform parameter {nm} is not supported", "transform:params")
        tv = TransformV(atl=n.atl, params=params, where=n.where, name=n.name)
        if not params or all(d is not None for _, d in params):
            tv.instrs = self.instantiate(tv, [], {}, n.where)
        return tv

    def instantiate(self, tv, args, kwargs, where):
        if tv.instrs is not None and not args and not kwargs:
            return tv.instrs
        env = {}
        names = [p for p, _ in tv.params]
        if len(args) > len(names):
            raise CompileError(where, f"too many arguments for transform {tv.name}")
        for (p, _), v in zip(tv.params, args):
            env[p] = v
        for k, v in kwargs.items():
            if k not in names:
                raise CompileError(where, f"transform {tv.name} has no parameter {k}")
            env[k] = v
        for p, d in tv.params:
            if p not in env:
                if d is None:
                    raise CompileError(where, f"transform {tv.name} needs a value for {p}")
                env[p] = self.static_eval(d, tv.where)
        return self.compile_atl(tv.atl, env, tv.where)

    def compile_atl(self, block, env, where):
        def ev(text, w, params):
            node = parse_expr(text, w)
            try:
                return self.static_eval(node, w, params)
            except NotStatic as e:
                raise CompileError(w, f"ATL value {text!r} is not known at build time ({e})", "atl:expr") from None

        def include(value, w):
            if isinstance(value, TransformV):
                return self.instantiate(value, [], {}, w)
            return None

        def child(value, w):
            return self.displayable(value, w)

        def transition(value, w):
            return self.transition_id(value, w)

        try:
            return atl_mod.AtlCompiler(ev, child, include, transition).compile(block, env)
        except atl_mod.AtlCompileError:
            raise

    def transform_id(self, instrs):
        key = json.dumps(instrs)
        if key not in self.transform_ids:
            self.transform_ids[key] = len(self.transforms)
            self.transforms.append(instrs)
        return self.transform_ids[key]

    # init python ------------------------------------------------------------------
    def init_python(self, n):
        stmts = parse_code(n.code, n.where)
        self._in_tl = isinstance(n, TranslatePython)
        prefix = "" if getattr(n, "store", "store") == "store" else n.store[len("store."):] + "."
        try:
            for st in stmts:
                w = f"{n.where.rsplit(':', 1)[0]}:{int(n.where.rsplit(':', 1)[1]) + st.lineno}" \
                    if ":" in n.where else n.where
                self.init_stmt(st, w, prefix)
        finally:
            self._in_tl = False

    def init_stmt(self, st, w, prefix, env=None):
        if isinstance(st, ast.For):
            if not isinstance(st.target, ast.Name) or st.orelse:
                raise CompileError(w, "only simple for loops are supported at init time", "py:stmt:For")
            try:
                items = self.static_eval(st.iter, w, env)
            except NotStatic as e:
                raise CompileError(w, f"for loop over a value not known at build time ({e})", "py:stmt:For") from None
            if not isinstance(items, (list, tuple, str)):
                raise CompileError(w, "for loop needs a list", "py:stmt:For")
            for item in items:
                local = dict(env or {})
                local[st.target.id] = item
                for sub in st.body:
                    self.init_stmt(sub, w, prefix, local)
            return
        if isinstance(st, ast.If):
            try:
                test = self.static_eval(st.test, w, env)
            except NotStatic as e:
                raise CompileError(w, f"if condition not known at build time ({e})", "py:stmt:If") from None
            for sub in (st.body if test else st.orelse):
                self.init_stmt(sub, w, prefix, env)
            return
        if env is not None and isinstance(st, ast.Assign) and all(isinstance(t, ast.Name) for t in st.targets):
            try:
                value = self.static_eval(st.value, w, env)
            except NotStatic as e:
                raise CompileError(w, f"value not known at build time inside a loop ({e})", "py:stmt:For") from None
            for t in st.targets:
                env[t.id] = value
            return
        if env is not None and isinstance(st, ast.AugAssign) and isinstance(st.target, ast.Name):
            from .expr import _binop
            env[st.target.id] = _binop(st.op, env[st.target.id], self.static_eval(st.value, w, env))
            return
        if isinstance(st, ast.Import) and all(a.name in ("os", "os.path") for a in st.names):
            return
        if isinstance(st, ast.Assign):
            for t in st.targets:
                name = dotted(t)
                if name is None:
                    if isinstance(t, ast.Subscript):
                        self.error(w, "subscript assignment at init time is not supported", "py:assign:subscript")
                    else:
                        self.error(w, "tuple assignment is not supported", "py:assign:tuple")
                    continue
                if name.split(".")[0] in CFG_ROOTS:
                    self.set_config(name, st.value, w)
                    continue
                if isinstance(t, ast.Name):
                    name = prefix + name
                try:
                    value = self.static_eval(st.value, w)
                except NotStatic:
                    self.runtime_define(Define(w, name=name, expr=ast.unparse(st.value)), st.value)
                    continue
                self.bind_static(name, value, w)
            return
        if isinstance(st, (ast.AugAssign, ast.AnnAssign)):
            name = dotted(st.target) or ""
            if name.split(".")[0] in CFG_ROOTS:
                return
            if isinstance(st, ast.AugAssign) and isinstance(st.op, ast.Add) and name:
                self.runtime_define(Define(w, name=prefix + name if isinstance(st.target, ast.Name) else name,
                                           op="+=", expr=ast.unparse(st.value)), st.value)
                return
            self.error(w, f"unsupported statement in init python: {ast.unparse(st)[:60]!r}", "py:augassign")
            return
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Constant):
            return
        if isinstance(st, ast.Pass):
            return
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Call):
            fn = dotted(st.value.func) or ""
            if fn.split(".")[0] in CFG_ROOTS:
                if fn == "gui.init" and len(st.value.args) == 2:
                    try:
                        wv, hv = (self.static_eval(a, w) for a in st.value.args)
                        self.config["screen_width"], self.config["screen_height"] = int(wv), int(hv)
                        self.static["config.screen_width"], self.static["config.screen_height"] = int(wv), int(hv)
                    except NotStatic:
                        self.error(w, "gui.init size is not a constant")
                return
            if fn == "define.move_transitions":
                self.move_transitions(st.value, w)
                return
            if fn in ("renpy.music.register_channel", "renpy.audio.music.register_channel"):
                self.register_channel(st.value, w)
                return
            if fn == "renpy.image":
                args = [self.static_eval(a, w, env) for a in st.value.args]
                if len(args) != 2:
                    self.error(w, "renpy.image takes a name and a displayable")
                    return
                name = tuple(args[0].split()) if isinstance(args[0], str) else tuple(args[0])
                self.define_image(name, args[1], w)
                return
            if fn in ("renpy.register_style_preference", "renpy.add_layer", "renpy.music.set_volume",
                      "renpy.register_statement", "renpy.add_python_directory") and fn != "renpy.register_statement":
                self.warn(w, f"{fn}() ignored")
                return
            self.error(w, f"unsupported call at init time: {fn or ast.unparse(st.value.func)}()",
                       "py:call:" + (fn or "?"))
            return
        if isinstance(st, ast.Import) or isinstance(st, ast.ImportFrom):
            self.error(w, "import is not supported", "py:stmt:Import")
            return
        self.error(w, f"unsupported Python statement {type(st).__name__} at init time", "py:stmt:" + type(st).__name__)

    def move_transitions(self, call, w):
        args = [self.static_eval(a, w) for a in call.args]
        kw = {k.arg: self.static_eval(k.value, w) for k in call.keywords}
        names = ["prefix", "delay", "time_warp", "in_time_warp", "out_time_warp"]
        p = dict(zip(names, args))
        p.update(kw)
        if not isinstance(p.get("prefix"), str):
            raise CompileError(w, "define.move_transitions needs a prefix string")
        delay = float(p.get("delay", 0.5))
        sides = {"": (None, None), "inright": ("_moveright", None), "inleft": ("_moveleft", None),
                 "intop": ("_movetop", None), "inbottom": ("_movebottom", None),
                 "outright": (None, "_moveright"), "outleft": (None, "_moveleft"),
                 "outtop": (None, "_movetop"), "outbottom": (None, "_movebottom")}
        for suffix, (enter, leave) in sides.items():
            t = Transition(T_MOVE, params=(delay, 0, 0, 0, 0, 0),
                           warper=self.warper_id(p.get("time_warp"), w),
                           enter_warper=self.warper_id(p.get("in_time_warp"), w),
                           leave_warper=self.warper_id(p.get("out_time_warp"), w))
            if enter:
                t.enter = self.transform_id(self.static[enter].instrs)
            if leave:
                t.leave = self.transform_id(self.static[leave].instrs)
            self.static[p["prefix"] + suffix] = TransV(self.add_transition(t))

    def warper_id(self, v, w):
        if v is None:
            return 0
        if isinstance(v, WarperV):
            return atl_mod.WARPERS[v.name]
        raise CompileError(w, "unsupported time warp", "trans:warper")

    def register_channel(self, call, w):
        args = [self.static_eval(a, w) for a in call.args]
        kw = {k.arg: self.static_eval(k.value, w) for k in call.keywords}
        name = args[0] if args else kw.get("name")
        mixer = args[1] if len(args) > 1 else kw.get("mixer", "music")
        loop = args[2] if len(args) > 2 else kw.get("loop", mixer == "music")
        if not isinstance(name, str):
            raise CompileError(w, "register_channel needs a channel name")
        mix = {"music": 0, "sfx": 1, "voice": 2}.get(mixer, 1)
        if any(c.name == name for c in self.channels):
            return
        if len(self.channels) >= 32:
            raise CompileError(w, "too many audio channels")
        self.channels.append(Channel(name, mix, bool(loop)))

    # -- build-time evaluation ---------------------------------------------------------
    def static_eval(self, node, where, params=None):
        params = params or {}

        def lookup(name):
            if name in params:
                return params[name]
            if self.current_store and self.current_store + "." + name in self.static:
                return self.static[self.current_store + "." + name]
            if name in self.static:
                return self.static[name]
            if name.startswith("store.") and name[6:] in self.static:
                return self.static[name[6:]]
            if name in ("config.screen_width", "config.screen_height"):
                v = self.config.get(name.split(".")[1])
                if v is None:
                    raise NotStatic(f"{name} is not set")
                return v
            if name == "config.gamedir":
                return ""
            if name in ("renpy.android", "renpy.ios", "renpy.mobile", "renpy.emscripten", "renpy.windows",
                        "renpy.macintosh", "renpy.linux", "renpy.web"):
                return False
            raise NotStatic(f"{name} is not known at build time")

        def call(name, args, kwargs, node):
            return self.static_call(name, args, kwargs, node, where, params)

        return StaticEvaluator(lookup, call).eval(node)

    def static_call(self, name, args, kwargs, node, where, params):
        ev = lambda n: self.static_eval(n, where, params)
        a = [ev(x) for x in args]
        k = {key: ev(v) for key, v in kwargs.items()}
        base = name.split(".")[-1]
        if name in ("_", "__", "_p"):
            return self.tr(a[0]) if a and isinstance(a[0], str) else (a[0] if a else None)
        if name in ("Character", "ADVCharacter", "NVLCharacter", "DynamicCharacter") or \
                (name in self.static and isinstance(self.static[name], CharV)):
            return self.make_character(name, a, k, where)
        if name in ("Dissolve", "Fade", "Pixellate", "MoveTransition", "Move", "CropMove", "PushMove", "Pause"):
            return self.make_transition(name, a, k, where)
        if name in ("Position", "Transform") and not a:
            return TransformV(instrs=self.props_instrs(k, where))
        if name in self.static and isinstance(self.static[name], TransformV):
            return TransformV(instrs=self.instantiate(self.static[name], a, k, where))
        if name == "renpy.list_files":
            return [f for f in self.game.files() if not f.lower().endswith((".rpy", ".rpyc", ".rpa"))]
        if name == "os.listdir":
            d = a[0].strip("/") if a and isinstance(a[0], str) else ""
            prefix = d + "/" if d else ""
            return sorted({f[len(prefix):].split("/")[0] for f in self.game.files() if f.startswith(prefix)})
        if name == "os.path.join":
            return "/".join(x.strip("/") for x in a if x)
        if name == "os.path.splitext":
            return os.path.splitext(a[0])
        if name == "os.path.basename":
            return os.path.basename(a[0])
        if name == "renpy.variant":
            return a[0] in ("psp", "small", "handheld") if a and isinstance(a[0], str) else False
        if name in ("len", "str", "int", "float", "min", "max", "abs", "round", "bool", "tuple", "list"):
            fn = {"len": len, "str": str, "int": int, "float": float, "min": min, "max": max, "abs": abs,
                  "round": round, "bool": bool, "tuple": tuple, "list": list}[name]
            try:
                return fn(*a)
            except (TypeError, ValueError) as e:
                raise NotStatic(str(e)) from None
        disp = self.make_displayable(name, a, k, where)
        if disp is not None:
            return disp
        if name in ("ImageDissolve", "AlphaDissolve", "ComposeTransition", "MultipleTransition",
                    "ConditionSwitch", "Movie", "LayeredImageProxy", "Animation", "Text", "SnowBlossom",
                    "ParameterizedText", "DynamicDisplayable", "Flatten", "AlphaMask"):
            raise CompileError(where, f"{name}() is not supported", "py:call:" + name)
        raise NotStatic(f"{name}() is not a build-time constructor")

    def props_instrs(self, k, where):
        props = []
        for name, value in k.items():
            if name in atl_mod.PAIRS:
                x, y = atl_mod.PAIRS[name]
                props.append((atl_mod.PROPS[x], atl_mod.encode_value(value[0], where, x)))
                props.append((atl_mod.PROPS[y], atl_mod.encode_value(value[1], where, y)))
            elif name in atl_mod.PROPS:
                props.append((atl_mod.PROPS[name], atl_mod.encode_value(value, where, name)))
            else:
                raise CompileError(where, f"transform property {name!r} is not supported", "atl:prop:" + name)
        return [("set", props)] if props else []

    def make_transition(self, name, a, k, where):
        def arg(i, key, default):
            if len(a) > i:
                return a[i]
            return k.get(key, default)
        if name == "Dissolve":
            t = Transition(T_DISSOLVE, params=(float(arg(0, "time", 0.5)), 0, 0, 0, 0, 0))
            t.warper = self.warper_id(k.get("time_warp"), where)
        elif name == "Fade":
            out = float(arg(0, "out_time", 0.5))
            hold = float(arg(1, "hold_time", 0.0))
            inn = float(arg(2, "in_time", 0.5))
            t = Transition(T_FADE, params=(out, hold, inn, 0, 0, 0), color=color(k.get("color", "#000"), where))
        elif name == "Pixellate":
            t = Transition(T_PIXELLATE, params=(float(arg(0, "time", 1.0)), float(arg(1, "steps", 5)), 0, 0, 0, 0))
        elif name == "MoveTransition":
            t = Transition(T_MOVE, params=(float(arg(0, "delay", 0.5)), 0, 0, 0, 0, 0),
                           warper=self.warper_id(k.get("time_warp"), where),
                           enter_warper=self.warper_id(k.get("enter_time_warp"), where),
                           leave_warper=self.warper_id(k.get("leave_time_warp"), where))
            for side in ("enter", "leave"):
                v = k.get(side)
                if v is not None:
                    if not isinstance(v, TransformV):
                        raise CompileError(where, f"MoveTransition {side}= must be a transform")
                    setattr(t, side, self.transform_id(self.instantiate(v, [], {}, where)))
        elif name == "Move":
            start, end = arg(0, "startpos", (0, 0)), arg(1, "endpos", (0, 0))
            period = float(arg(2, "period", 0.1))
            delay = float(k.get("delay", period))
            flags = (1 if k.get("bounce") else 0) | (2 if k.get("repeat") else 0)
            t = Transition(T_SHAKE, warper=flags, params=(float(start[0]), float(start[1]), float(end[0]),
                                                           float(end[1]), period, delay))
        elif name == "CropMove":
            mode = arg(1, "mode", "slideright")
            if mode not in CROPMOVE_MODES:
                raise CompileError(where, f"CropMove mode {mode!r} is not supported", "trans:ctor:CropMove")
            t = Transition(T_CROPMOVE, params=(float(arg(0, "time", 1.0)), float(CROPMOVE_MODES.index(mode)), 0, 0, 0, 0))
        elif name == "PushMove":
            mode = arg(1, "mode", "pushright")
            if mode not in PUSH_MODES:
                raise CompileError(where, f"PushMove mode {mode!r} is not supported")
            t = Transition(T_PUSHMOVE, params=(float(arg(0, "time", 1.0)), float(PUSH_MODES.index(mode)), 0, 0, 0, 0))
        else:
            t = Transition(T_PAUSE, params=(float(arg(0, "delay", 0.0)), 0, 0, 0, 0, 0))
        return TransV(self.add_transition(t))

    def make_character(self, name, a, k, where):
        c = Character()
        base = k.pop("kind", None)
        if name in self.static and isinstance(self.static[name], CharV):
            base = self.static[name]
        if name == "NVLCharacter":
            c.kind = 1
        if isinstance(base, CharV):
            b = self.characters[base.index]
            c = Character(**{f: getattr(b, f) for f in b.__dataclass_fields__})
            c.flags &= ~(CHAR_EXTEND | CHAR_CENTERED)
        elif base is not None:
            raise CompileError(where, "Character kind= must be another character")
        if a:
            c.name = a[0]
        if "name" in k:
            c.name = k.pop("name")
        dynamic = k.pop("dynamic", False) or name == "DynamicCharacter"
        if dynamic and isinstance(c.name, str):
            c.name = "[" + c.name + "]"
            c.flags |= CHAR_DYNAMIC_NAME
        if c.name is not None and not isinstance(c.name, str):
            raise CompileError(where, "character name must be a string")
        if c.name is not None:
            c.name = self.tr(c.name)
            self.check_text(c.name, where)
        for key, value in list(k.items()):
            if key in ("color", "who_color"):
                c.who_color = color(value, where) if value is not None else None
            elif key == "what_color":
                c.what_color = color(value, where) if value is not None else None
            elif key == "image":
                c.image = value
            elif key in ("what_prefix", "what_suffix", "who_prefix", "who_suffix"):
                if not isinstance(value, str):
                    raise CompileError(where, f"{key} must be a string")
                setattr(c, key, value)
                self.add_text(value)
            elif key == "show_side_image":
                c.side_image = self.displayable(value, where)
            elif key == "ctc":
                c.ctc = self.displayable(value, where) if value is not None else None
            elif key in ("voice_tag", "ctc_position", "ctc_pause", "ctc_timedpause", "show_two_window",
                         "statement_name", "interact", "mode", "callback", "advance", "condition"):
                if key in ("callback", "condition") and value is not None:
                    raise CompileError(where, f"Character {key}= is not supported", "char:kw:" + key)
            elif key.startswith(("what_", "who_", "window_", "show_")) or key in ("screen", "retain"):
                self.warn(where, f"Character style argument {key}= ignored (native text box)")
            else:
                raise CompileError(where, f"Character argument {key}= is not supported", "char:kw:" + key)
        self.characters.append(c)
        return CharV(len(self.characters) - 1)

    # -- displayables and assets ---------------------------------------------------------
    def make_displayable(self, name, a, k, where):
        def child(v):
            r = self.recipe(v, where)
            if r is None:
                raise CompileError(where, f"{name}() child must be an image file or image name")
            return r
        if name == "Image":
            if not a or not isinstance(a[0], str):
                raise CompileError(where, "Image() needs a file name")
            props = {key: v for key, v in k.items() if key in atl_mod.PROPS or key in atl_mod.PAIRS}
            bad = [key for key in k if key not in props]
            if bad:
                self.warn(where, f"Image() arguments ignored: {', '.join(bad)}")
            return DispV({"op": "file", "path": self.resolve_image_file(a[0], where)},
                         self.props_instrs(props, where) if props else [])
        if name == "Solid":
            return DispV({"op": "solid", "color": list(color(a[0], where))})
        if name == "Null":
            return DispV({"op": "null", "size": [int(a[0]) if a else int(k.get("width", 0)),
                                                  int(a[1]) if len(a) > 1 else int(k.get("height", 0))]})
        if name in ("Composite", "LiveComposite", "im.Composite"):
            size = a[0]
            parts = []
            rest = a[1:]
            if len(rest) % 2:
                raise CompileError(where, f"{name}() needs (pos, displayable) pairs")
            for i in range(0, len(rest), 2):
                pos = rest[i]
                parts.append([int(pos[0]), int(pos[1]), child(rest[i + 1])])
            if size is None:
                raise CompileError(where, f"{name}() needs a size")
            return DispV({"op": "composite", "size": [int(size[0]), int(size[1])], "parts": parts})
        if name in ("Crop", "LiveCrop", "im.Crop"):
            rect = a[0] if name != "im.Crop" else (a[1] if len(a) > 2 and not isinstance(a[1], (tuple, list))
                                                  else a[1])
            src = a[1] if name != "im.Crop" else a[0]
            if name == "im.Crop" and len(a) == 5:
                rect = tuple(a[1:5])
            return DispV({"op": "crop", "rect": [int(v) for v in rect], "child": child(src)})
        if name == "im.Scale":
            return DispV({"op": "scale", "size": [int(a[1]), int(a[2])], "child": child(a[0])})
        if name == "im.FactorScale":
            fx = float(a[1])
            fy = float(a[2]) if len(a) > 2 else fx
            return DispV({"op": "factor", "factor": [fx, fy], "child": child(a[0])})
        if name == "im.Flip":
            return DispV({"op": "flip", "horizontal": bool(k.get("horizontal", a[1] if len(a) > 1 else False)),
                          "vertical": bool(k.get("vertical", a[2] if len(a) > 2 else False)), "child": child(a[0])})
        if name in ("im.Grayscale", "im.Sepia"):
            return DispV({"op": name.split(".")[1].lower(), "child": child(a[0])})
        if name == "im.Alpha":
            return DispV({"op": "alpha", "alpha": float(a[1]), "child": child(a[0])})
        if name == "im.MatrixColor":
            m = a[1]
            if not isinstance(m, (tuple, list)) or len(m) not in (20, 25):
                raise CompileError(where, "im.MatrixColor needs a numeric matrix")
            return DispV({"op": "matrix", "matrix": [float(x) for x in m[:20]], "child": child(a[0])})
        if name in ("im.matrix.desaturate", "im.matrix.saturation", "im.matrix.tint", "im.matrix.brightness",
                    "im.matrix.opacity", "im.matrix.invert", "im.matrix.identity"):
            return color_matrix(name.split(".")[-1], a)
        if name == "Frame":
            r = child(a[0])
            nums = [float(x) for x in a[1:5]] if len(a) > 1 else [float(k.get(x, 0)) for x in
                                                                  ("left", "top", "right", "bottom")]
            if len(nums) == 2:
                nums = nums * 2
            return DispV({"op": "frame", "child": r, "borders": nums})
        if name == "Transform" and a:
            props = {key: v for key, v in k.items()}
            v = a[0]
            r = self.recipe(v, where)
            if r is None:
                raise CompileError(where, "Transform() child must be an image")
            return DispV(r, self.props_instrs(props, where))
        if name.startswith("im."):
            raise CompileError(where, f"{name}() is not supported", "py:call:" + name)
        return None

    def recipe(self, v, where):
        """Static texture recipe of a displayable value, or None."""
        if isinstance(v, DispV):
            if v.props:
                raise CompileError(where, "positioned images cannot be nested inside other images")
            return v.recipe
        if isinstance(v, str):
            if v.startswith("#"):
                return {"op": "solid", "color": list(color(v, where))}
            if "." in v or "/" in v:
                return {"op": "file", "path": self.resolve_image_file(v, where)}
            v = ImageNameV(v.split())
        if isinstance(v, ImageNameV):
            did = self.images.get(v.name)
            if did is None:
                raise CompileError(where, f"image {' '.join(v.name)!r} is not defined")
            return self.disp_recipes.get(did)
        return None

    def resolve_image_file(self, path, where):
        path = path.replace("\\", "/")
        if path.lower().endswith(".svg"):
            raise CompileError(where, f"SVG images are not supported: {path}")
        cands = []
        if self.language:
            cands += [f"tl/{self.language}/{path}", f"tl/{self.language}/images/{path}"]
        cands += [path, "images/" + path]
        for c in cands:
            if self.game.exists(c):
                return self.game.resolve(c)
        raise CompileError(where, f"image file {path!r} not found", "image:file-missing")

    def image_size(self, rel):
        if rel not in self.image_size_cache:
            rel = self.game.resolve(rel) or rel
            if rel in self.game.listed and rel not in self.game.loose:
                return (0, 0)
            from PIL import Image as PILImage
            try:
                with PILImage.open(io.BytesIO(self.game.read(rel))) as im:
                    self.image_size_cache[rel] = im.size
            except Exception as e:  # noqa: BLE001 - any decoder failure is a user-facing build error
                raise CompileError(rel, f"cannot read image: {e}") from None
        return self.image_size_cache[rel]

    def recipe_size(self, r):
        op = r["op"]
        if op == "file":
            return self.image_size(r["path"])
        if op in ("composite", "scale", "null"):
            return tuple(r["size"])
        if op == "crop":
            return (r["rect"][2], r["rect"][3])
        if op == "factor":
            w, h = self.recipe_size(r["child"])
            return (int(w * r["factor"][0]), int(h * r["factor"][1]))
        if op == "solid":
            return (0, 0)
        if "child" in r:
            return self.recipe_size(r["child"])
        return (0, 0)

    def displayable(self, v, where):
        if isinstance(v, tuple) and len(v) == 2 and v[0] == "atl":
            return self._disp(Disp(D_ATL, transform=v[1]), ("atl", v[1]))
        if isinstance(v, TransformV):
            raise CompileError(where, "a transform is not a displayable")
        if isinstance(v, ImageNameV) or (isinstance(v, str) and not v.startswith("#") and "." not in v
                                         and "/" not in v):
            name = v.name if isinstance(v, ImageNameV) else tuple(v.split())
            did = self.images.get(name)
            if did is None:
                raise CompileError(where, f"image {' '.join(name)!r} is not defined", "image:undefined")
            return did
        props = v.props if isinstance(v, DispV) else []
        r = v.recipe if isinstance(v, DispV) else self.recipe(v, where)
        if r is None:
            raise CompileError(where, f"not a displayable: {v!r}"[:120], "image:expr")
        if r["op"] == "solid":
            did = self._disp(Disp(D_SOLID, color=tuple(r["color"])), json.dumps(r))
        elif r["op"] == "null":
            did = self._disp(Disp(D_NULL, width=r["size"][0], height=r["size"][1]), json.dumps(r))
        elif r["op"] == "frame":
            inner = r["child"]
            key = "img/" + recipe_key(inner)
            aid = self.asset(key, A_IMAGE, {"recipe": inner})
            w, h = self.recipe_size(inner)
            did = self._disp(Disp(D_FRAME, asset=aid, width=w, height=h, borders=tuple(r["borders"])),
                             json.dumps(r))
        else:
            key = "img/" + recipe_key(r)
            aid = self.asset(key, A_IMAGE, {"recipe": r})
            w, h = self.recipe_size(r)
            did = self._disp(Disp(D_TEXTURE, asset=aid, width=float(w), height=float(h)), json.dumps(r))
        self.disp_recipes[did] = r
        if props:
            tid = self.transform_id(props)
            did = self._disp(Disp(D_TRANSFORMED, child=did, transform=tid), ("tf", did, tid))
        return did

    def _disp(self, d, key):
        key = json.dumps(key) if not isinstance(key, str) else key
        if key in self.disp_keys:
            did = self.disp_keys[key]
            if self.current_scene is not None:
                self.touch_disp(did)
            return did
        self.disp_keys[key] = len(self.displayables)
        self.displayables.append(d)
        return len(self.displayables) - 1

    def touch_disp(self, did, seen=None):
        d = self.displayables[did]
        if d.asset is not None:
            key = next(k for k, i in self.asset_index.items() if i == d.asset)
            self.scene_assets.setdefault(self.current_scene, set()).add(key)
        if d.child is not None:
            self.touch_disp(d.child)
        if d.transform is not None:
            for ins in _walk_atl(self.transforms[d.transform]):
                if ins[0] == "child":
                    self.touch_disp(ins[1])

    def transition_id(self, v, where):
        if v is None:
            return None
        if isinstance(v, TransV):
            return v.index
        if isinstance(v, MarkerV):
            raise CompileError(where, v.why, "trans:" + v.name)
        raise CompileError(where, "with needs a transition known at build time", "trans:variable")

    # -- text ------------------------------------------------------------------------------
    def check_text(self, s, where):
        for m in TAG_RE.finditer(s):
            if m.group(0) == "{{":
                continue
            tag = m.group(2)
            if tag.startswith("#") or tag == "":
                continue
            if tag not in TEXT_TAGS:
                self.error(where, f"text tag {{{tag}}} is not supported", "text:tag:" + tag)
            if tag == "font" and not m.group(1) and m.group(3):
                self.text_fonts.add(m.group(3))
        for fieldtext in interpolations(s):
            fm = INTERP_FIELD_RE.match(fieldtext)
            if not fm:
                self.error(where, f"text interpolation [{fieldtext}] is not supported", "text:interp-expr")
                continue
            name = fm.group(1)
            try:
                self.resolve_name(name, where)
            except ExprError as e:
                self.error(where, f"text interpolation [{fieldtext}]: {e.msg}", "text:interp")
        self.add_text(s)

    def add_text(self, s):
        plain = TAG_RE.sub(lambda m: "{" if m.group(0) == "{{" else "", s)
        plain = plain.replace("[[", "[").replace("%%", "%")
        self.texts.add(unicodedata.normalize("NFC", plain))

    def display_text(self, s, where):
        s = unicodedata.normalize("NFC", self.tr(s))
        self.check_text(s, where)
        return s

    # -- runtime expressions -----------------------------------------------------------------
    def resolve_name(self, name, where):
        if name.startswith("store."):
            name = name[6:]
        if self.current_store and self.current_store + "." + name in self.assigned:
            return "sym", self.sym(self.current_store + "." + name)
        if name in self.assigned or name.startswith("persistent."):
            return "sym", self.sym(name)
        if name in self.static:
            v = self.static[name]
            if self.is_runtime_value(v):
                return "const", self._runtime_form(v)
            kind = type(v).__name__.replace("V", "").lower()
            raise ExprError(where, f"{name} is a {kind}, which cannot be used as a value at run time",
                            "py:value:" + kind)
        if name in ("config.screen_width", "config.screen_height") and self.config.get(name[7:]):
            return "const", self.config[name[7:]]
        if name.split(".")[0] in ("renpy",) and name in ("renpy.android", "renpy.ios", "renpy.mobile",
                                                          "renpy.emscripten", "renpy.web"):
            return "const", False
        root = name.split(".")[0]
        if root in CFG_ROOTS:
            raise ExprError(where, f"{name} cannot be read at run time", "py:attr:" + root)
        raise ExprError(where, f"name {name!r} is never assigned", "py:name-undefined")

    def expr_compiler(self):
        return ExprCompiler(self.asm, self.resolve_name, self.tr, self.special_call)

    def special_call(self, name, node, where):
        ec = self.expr_compiler()
        if name == "renpy.input":
            sig = ["prompt", "default", "allow", "exclude", "length"]
            args = dict(zip(sig, node.args))
            for k in node.keywords:
                if k.arg not in sig and k.arg not in ("with_none", "pixel_width", "multiline", "mask"):
                    raise ExprError(where, f"renpy.input({k.arg}=) is not supported", "py:call:renpy.input")
                args[k.arg] = k.value
            prompt = args.get("prompt")
            if isinstance(prompt, ast.Constant) and isinstance(prompt.value, str):
                self.asm.emit("PUSH_STR", self.display_text(prompt.value, where))
            elif prompt is not None:
                ec.compile(prompt, where)
            else:
                self.asm.emit("PUSH_STR", "")
            for key in ("default", "length"):
                if key in args:
                    ec.compile(args[key], where)
                elif key == "default":
                    self.asm.emit("PUSH_STR", "")
                else:
                    self.asm.emit("PUSH_NONE")
            if "allow" in args or "exclude" in args:
                self.warn(where, "renpy.input allow=/exclude= are not enforced by the PSP keyboard")
            self.add_text("".join(chr(c) for c in range(32, 127)))
            self.asm.emit("CALL_FN", FN["renpy.input"], 3)
            return True
        if name == "renpy.variant":
            v = node.args[0].value if node.args and isinstance(node.args[0], ast.Constant) else None
            ec.const(v in ("psp", "small", "handheld"), where)
            return True
        if name == "renpy.music.set_volume":
            sig = ["volume", "delay", "channel"]
            args = dict(zip(sig, node.args))
            args.update({k.arg: k.value for k in node.keywords})
            ec.compile(args["volume"], where)
            ec.compile(args.get("delay", ast.Constant(0)), where)
            ch = self.static_eval(args.get("channel", ast.Constant("music")), where)
            self.asm.emit("PUSH_INT", self.channel_id(ch, where))
            self.asm.emit("CALL_FN", FN["renpy.music.set_volume"], 3)
            return True
        return False

    def channel_id(self, name, where):
        for i, c in enumerate(self.channels):
            if c.name == name:
                return i
        raise CompileError(where, f"unknown audio channel {name!r}", "audio:channel:unknown")

    # -- pass 3: story flow ---------------------------------------------------------------------
    def flow(self, nodes, top=False):
        group = []

        def flush():
            if not group:
                return
            ident, alt = self.tl_ids.ident(group)
            block = self.tl_blocks.get(ident)
            if block is None and alt:
                ident = alt
                block = self.tl_blocks.get(alt)
            if block is not None:
                self.tl_used.add(ident)
            self.flow_plain(block if block is not None else list(group))
            group.clear()

        for n in nodes:
            if isinstance(n, Menu):
                if n.name and n.name in self.labels:
                    self.asm.place(self.labels[n.name].entry)
                for cap in n.caption:
                    group.append(cap)
                    flush()
                flush()
                self.stmt(n)
            elif isinstance(n, Say):
                group.append(n)
                flush()
            elif translatable(n):
                group.append(n)
            else:
                flush()
                self.stmt(n)
        flush()

    def flow_plain(self, nodes):
        for n in nodes:
            self.stmt(n)

    def stmt(self, n):
        self.asm.mark(n.where)
        before = len(self.errors)
        try:
            handler = getattr(self, "s_" + type(n).__name__, None)
            if handler is None:
                self.error(n.where, f"internal: no handler for {type(n).__name__}")
            else:
                handler(n)
        except CompileError as e:
            self.errors.append(e)
        except ExprError as e:
            self.errors.append(CompileError(e.where, e.msg, e.feature))
        except NotStatic as e:
            self.error(n.where, f"value must be known at build time: {e}", "static:unknown")
        return len(self.errors) == before

    # non-flow nodes
    def s_Define(self, n): pass
    def s_Image(self, n): pass
    def s_Transform(self, n): pass
    def s_Init(self, n): pass
    def s_ScreenDef(self, n): pass
    def s_Ignored(self, n): pass
    def s_Translate(self, n): pass
    def s_TranslateStrings(self, n): pass
    def s_TranslatePython(self, n): pass
    def s_Pass(self, n): pass

    def s_Unsupported(self, n):
        self.error(n.where, f"unsupported statement: {n.text or n.feature}", n.feature)

    def s_Label(self, n):
        info = self.labels.get(n.name)
        if info is None or info.where != n.where:
            return
        if n.name.split(".")[0] != (self.current_scene or "").split(".")[0] or "." not in n.name:
            self.current_scene = n.name if "." not in n.name else n.name.split(".")[0]
        self.tl_ids.enter_label(n)
        self.asm.place(info.entry)
        if info.params:
            ec = self.expr_compiler()
            for p, default in info.params:
                if default is None:
                    self.asm.emit("PUSH_NONE")
                else:
                    ec.compile(default, n.where)
            self.asm.place(info.bound)
            for p, _ in reversed(info.params):
                self.asm.emit("BIND", self.sym(p))
        self.flow(n.block)

    def s_Jump(self, n):
        if n.expression:
            self.expr_compiler().compile(parse_expr(n.target, n.where), n.where)
            self.asm.emit("JUMP_DYNAMIC")
            return
        info = self.labels.get(n.target)
        if info is None:
            raise CompileError(n.where, f"jump to undefined label {n.target!r}", "jump:undefined")
        if any(d is None for _, d in info.params):
            raise CompileError(n.where, f"jump to {n.target!r}, which has required parameters")
        self.asm.emit("JUMP", info.entry)

    def s_Call(self, n):
        ec = self.expr_compiler()
        if n.expression:
            if n.args:
                raise CompileError(n.where, "call expression with arguments is not supported", "call:expression-args")
            ec.compile(parse_expr(n.target, n.where), n.where)
            self.asm.emit("CALL_DYNAMIC")
        else:
            info = self.labels.get(n.target)
            if info is None:
                raise CompileError(n.where, f"call to undefined label {n.target!r}", "call:undefined")
            if n.args is None:
                if any(d is None for _, d in info.params):
                    raise CompileError(n.where, f"call to {n.target!r} needs arguments")
                self.asm.emit("CALL", info.entry)
            else:
                call = parse_expr("f" + n.args, n.where)
                if any(isinstance(a, ast.Starred) for a in call.args) or any(k.arg is None for k in call.keywords):
                    raise CompileError(n.where, "star arguments in call are not supported")
                names = [p for p, _ in info.params]
                values = dict(zip(names, call.args))
                if len(call.args) > len(names):
                    raise CompileError(n.where, f"too many arguments for label {n.target!r}")
                for k in call.keywords:
                    if k.arg not in names:
                        raise CompileError(n.where, f"label {n.target!r} has no parameter {k.arg!r}")
                    values[k.arg] = k.value
                for p, default in info.params:
                    node = values.get(p, default)
                    if node is None:
                        raise CompileError(n.where, f"label {n.target!r} needs a value for {p!r}")
                    ec.compile(node, n.where)
                self.asm.emit("CALL", info.bound)
        if n.from_:
            info = self.labels.get(n.from_)
            if info is not None and info.entry.addr is None:
                self.asm.place(info.entry)

    def s_Return(self, n):
        if n.expr:
            self.expr_compiler().compile(parse_expr(n.expr, n.where), n.where)
            self.asm.emit("STORE", self.sym("_return"))
        self.asm.emit("RETURN")

    def s_If(self, n):
        ec = self.expr_compiler()
        end = AsmLabel("endif")
        for cond, block, where in n.entries:
            nxt = AsmLabel("elif")
            if cond is not None:
                self.asm.mark(where)
                ec.compile(parse_expr(cond, where), where)
                self.asm.emit("JUMP_IF_FALSE", nxt)
            self.flow(block)
            self.asm.emit("JUMP", end)
            self.asm.place(nxt)
        self.asm.place(end)

    def s_While(self, n):
        top, end = AsmLabel("while"), AsmLabel("endwhile")
        self.asm.place(top)
        self.expr_compiler().compile(parse_expr(n.cond, n.where), n.where)
        self.asm.emit("JUMP_IF_FALSE", end)
        self.flow(n.block)
        self.asm.emit("JUMP", top)
        self.asm.place(end)

    def s_Menu(self, n):
        if n.args:
            raise CompileError(n.where, "menu arguments are not supported", "menu:args")
        ec = self.expr_compiler()
        set_node = parse_expr(n.set_expr, n.where) if n.set_expr else None
        items = []
        for it in n.items:
            if it.args:
                raise CompileError(it.where, "menu choice arguments are not supported", "menu:choice-args")
            text = self.display_text(it.text, it.where)
            if it.cond:
                ec.compile(parse_expr(it.cond, it.where), it.where)
            else:
                self.asm.emit("PUSH_TRUE")
            if set_node is not None:
                end_and = AsmLabel("and")
                self.asm.emit("JUMP_IF_FALSE_OR_POP", end_and)
                self.asm.emit("PUSH_STR", it.text)
                ec.compile(set_node, n.where)
                self.asm.emit("COMPARE", 7)
                self.asm.place(end_and)
            items.append((text, AsmLabel("choice")))
        if len(items) > 255:
            raise CompileError(n.where, "too many menu choices")
        end = AsmLabel("endmenu")
        self.asm.emit("MENU", items)
        for it, (_, lab) in zip(n.items, items):
            self.asm.place(lab)
            if set_node is not None:
                ec.compile(set_node, n.where)
                self.asm.emit("PUSH_STR", it.text)
                self.asm.emit("CALL_FN", FN["menu_set_add"], 2)
                self.asm.emit("POP")
            self.flow(it.block)
            self.asm.emit("JUMP", end)
        self.asm.place(end)

    def character_for(self, n):
        if n.who is None:
            return 0
        if n.who_is_string:
            name = ast.literal_eval(n.who)
            c = Character(name=self.display_text(name, n.where))
            for i, existing in enumerate(self.characters):
                if existing == c:
                    return i
            self.characters.append(c)
            return len(self.characters) - 1
        try:
            v = self.static_eval(parse_expr(n.who, n.where), n.where)
        except NotStatic:
            raise CompileError(n.where, f"speaker {n.who!r} is not a character defined at build time",
                               "say:who-unknown") from None
        if not isinstance(v, CharV):
            if isinstance(v, str):
                c = Character(name=self.display_text(v, n.where))
                self.characters.append(c)
                return len(self.characters) - 1
            raise CompileError(n.where, f"speaker {n.who!r} is not a character", "say:who-unknown")
        return v.index

    def s_Say(self, n):
        if n.args:
            raise CompileError(n.where, "say arguments are not supported", "say:args")
        char = self.character_for(n)
        c = self.characters[char]
        if (n.attrs or n.temp_attrs) and c.flags & CHAR_EXTEND:
            self.asm.emit("SAY_ATTRS", char, 1 if n.temp_attrs else 0, n.attrs or n.temp_attrs)
        elif n.attrs or n.temp_attrs:
            if c.image is None:
                raise CompileError(n.where, f"say attributes need a character with image=", "say:attrs")
            for a in n.attrs + n.temp_attrs:
                a2 = a.lstrip("-")
                if not any(name[0] in (c.image, "side") and a2 in name[1:] for name in self.images):
                    raise CompileError(n.where, f"no image of {c.image!r} has attribute {a2!r}", "say:attrs")
            if n.attrs:
                self.asm.emit("SAY_ATTRS", char, 0, n.attrs)
            if n.temp_attrs:
                self.asm.emit("SAY_ATTRS", char, 1, n.temp_attrs)
        trans = self.transition_id(self.static_eval_trans(n.with_, n.where), n.where) if n.with_ else None
        text = self.display_text(n.what, n.where)
        self.asm.emit("SAY", char, text, trans, SAY_INTERACT if n.interact else 0)

    def static_eval_trans(self, text, where):
        try:
            return self.static_eval(parse_expr(text, where), where)
        except NotStatic:
            raise CompileError(where, f"transition {text!r} is not known at build time", "trans:variable") from None

    def s_With(self, n):
        v = self.static_eval_trans(n.expr, n.where)
        self.asm.emit("WITH", self.transition_id(v, n.where))

    def s_Show(self, n):
        if n.layer and n.layer != "master":
            raise CompileError(n.where, f"onlayer {n.layer} is not supported", n.kind + ":onlayer")
        if n.kind == "scene" and n.name is None and n.expression is None:
            self.asm.emit("SCENE", None)
            if n.with_:
                self.asm.emit("WITH", self.transition_id(self.static_eval_trans(n.with_, n.where), n.where))
            return
        rec = ShowRec(tag="")
        dynamic = None
        if n.text is not None:
            text = self.display_text(n.text, n.where)
            rec.tag = n.as_ or "text"
            rec.displayable = self._disp(Disp(D_TEXT, text=text), ("text", text))
        elif n.expression is not None:
            node = parse_expr(n.expression, n.where)
            try:
                v = self.static_eval(node, n.where)
                if isinstance(v, str) and not v.startswith("#") and "." not in v and "/" not in v:
                    v = ImageNameV(v.split())
                rec.displayable = self.displayable(v, n.where)
                if isinstance(v, ImageNameV):
                    rec.tag = n.as_ or v.name[0]
                    rec.displayable = None
                    rec.attrs = list(v.name[1:])
                    self.check_image(v.name, n.where)
                else:
                    rec.tag = n.as_ or n.expression
            except NotStatic:
                if n.kind == "hide":
                    raise CompileError(n.where, "hide expression needs a build-time value") from None
                dynamic = node
                rec.tag = n.as_ or ""
        else:
            rec.tag = n.as_ or n.name[0]
            rec.attrs = list(n.name[1:])
            if n.kind != "hide":
                self.check_image(n.name, n.where)
            elif len(n.name) > 1 and not any(name[0] == n.name[0] for name in self.images):
                self.warn(n.where, f"hide of unknown image tag {n.name[0]!r}")
        if n.kind != "hide" and n.name is not None and n.as_:
            rec.attrs = list(n.name)
        for at in n.at_list:
            rec.at.append(self.at_transform(at, n.where))
        rec.behind = list(n.behind)
        if n.zorder is not None:
            z = self.static_eval(parse_expr(n.zorder, n.where), n.where)
            if not isinstance(z, int):
                raise CompileError(n.where, "zorder must be an integer")
            rec.zorder = z
        if n.atl is not None:
            rec.atl = self.transform_id(self.compile_atl(n.atl, {}, n.where))
        if len(self.shows) >= 0xFFFF:
            raise CompileError(n.where, "too many show statements")
        self.shows.append(rec)
        rid = len(self.shows) - 1
        if n.with_:
            self.asm.emit("WITH", None)
        if dynamic is not None:
            self.expr_compiler().compile(dynamic, n.where)
            if n.kind == "scene":
                self.asm.emit("SCENE", None)
            self.asm.emit("SHOW_DYNAMIC", rid)
        else:
            self.asm.emit({"show": "SHOW", "scene": "SCENE", "hide": "HIDE"}[n.kind], rid)
        if n.with_:
            self.asm.emit("WITH", self.transition_id(self.static_eval_trans(n.with_, n.where), n.where))

    def check_image(self, name, where):
        tag = name[0]
        want = {a for a in name[1:] if not a.startswith("-")}
        matches = [nm for nm in self.images if nm[0] == tag and want <= set(nm[1:])]
        if not matches:
            # Ren'Py shows nothing for a missing image when not in developer mode.
            self.warn(where, f"image {' '.join(name)!r} is not defined; it will show nothing")
        for nm in matches:
            self.touch_disp(self.images[nm]) if self.current_scene is not None else None

    def at_transform(self, text, where):
        node = parse_expr(text, where)
        try:
            v = self.static_eval(node, where)
        except NotStatic as e:
            raise CompileError(where, f"at {text!r} is not a build-time transform ({e})", "at:unknown") from None
        if isinstance(v, TransformV):
            return self.transform_id(self.instantiate(v, [], {}, where))
        raise CompileError(where, f"at {text!r} is not a transform", "at:expr")

    def audio_files(self, text, where, kind):
        node = parse_expr(text, where)
        if isinstance(node, ast.Name) and node.id not in self.static and "audio." + node.id in self.static:
            v = self.static["audio." + node.id]
        else:
            try:
                v = self.static_eval(node, where)
            except NotStatic:
                raise CompileError(where, f"audio {text!r} is chosen at run time", "audio:expr") from None
        files = v if isinstance(v, (list, tuple)) else [v]
        out = []
        for f in files:
            if not isinstance(f, str):
                raise CompileError(where, f"audio file must be a string, got {f!r}")
            if f.startswith("<"):
                raise CompileError(where, f"partial audio playback {f[:30]!r} is not supported", "audio:partial")
            path = self.resolve_audio_file(f, where)
            akind = {0: A_MUSIC, 1: A_SFX, 2: A_VOICE}[kind]
            out.append(self.asset(f"snd/{['music', 'sfx', 'voice'][kind]}/{path}", akind, {"path": path}))
        return out

    def resolve_audio_file(self, path, where):
        path = path.replace("\\", "/")
        cands = ([f"tl/{self.language}/{path}"] if self.language else []) + [path, "audio/" + path]
        for c in cands:
            if self.game.exists(c):
                return self.game.resolve(c)
        raise CompileError(where, f"audio file {path!r} not found", "audio:file-missing")

    def s_Play(self, n):
        ch = self.channel_id(n.channel, n.where)
        if n.kind == "stop":
            fade = float(self.static_eval(parse_expr(n.fadeout, n.where), n.where)) if n.fadeout else -1.0
            self.asm.emit("STOP", ch, fade)
            return
        rec = AudioRec(channel=ch)
        rec.assets = self.audio_files(n.file, n.where, self.channels[ch].mixer)
        loop = self.channels[ch].loop if n.loop is None else n.loop
        rec.flags = (1 if loop else 0) | (4 if n.if_changed else 0) | (8 if n.kind == "queue" else 0)
        num = lambda t, d: float(self.static_eval(parse_expr(t, n.where), n.where)) if t else d
        rec.fadein = num(n.fadein, 0.0)
        rec.fadeout = num(n.fadeout, -1.0)
        rec.volume = num(n.volume, 1.0)
        self.audio.append(rec)
        self.asm.emit("PLAY", len(self.audio) - 1)

    def s_Voice(self, n):
        if n.sustain:
            self.asm.emit("VOICE_SUSTAIN")
            return
        ids = self.audio_files(n.expr, n.where, 2)
        self.asm.emit("VOICE", ids[0])

    def s_Pause(self, n):
        if n.expr is None:
            self.asm.emit("PUSH_NONE")
        else:
            self.expr_compiler().compile(parse_expr(n.expr, n.where), n.where)
        self.asm.emit("PAUSE")

    def s_Window(self, n):
        trans = self.transition_id(self.static_eval_trans(n.trans, n.where), n.where) if n.trans else None
        self.asm.emit("WINDOW", WINDOW_MODES.index(n.mode), trans)

    def s_Nvl(self, n):
        trans = self.transition_id(self.static_eval_trans(n.trans, n.where), n.where) if n.trans else None
        self.asm.emit("NVL", NVL_MODES.index(n.mode), trans)

    def s_ScreenUse(self, n):
        if n.name in SPECIAL_SCREENS:
            self.warn(n.where, f"{n.how} screen {n.name} ignored (native UI)")
            return
        raise CompileError(n.where, f"custom screen {n.name!r} is not supported", f"screen:{n.how}:custom")

    # python in the flow
    def s_Python(self, n):
        if n.init:
            return
        if n.early:
            raise CompileError(n.where, "python early is not supported", "py:early-block")
        stmts = parse_code(n.code, n.where)
        self.current_store = None if n.store == "store" else n.store[len("store."):]
        try:
            base = int(n.where.rsplit(":", 1)[1])
            for st in stmts:
                w = n.where if n.inline else f"{n.where.rsplit(':', 1)[0]}:{base + st.lineno}"
                try:
                    self.py_stmt(st, w)
                except ExprError as e:
                    self.errors.append(CompileError(e.where, e.msg, e.feature))
                except CompileError as e:
                    self.errors.append(e)
                except NotStatic as e:
                    self.error(w, f"value must be known at build time: {e}", "static:unknown")
        finally:
            self.current_store = None

    def store_target(self, t, w):
        d = dotted(t)
        if isinstance(t, ast.Name):
            if self.current_store:
                return self.sym(self.current_store + "." + d)
            return self.sym(d)
        if d is not None:
            root = d.split(".")[0]
            if root in CFG_ROOTS:
                raise ExprError(w, f"assigning {d} at run time is not supported", "py:assign:config")
            if d in self.assigned or root == "persistent" or root == "store":
                return self.sym(d)
            raise ExprError(w, f"attribute assignment to {d} is not supported", "py:assign:attr")
        raise ExprError(w, f"unsupported assignment target {ast.unparse(t)[:40]!r}", "py:assign:" +
                        type(t).__name__.lower())

    def py_stmt(self, st, w):
        ec = self.expr_compiler()
        if isinstance(st, ast.Assign):
            ec.compile(st.value, w)
            for i, t in enumerate(st.targets):
                if i < len(st.targets) - 1:
                    self.asm.emit("DUP")
                if isinstance(t, ast.Subscript) and not isinstance(t.slice, ast.Slice):
                    tmp = self.sym("_rpy2psp_tmp")
                    self.asm.emit("STORE", tmp)
                    ec.compile(t.value, w)
                    ec.compile(t.slice, w)
                    self.asm.emit("LOAD", tmp)
                    self.asm.emit("STORE_SUBSCR")
                else:
                    self.asm.emit("STORE", self.store_target(t, w))
            return
        if isinstance(st, ast.AugAssign):
            from .bytecode import BINOPS
            from .expr import BIN
            op = BIN.get(type(st.op))
            if op is None:
                raise ExprError(w, f"operator {type(st.op).__name__} is not supported", "py:op")
            if isinstance(st.target, ast.Subscript):
                ec.compile(st.target.value, w)
                ec.compile(st.target.slice, w)
                ec.compile(st.target, w)
                ec.compile(st.value, w)
                self.asm.emit("BINARY", BINOPS.index(op))
                self.asm.emit("STORE_SUBSCR")
                return
            sym = self.store_target(st.target, w)
            self.asm.emit("LOAD", sym)
            ec.compile(st.value, w)
            self.asm.emit("BINARY", BINOPS.index(op))
            self.asm.emit("STORE", sym)
            return
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Constant):
            return
        if isinstance(st, ast.Pass):
            return
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Call):
            if self.statement_call(st.value, w):
                return
            ec.compile(st.value, w)
            self.asm.emit("POP")
            return
        if isinstance(st, (ast.Import, ast.ImportFrom)):
            raise ExprError(w, "import is not supported", "py:stmt:Import")
        raise ExprError(w, f"Python statement {type(st).__name__} is not supported", "py:stmt:" + type(st).__name__)

    def statement_call(self, call, w):
        name = dotted(call.func)
        ec = self.expr_compiler()
        kw = {k.arg: k.value for k in call.keywords}
        if name == "renpy.pause":
            delay = call.args[0] if call.args else kw.get("delay")
            if "hard" in kw:
                self.warn(w, "renpy.pause(hard=) is treated as a normal pause")
            if delay is None:
                self.asm.emit("PUSH_NONE")
            else:
                ec.compile(delay, w)
            self.asm.emit("PAUSE")
            return True
        if name == "renpy.say":
            who, what = (call.args + [None, None])[:2]
            what = what or kw.get("what")
            who = who if who is not None else kw.get("who")
            char = 0
            if who is not None and not (isinstance(who, ast.Constant) and who.value is None):
                v = self.static_eval(who, w)
                if not isinstance(v, CharV):
                    raise ExprError(w, "renpy.say needs a character known at build time", "say:who-unknown")
                char = v.index
            if isinstance(what, ast.Constant) and isinstance(what.value, str):
                self.asm.emit("SAY", char, self.display_text(what.value, w), None, SAY_INTERACT)
            else:
                ec.compile(what, w)
                self.asm.emit("SAY_DYNAMIC", char, None, SAY_INTERACT)
            return True
        if name in ("renpy.jump", "renpy.call"):
            if len(call.args) != 1 or call.keywords:
                raise ExprError(w, f"{name}() needs exactly one label argument", "py:call:" + name)
            target = call.args[0]
            if isinstance(target, ast.Constant) and isinstance(target.value, str):
                node = (Jump if name == "renpy.jump" else Call)(w, target=target.value)
                (self.s_Jump if name == "renpy.jump" else self.s_Call)(node)
            else:
                ec.compile(target, w)
                self.asm.emit("JUMP_DYNAMIC" if name == "renpy.jump" else "CALL_DYNAMIC")
            return True
        if name in ("renpy.music.play", "renpy.sound.play", "renpy.music.queue", "renpy.sound.queue"):
            sig = ["filenames", "channel", "loop", "fadeout", "synchro_start", "fadein", "tight", "if_changed"]
            args = dict(zip(sig, call.args))
            args.update(kw)
            default_ch = "music" if ".music." in name else "sound"
            ch = self.static_eval(args["channel"], w) if "channel" in args else default_ch
            node = Play(w, kind="queue" if name.endswith("queue") else "play", channel=ch,
                        file=ast.unparse(args["filenames"]),
                        fadein=ast.unparse(args["fadein"]) if "fadein" in args else None,
                        fadeout=ast.unparse(args["fadeout"]) if "fadeout" in args else None)
            if "loop" in args:
                lv = self.static_eval(args["loop"], w)
                node.loop = None if lv is None else bool(lv)
            if "if_changed" in args:
                node.if_changed = bool(self.static_eval(args["if_changed"], w))
            self.s_Play(node)
            return True
        if name in ("renpy.music.stop", "renpy.sound.stop"):
            args = dict(zip(["channel", "fadeout"], call.args))
            args.update(kw)
            ch = self.static_eval(args["channel"], w) if "channel" in args else \
                ("music" if ".music." in name else "sound")
            node = Play(w, kind="stop", channel=ch,
                        fadeout=ast.unparse(args["fadeout"]) if "fadeout" in args else None)
            self.s_Play(node)
            return True
        if name == "renpy.movie_cutscene":
            f = self.static_eval(call.args[0], w)
            path = self.resolve_audio_file(f, w) if not self.game.exists(f) else f
            aid = self.asset("movie/" + path, A_MOVIE, {"path": path})
            self.asm.emit("MOVIE", aid)
            return True
        if name in ("renpy.show", "renpy.hide", "renpy.scene", "renpy.with_statement", "renpy.transition",
                    "renpy.show_screen", "renpy.call_screen", "renpy.hide_screen", "renpy.restart_interaction"):
            raise ExprError(w, f"{name}() is not supported; use the show/hide/with statements", "py:call:" + name)
        return False

    # -- output ------------------------------------------------------------------------------
    @property
    def width(self):
        return self.config["screen_width"] or 800

    @property
    def height(self):
        return self.config["screen_height"] or 600

    def bytecode(self):
        return build_container(self)

    def manifest(self):
        fonts = dict(self.config["fonts"])
        return {
            "format": "rpy2psp-manifest/1",
            "language": self.language,
            "source_size": [self.width, self.height],
            "config": {k: v for k, v in self.config.items() if k not in ("fonts",)},
            "fonts": fonts,
            "text_fonts": sorted(self.text_fonts),
            "glyphs": "".join(sorted(set("".join(self.texts)) | set(chr(c) for c in range(32, 127)))),
            "assets": {k: self.assets[k] for k, _ in sorted(self.asset_index.items(), key=lambda x: x[1])},
            "scenes": {k: sorted(v) for k, v in sorted(self.scene_assets.items())},
        }

    def label_addresses(self):
        out = {}
        for name, info in self.labels.items():
            if info.entry.addr is not None:
                out[info.entry.addr] = name
        out[self.init_entry.addr] = "__init__"
        out.setdefault(self.defaults_entry.addr, "__defaults__")
        return out


def _walk_atl(instrs):
    for ins in instrs:
        yield ins
        if ins[0] in ("block",):
            yield from _walk_atl(ins[1])
        elif ins[0] == "parallel":
            for b in ins[1]:
                yield from _walk_atl(b)
        elif ins[0] == "choice":
            for _, b in ins[1]:
                yield from _walk_atl(b)
        elif ins[0] == "on":
            yield from _walk_atl(ins[2])


class ColorMatrix(tuple):
    """im.matrix value: 4x5 colour matrix; `a * b` applies b first, then a (Ren'Py semantics)."""

    def __mul__(self, other):
        if not isinstance(other, ColorMatrix):
            return NotImplemented
        A = [list(self[r * 5:r * 5 + 5]) for r in range(4)] + [[0, 0, 0, 0, 1]]
        B = [list(other[r * 5:r * 5 + 5]) for r in range(4)] + [[0, 0, 0, 0, 1]]
        C = [[sum(A[i][k] * B[k][j] for k in range(5)) for j in range(5)] for i in range(4)]
        return ColorMatrix(v for row in C for v in row)


def color_matrix(kind, a):
    return ColorMatrix(_color_matrix(kind, a))


def _color_matrix(kind, a):
    ident = [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0]
    if kind == "identity":
        return ident
    if kind in ("desaturate", "saturation"):
        level = 0.0 if kind == "desaturate" else float(a[0])
        r, g, b = 0.2126, 0.7152, 0.0722
        return [r + (1 - r) * level, g - g * level, b - b * level, 0, 0,
                r - r * level, g + (1 - g) * level, b - b * level, 0, 0,
                r - r * level, g - g * level, b + (1 - b) * level, 0, 0,
                0, 0, 0, 1, 0]
    if kind == "tint":
        r, g, b = (float(x) for x in a[:3])
        return [r, 0, 0, 0, 0, 0, g, 0, 0, 0, 0, 0, b, 0, 0, 0, 0, 0, 1, 0]
    if kind == "brightness":
        v = float(a[0])
        return [1, 0, 0, 0, v, 0, 1, 0, 0, v, 0, 0, 1, 0, v, 0, 0, 0, 1, 0]
    if kind == "opacity":
        return [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, float(a[0]), 0]
    if kind == "invert":
        return [-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0]
    raise ValueError(kind)


def compile_game(path, language=None, listed=()):
    game = load_game(path, listed)
    try:
        return Compiler(game, language).compile()
    finally:
        game.close()
