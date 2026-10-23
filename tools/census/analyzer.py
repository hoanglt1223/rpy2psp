"""Feature census for one Ren'Py game directory.

Walks every .rpy file (translations under tl/ are counted separately), parses
statements with rpy_lexer, parses Python fragments with the stdlib `ast`
module (parse only, never compile or exec), and records feature keys such as
`stmt:show`, `trans:dissolve`, `atl:warper:ease`, `py:call:renpy.pause`.
"""

import ast
import warnings
import os
import re
from collections import defaultdict

warnings.filterwarnings("ignore", category=SyntaxWarning)

from rpy_lexer import block_source, parse_file, string_value, tokenize

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".avif", ".svg"}
AUDIO_EXT = {".ogg", ".opus", ".mp3", ".wav", ".flac", ".m4a", ".aac"}
VIDEO_EXT = {".webm", ".ogv", ".mpg", ".mpeg", ".mp4", ".avi", ".mkv", ".m2v"}
FONT_EXT = {".ttf", ".otf", ".ttc", ".woff", ".woff2"}
SCRIPT_EXT = {".rpy", ".rpyc", ".rpym", ".rpymc", ".rpa", ".rpi"}

BUILTIN_TRANSITIONS = set("""
fade dissolve pixellate move moveinright moveinleft moveintop moveinbottom
moveoutright moveoutleft moveouttop moveoutbottom ease easeinright easeinleft
easeintop easeinbottom easeoutright easeoutleft easeouttop easeoutbottom
zoomin zoomout zoominout vpunch hpunch blinds squares wipeleft wiperight
wipeup wipedown slideleft slideright slideup slidedown slideawayleft
slideawayright slideawayup slideawaydown pushright pushleft pushup pushdown
irisin irisout None
""".split())

TRANSITION_CTORS = set("""
Dissolve Fade Pixellate MoveTransition ImageDissolve AlphaDissolve CropMove
PushMove ComposeTransition MultipleTransition Move Swing Transition Pause
""".split())

# Files from the standard Ren'Py project template that only configure the
# default UI. vnpsp replaces that UI natively, so their features get a "ui:"
# prefix and are reported separately from the story script.
UI_FILES = {"gui.rpy", "screens.rpy", "options.rpy"}
PLAIN_PREFIXES = ("cfg:", "ui:", "sl:", "era:", "tl:", "info:", "census:")

CHARACTER_CTORS = {"Character", "DynamicCharacter", "ADVCharacter", "NVLCharacter", "SpriteCharacter"}

BUILTIN_POSITIONS = set("""
left right center truecenter topleft topright top offscreenleft offscreenright
default reset
""".split())

DEFAULT_SCREENS = set("""
say input choice quick_menu navigation main_menu game_menu about save load
file_slots preferences pref history help keyboard_help mouse_help gamepad_help
confirm skip_indicator notify nvl nvl_dialogue bubble file_picker yesno_prompt
ctc window side_image
""".split())

CFG_BASES = {"config", "gui", "build", "style", "theme", "layout", "_preferences",
             "preferences", "_game_menu_screen", "_confirm_quit", "iap", "achievement",
             "updater", "mouse_visible"}

ATL_PROPS = set("""
pos xpos ypos anchor xanchor yanchor align xalign yalign xcenter ycenter
xycenter offset xoffset yoffset rotate rotate_pad transform_anchor zoom xzoom
yzoom nearest alpha additive around alignaround angle radius crop
crop_relative corner1 corner2 xysize size xsize ysize fit subpixel delay events
xtile ytile xpan ypan matrixcolor matrixtransform matrixanchor blur zpos zzoom
perspective point_to orientation xrotate yrotate zrotate mesh mesh_pad shader
blend fps show_cancels_hide maxsize xfill yfill xmaximum ymaximum xminimum
yminimum xanchoraround yanchoraround child
""".split())
ATL_SPLINE = {"knot", "clockwise", "counterclockwise", "circles"}
WARPER_RE = re.compile(r"^(linear|instant|ease(in|out)?(_\w+)?)$")

SAFE_NODES = (ast.Expression, ast.Module, ast.Expr, ast.Constant, ast.Name, ast.Load,
              ast.Store, ast.BinOp, ast.UnaryOp, ast.BoolOp, ast.Compare,
              ast.Subscript, ast.Slice, ast.List, ast.Tuple, ast.Dict, ast.keyword,
              ast.Attribute, ast.Call, ast.operator, ast.unaryop, ast.boolop, ast.cmpop)
SAFE_OPS = {"Add", "Sub", "Mult", "Div", "FloorDiv", "Mod", "USub", "UAdd", "Not",
            "And", "Or", "Eq", "NotEq", "Lt", "LtE", "Gt", "GtE", "In", "NotIn",
            "Is", "IsNot"}
SAFE_ATTR_ROOTS = {"persistent", "store", "audio"}
API_ROOTS = {"renpy", "im", "define", "layout", "theme", "store", "ui", "achievement",
             "gui", "build", "config", "_", "Matrix", "os", "math", "random", "time"}
STATIC_DISPLAYABLES = {"Image", "Solid", "Null", "Composite", "LiveComposite", "Crop",
                       "LiveCrop", "Frame", "Fixed", "Text"}
MOVE_SUFFIXES = ["", "inright", "inleft", "intop", "inbottom",
                 "outright", "outleft", "outtop", "outbottom"]
