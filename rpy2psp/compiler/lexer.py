"""Ren'Py script lexer: logical lines, indentation tree, and a per-line cursor.

Follows the rules of renpy/lexer.py (MIT): comments, strings spanning physical
lines, bracket continuation, backslash continuation, and Ren'Py's string
unescaping and whitespace collapsing. Parse only; nothing here executes code.
"""

import re
from dataclasses import dataclass, field


@dataclass
class Line:
    file: str
    lineno: int
    end_lineno: int
    indent: int
    text: str
    children: list = field(default_factory=list)
    raw: str = None  # python block source when the line was rebuilt from a .rpyc

    @property
    def where(self):
        return f"{self.file}:{self.lineno}"

    def last_lineno(self):
        node = self
        while node.children:
            node = node.children[-1]
        return node.end_lineno


def read_source(path):
    with open(path, "rb") as f:
        data = f.read()
    return decode_source(data)


def decode_source(data):
    text = data.decode("utf-8-sig", errors="replace")
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _scan_string(src, i, lineno):
    """Return (end_index, lineno) for the string starting at src[i]."""
    q = src[i]
    triple = src.startswith(q * 3, i)
    close = q * 3 if triple else q
    j = i + (3 if triple else 1)
    n = len(src)
    while j < n:
        c = src[j]
        if c == "\\":
            if j + 1 < n and src[j + 1] == "\n":
                lineno += 1
            j += 2
            continue
        if c == "\n":
            lineno += 1
        if src.startswith(close, j):
            return j + len(close), lineno
        j += 1
    return n, lineno


def logical_lines(src, filename):
    out = []
    i, n, lineno = 0, len(src), 1
    while i < n:
        start = lineno
        indent = 0
        while i < n and src[i] in " \t\f":
            indent = indent + 1 if src[i] != "\t" else indent + 8 - indent % 8
            i += 1
        buf, depth, end = [], 0, None
        while i < n:
            c = src[i]
            if c == "\n":
                if depth == 0:
                    end = lineno
                lineno += 1
                i += 1
                if depth > 0:
                    buf.append("\n")
                    continue
                break
            if c == "\\" and i + 1 < n and src[i + 1] == "\n":
                buf.append(" ")
                lineno += 1
                i += 2
                continue
            if c == "#":
                while i < n and src[i] != "\n":
                    i += 1
                continue
            if c in "\"'`":
                j, lineno = _scan_string(src, i, lineno)
                buf.append(src[i:j])
                i = j
                continue
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth = max(0, depth - 1)
            buf.append(c)
            i += 1
        text = "".join(buf).strip()
        if text:
            out.append(Line(filename, start, end if end is not None else lineno, indent, text))
    return out


def build_tree(lines):
    """Nest lines by indentation. Lenient: any deeper line is a child."""
    root = Line("", 0, 0, -1, "")
    stack = [root]
    for ln in lines:
        while stack[-1].indent >= ln.indent:
            stack.pop()
        stack[-1].children.append(ln)
        stack.append(ln)
    return root.children


def parse_file(path, display_name):
    src = read_source(path)
    return src, build_tree(logical_lines(src, display_name))


def parse_source(src, display_name):
    return build_tree(logical_lines(src, display_name))


def block_source(src_lines, block):
    """Raw physical text of a block's children, dedented to the first child."""
    if not block.children:
        return ""
    first, last = block.children[0].lineno, block.last_lineno()
    width = block.children[0].indent
    out = []
    for raw in src_lines[first - 1:last]:
        lead = len(raw) - len(raw.lstrip(" "))
        out.append(raw[min(lead, width):])
    return "\n".join(out)


TOKEN_RE = re.compile(
    r"""
    (?P<str>[rRuUbBfF]{0,2}(?:\"\"\"[\s\S]*?\"\"\"|'''[\s\S]*?'''|"(?:[^"\\]|\\[\s\S])*"|'(?:[^'\\]|\\[\s\S])*'|`(?:[^`\\]|\\[\s\S])*`))
    |(?P<num>\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|\.\d+)
    |(?P<name>[A-Za-z_ -￿][0-9A-Za-z_ -￿]*)
    |(?P<op>\*\*|//|==|!=|<=|>=|<>|->|[-+*/%<>=!@$:,()\[\]{}.;~^&|?])
    |(?P<ws>\s+)
    |(?P<other>.)
    """,
    re.VERBOSE,
)


