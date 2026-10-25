"""Opt-in checks against the three target games (game data stays outside the repo).

Fetch them with `python -I tools/census/fetch_games.py --full <dir> the_question arcance zero_echoes`
and point RPY2PSP_TARGETS at <dir>.
Skipped when the folder is missing.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from refvm import Program, explore  # noqa: E402
from rpy2psp.compiler.compile import compile_game  # noqa: E402

ROOT = os.environ.get("RPY2PSP_TARGETS", os.path.join(tempfile.gettempdir(), "rpy2psp-census", "full"))
GAMES = {"the_question": "the_question/the_question", "zero_echoes": "zero_echoes", "arcance": "arcance"}


def game_path(name):
    path = os.path.join(ROOT, GAMES[name])
    if not os.path.isdir(path):
        raise unittest.SkipTest(f"target game not present: {path}")
    return path


class TargetGamesTest(unittest.TestCase):
    def test_compile_without_errors_and_play_all_paths(self):
        for name, inputs in (("the_question", ()), ("zero_echoes", ("20000617", "2180")), ("arcance", ())):
            with self.subTest(game=name):
                comp = compile_game(game_path(name))
                self.assertEqual([str(e) for e in comp.errors], [])
                prog = Program(comp.bytecode())
                runs = explore(prog, max_depth=12, max_paths=3000, inputs=inputs)
                self.assertTrue(any(finished for _, _, finished in runs))

    def test_the_question_reaches_every_line(self):
        prog = Program(compile_game(game_path("the_question")).bytecode())
        runs = explore(prog)
        lines = {e[2] for _, events, _ in runs for e in events if e[0] == "say"}
        self.assertEqual(len(lines), 71)
        self.assertEqual(len(runs), 3)

    def test_the_question_translations_match_every_line(self):
        for lang in ("japanese", "korean", "schinese", "tchinese"):
            with self.subTest(language=lang):
                comp = compile_game(game_path("the_question"), lang)
                self.assertEqual(comp.errors, [])
                self.assertEqual(len(comp.tl_blocks), 70)
                self.assertEqual(comp.tl_used, set(comp.tl_blocks))
                self.assertEqual(comp.manifest()["fonts"]["gui.text_font"], "SourceHanSansLite.ttf")


if __name__ == "__main__":
    unittest.main()
