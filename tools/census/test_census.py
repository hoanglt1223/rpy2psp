"""Unit tests on self-authored Ren'Py snippets.  python -I -m unittest test_census  (run from this folder)"""

import os
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analyzer import GameCensus  # noqa: E402
from rpy_lexer import build_tree, logical_lines  # noqa: E402

SCRIPT = textwrap.dedent('''\
    define e = Character("Eileen", color="#c8ffc8")
    define flash = Fade(0.1, 0.0, 0.5, color="#fff")
    default points = 0

    transform bounce:
        linear 0.5 yoffset -10
        ease 0.5 yoffset 0
        repeat

    label start:
        scene bg room with dissolve
        show eileen happy at left, bounce
        e "Hello, {b}world{/b}! # not a comment" # a comment
        "A string that
         spans two lines."
        menu:
            "Ask her" if points > 1:
                $ points += 1
                jump ask
            "Leave":
                call screen tally
        with flash
        play music "theme.ogg" fadein 1.0
        $ name = renpy.input("Name?").strip()
        return

    screen tally():
        timer 2.0 action Jump("start")
    ''')


class LexerTest(unittest.TestCase):
    def test_logical_lines_and_tree(self):
        lines = logical_lines(SCRIPT, "script.rpy")
        say = [ln for ln in lines if ln.text.startswith('e "Hello')][0]
        self.assertNotIn("a comment", say.text.replace("# not a comment", ""))
        multi = [ln for ln in lines if ln.text.startswith('"A string')][0]
        self.assertEqual((multi.lineno, multi.end_lineno), (14, 15))
        tree = build_tree(lines)
        label = [n for n in tree if n.text.startswith("label start")][0]
        self.assertEqual(label.children[0].text, "scene bg room with dissolve")


class AnalyzerTest(unittest.TestCase):
    def test_feature_keys(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "script.rpy"), "w", encoding="utf-8") as f:
                f.write(SCRIPT)
            feats = GameCensus("t", d).run().to_dict()["features"]
        for key in ("stmt:say", "say:character", "say:narrator", "text:tag:b", "trans:dissolve",
                    "trans:defined:Fade", "at:builtin", "at:atl", "atl:warper:linear", "atl:repeat",
                    "menu:cond", "py:augassign", "screen:call:custom-static", "audio:fadein",
                    "py:call:renpy.input", "py:method:strip", "char:kw:color", "stmt:default"):
            self.assertIn(key, feats, key)
        self.assertEqual(feats["stmt:say"]["count"], 2)


if __name__ == "__main__":
    unittest.main()
