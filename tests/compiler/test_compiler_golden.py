"""Golden disassembly tests: one self-written script per statement family.

Set UPDATE_GOLDEN=1 to rewrite the expected listings after an intended change.
"""

import os
import shutil
import sys
import tempfile
import unittest

from PIL import Image as PILImage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from refvm import VM, Program  # noqa: E402
from rpy2psp.compiler.bytecode import disassemble_container  # noqa: E402
from rpy2psp.compiler.compile import compile_game  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")
GOLDEN = os.path.join(HERE, "golden")


def make_game(case):
    """Temporary game folder with the case script and tiny generated assets."""
    root = tempfile.mkdtemp(prefix="rpy2psp-test-")
    game = os.path.join(root, "game")
    os.makedirs(os.path.join(game, "images"))
    shutil.copy(os.path.join(CASES, case + ".rpy"), os.path.join(game, "script.rpy"))
    PILImage.new("RGB", (64, 36), (10, 20, 30)).save(os.path.join(game, "images", "bg room.png"))
    PILImage.new("RGBA", (40, 80), (200, 100, 50, 255)).save(os.path.join(game, "eileen happy.png"))
    for mood in ("neutral", "happy", "sad"):
        PILImage.new("RGBA", (40, 80), (200, 100, 50, 255)).save(os.path.join(game, "images", f"eileen {mood}.png"))
    for name in ("theme.ogg", "click.wav", "line1.ogg"):
        with open(os.path.join(game, name), "wb") as f:
            f.write(b"placeholder")
    return root


def listing(comp):
    return disassemble_container(comp.bytecode()) + "-- warnings --\n" + "".join(w + "\n" for w in comp.warnings)


def symbol_values(vm):
    return {vm.p.symbols[i][0]: v for i, v in vm.vars.items()}


class GoldenTest(unittest.TestCase):
    maxDiff = None

    def compile_case(self, case, language=None):
        root = make_game(case)
        self.addCleanup(shutil.rmtree, root, True)
        return compile_game(root, language)

    def check_golden(self, name, comp):
        self.assertEqual([str(e) for e in comp.errors], [])
        text = listing(comp)
        path = os.path.join(GOLDEN, name + ".txt")
        if os.environ.get("UPDATE_GOLDEN") or not os.path.exists(path):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        with open(path, encoding="utf-8") as f:
            self.assertEqual(text, f.read())

    def test_say_and_characters(self):
        comp = self.compile_case("say_and_characters")
        self.check_golden("say_and_characters", comp)
        events = VM(Program(comp.bytecode())).play()
        says = [e for e in events if e[0] == "say"]
        self.assertEqual(says[0], ("say", None, "Narration with {b}bold{/b} and an escaped {{brace} and [[bracket]."))
        self.assertEqual(says[1], ("say", "Eileen", "Hello, [player_name]!"))
        self.assertIn(("attrs", "Eileen", ["happy"]), events)
        self.assertIn(("say", "[player_name]", "Dynamic name."), says)
        self.assertEqual(says[-2:], [("say", None, "Monologue one."), ("say", None, "Monologue two.")])

    def test_control_flow(self):
        comp = self.compile_case("control_flow")
        self.check_golden("control_flow", comp)
        vm = VM(Program(comp.bytecode()), choices=[0])
        events = vm.play()
        says = [e[2] for e in events if e[0] == "say"]
        self.assertEqual(says, ["Big.", "Hi [name] x[times].", "Hi [name] x[times].", "Pick one.", "Done: [color]"])
        values = symbol_values(vm)
        self.assertEqual(values["score"], 5)
        self.assertEqual(values["color"], "red")
        self.assertEqual(values["seen"], {"Red"})
        self.assertEqual(values["total"], 3)
        self.assertNotIn("name", values)  # label parameters are restored on return

    def test_display_and_audio(self):
        comp = self.compile_case("display_and_audio")
        self.check_golden("display_and_audio", comp)
        m = comp.manifest()
        kinds = sorted(a["kind"] for a in m["assets"].values())
        self.assertEqual(kinds.count("music"), 1)
        self.assertIn("sfx", kinds)
        self.assertIn("voice", kinds)
        recipes = [a["recipe"] for a in m["assets"].values() if a["kind"] == "image"]
        self.assertIn({"op": "factor", "factor": [0.5, 0.5], "child": {"op": "file", "path": "eileen happy.png"}},
                      recipes)
        self.assertIn("start", m["scenes"])

    def test_expressions(self):
        comp = self.compile_case("expressions")
        self.check_golden("expressions", comp)
        vm = VM(Program(comp.bytecode()), inputs=["  Kim  "])
        vm.play()
        values = symbol_values(vm)
        self.assertEqual(values["name"], "Kim")
        self.assertEqual(values["inventory"], ["key", "map"])
        self.assertEqual(values["stats"], {"str": 4})
        self.assertEqual(values["ratio"], (4 * 2.5 - 1) % 7)
        self.assertEqual(values["persistent.endings"], 1)
        self.assertEqual(values["part"], "im")
        self.assertIs(values["mobile"], True)

    def test_translations(self):
        comp = self.compile_case("translations", "vietnamese")
        self.check_golden("translations-vietnamese", comp)
        self.assertEqual(comp.warnings, [])
        events = VM(Program(comp.bytecode()), choices=[1]).play()
        self.assertEqual([e[1:] for e in events if e[0] == "say"],
                         [("Ai Linh", "Chào buổi sáng."), ("Ai Linh", "Cà phê hay trà?"), (None, "You chose tea.")])
        menu = next(e for e in events if e[0] == "menu")
        self.assertEqual(menu[1], ["Cà phê", "Trà"])
        self.assertIn("à", comp.manifest()["glyphs"])
        original = self.compile_case("translations")
        says = [e[2] for e in VM(Program(original.bytecode()), choices=[0]).play() if e[0] == "say"]
        self.assertEqual(says, ["Good morning.", "Coffee or tea?", "You chose coffee."])

    def test_unsupported_reports_file_and_line(self):
        comp = self.compile_case("unsupported")
        got = sorted((e.where.rsplit("/", 1)[-1], e.feature) for e in comp.errors)
        self.assertEqual(got, sorted([
            ("script.rpy:2", "py:stmt:FunctionDef"),
            ("script.rpy:9", "screen:show:custom"),
            ("script.rpy:10", "py:call:helper"),
            ("script.rpy:11", "py:assign:tuple"),
            ("script.rpy:12", "py:call:ImageDissolve"),
            ("script.rpy:13", "trans:irisin"),
            ("script.rpy:14", "stmt:layeredimage"),
            ("script.rpy:16", "text:tag:image"),
            ("script.rpy:17", "atl:prop:function"),
            ("script.rpy:18", "text:interp-expr"),
            ("script.rpy:19", "jump:undefined"),
        ]))


if __name__ == "__main__":
    unittest.main()