def tokenize(text):
    toks = []
    for m in TOKEN_RE.finditer(text):
        kind = m.lastgroup
        if kind == "ws":
            continue
        toks.append((kind, m.group(), m.start()))
    return toks


def string_value(tok):
    """Body of a string token without prefix and quotes (escapes left as-is)."""
    s = tok.lstrip("rRuUbBfF")
    q = 3 if s[:3] in ('"""', "'''") else 1
    return s[q:-q]


# ---- Ren'Py string semantics ---------------------------------------------

def _dequote(m):
    c = m.group(1)
    if c == "{":
        return "{{"
    if c == "[":
        return "[["
    if c == "%":
        return "%%"
    if c == "n":
        return "\n"
    if c[0] == "u" and m.group(2):
        return chr(int(m.group(2), 16))
    return c


_ESCAPE_RE = re.compile(r"\\(u([0-9a-fA-F]{1,4})|.)", re.S)


def renpy_string(body, raw=False):
    """Body of a single-quoted Ren'Py string literal as the Ren'Py lexer returns it."""
    if raw:
        return body
    s = re.sub(r"[ \n]+", " ", body)
    return _ESCAPE_RE.sub(_dequote, s)


def renpy_triple_string(body, raw=False, monologue=None):
    """Paragraphs of a triple-quoted string (Ren'Py monologue rules)."""
    if raw:
        return [body]
    s = re.sub(r" *\n *", "\n", body)
    parts = s.split(monologue) if monologue else [s]
    out = []
    for part in parts:
        part = part.strip()
        if not part:
            continue
        part = re.sub(r"[ \n]+", " ", part) if monologue else re.sub(r" +", " ", part)
        out.append(_ESCAPE_RE.sub(_dequote, part))
    return out


# ---- per-line cursor --------------------------------------------------------

KEYWORDS = {"as", "if", "in", "return", "with", "while"}
IMAGE_KEYWORDS = {"behind", "at", "onlayer", "with", "zorder", "transform"}
WORD = r"[a-zA-Z_ -�][0-9a-zA-Z_ -�]*"
IMAGE_WORD = r"[-0-9a-zA-Z_ -�][-0-9a-zA-Z_ -�]*"
OPERATOR_RE = re.compile(
    "|".join([re.escape(o) for o in ["<>", "<<", "<=", "<", ">>", ">=", ">", "!=", "==", "|", "^", "&",
                                     "+", "-", "**", "*", "//", "/", "%", "~", "@", ":="]]
             + [r"\bor\b", r"\band\b", r"\bnot\b", r"\bin\b", r"\bis\b"]))
STRING_RE = re.compile(r'r?"(?:[^\\"]|\\.)*"|r?\'(?:[^\\\']|\\.)*\'|r?`(?:[^\\`]|\\.)*`', re.S)
TRIPLE_RE = re.compile(r'r?"""(?:[^\\"]|\\.|"(?!""))*"""|r?\'\'\'(?:[^\\\']|\\.|\'(?!\'\'))*\'\'\'', re.S)
PY_STRING_RE = re.compile(
    r'[urUR]{0,2}(?:"""(?:[^\\]|\\.)*?"""|\'\'\'(?:[^\\]|\\.)*?\'\'\'|"(?:[^\\"]|\\.)*"|\'(?:[^\\\']|\\.)*\')', re.S)
FLOAT_RE = re.compile(r"(\+|\-)?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")


class LexError(Exception):
    pass


