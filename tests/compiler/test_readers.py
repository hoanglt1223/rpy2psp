"""Lexer string rules, .rpa archives, and the restricted .rpyc decoder."""

import os
import pickle
import shutil
import struct
import sys
import tempfile
import types
import unittest
import zlib

from rpy2psp.compiler.bytecode import disassemble_container
from rpy2psp.compiler.compile import compile_game
from rpy2psp.compiler.lexer import LineLexer, Line, renpy_string, renpy_triple_string
from rpy2psp.compiler.reader import Game, Say, parse_rpy_text
from rpy2psp.compiler.rpa import RpaArchive, RpaError, load_index, write_rpa3
from rpy2psp.compiler.rpyc_decode import RPYC2_HEADER, RpycError, decode_rpyc


class LexerTest(unittest.TestCase):
    def test_renpy_string_rules(self):
        self.assertEqual(renpy_string("a  b\n   c"), "a b c")
        self.assertEqual(renpy_string(r"x\ \ y"), "x  y")
        self.assertEqual(renpy_string(r"\{b\} \[v\] 50\% \n \"q\""), '{{b} [[v] 50%% \n "q"')
        self.assertEqual(renpy_string(r"\u00e9"), "é")
        self.assertEqual(renpy_string(r"raw\n", raw=True), r"raw\n")
        self.assertEqual(renpy_triple_string("One\n   line.\n\n   Two.", monologue="\n\n"), ["One line.", "Two."])

    def test_line_lexer(self):
        lx = LineLexer(Line("f", 1, 1, 0, 'e happy @ sad "Hi" with Dissolve(1.0, alpha=True)'))
        self.assertEqual(lx.simple_expression(operator=False), "e")
        self.assertEqual(lx.image_name_component(), "happy")
        self.assertTrue(lx.match("@"))
        self.assertEqual(lx.image_name_component(), "sad")
        self.assertEqual(lx.string(), "Hi")
        self.assertTrue(lx.keyword("with"))
        self.assertEqual(lx.simple_expression(), "Dissolve(1.0, alpha=True)")
        self.assertTrue(lx.eol())

    def test_say_continuation_and_menu_caption(self):
        nodes = parse_rpy_text('label a:\n    "One \\\n    two"\n    menu:\n        "Q?"\n        "A":\n            pass\n', "x.rpy")
        say = nodes[0].block[0]
        self.assertIsInstance(say, Say)
        self.assertEqual(say.what, "One  two")
        menu = nodes[0].block[1]
        self.assertEqual(menu.caption[0].what, "Q?")
        self.assertFalse(menu.caption[0].interact)
        self.assertEqual([i.text for i in menu.items], ["A"])


class RpaTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="rpy2psp-rpa-")
        self.addCleanup(shutil.rmtree, self.dir, True)

    def test_round_trip_and_game_view(self):
        game = os.path.join(self.dir, "game")
        os.makedirs(game)
        write_rpa3(os.path.join(game, "archive.rpa"), {"script.rpy": b'label start:\n    "From an archive."\n',
                                                      "images/bg x.png": b"\x89PNG"})
        arc = RpaArchive(os.path.join(game, "archive.rpa"))
        self.addCleanup(arc.close)
        self.assertEqual(arc.names(), ["images/bg x.png", "script.rpy"])
        self.assertEqual(arc.read("images/bg x.png"), b"\x89PNG")
        g = Game(game)
        self.addCleanup(g.close)
        self.assertTrue(g.exists("script.rpy"))
        self.assertIn(b"From an archive", g.read("script.rpy"))

    def test_index_refuses_globals(self):
        evil = b"cos\nsystem\n(S'echo pwned'\ntR."
        with self.assertRaises(RpaError):
            load_index(zlib.compress(evil))

    def test_unknown_header(self):
        path = os.path.join(self.dir, "x.rpa")
        with open(path, "wb") as f:
            f.write(b"NOT-AN-RPA\n")
        with self.assertRaises(RpaError):
            RpaArchive(path)