NAME = r"[\w.\u00a0-\uffff]+"

# A custom screen is "static" when it only lays out displayables and buttons
# whose actions are plain control flow, so it could be compiled to a fixed UI
# tree. Anything that runs Python per frame or needs a Ren'Py-only widget is not.
SL_DYNAMIC = {"$", "python", "for", "drag", "draggroup", "input", "mousearea",
              "transclude", "nearrect", "vpgrid", "areapicker", "showif"}
SL_ACTIONS = {"Jump", "Return", "Call", "SetVariable", "SetScreenVariable",
              "ToggleVariable", "ToggleScreenVariable", "SetDict", "Hide", "Show",
              "NullAction", "AnimatedValue", "If", "Play", "Stop", "Notify",
              "VariableValue", "ScreenVariableValue", "StaticValue", "_", "With",
              "ShowMenu", "Confirm", "Preference", "Rollback", "MainMenu", "Quit"}
SL_NOT_CALLS = {"if", "elif", "while", "not", "and", "or", "in", "hotspot", "sensitive",
                "showif", "area", "action", "hovered", "unhovered", "at", "has"}
CALL_RE = re.compile(r"(?<![\w.])([A-Za-z_][\w.]*)\(")

STMT_KEYWORDS = set("""
label jump call return menu if elif else while pass python init define default
image transform screen style translate show scene hide with play queue stop
voice pause window nvl camera layeredimage testcase testsuite rpy early
""".split())
SHOW_CLAUSES = {"at", "as", "behind", "onlayer", "zorder", "with"}
TAG_RE = re.compile(r"\{(?!\{)/?([^}=\s]*)")
INTERP_RE = re.compile(r"(?<!\[)\[(?!\[)[^\]]+\]")