class LineLexer:
    """Cursor over one logical line, mirroring the Ren'Py Lexer methods we need."""

    def __init__(self, line, monologue=None):
        self.line = line
        self.text = line.text
        self.pos = 0
        self.monologue = monologue

    @property
    def where(self):
        return self.line.where

    def error(self, msg):
        raise LexError(msg)

    def skip_ws(self):
        while self.pos < len(self.text) and self.text[self.pos] in " \t\n\f":
            self.pos += 1

    def eol(self):
        self.skip_ws()
        return self.pos >= len(self.text)

    def at(self, s):
        self.skip_ws()
        return self.text.startswith(s, self.pos)

    def match(self, regex):
        self.skip_ws()
        if isinstance(regex, str):
            regex = re.compile(regex)
        m = regex.match(self.text, self.pos)
        if not m:
            return None
        self.pos = m.end()
        return m.group(0)

    def keyword(self, word):
        old = self.pos
        w = self.match(WORD)
        if w == word:
            return True
        self.pos = old
        return False

    def word(self):
        return self.match(WORD)

    def name(self):
        old = self.pos
        w = self.match(WORD)
        if w is None:
            return None
        if w in ("r", "u", "ur") and self.text[self.pos:self.pos + 1] in ('"', "'", "`"):
            self.pos = old
            return None
        if w in KEYWORDS:
            self.pos = old
            return None
        return w

    def dotted_name(self):
        old = self.pos
        n = self.name()
        if n is None:
            return None
        parts = [n]
        while self.text.startswith(".", self.pos):
            self.pos += 1
            w = self.word()
            if not w:
                self.pos = old
                return None
            parts.append(w)
        return ".".join(parts)

    def label_name(self, global_label=None):
        """`name`, `.local`, or `global.local`; returns the absolute name."""
        old = self.pos
        self.skip_ws()
        local = self.text.startswith(".", self.pos)
        if local:
            self.pos += 1
            w = self.word()
            if not w or not global_label:
                self.pos = old
                return None
            return global_label + "." + w
        n = self.name()
        if n is None:
            return None
        if self.text.startswith(".", self.pos):
            self.pos += 1
            w = self.word()
            if not w:
                self.pos = old
                return None
            return n + "." + w
        return n

    def image_name_component(self):
        old = self.pos
        w = self.match(IMAGE_WORD)
        if w is None:
            return None
        if w in ("r", "u", "ur") and self.text[self.pos:self.pos + 1] in ('"', "'", "`"):
            self.pos = old
            return None
        if w in KEYWORDS or w in IMAGE_KEYWORDS:
            self.pos = old
            return None
        return w

    def integer(self):
        return self.match(r"(\+|\-)?\d+")

    def float(self):
        return self.match(FLOAT_RE)

    def string(self):
        """A single-quoted Ren'Py string, unescaped. None when absent."""
        s = self.match(STRING_RE)
        if s is None:
            return None
        raw = s[0] == "r"
        if raw:
            s = s[1:]
        return renpy_string(s[1:-1], raw)

    def triple_string(self):
        s = self.match(TRIPLE_RE)
        if s is None:
            return None
        raw = s[0] == "r"
        if raw:
            s = s[1:]
        return renpy_triple_string(s[3:-3], raw, self.monologue)

    def python_string(self):
        return self.match(PY_STRING_RE)

    def delimited(self):
        """Balanced bracket group starting at the cursor; returns its raw text."""
        self.skip_ws()
        if self.pos >= len(self.text) or self.text[self.pos] not in "([{":
            return None
        start = self.pos
        stack = []
        i = self.pos
        n = len(self.text)
        pairs = {"(": ")", "[": "]", "{": "}"}
        while i < n:
            c = self.text[i]
            if c in "\"'`":
                m = PY_STRING_RE.match(self.text, i) or STRING_RE.match(self.text, i)
                if not m:
                    self.error("unterminated string")
                i = m.end()
                continue
            if c in pairs:
                stack.append(pairs[c])
            elif c in ")]}":
                if not stack or stack.pop() != c:
                    self.error(f"unmatched '{c}'")
                if not stack:
                    self.pos = i + 1
                    return self.text[start:self.pos]
            i += 1
        self.error("unclosed bracket")

    def simple_expression(self, comma=False, operator=True, image=False):
        """Raw text of a Ren'Py simple_expression, or None."""
        self.skip_ws()
        start = self.pos

        def lex_name():
            old = self.pos
            n = self.name()
            if n is not None and image and n in IMAGE_KEYWORDS:
                self.pos = old
                return None
            return n

        while True:
            while self.match(OPERATOR_RE):
                pass
            if self.eol():
                break
            if not (self.python_string() or lex_name() or self.float() or self.delimited()):
                break
            while True:
                if self.eol():
                    break
                if self.text.startswith(".", self.pos) and not self.text.startswith("..", self.pos):
                    save = self.pos
                    self.pos += 1
                    if not self.word():
                        self.pos = save
                        break
                    continue
                if self.text[self.pos] in "([{" and self.delimited():
                    continue
                break
            if operator:
                save = self.pos
                op = self.match(OPERATOR_RE)
                if op:
                    if image and op.strip() in IMAGE_KEYWORDS:
                        self.pos = save
                        break
                    continue
            if comma and self.match(","):
                continue
            break
        text = self.text[start:self.pos].strip()
        return text or None

    def rest(self):
        self.skip_ws()
        s = self.text[self.pos:].strip()
        self.pos = len(self.text)
        return s

    def expect_eol(self, what):
        if not self.eol():
            self.error(f"unexpected text after {what}: {self.text[self.pos:][:40]!r}")
