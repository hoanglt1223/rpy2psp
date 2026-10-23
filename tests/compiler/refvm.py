"""Reference interpreter for script.vnb, used by tests to play compiled games on PC.

It implements the semantics in docs/dev/bytecode.md for control flow, values,
and dialogue; display, audio, and transitions are recorded as events.
"""

import random
import struct

from rpy2psp.compiler.bytecode import (BINOPS, CMPOPS, FUNCTIONS, METHODS, NONE16, NONE32, OPCODES,
                                       decode_ops, read_container, read_strings)


class VMError(Exception):
    pass


class NeedChoice(Exception):
    def __init__(self, count):
        super().__init__(count)
        self.count = count


class Unset:
    pass


UNSET = Unset()


class Program:
    def __init__(self, data):
        secs = read_container(data)
        self.strings = read_strings(secs["STRS"])
        self.code = secs["CODE"]
        self.ops = {addr: (name, vals) for addr, name, vals in decode_ops(self.code)}
        self.next = {}
        addrs = sorted(self.ops)
        for a, b in zip(addrs, addrs[1:] + [len(self.code)]):
            self.next[a] = b
        s = secs["SYMS"]
        n = struct.unpack_from("<I", s, 0)[0]
        self.symbols = []
        for i in range(n):
            sid, kind = struct.unpack_from("<IB", s, 4 + 8 * i)
            self.symbols.append((self.strings[sid], kind))
        lab = secs["LABL"]
        n = struct.unpack_from("<I", lab, 0)[0]
        self.labels = {}
        for i in range(n):
            sid, addr = struct.unpack_from("<II", lab, 4 + 8 * i)
            self.labels[self.strings[sid]] = addr
        self.init, self.defaults, self.start = struct.unpack_from("<III", secs["HEAD"], 0)
        c = secs["CHRS"]
        n = struct.unpack_from("<I", c, 0)[0]
        self.char_names = []
        for i in range(n):
            sid = struct.unpack_from("<I", c, 4 + 38 * i)[0]
            self.char_names.append(None if sid == NONE32 else self.strings[sid])
        self.shows = []
        sh = secs["SHOW"]
        n = struct.unpack_from("<I", sh, 0)[0]
        pos = 4
        for _ in range(n):
            tag = self.strings[struct.unpack_from("<I", sh, pos)[0]]; pos += 4
            na = sh[pos]; pos += 1
            attrs = [self.strings[x] for x in struct.unpack_from(f"<{na}I", sh, pos)]; pos += 4 * na
            nat = sh[pos]; pos += 1 + 2 * nat
            pos += 4
            nb = sh[pos]; pos += 1 + 4 * nb
            pos += 5 + 4 + 2 + 2
            self.shows.append((tag, attrs))


