"""Dialogue translation identifiers, computed the way Ren'Py's Restructurer does.

Ren'Py (renpy/translation/__init__.py, MIT) groups translatable statements up
to and including each say statement and names the group
`<label>_<first 8 hex digits of md5(get_code() + "\\r\\n" for each say)>`,
adding `_1`, `_2`, ... on collisions. `translate <language> <id>:` blocks
replace the group when that language is selected at build time.
"""

import hashlib
import re

from .reader import Nvl, Say, Voice


def encode_say_string(s):
    s = s.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')
    s = re.sub(r"(?<= ) ", "\\ ", s)
    return '"' + s + '"'


def encode_say_code(say):
    """Ren'Py's Say.get_code()."""
    rv = []
    if say.who:
        rv.append(say.who)
    if say.attrs:
        rv.extend(say.attrs)
    if say.temp_attrs:
        rv.append("@")
        rv.extend(say.temp_attrs)
    rv.append(encode_say_string(say.what))
    if not say.interact:
        rv.append("nointeract")
    if say.ident:
        rv += ["id", say.ident]
    if say.args:
        rv.append(say.args)
    if say.with_:
        rv += ["with", say.with_]
    return " ".join(rv)


def translatable(node):
    return isinstance(node, (Say, Voice)) or (isinstance(node, Nvl) and node.mode == "clear")


class TranslationIds:
    def __init__(self):
        self.label = None
        self.used = set()

    def start_file(self):
        self.label = None

    def enter_label(self, node):
        if not node.hide and not node.name.startswith("_"):
            self.label = node.name

    def _unique(self, digest):
        base = digest if self.label is None else self.label.replace(".", "_") + "_" + digest
        i, ident = 0, base
        while ident in self.used:
            i += 1
            ident = f"{base}_{i}"
        return ident

    def ident(self, group):
        """Return (identifier, alternate) for a group of translatable statements."""
        md5 = hashlib.md5()
        for n in group:
            if isinstance(n, Say):
                md5.update((encode_say_code(n) + "\r\n").encode("utf-8"))
        generated = self._unique(md5.hexdigest()[:8])
        explicit = None
        for n in group:
            if isinstance(n, Say) and n.ident:
                explicit = n.ident
        self.used.add(generated)
        if explicit:
            self.used.add(explicit)
            return explicit, generated
        return generated, None
