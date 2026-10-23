"""Standalone Ren'Py script lexer.

Turns .rpy source into logical lines and an indentation tree, following the
rules of renpy/lexer.py closely enough for a census: comments, strings that
span physical lines, bracket continuation, and backslash continuation. It never
imports Ren'Py and never executes game code.
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
    |(?P<name>[A-Za-z_\u00a0-\uffff][0-9A-Za-z_\u00a0-\uffff]*)
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