class VM:
    def __init__(self, prog, choices=(), inputs=(), max_steps=200000, seed=0):
        self.p = prog
        self.choices = list(choices)
        self.inputs = list(inputs)
        self.max_steps = max_steps
        self.vars = {}
        self.events = []
        self.rng = random.Random(seed)

    def name(self, sym):
        return self.p.symbols[sym][0]

    def load(self, sym):
        v = self.vars.get(sym, UNSET)
        if v is UNSET:
            if self.p.symbols[sym][1] == 1:
                return None
            raise VMError(f"variable {self.name(sym)} is not set")
        return v

    def run_routine(self, addr):
        self.execute(addr, stop_on_return=True)

    def play(self):
        self.run_routine(self.p.init)
        self.run_routine(self.p.defaults)
        self.execute(self.p.start)
        return self.events

    def execute(self, pc, stop_on_return=False):
        stack = []
        frames = []
        steps = 0
        while True:
            steps += 1
            if steps > self.max_steps:
                raise VMError("step limit reached (infinite loop?)")
            if pc >= len(self.p.code):
                raise VMError("ran off the end of the code")
            op, vals = self.p.ops[pc]
            nxt = self.p.next[pc]
            s = self.p.strings
            if op == "NOP":
                pass
            elif op == "PUSH_NONE":
                stack.append(None)
            elif op == "PUSH_TRUE":
                stack.append(True)
            elif op == "PUSH_FALSE":
                stack.append(False)
            elif op in ("PUSH_INT", "PUSH_FLOAT"):
                stack.append(vals[0])
            elif op == "PUSH_STR":
                stack.append(s[vals[0]])
            elif op == "LOAD":
                stack.append(self.load(vals[0]))
            elif op == "STORE":
                self.vars[vals[0]] = stack.pop()
            elif op == "STORE_DEFAULT":
                v = stack.pop()
                if self.vars.get(vals[0], UNSET) is UNSET or (self.p.symbols[vals[0]][1] == 1 and self.vars[vals[0]] is None):
                    self.vars[vals[0]] = v
            elif op == "POP":
                stack.pop()
            elif op == "DUP":
                stack.append(stack[-1])
            elif op in ("BUILD_LIST", "BUILD_TUPLE", "BUILD_SET"):
                n = vals[0]
                items = stack[len(stack) - n:]
                del stack[len(stack) - n:]
                stack.append({"BUILD_LIST": list, "BUILD_TUPLE": tuple, "BUILD_SET": set}[op](items))
            elif op == "BUILD_DICT":
                n = vals[0]
                items = stack[len(stack) - 2 * n:]
                del stack[len(stack) - 2 * n:]
                stack.append(dict(zip(items[0::2], items[1::2])))
            elif op == "SUBSCR":
                k = stack.pop()
                stack.append(stack.pop()[k])
            elif op == "SLICE":
                hi, lo = stack.pop(), stack.pop()
                stack.append(stack.pop()[lo:hi])
            elif op == "STORE_SUBSCR":
                v, k, c = stack.pop(), stack.pop(), stack.pop()
                c[k] = v
            elif op == "BINARY":
                b, a = stack.pop(), stack.pop()
                o = BINOPS[vals[0]]
                stack.append({"ADD": lambda: a + b, "SUB": lambda: a - b, "MUL": lambda: a * b,
                              "DIV": lambda: a / b, "FLOORDIV": lambda: a // b, "MOD": lambda: a % b}[o]())
            elif op == "UNARY":
                a = stack.pop()
                stack.append({"NEG": lambda: -a, "POS": lambda: +a, "NOT": lambda: not a}[["NEG", "POS", "NOT"][vals[0]]]())
            elif op == "COMPARE":
                b, a = stack.pop(), stack.pop()
                o = CMPOPS[vals[0]]
                stack.append({"EQ": a == b, "NE": a != b, "LT": None, "LE": None, "GT": None, "GE": None,
                              "IN": None, "NOTIN": None, "IS": a is b, "ISNOT": a is not b}[o]
                             if o in ("EQ", "NE", "IS", "ISNOT") else
                             {"LT": lambda: a < b, "LE": lambda: a <= b, "GT": lambda: a > b, "GE": lambda: a >= b,
                              "IN": lambda: a in b, "NOTIN": lambda: a not in b}[o]())
            elif op == "JUMP":
                nxt = vals[0]
            elif op == "JUMP_IF_FALSE":
                if not stack.pop():
                    nxt = vals[0]
            elif op == "JUMP_IF_TRUE":
                if stack.pop():
                    nxt = vals[0]
            elif op == "JUMP_IF_FALSE_OR_POP":
                if not stack[-1]:
                    nxt = vals[0]
                else:
                    stack.pop()
            elif op == "JUMP_IF_TRUE_OR_POP":
                if stack[-1]:
                    nxt = vals[0]
                else:
                    stack.pop()
            elif op == "CALL_FN":
                n = vals[1]
                args = stack[len(stack) - n:]
                del stack[len(stack) - n:]
                stack.append(self.call_fn(FUNCTIONS[vals[0]][0], args))
            elif op == "CALL_METHOD":
                n = vals[1]
                args = stack[len(stack) - n:]
                del stack[len(stack) - n:]
                obj = stack.pop()
                stack.append(getattr(obj, METHODS[vals[0]])(*args))
            elif op == "CALL":
                frames.append((nxt, {}))
                nxt = vals[0]
            elif op in ("CALL_DYNAMIC", "JUMP_DYNAMIC"):
                name = stack.pop()
                if name not in self.p.labels:
                    raise VMError(f"no label {name!r}")
                if op == "CALL_DYNAMIC":
                    frames.append((nxt, {}))
                nxt = self.p.labels[name]
            elif op == "BIND":
                v = stack.pop()
                if frames:
                    frames[-1][1].setdefault(vals[0], self.vars.get(vals[0], UNSET))
                self.vars[vals[0]] = v
            elif op == "RETURN":
                if not frames:
                    if stop_on_return:
                        return
                    self.events.append(("end",))
                    return
                nxt, saved = frames.pop()
                for k, v in saved.items():
                    if v is UNSET:
                        self.vars.pop(k, None)
                    else:
                        self.vars[k] = v
            elif op == "SAY":
                self.events.append(("say", self.p.char_names[vals[0]], s[vals[1]]))
            elif op == "SAY_DYNAMIC":
                self.events.append(("say", self.p.char_names[vals[0]], str(stack.pop())))
            elif op == "SAY_ATTRS":
                self.events.append(("attrs", self.p.char_names[vals[0]], [s[x] for x in vals[2]]))
            elif op == "MENU":
                items = vals[0]
                conds = stack[len(stack) - len(items):]
                del stack[len(stack) - len(items):]
                visible = [i for i, c in enumerate(conds) if c]
                if not visible:
                    pass  # Ren'Py skips a menu with no visible choices
                else:
                    if not self.choices:
                        raise NeedChoice(len(visible))
                    k = self.choices.pop(0)
                    if not 0 <= k < len(visible):
                        raise VMError(f"menu choice {k} not available ({len(visible)} visible)")
                    pick = visible[k]
                    self.events.append(("menu", [s[items[i][0]] for i in visible], s[items[pick][0]]))
                    nxt = items[pick][1]
            elif op == "PAUSE":
                self.events.append(("pause", stack.pop()))
            elif op == "WITH":
                self.events.append(("with", None if vals[0] == NONE16 else vals[0]))
            elif op in ("SHOW", "SCENE", "HIDE"):
                rec = None if vals[0] == NONE16 else self.p.shows[vals[0]]
                self.events.append((op.lower(), rec))
            elif op == "SHOW_DYNAMIC":
                self.events.append(("show", (stack.pop(), [])))
            elif op in ("PLAY", "VOICE", "MOVIE"):
                self.events.append((op.lower(), vals[0]))
            elif op == "STOP":
                self.events.append(("stop", vals[0]))
            elif op in ("WINDOW", "NVL", "VOICE_SUSTAIN"):
                self.events.append((op.lower(), *vals))
            else:
                raise VMError(f"opcode {op} not handled")
            pc = nxt

    def call_fn(self, name, args):
        simple = {"len": len, "str": str, "int": int, "float": float, "bool": bool, "min": min, "max": max,
                  "abs": abs, "round": round, "set": set, "list": list, "tuple": tuple}
        if name in simple:
            return simple[name](*args)
        if name == "renpy.random.randint":
            return self.rng.randint(*args)
        if name == "renpy.random.choice":
            return self.rng.choice(args[0])
        if name == "renpy.random.random":
            return self.rng.random()
        if name == "renpy.input":
            value = self.inputs.pop(0) if self.inputs else args[1]
            self.events.append(("input", args[0], value))
            return value
        if name == "menu_set_add":
            c, text = args
            c.add(text) if isinstance(c, set) else c.append(text)
            return None
        if name == "renpy.has_label":
            return args[0] in self.p.labels
        self.events.append(("call", name, args))
        return None


def explore(prog, max_depth=12, max_paths=2000, inputs=()):
    """Play every menu path up to `max_depth` choices. Returns [(choices, events, finished)]."""
    out = []
    pending = [[]]
    while pending and len(out) < max_paths:
        prefix = pending.pop()
        vm = VM(prog, choices=list(prefix), inputs=list(inputs))
        try:
            events = vm.play()
            out.append((prefix, events, True))
        except NeedChoice as nc:
            if len(prefix) >= max_depth:
                out.append((prefix, vm.events, False))
                continue
            for alt in range(nc.count):
                pending.append(prefix + [alt])
    return out