def dotted(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


def root_name(node):
    while isinstance(node, (ast.Attribute, ast.Subscript, ast.Call)):
        node = node.value if not isinstance(node, ast.Call) else node.func
    return node.id if isinstance(node, ast.Name) else None


class GameCensus:
    def __init__(self, name, root, files=None):
        self.name = name
        self.root = root
        self.features = defaultdict(lambda: [0, None])
        self.details = defaultdict(lambda: defaultdict(int))
        self.stats = defaultdict(int)
        self.languages = set()
        self.assets = defaultdict(int)
        self.asset_list = files
        self.defs = {}             # name -> kind ('char', 'trans:Ctor', 'atl', 'transform-py', 'var')
        self.screens = {}          # custom screen name -> list of non-static reasons
        self.stores = set()        # named stores: `default ch1.x`, `init python in ch1`
        self.src_lines = []
        self.ui_file = False

    # ---- recording -------------------------------------------------------
    def add(self, key, where, detail=None, n=1):
        if self.ui_file and not key.startswith(PLAIN_PREFIXES):
            key = "ui:" + key
        rec = self.features[key]
        rec[0] += n
        if rec[1] is None:
            rec[1] = where
        if detail is not None:
            self.details[key][detail] += 1

    # ---- driver ----------------------------------------------------------
    def run(self):
        files = []
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = sorted(d for d in dirnames if d not in (".git", "renpy", "cache", "saves"))
            for fn in sorted(filenames):
                if fn.endswith(".rpy"):
                    files.append(os.path.join(dirpath, fn))
        parsed = []
        for path in files:
            rel = os.path.relpath(path, self.root).replace("\\", "/")
            src, tree = parse_file(path, rel)
            is_tl = "/tl/" in "/" + rel and not rel.startswith("tl/None")
            parsed.append((rel, src.split("\n"), tree, is_tl))
            self.stats["files"] += 1
            self.stats["physical_lines"] += src.count("\n") + 1
        for rel, lines, tree, is_tl in parsed:
            self.src_lines = lines
            self.collect_defs(tree)
        for rel, lines, tree, is_tl in parsed:
            self.src_lines = lines
            self.ui_file = os.path.basename(rel) in UI_FILES
            if is_tl:
                self.stats["tl_files"] += 1
                for ln in tree:
                    toks = tokenize(ln.text)
                    if len(toks) > 1 and toks[0][1] == "translate":
                        self.languages.add(toks[1][1])
                continue
            self.walk_script(tree)
        self.count_assets()
        return self

    # ---- pass 1: definitions --------------------------------------------
    def collect_defs(self, nodes):
        for ln in nodes:
            toks = tokenize(ln.text)
            if not toks:
                continue
            w = toks[0][1]
            words = [t[1] for t in toks]
            if "python" in words[:5] and "in" in words:
                self.stores.add(words[words.index("in") + 1])
            if w == "init":
                if any(t[1] == "python" for t in toks[:5]):
                    self.collect_py_defs(block_source(self.src_lines, ln))
                else:
                    self.collect_defs(ln.children)
            elif w == "python":
                self.collect_py_defs(block_source(self.src_lines, ln))
            elif w == "transform" and len(toks) > 1:
                self.defs[toks[1][1]] = "atl"
            elif w == "screen" and len(toks) > 1:
                self.screens[toks[1][1]] = ln.children
            elif w in ("define", "default"):
                m = re.match(r"(?:define|default)\s+(?:-?\d+\s+)?(" + NAME + r")\s*\+?=\s*(.*)$", ln.text, re.S)
                if m:
                    self.defs[m.group(1)] = self.classify_value(m.group(2))
                    root = m.group(1).split(".")[0]
                    if "." in m.group(1) and root not in CFG_BASES and root not in SAFE_ATTR_ROOTS:
                        self.stores.add(root)
            elif w == "label":
                self.collect_defs(ln.children)

    def screen_reasons(self, nodes):
        reasons = set()
        for ln in nodes:
            toks = tokenize(ln.text)
            if toks and toks[0][1] in SL_DYNAMIC:
                reasons.add(toks[0][1])
            for m in CALL_RE.finditer(ln.text):
                fn = m.group(1)
                if (fn in SL_ACTIONS or fn in STATIC_DISPLAYABLES or fn.startswith("im.")
                        or fn in BUILTIN_TRANSITIONS or fn in TRANSITION_CTORS
                        or fn in SL_NOT_CALLS or fn in ATL_PROPS):
                    continue
                if fn in self.defs and self.defs[fn] in ("atl", "position", "transform-static"):
                    continue
                reasons.add(fn + "()")
            reasons |= self.screen_reasons(ln.children)
        return reasons

    def classify_value(self, text):
        try:
            node = ast.parse(text.strip(), mode="eval").body
        except SyntaxError:
            return "var"
        if isinstance(node, ast.Call):
            fn = dotted(node.func) or ""
            base = fn.split(".")[-1]
            if base in CHARACTER_CTORS:
                kind = next((k.value for k in node.keywords if k.arg == "kind"), None)
                return "char:nvl" if kind is not None and dotted(kind) == "nvl" else "char"
            if base in TRANSITION_CTORS:
                return "trans:" + base
            if base == "Position":
                return "position"
            if base == "Transform":
                static = not node.args and all(
                    k.arg in ATL_PROPS and isinstance(k.value, (ast.Constant, ast.Tuple, ast.UnaryOp))
                    for k in node.keywords)
                return "transform-static" if static else "transform-py"
            if fn in self.defs and self.defs[fn].startswith("char"):
                return self.defs[fn]
            return "call:" + fn
        if isinstance(node, ast.Name) and node.id in self.defs:
            return self.defs[node.id]
        if isinstance(node, ast.Name) and node.id in BUILTIN_TRANSITIONS:
            return "trans:" + node.id
        return "var"

    def collect_py_defs(self, code):
        try:
            mod = ast.parse(code)
        except SyntaxError:
            return
        for n in ast.walk(mod):
            if (isinstance(n, ast.Call) and dotted(n.func) == "define.move_transitions"
                    and n.args and isinstance(n.args[0], ast.Constant)):
                for suffix in MOVE_SUFFIXES:
                    self.defs[n.args[0].value + suffix] = "trans:MoveTransition"
        for st in mod.body:
            if isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
                self.defs[st.targets[0].id] = self.classify_value(ast.unparse(st.value))
            elif isinstance(st, ast.FunctionDef):
                self.defs[st.name] = "func"

    # ---- pass 2: script statements --------------------------------------
    def walk_script(self, nodes):
        for ln in nodes:
            try:
                self.statement(ln)
            except Exception as e:  # a census must not stop on one odd line
                self.add("census:error", ln.where, f"{type(e).__name__}: {e}")

    def statement(self, ln):
        toks = tokenize(ln.text)
        if not toks:
            return
        kind, w = toks[0][0], toks[0][1]
        where = ln.where
        if kind == "op" and w == "$":
            self.add("stmt:python-inline", where)
            self.python(ln.text[1:].strip(), where)
            return
        if kind == "str" or (kind == "name" and w not in STMT_KEYWORDS and self.is_say(toks)):
            self.say(toks, ln)
            return
        if kind != "name":
            self.add("stmt:unknown:" + w, where)
            return
        handler = getattr(self, "st_" + w, None)
        if handler is None:
            self.add("stmt:unknown:" + w, where, ln.text[:60])
            return
        handler(toks, ln)

    @staticmethod
    def is_say(toks):
        for i, (k, v, _) in enumerate(toks):
            if k == "str":
                return i > 0
            if k == "name" or (k == "op" and v in "-@"):
                continue
            return False
        return False

    # say -------------------------------------------------------------------
    def say(self, toks, ln):
        where = ln.where
        self.add("stmt:say", where)
        self.stats["say"] += 1
        idx = next(i for i, t in enumerate(toks) if t[0] == "str")
        what_i = idx
        if idx == 0 and len(toks) > 1 and toks[1][0] == "str":
            what_i = 1
            self.add("say:who-string", where)
            self.text(string_value(toks[0][1]), where)
        elif idx == 0:
            self.add("say:narrator", where)
        else:
            who = toks[0][1]
            if who == "extend":
                self.add("say:extend", where)
            elif who == "centered":
                self.add("say:centered", where)
            elif who == "nvl" or self.defs.get(who) == "char:nvl":
                self.add("say:nvl", where)
            elif self.defs.get(who, "").startswith("char") or who == "narrator":
                self.add("say:character", where)
            else:
                self.add("say:who-unknown", where, who)
            attrs = toks[1:idx]
            if any(t[1] == "@" for t in attrs):
                self.add("say:temp-attrs", where)
            elif attrs:
                self.add("say:attrs", where)
        what = string_value(toks[what_i][1])
        self.stats["say_chars"] += len(what)
        self.text(what, where)
        rest = toks[what_i + 1:]
        words = [t[1] for t in rest]
        if "with" in words:
            self.add("say:with", where)
            self.transition_expr(ln.text[rest[words.index("with")][2]:][4:].strip(), where)
        if words[:1] == ["("]:
            self.add("say:args", where)
        if "id" in words:
            self.add("say:id", where)

    def text(self, s, where):
        for m in TAG_RE.finditer(s):
            tag = m.group(1) or "?"
            if m.group(0).startswith("{/"):
                continue
            if tag.startswith("#"):
                tag = "#"
            self.add("text:tag:" + tag, where)
        if INTERP_RE.search(s):
            self.add("text:interp", where, INTERP_RE.search(s).group(0)[:40])
        if "%(" in s:
            self.add("text:pct-interp", where)

    # control flow ----------------------------------------------------------
    def st_label(self, toks, ln):
        self.add("stmt:label", ln.where)
        self.stats["labels"] += 1
        name = toks[1][1] if len(toks) > 1 else ""
        if name == "." or (len(toks) > 2 and toks[2][1] == "."):
            self.add("label:local", ln.where)
        if any(t[1] == "(" for t in toks[1:4]):
            self.add("label:params", ln.where)
        if any(t[1] == "hide" for t in toks):
            self.add("label:hide", ln.where)
        self.walk_script(ln.children)

    def st_jump(self, toks, ln):
        self.add("stmt:jump", ln.where)
        if len(toks) > 1 and toks[1][1] == "expression":
            self.add("jump:expression", ln.where, ln.text[:60])

    def st_call(self, toks, ln):
        w = toks[1][1] if len(toks) > 1 else ""
        if w == "screen":
            self.screen_use("call", toks, ln)
            return
        self.add("stmt:call", ln.where)
        if w == "expression":
            self.add("call:expression", ln.where, ln.text[:60])
        elif len(toks) > 2 and toks[2][1] == "(" or (len(toks) > 4 and toks[4][1] == "("):
            self.add("call:args", ln.where)
            m = re.search(r"\((.*)\)", ln.text, re.S)
            if m:
                self.args_expr(m.group(1), ln.where)

    def st_return(self, toks, ln):
        self.add("stmt:return", ln.where)
        if len(toks) > 1:
            self.add("return:value", ln.where)

    def st_pass(self, toks, ln):
        self.add("stmt:pass", ln.where)

    def st_if(self, toks, ln):
        key = "stmt:" + toks[0][1]
        self.add(key, ln.where)
        cond = ln.text[len(toks[0][1]):].rstrip().rstrip(":")
        if cond.strip():
            self.expr(cond, ln.where)
        self.walk_script(ln.children)

    st_elif = st_if
    st_while = st_if

    def st_else(self, toks, ln):
        self.walk_script(ln.children)

    def st_menu(self, toks, ln):
        where = ln.where
        self.add("stmt:menu", where)
        if len(toks) > 1 and toks[1][0] == "name":
            self.add("menu:named", where)
        if any(t[1] == "(" for t in toks[1:3]):
            self.add("menu:args", where)
        for ch in ln.children:
            ct = tokenize(ch.text)
            if not ct:
                continue
            if ct[0][1] == "set":
                self.add("menu:set", ch.where)
                continue
            if ct[0][0] == "str" and ch.text.endswith(":"):
                self.add("menu:choice", ch.where)
                self.text(string_value(ct[0][1]), ch.where)
                words = [t[1] for t in ct]
                if len(ct) > 1 and ct[1][1] == "(":
                    self.add("menu:choice-args", ch.where)
                if "if" in words:
                    self.add("menu:cond", ch.where)
                    cond = ch.text[ct[words.index("if")][2] + 2:].rstrip().rstrip(":")
                    self.expr(cond, ch.where)
                self.walk_script(ch.children)
            else:
                self.add("menu:caption", ch.where)
                self.statement(ch)

    # python ----------------------------------------------------------------
    def st_python(self, toks, ln, init=False):
        words = [t[1] for t in toks]
        code = block_source(self.src_lines, ln)
        if "early" in words and not self.all_cfg(code):
            self.add("py:early-block", ln.where)
        self.add("stmt:init-python" if init else "stmt:python-block", ln.where)
        self.python(code, ln.where, block=True)

    def all_cfg(self, code):
        try:
            return all(self.is_cfg_stmt(s) for s in ast.parse(code).body)
        except SyntaxError:
            return False

    def st_init(self, toks, ln):
        words = [t[1] for t in toks]
        if "python" in words:
            self.st_python(toks, ln, init=True)
        elif "offset" in words:
            self.add("cfg:init-offset", ln.where)
        elif "label" in words:
            self.add("stmt:init-label", ln.where)
            self.walk_script(ln.children)
        else:
            self.add("stmt:init-block", ln.where)
            self.walk_script(ln.children)

    def python(self, code, where, block=False):
        if "gui.init(" in code:
            self.add("era:gui-init", where)
        m = re.search(r"config\.script_version\s*=\s*\(([^)]*)\)", code)
        if m:
            self.add("era:script-version", where, m.group(1).replace(" ", ""))
        try:
            mod = ast.parse(code)
        except SyntaxError:
            self.add("py:unparsable", where, code.strip().split("\n")[0][:60])
            return
        if block and all(self.is_cfg_stmt(s) for s in mod.body):
            self.add("cfg:python-block", where)
            return
        body = mod.body
        if any(self.is_auto_image_loop(st) for st in body):
            # Cookbook "define every file in a folder as an image" loop. The
            # compiler can expand it at build time from the file listing.
            self.add("idiom:auto-images", where)
            body = [st for st in body if not self.is_auto_image_loop(st)
                    and not (isinstance(st, ast.Import) and all(a.name == "os" for a in st.names))]
        for st in body:
            self.py_stmt(st, where)

    @staticmethod
    def is_auto_image_loop(st):
        if not isinstance(st, ast.For) or not isinstance(st.iter, ast.Call):
            return False
        if dotted(st.iter.func) not in ("os.listdir", "renpy.list_files"):
            return False
        calls = {dotted(n.func) for n in ast.walk(st) if isinstance(n, ast.Call)}
        return "renpy.image" in calls and not any(isinstance(n, (ast.FunctionDef, ast.ClassDef)) for n in ast.walk(st))

    def is_cfg_stmt(self, st):
        if isinstance(st, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
            targets = st.targets if isinstance(st, ast.Assign) else [st.target]
            return all(root_name(t) in CFG_BASES for t in targets)
        if isinstance(st, ast.Expr) and isinstance(st.value, ast.Call):
            fn = dotted(st.value.func) or ""
            return fn.split(".")[0] in CFG_BASES or fn in ("renpy.music.register_channel", "renpy.register_channel")
        return isinstance(st, ast.Pass)

    def py_stmt(self, st, where):
        if self.is_cfg_stmt(st):
            self.add("cfg:python", where)
            return
        if isinstance(st, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
            targets = st.targets if isinstance(st, ast.Assign) else [st.target]
            for t in targets:
                if isinstance(t, ast.Name):
                    k = "py:assign:name"
                elif isinstance(t, ast.Attribute) and root_name(t) in SAFE_ATTR_ROOTS:
                    k = "py:assign:" + root_name(t)
                elif isinstance(t, ast.Attribute) and root_name(t) in self.stores:
                    k = "py:assign:named-store"
                elif isinstance(t, ast.Attribute):
                    k = "py:assign:attr"
                    self.details["py:assign:attr"][dotted(t) or "?"] += 1
                elif isinstance(t, ast.Subscript):
                    k = "py:assign:subscript"
                else:
                    k = "py:assign:" + type(t).__name__.lower()
                self.add(k, where)
            if isinstance(st, ast.AugAssign):
                self.add("py:augassign", where)
                op = type(st.op).__name__
                if op not in SAFE_OPS:
                    self.add("py:op:" + op, where)
            if st.value is not None:
                self.expr_node(st.value, where)
            return
        if isinstance(st, ast.Expr):
            if isinstance(st.value, ast.Call):
                self.expr_node(st.value, where)
            elif isinstance(st.value, ast.Constant):
                self.add("py:docstring", where)
            else:
                self.add("py:stmt:Expr-" + type(st.value).__name__, where)
            return
        self.add("py:stmt:" + type(st).__name__, where, getattr(st, "name", None))

    def expr(self, text, where):
        text = text.strip()
        if not text:
            return
        try:
            node = ast.parse(text, mode="eval")
        except SyntaxError:
            self.add("py:unparsable", where, text[:60])
            return
        self.expr_node(node, where)

    def args_expr(self, inner, where):
        try:
            call = ast.parse("f(" + inner + ")", mode="eval").body
        except SyntaxError:
            self.add("py:unparsable", where, inner[:60])
            return
        for a in list(call.args) + [k.value for k in call.keywords]:
            self.expr_node(a, where)

    def expr_node(self, node, where):
        for n in ast.walk(node):
            if isinstance(n, ast.Call):
                fn = dotted(n.func)
                if fn is None and isinstance(n.func, ast.Attribute):
                    self.add("py:method:" + n.func.attr, where, ast.unparse(n.func)[:40])
                elif fn is None:
                    self.add("py:call:<complex>", where)
                else:
                    kind = self.defs.get(fn, "")
                    root = fn.split(".")[0]
                    how = {"renpy.call_screen": "call", "renpy.show_screen": "show",
                           "renpy.hide_screen": "hide"}.get(fn)
                    if how and n.args and isinstance(n.args[0], ast.Constant):
                        self.screen_key(how, n.args[0].value, where)
                    elif fn.startswith("im."):
                        self.add("py:call:im.*", where, fn)
                    elif fn in STATIC_DISPLAYABLES:
                        self.add("py:call:static-displayable", where, fn)
                    elif "." in fn and root not in API_ROOTS and fn not in self.defs and self.defs.get(root) != "func":
                        self.add("py:method:" + fn.split(".")[-1], where, fn)
                    elif kind == "func":
                        self.add("py:call:user-func", where, fn)
                    elif kind.startswith("char"):
                        self.add("py:call:character-say", where, fn)
                    elif fn.split(".")[-1] in CHARACTER_CTORS:
                        self.add("char:ctor:" + fn.split(".")[-1], where)
                        for kw in n.keywords:
                            self.add("char:kw:" + (kw.arg or "**"), where)
                    elif fn.split(".")[-1] in TRANSITION_CTORS:
                        self.add("trans:ctor:" + fn.split(".")[-1], where)
                    else:
                        self.add("py:call:" + fn, where)
            elif isinstance(n, ast.Attribute):
                r = root_name(n)
                if r and r not in SAFE_ATTR_ROOTS and not self.is_call_func(node, n):
                    if dotted(n) in ("config.screen_width", "config.screen_height"):
                        self.add("py:attr:config-screen-size", where)
                    elif r in self.stores:
                        self.add("py:attr:named-store", where, dotted(n))
                    else:
                        self.add("py:attr:" + r, where, dotted(n))
            elif not isinstance(n, SAFE_NODES):
                self.add("py:expr:" + type(n).__name__, where)
            elif isinstance(n, (ast.operator, ast.unaryop, ast.boolop, ast.cmpop)):
                if type(n).__name__ not in SAFE_OPS:
                    self.add("py:op:" + type(n).__name__, where)

    @staticmethod
    def is_call_func(tree, attr):
        for n in ast.walk(tree):
            if isinstance(n, ast.Call):
                f = n.func
                while isinstance(f, ast.Attribute):
                    if f is attr:
                        return True
                    f = f.value
        return False

    # definitions -----------------------------------------------------------
    def st_define(self, toks, ln):
        where = ln.where
        m = re.match(r"(define|default)\s+(?:-?\d+\s+)?(" + NAME + r")\s*(\+?=|\|=)\s*(.*)$", ln.text, re.S)
        if not m:
            self.add("stmt:" + toks[0][1], where)
            self.add("py:unparsable", where, ln.text[:60])
            return
        stmt, name, op, value = m.groups()
        root = name.split(".")[0]
        if root in CFG_BASES:
            self.add("cfg:define", where)
            return
        self.add("stmt:" + stmt, where)
        if root == "persistent":
            self.add("var:persistent", where)
        elif root in self.stores:
            self.add("var:named-store", where)
        elif root == "audio":
            self.add("define:audio", where)
        if op != "=":
            self.add("define:augmented", where)
        self.expr(value, where)

    st_default = st_define

    def st_image(self, toks, ln):
        where = ln.where
        self.add("stmt:image", where)
        if ln.text.endswith(":") and "=" not in ln.text:
            self.add("image:atl", where)
            self.atl(ln.children)
            return
        value = ln.text.split("=", 1)[1].strip() if "=" in ln.text else ""
        try:
            node = ast.parse(value, mode="eval").body
        except SyntaxError:
            self.add("py:unparsable", where, value[:60])
            return
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if node.value.startswith("#"):
                self.add("image:color", where)
            elif "[" in node.value:
                self.add("image:dynamic", where)
            else:
                self.add("image:file", where)
        elif isinstance(node, ast.Call):
            fn = dotted(node.func) or "<complex>"
            fn = "im.*" if fn.startswith("im.") else fn
            self.add("image:call:" + fn, where)
            if fn in STATIC_DISPLAYABLES or fn == "im.*":
                for sub in ast.walk(node):
                    if isinstance(sub, ast.Call) and sub is not node:
                        f2 = dotted(sub.func) or ""
                        if not (f2.startswith("im.") or f2 in STATIC_DISPLAYABLES):
                            self.expr_node(sub, where)
                return
            for arg in list(node.args) + [k.value for k in node.keywords]:
                self.expr_node(arg, where) if not isinstance(arg, ast.Constant) else None
        elif isinstance(node, ast.Name):
            self.add("image:alias", where)
        else:
            self.add("image:expr", where)

    def st_transform(self, toks, ln):
        self.add("stmt:transform", ln.where)
        if any(t[1] == "(" for t in toks[2:3]):
            self.add("transform:params", ln.where)
        self.atl(ln.children)

    def st_screen(self, toks, ln):
        name = toks[1][1] if len(toks) > 1 else "?"
        if name in DEFAULT_SCREENS:
            self.add("ui:default-screen", ln.where, name)
            return
        self.add("ui:custom-screen-def", ln.where, name)
        self.sl(ln.children)

    def sl(self, nodes):
        for ch in nodes:
            t = tokenize(ch.text)
            if t:
                self.add("sl:" + t[0][1], ch.where)
            self.sl(ch.children)

    def st_style(self, toks, ln):
        self.add("cfg:style", ln.where)

    def st_translate(self, toks, ln):
        if len(toks) > 1:
            self.languages.add(toks[1][1])
        self.add("tl:block", ln.where)

    def st_layeredimage(self, toks, ln):
        self.add("stmt:layeredimage", ln.where)

    def st_testcase(self, toks, ln):
        self.add("info:testcase", ln.where)

    st_testsuite = st_testcase

    def st_rpy(self, toks, ln):
        self.add("rpy:" + " ".join(t[1] for t in toks[1:]), ln.where)

    def st_camera(self, toks, ln):
        self.add("stmt:camera", ln.where)
        self.atl(ln.children)

    # display ---------------------------------------------------------------
    def st_show(self, toks, ln, stmt="show"):
        where = ln.where
        words = [t[1] for t in toks]
        if len(words) > 1 and words[1] == "screen":
            self.screen_use(stmt, toks, ln)
            return
        self.add("stmt:" + stmt, where)
        if len(words) > 1 and words[1] == "layer":
            self.add(stmt + ":layer", where)
        elif len(words) > 1 and words[1] == "expression":
            self.add(stmt + ":expression", where, ln.text[:60])
            j = next((i for i, t in enumerate(toks) if i > 1 and t[0] == "name" and t[1] in SHOW_CLAUSES), len(toks))
            end = toks[j][2] if j < len(toks) else len(ln.text)
            if len(toks) > 2:
                self.expr(ln.text[toks[2][2]:end].rstrip(":"), where)
        elif len(words) > 1 and words[1] == "text" and len(toks) > 2 and toks[2][0] == "str":
            self.add(stmt + ":text", where)
        elif stmt == "scene" and len(words) == 1:
            self.add("scene:bare", where)
        i = 1
        while i < len(toks):
            w = toks[i][1]
            if toks[i][0] == "name" and w in SHOW_CLAUSES:
                start = toks[i][2] + len(w)
                j = i + 1
                depth = 0
                while j < len(toks):
                    v = toks[j][1]
                    depth += v in "([{" and toks[j][0] == "op"
                    depth -= v in ")]}" and toks[j][0] == "op"
                    if depth == 0 and toks[j][0] == "name" and v in SHOW_CLAUSES:
                        break
                    j += 1
                end = toks[j][2] if j < len(toks) else len(ln.text)
                arg = ln.text[start:end].strip().rstrip(":").strip()
                if w == "with":
                    self.add(stmt + ":with", where)
                    self.transition_expr(arg, where)
                elif w == "at":
                    self.at_list(arg, where)
                else:
                    self.add(stmt + ":" + w, where)
                i = j
                continue
            i += 1
        if ln.text.endswith(":"):
            self.add(stmt + ":atl-block", where)
            self.atl(ln.children)

    def st_scene(self, toks, ln):
        self.st_show(toks, ln, "scene")

    def st_hide(self, toks, ln):
        self.st_show(toks, ln, "hide")

    def screen_use(self, how, toks, ln):
        name = toks[2][1] if len(toks) > 2 else "?"
        if name == "expression":
            self.add("screen:" + how + ":expression", ln.where)
            return
        self.screen_key(how, name, ln.where)

    def screen_key(self, how, name, where):
        if name in DEFAULT_SCREENS:
            self.add(f"screen:{how}:default", where, name)
        elif name not in self.screens:
            self.add(f"screen:{how}:undefined", where, name)
        else:
            reasons = sorted(self.screen_reasons(self.screens[name]))
            if reasons:
                self.add(f"screen:{how}:custom-dynamic", where, f"{name}: {', '.join(reasons[:3])}")
            else:
                self.add(f"screen:{how}:custom-static", where, name)

    def at_list(self, text, where):
        try:
            node = ast.parse(text, mode="eval").body
        except SyntaxError:
            self.add("py:unparsable", where, text[:60])
            return
        items = node.elts if isinstance(node, ast.Tuple) else [node]
        for it in items:
            if isinstance(it, ast.Name):
                if it.id == "Transform":
                    self.add("at:position", where, it.id)
                elif it.id in BUILTIN_POSITIONS:
                    self.add("at:builtin", where, it.id)
                elif self.defs.get(it.id) == "atl":
                    self.add("at:atl", where)
                elif self.defs.get(it.id) in ("position", "transform-static"):
                    self.add("at:position", where, it.id)
                elif self.defs.get(it.id) == "transform-py":
                    self.add("at:transform-py", where, it.id)
                else:
                    self.add("at:unknown", where, it.id)
            elif isinstance(it, ast.Call):
                fn = dotted(it.func) or "<complex>"
                if self.defs.get(fn) == "atl":
                    self.add("at:atl-params", where)
                else:
                    self.add("at:call:" + fn, where)
            else:
                self.add("at:expr", where)

    def transition_expr(self, text, where):
        text = text.strip()
        if not text:
            return
        try:
            node = ast.parse(text, mode="eval").body
        except SyntaxError:
            self.add("py:unparsable", where, text[:60])
            return
        if isinstance(node, ast.Name):
            if node.id in BUILTIN_TRANSITIONS:
                self.add("trans:" + node.id, where)
            elif self.defs.get(node.id, "")[6:] in BUILTIN_TRANSITIONS:
                self.add("trans:" + self.defs[node.id][6:], where, node.id)
            elif self.defs.get(node.id, "").startswith("trans:"):
                self.add("trans:defined:" + self.defs[node.id][6:], where, node.id)
            elif self.defs.get(node.id, "").startswith("call:"):
                self.add("trans:custom", where, f"{node.id} = {self.defs[node.id][5:]}()")
            else:
                self.add("trans:unknown", where, node.id)
        elif isinstance(node, ast.Constant) and node.value is None:
            self.add("trans:None", where)
        elif isinstance(node, ast.Attribute):
            self.add("trans:variable", where, dotted(node))
        elif isinstance(node, ast.Call):
            fn = (dotted(node.func) or "<complex>").split(".")[-1]
            self.add("trans:ctor:" + fn if fn in TRANSITION_CTORS else "trans:call:" + fn, where)
            self.expr_node(ast.Tuple(elts=list(node.args) + [k.value for k in node.keywords], ctx=ast.Load()), where)
        else:
            self.add("trans:expr", where)

    def st_with(self, toks, ln):
        self.add("stmt:with", ln.where)
        self.transition_expr(ln.text[4:], ln.where)

    def st_pause(self, toks, ln):
        self.add("stmt:pause", ln.where)
        if len(toks) > 1:
            self.expr(ln.text[5:], ln.where)
        else:
            self.add("pause:bare", ln.where)

    def st_window(self, toks, ln):
        self.add("stmt:window", ln.where)
        rest = toks[1:]
        if rest:
            self.add("window:" + rest[0][1], ln.where)
            rest = rest[1:]
        if rest and toks[1][1] == "auto" and rest[0][1] in ("show", "hide"):
            rest = rest[1:]
        if rest:
            self.transition_expr(ln.text[rest[0][2]:], ln.where)

    def st_nvl(self, toks, ln):
        if len(toks) > 1 and toks[1][0] == "str":
            self.say(toks, ln)
            return
        self.add("stmt:nvl", ln.where)
        if len(toks) > 1:
            self.add("nvl:" + toks[1][1], ln.where)

    # audio -----------------------------------------------------------------
    def st_play(self, toks, ln, stmt="play"):
        where = ln.where
        self.add("stmt:" + stmt, where)
        if len(toks) < 2:
            return
        ch = toks[1][1]
        self.add("audio:channel:" + (ch if ch in ("music", "sound", "voice", "audio") else "custom"), where, ch)
        words = [t[1] for t in toks]
        for opt in ("fadein", "fadeout", "loop", "noloop", "if_changed", "volume", "synchro_start"):
            if opt in words[2:]:
                self.add("audio:" + opt, where)
        if stmt == "stop":
            return
        if len(toks) > 2:
            k, v = toks[2][0], toks[2][1]
            if k == "str":
                self.add("audio:file-literal", where)
            elif v == "[":
                self.add("audio:playlist", where)
            elif k == "name" and (v == "audio" or v in self.defs):
                self.add("audio:alias", where)
            elif k == "name" and len(toks) == 3 or (k == "name" and toks[3][1] in ("fadein", "fadeout", "loop", "noloop", "if_changed", "volume")):
                self.add("audio:auto-name", where, v)
            else:
                self.add("audio:expr", where, ln.text[:60])

    def st_queue(self, toks, ln):
        self.st_play(toks, ln, "queue")

    def st_stop(self, toks, ln):
        self.st_play(toks, ln, "stop")

    def st_voice(self, toks, ln):
        self.add("stmt:voice", ln.where)
        if len(toks) > 1 and toks[1][1] == "sustain":
            self.add("voice:sustain", ln.where)

    # ATL -------------------------------------------------------------------
    def atl(self, nodes):
        for ln in nodes:
            toks = tokenize(ln.text)
            if not toks:
                continue
            where = ln.where
            k, w = toks[0][0], toks[0][1]
            rest = toks
            if k == "name" and WARPER_RE.match(w):
                self.add("atl:warper:" + w, where)
                rest = toks[1:]
            elif w == "warp":
                self.add("atl:warper:warp", where)
            elif w in ("pause", "repeat", "parallel", "block", "choice", "contains",
                       "function", "time", "event", "animation"):
                self.add("atl:" + w, where)
                if w == "function":
                    self.add("py:atl-function", where)
            elif w == "on":
                self.add("atl:on", where, " ".join(t[1] for t in toks[1:]).rstrip(":"))
            elif k == "num":
                self.add("atl:pause", where)
            elif k == "str":
                self.add("atl:displayable", where)
            elif k == "name" and w not in ATL_PROPS and w not in ATL_SPLINE:
                if w in BUILTIN_POSITIONS or self.defs.get(w) in ("atl", "position", "transform-static"):
                    self.add("atl:include", where, w)
                elif self.defs.get(w) == "transform-py":
                    self.add("atl:include-python", where, w)
                else:
                    self.add("atl:expr", where, ln.text[:50])
            for t in rest:
                if t[0] == "name" and t[1] in ATL_PROPS:
                    self.add("atl:prop:" + t[1], where)
                elif t[0] == "name" and t[1] in ATL_SPLINE:
                    self.add("atl:spline", where)
                elif t[0] == "name" and t[1] == "with" and k == "str":
                    self.add("atl:displayable-with", where)
            if ln.children:
                self.atl(ln.children)

    # assets ----------------------------------------------------------------
    def count_assets(self):
        paths = self.asset_list
        if paths is None:
            paths = []
            for dirpath, dirnames, filenames in os.walk(self.root):
                dirnames[:] = [d for d in dirnames if d != ".git"]
                paths += [os.path.join(dirpath, f) for f in filenames]
        for p in paths:
            ext = os.path.splitext(p)[1].lower()
            for cls, exts in (("img", IMAGE_EXT), ("audio", AUDIO_EXT), ("video", VIDEO_EXT),
                              ("font", FONT_EXT), ("script", SCRIPT_EXT)):
                if ext in exts:
                    self.assets[f"{cls}:{ext[1:]}"] += 1
                    break

    def to_dict(self):
        return {
            "name": self.name,
            "stats": dict(self.stats),
            "languages": sorted(self.languages),
            "assets": dict(sorted(self.assets.items())),
            "features": {k: {"count": v[0], "first": v[1],
                             "details": dict(sorted(self.details[k].items(), key=lambda x: -x[1])[:15])}
                         for k, v in sorted(self.features.items())},
        }
