"""Python expression subset: build-time evaluation and bytecode compilation.

Python text is parsed with `ast.parse` only; it is never compiled or executed.
`StaticEvaluator` folds expressions whose value is known at build time
(definitions, image and transform arguments). `ExprCompiler` emits stack-VM
code for the runtime subset; anything outside it raises ExprError with the
source location.
"""

import ast
import warnings

from .bytecode import BINOPS, CMPOPS, FN, FUNCTIONS, METHOD, UNOPS

warnings.filterwarnings("ignore", category=SyntaxWarning)


class ExprError(Exception):
    def __init__(self, where, msg, feature=None):
        super().__init__(msg)
        self.where = where
        self.msg = msg
        self.feature = feature


class NotStatic(Exception):
    pass


def parse_expr(text, where):
    try:
        return ast.parse(text.strip(), mode="eval").body
    except SyntaxError as e:
        raise ExprError(where, f"unparsable Python expression {text.strip()[:60]!r} ({e.msg})",
                        "py:unparsable") from None


def parse_code(text, where):
    import textwrap
    try:
        return ast.parse(textwrap.dedent(text)).body
    except SyntaxError as e:
        raise ExprError(where, f"unparsable Python ({e.msg}) near {text.strip()[:60]!r}", "py:unparsable") from None