def _fake_renpy():
    """Classes named like Ren'Py's, used only to *write* a test .rpyc."""
    renpy = types.ModuleType("renpy")
    ast_mod = types.ModuleType("renpy.ast")
    renpy.ast = ast_mod

    def cls(name):
        c = type(name, (object,), {"__module__": "renpy.ast"})
        setattr(ast_mod, name, c)
        return c

    names = ["Label", "Say", "Define", "PyCode", "Menu", "Jump", "Return", "Init", "Show", "With", "UserStatement",
             "If", "Python"]
    classes = {n: cls(n) for n in names}

    def make(_cls, **attrs):
        o = classes[_cls].__new__(classes[_cls])
        o.__dict__.update(attrs)
        return o
    return renpy, ast_mod, make


class RpycTest(unittest.TestCase):
    def write_rpyc(self, stmts):
        renpy, ast_mod, _ = self.fake
        old = {k: sys.modules.get(k) for k in ("renpy", "renpy.ast")}
        sys.modules["renpy"], sys.modules["renpy.ast"] = renpy, ast_mod
        try:
            payload = zlib.compress(pickle.dumps(({"version": 5003000}, stmts), 2))
        finally:
            for k, v in old.items():
                if v is None:
                    sys.modules.pop(k, None)
                else:
                    sys.modules[k] = v
        header = RPYC2_HEADER + struct.pack("<III", 1, len(RPYC2_HEADER) + 24, len(payload)) + struct.pack("<III", 0, 0, 0)
        return header + payload

    def setUp(self):
        self.fake = _fake_renpy()

    def test_decode_and_compile_like_source(self):
        _, _, make = self.fake
        stmts = [
            make("Init", priority=0, linenumber=1, filename="s.rpy",
                 block=[make("Define", varname="e", store="store", operator="=", index=None, linenumber=1,
                             code=make("PyCode", source='Character("Eve", color="#ff0000")'))]),
            make("Label", name="start", parameters=None, hide=False, linenumber=3, block=[
                make("Say", who="e", what="Hello  there.", with_=None, interact=True, attributes=None,
                     temporary_attributes=None, arguments=None, linenumber=4),
                make("UserStatement", line='pause 1.0', block=[], linenumber=5),
                make("Menu", items=[("Yes", "True", [make("Jump", target="start", expression=False, linenumber=7)]),
                                    ("No", "True", [make("Return", expression=None, linenumber=9)])],
                     set=None, arguments=None, linenumber=6),
            ]),
        ]
        data = self.write_rpyc(stmts)
        tree = decode_rpyc(data, "s.rpyc")
        self.assertEqual(tree[1].children[0].text, 'e "Hello \\ there."')

        root = tempfile.mkdtemp(prefix="rpy2psp-rpyc-")
        self.addCleanup(shutil.rmtree, root, True)
        os.makedirs(os.path.join(root, "a", "game"))
        os.makedirs(os.path.join(root, "b", "game"))
        with open(os.path.join(root, "a", "game", "s.rpyc"), "wb") as f:
            f.write(data)
        with open(os.path.join(root, "b", "game", "s.rpy"), "w", encoding="utf-8") as f:
            f.write('define e = Character("Eve", color="#ff0000")\n\nlabel start:\n    e "Hello \\ there."\n'
                    '    pause 1.0\n    menu:\n        "Yes":\n            jump start\n        "No":\n'
                    '            return\n')
        a = compile_game(os.path.join(root, "a"))
        b = compile_game(os.path.join(root, "b"))
        self.assertEqual(a.errors, [])
        self.assertEqual(disassemble_container(a.bytecode()), disassemble_container(b.bytecode()))

    def test_refuses_foreign_globals(self):
        evil = zlib.compress(b"cos\nsystem\n(S'echo pwned'\ntR.")
        with self.assertRaises(RpycError):
            decode_rpyc(evil, "evil.rpyc")
        evil2 = zlib.compress(b"cbuiltins\neval\n(S'1'\ntR.")
        with self.assertRaises(RpycError):
            decode_rpyc(evil2, "evil.rpyc")


if __name__ == "__main__":
    unittest.main()