def dotted(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


BIN = {ast.Add: "ADD", ast.Sub: "SUB", ast.Mult: "MUL", ast.Div: "DIV", ast.FloorDiv: "FLOORDIV", ast.Mod: "MOD"}
UN = {ast.USub: "NEG", ast.UAdd: "POS", ast.Not: "NOT"}
CMP = {ast.Eq: "EQ", ast.NotEq: "NE", ast.Lt: "LT", ast.LtE: "LE", ast.Gt: "GT", ast.GtE: "GE",
       ast.In: "IN", ast.NotIn: "NOTIN", ast.Is: "IS", ast.IsNot: "ISNOT"}


def _binop(op, a, b):
    if isinstance(op, ast.Add):
        return a + b
    if isinstance(op, ast.Sub):
        return a - b
    if isinstance(op, ast.Mult):
        return a * b
    if isinstance(op, ast.Div):
        return a / b
    if isinstance(op, ast.FloorDiv):
        return a // b
    if isinstance(op, ast.Mod):
        return a % b
    raise NotStatic(type(op).__name__)


def _cmp(op, a, b):
    import operator as o
    table = {ast.Eq: o.eq, ast.NotEq: o.ne, ast.Lt: o.lt, ast.LtE: o.le, ast.Gt: o.gt, ast.GtE: o.ge,
             ast.In: lambda x, y: x in y, ast.NotIn: lambda x, y: x not in y,
             ast.Is: lambda x, y: x is y, ast.IsNot: lambda x, y: x is not y}
    return table[type(op)](a, b)


STATIC_METHODS = {"startswith", "endswith", "replace", "lower", "upper", "strip", "lstrip", "rstrip", "split",
                  "join", "capitalize", "title", "count", "find", "index", "format"}


class StaticEvaluator:
    """Evaluates an AST at build time. `lookup(dotted_name)` returns a value or raises
    NotStatic; `call(dotted_name, args, kwargs, node)` handles constructor calls."""

    def __init__(self, lookup, call):
        self.lookup = lookup
        self.call = call

    def eval(self, node):
        m = getattr(self, "e_" + type(node).__name__, None)
        if m is None:
            raise NotStatic(type(node).__name__)
        return m(node)

    def e_Constant(self, n):
        return n.value

    def e_Name(self, n):
        return self.lookup(n.id)

    def e_Attribute(self, n):
        d = dotted(n)
        if d is None:
            raise NotStatic("attribute")
        return self.lookup(d)

    def e_Tuple(self, n):
        return tuple(self.eval(e) for e in n.elts)

    def e_List(self, n):
        return [self.eval(e) for e in n.elts]

    def e_Set(self, n):
        return {self.eval(e) for e in n.elts}

    def e_Dict(self, n):
        if any(k is None for k in n.keys):
            raise NotStatic("dict unpacking")
        return {self.eval(k): self.eval(v) for k, v in zip(n.keys, n.values)}

    def e_BinOp(self, n):
        a, b = self.eval(n.left), self.eval(n.right)
        if not isinstance(a, (int, float, str, tuple, list)) or not isinstance(b, (int, float, str, tuple, list)):
            raise NotStatic("operand")
        try:
            return _binop(n.op, a, b)
        except (TypeError, ZeroDivisionError, ValueError) as e:
            raise NotStatic(str(e)) from None

    def e_UnaryOp(self, n):
        v = self.eval(n.operand)
        if isinstance(n.op, ast.Not):
            return not v
        if not isinstance(v, (int, float)):
            raise NotStatic("operand")
        return -v if isinstance(n.op, ast.USub) else +v

    def e_BoolOp(self, n):
        v = None
        for e in n.values:
            v = self.eval(e)
            if isinstance(n.op, ast.And) and not v:
                return v
            if isinstance(n.op, ast.Or) and v:
                return v
        return v

    def e_Compare(self, n):
        left = self.eval(n.left)
        for op, comp in zip(n.ops, n.comparators):
            right = self.eval(comp)
            try:
                if not _cmp(op, left, right):
                    return False
            except TypeError as e:
                raise NotStatic(str(e)) from None
            left = right
        return True

    def e_IfExp(self, n):
        return self.eval(n.body) if self.eval(n.test) else self.eval(n.orelse)

    def e_Subscript(self, n):
        v = self.eval(n.value)
        if isinstance(n.slice, ast.Slice):
            parts = [None if p is None else self.eval(p) for p in (n.slice.lower, n.slice.upper, n.slice.step)]
            if not isinstance(v, (str, list, tuple)) or not all(p is None or isinstance(p, int) for p in parts):
                raise NotStatic("slice")
            return v[slice(*parts)]
        k = self.eval(n.slice)
        try:
            return v[k]
        except (TypeError, KeyError, IndexError) as e:
            raise NotStatic(str(e)) from None

    def e_Call(self, n):
        if isinstance(n.func, ast.Attribute) and n.func.attr in STATIC_METHODS:
            try:
                obj = self.eval(n.func.value)
            except NotStatic:
                obj = None
            if isinstance(obj, (str, list, tuple)) and not n.keywords:
                args = [self.eval(a) for a in n.args]
                try:
                    return getattr(obj, n.func.attr)(*args)
                except (AttributeError, TypeError, ValueError) as e:
                    raise NotStatic(str(e)) from None
        name = dotted(n.func)
        if name is None:
            raise NotStatic("call")
        if any(isinstance(a, ast.Starred) for a in n.args) or any(k.arg is None for k in n.keywords):
            raise NotStatic("star arguments")
        return self.call(name, n.args, {k.arg: k.value for k in n.keywords}, n)


SIMPLE_FUNCS = {"len", "str", "int", "float", "bool", "min", "max", "abs", "round", "set", "list", "tuple",
                "renpy.random.randint", "renpy.random.choice", "renpy.random.random", "renpy.notify",
                "renpy.full_restart", "renpy.block_rollback", "renpy.save_persistent",
                "renpy.force_autosave", "renpy.has_label", "renpy.showing"}


class ExprCompiler:
    """Emits code for a runtime expression.

    `resolve(dotted, where)` returns ('sym', index), ('const', value), or raises ExprError.
    `translate(s)` applies build-time string translation to `_()`/`__()` literals.
    `fold(name)` returns a build-time value for config.* style names, or raises NotStatic.
    `special_call(name, node, where)` may emit code for a call and return True.
    """

    def __init__(self, asm, resolve, translate=None, special_call=None):
        self.asm = asm
        self.resolve = resolve
        self.translate = translate or (lambda s: s)
        self.special_call = special_call

    def err(self, where, msg, feature=None):
        raise ExprError(where, msg, feature)

    def const(self, v, where):
        a = self.asm
        if v is None:
            a.emit("PUSH_NONE")
        elif v is True:
            a.emit("PUSH_TRUE")
        elif v is False:
            a.emit("PUSH_FALSE")
        elif isinstance(v, int):
            if not -2**31 <= v < 2**31:
                self.err(where, f"integer {v} does not fit in 32 bits")
            a.emit("PUSH_INT", v)
        elif isinstance(v, float):
            a.emit("PUSH_FLOAT", v)
        elif isinstance(v, str):
            a.emit("PUSH_STR", v)
        elif isinstance(v, tuple):
            for x in v:
                self.const(x, where)
            a.emit("BUILD_TUPLE", len(v))
        elif isinstance(v, list):
            for x in v:
                self.const(x, where)
            a.emit("BUILD_LIST", len(v))
        elif isinstance(v, dict):
            for k, x in v.items():
                self.const(k, where)
                self.const(x, where)
            a.emit("BUILD_DICT", len(v))
        else:
            self.err(where, f"value of type {type(v).__name__} cannot be used at run time")

    def compile(self, node, where):
        m = getattr(self, "c_" + type(node).__name__, None)
        if m is None:
            self.err(where, f"unsupported Python expression: {type(node).__name__} in {ast.unparse(node)[:60]!r}",
                     "py:expr:" + type(node).__name__)
        m(node, where)

    def c_Constant(self, n, w):
        if isinstance(n.value, bytes):
            self.err(w, "bytes literals are not supported")
        self.const(n.value, w)

    def c_Name(self, n, w):
        self._load(n.id, w)

    def c_Attribute(self, n, w):
        d = dotted(n)
        if d is None:
            self.err(w, f"attribute access on an expression is not supported: {ast.unparse(n)[:60]!r}",
                     "py:attr:expr")
        self._load(d, w)

    def _load(self, name, w):
        kind, val = self.resolve(name, w)
        if kind == "sym":
            self.asm.emit("LOAD", val)
        else:
            self.const(val, w)

    def c_Tuple(self, n, w):
        for e in n.elts:
            self.compile(e, w)
        self.asm.emit("BUILD_TUPLE", len(n.elts))

    def c_List(self, n, w):
        for e in n.elts:
            self.compile(e, w)
        self.asm.emit("BUILD_LIST", len(n.elts))

    def c_Set(self, n, w):
        for e in n.elts:
            self.compile(e, w)
        self.asm.emit("BUILD_SET", len(n.elts))

    def c_Dict(self, n, w):
        for k, v in zip(n.keys, n.values):
            if k is None:
                self.err(w, "dict unpacking is not supported")
            self.compile(k, w)
            self.compile(v, w)
        self.asm.emit("BUILD_DICT", len(n.keys))

    def c_BinOp(self, n, w):
        op = BIN.get(type(n.op))
        if op is None:
            self.err(w, f"operator {type(n.op).__name__} is not supported", "py:op:" + type(n.op).__name__)
        self.compile(n.left, w)
        self.compile(n.right, w)
        self.asm.emit("BINARY", BINOPS.index(op))

    def c_UnaryOp(self, n, w):
        op = UN.get(type(n.op))
        if op is None:
            self.err(w, f"operator {type(n.op).__name__} is not supported", "py:op:" + type(n.op).__name__)
        self.compile(n.operand, w)
        self.asm.emit("UNARY", UNOPS.index(op))

    def c_BoolOp(self, n, w):
        from .bytecode import Label
        end = Label("boolop")
        jump = "JUMP_IF_FALSE_OR_POP" if isinstance(n.op, ast.And) else "JUMP_IF_TRUE_OR_POP"
        for i, e in enumerate(n.values):
            self.compile(e, w)
            if i < len(n.values) - 1:
                self.asm.emit(jump, end)
        self.asm.place(end)

    def c_Compare(self, n, w):
        if len(n.ops) > 1:
            # a < b < c  ->  (a < b) and (b < c), re-evaluating b (only names and constants allowed)
            for c in n.comparators[:-1]:
                if not isinstance(c, (ast.Name, ast.Constant, ast.Attribute)):
                    self.err(w, "chained comparison with a complex middle operand is not supported")
            parts = [ast.Compare(left=l, ops=[op], comparators=[r])
                     for l, op, r in zip([n.left] + n.comparators[:-1], n.ops, n.comparators)]
            self.c_BoolOp(ast.BoolOp(op=ast.And(), values=parts), w)
            return
        self.compile(n.left, w)
        self.compile(n.comparators[0], w)
        self.asm.emit("COMPARE", CMPOPS.index(CMP[type(n.ops[0])]))

    def c_IfExp(self, n, w):
        from .bytecode import Label
        orelse, end = Label("else"), Label("endif")
        self.compile(n.test, w)
        self.asm.emit("JUMP_IF_FALSE", orelse)
        self.compile(n.body, w)
        self.asm.emit("JUMP", end)
        self.asm.place(orelse)
        self.compile(n.orelse, w)
        self.asm.place(end)

    def c_Subscript(self, n, w):
        self.compile(n.value, w)
        if isinstance(n.slice, ast.Slice):
            if n.slice.step is not None:
                self.err(w, "slice steps are not supported")
            for part in (n.slice.lower, n.slice.upper):
                if part is None:
                    self.asm.emit("PUSH_NONE")
                else:
                    self.compile(part, w)
            self.asm.emit("SLICE")
            return
        self.compile(n.slice, w)
        self.asm.emit("SUBSCR")

    def c_Call(self, n, w):
        if any(isinstance(a, ast.Starred) for a in n.args) or any(k.arg is None for k in n.keywords):
            self.err(w, "star arguments are not supported")
        name = dotted(n.func)
        if name in ("_", "__", "_p"):
            if len(n.args) != 1 or n.keywords:
                self.err(w, f"{name}() takes one argument")
            a = n.args[0]
            if isinstance(a, ast.Constant) and isinstance(a.value, str):
                self.const(self.translate(a.value), w)
            else:
                self.compile(a, w)
            return
        if self.special_call is not None and name is not None and self.special_call(name, n, w):
            return
        if name in SIMPLE_FUNCS:
            if n.keywords:
                self.err(w, f"keyword arguments to {name}() are not supported")
            _, lo, hi = FUNCTIONS[FN[name]]
            if not lo <= len(n.args) <= hi:
                self.err(w, f"{name}() takes {lo} to {hi} arguments")
            for a in n.args:
                self.compile(a, w)
            self.asm.emit("CALL_FN", FN[name], len(n.args))
            return
        if isinstance(n.func, ast.Attribute) and n.func.attr in METHOD:
            root = dotted(n.func.value)
            if root is not None and root.split(".")[0] in ("renpy", "config", "im"):
                self.err(w, f"unsupported Ren'Py API call {name}()", "py:call:" + (name or "?"))
            if n.keywords:
                self.err(w, f"keyword arguments to .{n.func.attr}() are not supported")
            self.compile(n.func.value, w)
            for a in n.args:
                self.compile(a, w)
            self.asm.emit("CALL_METHOD", METHOD[n.func.attr], len(n.args))
            return
        label = name or ast.unparse(n.func)[:40]
        self.err(w, f"call to {label}() is not supported at run time", "py:call:" + label)
